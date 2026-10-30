# Step 1 towards a mounted root: `options MOCKFS`, and where a ramdisk already comes from

Written 2026-10-30 21:3x, after the tick landed (`690cb43`, `4ac085d`) and
[`ROOTFS-PLAN.md`](ROOTFS-PLAN.md) was re-checked against the patched tree in `D:\aios\xnu`
(`XNU 8796.141.3` + `patches/xnu/*.patch`). Line numbers are from that tree at the time of
writing; re-grep the anchor name if a number has drifted. Nothing here has been booted.

How to read the citations — the same method as [`GIC-FORENSICS.md`](GIC-FORENSICS.md) §1:

| Tag | Meaning |
|---|---|
| **[M]** | measured on this phone, in a log kept in this repo (`docs/logs/`) |
| **[TREE]** | read out of the XNU tree / this repo at the cited `file:line` |
| **[BIN]** | the string or pattern is present in a shipped binary in `Resources/UniXNU/` |
| **[INF]** | inference from the above, labelled so it can be attacked |

---

## 0. The short version

| Question | Answer |
|---|---|
| Is `md` built for `MACHINE=dandelion`? | **Yes** — `bsd/dev/memdev.c` is `standard` ([TREE] `bsd/conf/files:106`), the `mdevdevice` pseudo-device in `config/MASTER:483` is **untagged** so it is in every configuration, and the file has no `#if` around `mdevadd()`/`mdevstrategy()` ([TREE] `bsd/dev/memdev.c:639,262`). `mdevinit()` itself is an empty function unless `CONFIG_MEMDEV_INSECURE` ([TREE] `memdev.c:487-551`), so the `pmd0=`/`vmd0=` **boot arguments do nothing on this port** |
| Is mockfs the only `vfc_mountroot`? | **Yes** — one non-NULL entry, `mockfs_mountroot` ([TREE] `bsd/vfs/vfs_conf.c:205`), inside `#if MOCKFS` (`:197-214`); the other seven entries are `NULL` (`:130,146,168,186,224,241,256`) |
| Does `vfs_mountroot()` need a device first? | **Yes** — `bdevvp(rootdev, &rootvp)` is the first thing after the `mountroot` hook ([TREE] `bsd/vfs/vfs_subr.c:1240`), and it fails only on `NODEV` ([TREE] `:2102`). It does **not** check that a `bdevsw` exists for the major |
| Minimal change to turn mockfs on | **One untagged line** in `config/MASTER.arm64.dandelion` (`options   MOCKFS`). No `bsd/conf/files` edit, no patch: the sources are already `optional mockfs` and `config/MASTER:115` already declares the option |
| Where does a ramdisk come from? | **It is already implemented end to end, and none of it is in XNU.** `UniXNU.toml` `initrd` → UniXNU reads the file → `DarwinBoot` copies it into the kernel window and writes `/chosen/memory-map/RAMDisk` in the ADT → `IOFindBSDRoot()` calls `mdevadd()` → `rd=md0` picks it up. Two things are missing on our side: the `RAMDisk` **placeholder in `bootshim/adt.c`**, and the **file in the ESP** |
| Does mockfs then exec the image? | **Not without one more kernel change.** The file node's size comes from `si_devsize` ([TREE] `bsd/miscfs/mockfs/mockfs_fsnode.c:81`), which in this tree is written **only** by `spec_open()` ([TREE] `bsd/miscfs/specfs/spec_vnops.c:452`), and the mountroot path never opens the device. Hence `zzz2-dandelion-mockfs-size.patch` |
| First thing to change | `options MOCKFS` (kernel) — then the ADT/initrd pair (firmware/ESP), which needs no kernel rebuild at all |

---

## 1. What `ROOTFS-PLAN.md` relies on, re-verified

### 1.1 `md` is compiled for this board

* `bsd/conf/files:106` — `bsd/dev/memdev.c   standard`. `standard` means "always", no option
  needed ([TREE]).
* `config/MASTER:483` — `pseudo-device  mdevdevice  1  init  mdevinit`, **no `# <tag>` suffix**,
  therefore part of every configuration, including `DEVELOPMENT` (see §1.4 for how the tag
  filter works).
* `bsd/kern/bsd_init.c:1269-1275` walks `pseudo_inits` in `bsd_autoconf()`, which is where
  `mdevinit()` would run ([TREE]).
* **Correction to the plan's step 2**: `mdevinit()` is empty unless `CONFIG_MEMDEV_INSECURE`
  ([TREE] `memdev.c:490-548`), so `pmd0=<phys>.<size>` on the command line is not an option on
  this port, and `mdevadd()` must be called from code (Apple's own call site is
  `IOFindBSDRoot()`, §2.1).
* What memdev accepts ([TREE] `memdev.c`): `mdevadd(devid, base, size, phys)` takes **page
  counts**, not bytes (`mdBase`/`mdSize` are pages, `<< 12` everywhere: `:161-162,288,307`);
  `phys = 0` means `base` is a kernel **virtual** address and the strategy does a plain `bcopy`
  (`:309-313`); `phys = 1` means physical and goes through `pmap_find_phys()`/`bcopy_phys()`
  (`:314-331`). `mdevadd()` registers `bdevsw`/`cdevsw` and makes `/dev/mdN` + `/dev/rmdN`
  itself (`:669-701`) and prints
  `Added memory device md%x/rmd%x (%08X/%08X) at %016llX for %016llX` (`:710`).
* `mdevstrategy()` copies inline and calls `buf_biodone()` (`:262-366`), so an md read is
  synchronous — no thread is ever parked on it (relevant now that the tick exists but is the
  only thing that wakes threads).

### 1.2 mockfs is the only filesystem that can be a root here

`bsd/vfs/vfs_conf.c` has exactly eight `vfsconf` entries; `vfc_mountroot` is `NULL` for all
except mockfs:

```
130  devfs      .vfc_mountroot = NULL
146  devfs(alt) .vfc_mountroot = NULL
168  nullfs     .vfc_mountroot = NULL
186  bindfs     .vfc_mountroot = NULL
205  mockfs     .vfc_mountroot = mockfs_mountroot      <- inside #if MOCKFS (197-214)
224  routefs    .vfc_mountroot = NULL
241  ...        .vfc_mountroot = NULL
256  ...        .vfc_mountroot = NULL
```

`vfs_mountroot()` skips every entry whose `vfc_mountroot` is `NULL` **and** which lacks
`VFC_VFSCANMOUNTROOT` ([TREE] `bsd/vfs/vfs_subr.c:1253-1256`), so with `MOCKFS` off the loop
body never runs and the function returns `ENODEV` at `:1398`. That is the current
`[dl] vfs_mountroot -> 19` ([INF]: the number follows from `:1393-1398`, the line itself has
not been seen yet — the log in `docs/logs/` predates the `4ac085d` rewrite of the loop).

### 1.3 the device comes before the filesystem

`vfs_mountroot()` order ([TREE] `vfs_subr.c`):

1. `:1231` if `mountroot != NULL` (netboot) take that path and return;
2. `:1240` `bdevvp(rootdev, &rootvp)` — on failure prints
   `vfs_mountroot: can't setup bdevvp` and returns;
3. `:1252` walk `vfsconf` → `(*vfsp->vfc_mountroot)(mp, rootvp, ctx)`;
4. `:1294` `vfs_init_io_attributes(rootvp, mp)` (ioctls; failure is deliberately ignored);
5. `:1361` with `CONFIG_MACF` on and a single-label filesystem it returns 0 **without**
   calling `VFS_ROOT` — mockfs is single-label, so `VFS_ROOT` happens later, in `bsd_init()`
   ([TREE] `bsd/kern/bsd_init.c:1035`).

`bdevvp()` ([TREE] `:2094-2130`) builds a `VBLK` vnode with `spec_vnodeop_p` and
`vnfs_filesize = 0`; its only failure mode is `dev == NODEV` (`:2102`), plus allocation
failures. It never consults `bdevsw`, so a **fabricated** `dev_t` still yields a vnode;
`spec_ioctl()` on such a major lands on the `NO_BDEVICE` stubs
([TREE] `bsd/sys/conf.h:179-181`: `eno_opcl, eno_opcl, eno_strat, eno_ioctl, eno_dump,
eno_psize`) and returns an error rather than panicking ([TREE] `spec_vnops.c:797-802`).

Today `setconf()` fabricates exactly that device when `IOFindBSDRoot()` fails:
`rootdev = makedev(6, 0)`, `rootdevice = "sd0a"`, `flags = 0`, `mountroot = NULL`
([TREE] `bsd/kern/bsd_init.c:1289-1305`), and the `DANDELION` block in `IOFindBSDRoot()`
returns `kIOReturnNotFound` after three 1-second waits instead of hanging
([TREE] `iokit/bsddev/IOKitBSDInit.cpp:943-978`). So the mount loop does reach
`vfs_mountroot()` with a valid-looking but non-existent block device. That matters for §3.

### 1.4 the minimal way to turn mockfs on

`config/MASTER.arm64.dandelion` is the machine configuration (the repo copy is authoritative —
`tools/ci/apply-board.sh:27` copies it over the XNU tree, and `check_series.py` lists it under
`BOARD_FILES`). The option lines in it are read by the config tool as follows:

* `SETUP/config/doconf:196-268` concatenates `MASTER` +
  `MASTER.<machine>` and filters every line that carries a `# <tag>` trailer against the set
  lists, which are the commented `#  NAME = [ … ]` lines at the top of the file
  (`doconf:222` turns them into `NAME#members`);
* a line **without** a `# <tag>` trailer is kept for every configuration — that is how
  `options CONFIG_MACH_APPROXIMATE_TIME` (line 92) and `options XNU_EMMC_SERIAL=1` (line 97)
  work today, the latter being proven by the eMMC log channel itself ([M]
  `docs/LOG-CHANNEL.md`);
* `options MOCKFS` becomes `-DMOCKFS` ([TREE] `SETUP/config/mkmakefile.c:203-205`: a valueless
  option is emitted as ` -DNAME`), and `-DNAME` defines it as `1`, so the `#if MOCKFS` in
  `vfs_conf.c:197` is true;
* the three mockfs sources are then included by their `optional mockfs` lines
  (`bsd/conf/files:155-157`).

So the whole change is:

```diff
--- a/config/MASTER.arm64.dandelion
+++ b/config/MASTER.arm64.dandelion
@@
 options   XNU_EMMC_SERIAL=1
 
+options   MOCKFS
+
 options   CONFIG_KERNEL_INTEGRITY		# <kernel_integrity>
```

(with the comment block that is in the file now). **No** `bsd/conf/files` edit, **no**
`config/MASTER` edit, **no** `MOCKFS` in a set list: `mockfs` appears in no set, which is
exactly why the option has to be added here. `patches/xnu/ci-arm64-config.patch` is unrelated
(it edits `MASTER.arm64.bcm2837`).

Cost note for CI: this is `config/**`, so a push triggers `build-xnu` — and it is the only
`config/**` change this step needs.

---

## 2. Where the ramdisk image comes from (the answer is: a file on the ESP)

### 2.1 the mechanism exists, and XNU is its reader

The chain, all [TREE] unless tagged otherwise:

1. `Resources/UniXNU/UniXNU.toml` — `[[entry]] type = "xnu"` accepts an `initrd` key
   (`UniXNU Config.c:1196`: `Entry->Initrd = MakePath (UNIXNU_CONFIG_DIR, TomlGetString (Table,
   "initrd", NULL))`).
2. `UniXNU Boot.c:146-151` reads that file into memory, and `:164-166` passes it to the loader
   as `Request.Ramdisk` / `Request.RamdiskSize` (`:88-89`; the protocol is
   `Include/Protocol/UniXNULoader.h:47-48`, "Entry `initrd` contents, or NULL"). For a non-`xnu`
   entry the same file is published as a LoadFile2 initrd instead (`Boot.c:153-161`) — that is
   what `UniXNURamDiskDxe` is for, and it is *not* on our path.
3. `DarwinBoot.c:512-515` validates the blob's layout — raw bytes, or packed (`DBRDISK1`,
   `PackedRamdisk.h`). `:540-550` reads `<kernel path>.adt` (our `kernel.macho.adt`).
4. `DarwinBoot.c:186-273` (`AdtNode`) walks the ADT looking for a **16-byte** property named
   `RAMDisk` (or `PackedRAMDisk`) under a `chosen` → `memory-map` path
   (`:222-230`, `:249-254`), and `:561-567` enforces *both-or-neither*:

   ```c
   if (Request->Ramdisk != NULL) {
     if ((RamdiskOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + RamdiskOffset) != 0) ||
         (U64 ((UINT8 *)Tree + RamdiskOffset + 8) != 0)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
   } else if (RamdiskOffset != MAX_UINTN) {
     Status = EFI_UNSUPPORTED; goto Fail;
   }
   ```

5. `DarwinBoot.c:606-638` places the ADT in the kernel window, copies the (expanded) ramdisk
   right behind it at `ALIGN_VALUE(TreeAddress + TreeSize, EFI_PAGE_SIZE)`, and patches the
   **copied** tree's property:

   ```c
   WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset), RamdiskAddress);
   WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset + 8), RamdiskBytes);
   ```

   The blob on the ESP therefore stays `{0,0}` across boots.
6. XNU reads it in `IOFindBSDRoot()` — before it looks at `rd=` at all:

   ```c
   if ((regEntry = IORegistryEntry::fromPath( "/chosen/memory-map", gIODTPlane ))) {
           data = (OSData *)regEntry->getProperty("RAMDisk");
           if (data) {
                   ramdParms = (uintptr_t *)data->getBytesNoCopy();
                   (void)mdevadd(-1, ml_static_ptovirt(ramdParms[0]) >> 12,
                       (unsigned int) (ramdParms[1] >> 12), 0);
   ```

   ([TREE] `iokit/bsddev/IOKitBSDInit.cpp:796-813`; the `phys = 0` argument means "base is now a
   virtual address", which is why the `ml_static_ptovirt()` conversion is there.)
7. With `rd=md0` on the command line, `:819-856` resolves the just-registered device,
   `IOLog("BSD root: %s, major %d, minor %d\n", ...)` (`:840`) and `goto iofrootx` — no
   `waitForService()` for an `IOMedia` happens at all. Without a registered `mdN` it panics
   instead (`:854`), so `rd=md0` is never safe on its own.

Two side facts that make the log readable:

* `IOLog()` is **not** `kprintf()`: it formats through
  `__doprnt(format, ap2, console_printbuf_putc, ...)` ([TREE] `iokit/Kernel/IOLib.cpp:1570-1590`),
  the same console path `printf()` uses, so `BSD root: md0 …` *is* visible in the eMMC log
  ([M] the same channel carries `bsd_init: calling setconf`). Only `kprintf()` is dropped by
  `debug=0x144` ([`LOG-CHANNEL.md`](LOG-CHANNEL.md)).
* The `RAMDisk` property must be at `/chosen/memory-map` in the **ADT**, not in the UEFI FDT.
  `DarwinBoot` rejects a request that carries `Request->DeviceTree != NULL`
  ([TREE] `DarwinBoot.c:507-510`), so the FDT that `PayloadBootDxe` publishes
  (`PublishDeviceTree()`, `.../PayloadBootDxe.c:414-496`) is irrelevant here.

### 2.2 what is actually missing, and it is small

`bootshim/adt.c` builds our ADT and already has the right node — `/chosen/memory-map` with one
property (`:187-190`):

```c
	/* /chosen/memory-map: kernel range */
	w_node(&w, 2, 0);
	w_text(&w, "name", "memory-map");
	w_prop(&w, "Kernel-Dandelion", mem_range, sizeof(mem_range));
```

The shipped `Resources/UniXNU/kernel.macho.adt` (5 992 bytes, generated by
`tools/mk_adt.c`, which is a `main()` around `dandelion_make_adt()`) contains
`chosen`, `memory-map`, `Kernel-Dandelion`, `darwinboot-memory-size`, `emmc-log` — and **no
`RAMDisk`** [BIN]. The shipped `DarwinBoot.efi` does contain `RAMDisk`, `PackedRAMDisk` and
`DBRDISK1` [BIN], and the shipped `UniXNU.efi` contains `cannot read initrd` and
`xnu: handing off` [BIN]. The reader and the writer are both already in the payload; only the
placeholder and the file are absent.

**Missing piece 1 — the ADT placeholder** (`bootshim/adt.c`, then regenerate the `.adt`):

```diff
 	/* /chosen/memory-map: kernel range */
-	w_node(&w, 2, 0);
+	w_node(&w, 3, 0);
 	w_text(&w, "name", "memory-map");
 	w_prop(&w, "Kernel-Dandelion", mem_range, sizeof(mem_range));
+	/* DarwinBoot fills this in; the two zero cells mean "no ramdisk yet". */
+	static const uint64_t ramdisk_slot[2] = { 0, 0 };
+	w_prop(&w, "RAMDisk", ramdisk_slot, sizeof(ramdisk_slot));
```

and bump the root node's property/child counts only if a new node is added (none is: the
property lives in the existing `memory-map` node, whose `w_node(&w, 2, 0)` becomes `3`).
`tools/mk_adt.c` output does not change, so regenerating the blob is:

```
cc -O2 -I. -Ipexpert -Ipexpert/pexpert/arm64 -Ibootshim tools/mk_adt.c bootshim/adt.c -o out/mk_adt
out/mk_adt -o Resources/UniXNU/kernel.macho.adt
```

**Missing piece 2 — the file and its two names** (`Resources/UniXNU/` + `tools/mk_unixnu_esp.py`
+ `Resources/UniXNU/UniXNU.toml`):

```diff
 LAYOUT = [
     …
     ("EFI/UniXNU/kernel.macho.adt", "kernel.macho.adt"),
+    ("EFI/UniXNU/kernel.macho.rd", "kernel.macho.rd"),
 ]
```

```diff
 [[entry]]
 id = "xnu"
 title = "XNU (dandelion)"
 type = "xnu"
 path = "kernel.macho"
-args = "-v keepsyms=1 debug=0x144"
+args = "-v keepsyms=1 debug=0x144 rd=md0"
+initrd = "kernel.macho.rd"
```

Order matters: the ADT property and the `initrd` key must be flashed **together**. With the
property but no `initrd`, `DarwinBoot` returns `EFI_UNSUPPORTED` (`DarwinBoot.c:564-567`), the
kernel never starts, and the panel shows UniXNU's `"XNU (dandelion)" returned: Unsupported`
(the same string that was photographed in `docs/M6-UNIXNU.md` §6).

What goes in `kernel.macho.rd` for the *mount* round: anything non-empty. `RamdiskLayout()`
accepts any file ≤ 256 MiB that does not start with `DBRDISK1` (`PackedRamdisk.h:14-43`), and
mockfs never validates the contents (`mockfs_vfsops.c:74-77` is literally a `TODO: Validate
that the device at least LOOKS like a mach-o`). A 4 KiB zero page is enough to prove the
handoff; the real arm64 Mach-O is only needed for the exec rung (§4, step D).

### 2.3 what does *not* work, so nobody tries it again

* `pmd0=<phys>.<pages>` / `vmd0=<vaddr>.<pages>` on the command line: dead, `mdevinit()` is
  compiled out without `CONFIG_MEMDEV_INSECURE` ([TREE] `memdev.c:487-551`).
* A fixed physical range in the DarwinBoot handoff: there is no such thing to use. DarwinBoot
  *owns* the placement (`AllocateWindow()` in `DarwinBoot.c:602`, then `:606`) — the ADT
  property is the only fixed point in the contract, and it is filled by the loader, not by us.
* `UniXNURamDiskDxe`: reads `linux,initrd-start`/`-end` from the **UEFI FDT** `/chosen`
  (`UniXNURamDiskDxe.c:147-180`) and publishes a `BlockIo`. It exists for `type = "efi"`/
  `"linux"` entries and never touches the ADT, so it is not a route to `md0`.
* `rd=md0` alone: panics `IOFindBSDRoot: specified root memory device, md0, has not been
  configured` ([TREE] `IOKitBSDInit.cpp:854`) unless something registered `md0` first.
* Linking a blob into the kernel and calling `mdevadd()` from the `bsd_init.c` `DANDELION`
  block (what `ROOTFS-PLAN.md` §6 R1/§7 recommends): works in principle, but it is strictly
  more work than the loader path — it needs a `files` entry for the blob, a kernel rebuild for
  every image change, and it collides with the `DANDELION` block that is being edited right
  now. Keep it in reserve only if the ESP route fails.

---

## 3. Decision: (a) `rd=md0` + mockfs + an image, not (b) mount whatever is at hand

Option (b) — "enable `MOCKFS` and let it mount the fabricated `makedev(6,0)`" — is *possible*
and costs one config line: `bdevvp()` does not check `bdevsw`, `spec_ioctl()` degrades to
`eno_ioctl`, and `mockfs_mountroot()` ignores the failed `DKIOCGETMEMDEVINFO` and succeeds
([TREE] `mockfs_vfsops.c:98-110`). It would print `[dl] root mount: success` and then
`panic("rootvp not authenticated after mounting")` ([TREE] `bsd_init.c:1086-1106`, because
`bsd_rooted_ramdisk()` is false without `rd=md0…`).

It is rejected as the *goal*, for three reasons:

1. **It ends in a panic either way.** Without `rd=md0` the ROOTAUTH check fires before
   anything useful happens, so the round has to be repeated with `rd=md0` anyway.
2. **It proves nothing downstream.** A phantom device has no blocks, so `cluster_read()`
   through `mockfs_strategy()` can never be exercised, and the very next rung (exec) needs a
   real device with a real size.
3. **Cost is nearly the same.** Option (a)'s XNU-side work is *zero lines of C* — the reader
   already exists (§2.1) — and the rest is a firmware/ESP change that does not depend on CI at
   all.

So: **(a)**, but sequenced so that its cheap half is a separate, independently readable
observation. Step A below is exactly option (b) as a *measurement* — one config line, no
firmware change, and its log line (`vfs_mountroot -> 0` or `mockfs_mountroot failed: …`) is
what tells us whether mockfs is in `vfsconf` at all. Steps B/C are the firmware half.

The one thing that makes (a) more expensive than the plan thought is §1.2's missing
`si_devsize`: the mount rung needs no patch, the exec rung needs one small one. That patch is
`patches/xnu/zzz2-dandelion-mockfs-size.patch` (35 lines, one hunk, guarded by
`#if defined(DANDELION)`, inert while `MOCKFS` is off because the file is not compiled).

---

## 4. The steps, with the signal each one produces

The log channel is the `DLXLOG01` block at `flashinfo` +30000 sectors, decoded with
`tools/read_log_block.py --scan` ([`LOG-CHANNEL.md`](LOG-CHANNEL.md)); the firmware's own
`DDNLOG01` block at +30512 is decoded with `tools/extract_log_block.py --lba 30512`. Both are
quoted below, labelled `kernel:` and `firmware:`.

### Step A — `options MOCKFS` only (kernel; CI; no firmware change)

**What it tests**: that mockfs compiles for this configuration and that the `vfsconf` walk now
has a candidate. Nothing else in the boot changes.

Expected, in this order:

| Line | Source | Meaning |
|---|---|---|
| `setconf: IOFindBSDRoot returned an error (…);setting rootdevice to 'sd0a'.` | [TREE] `bsd_init.c:1291` | `kIOReturnNotFound` (0xE00002F0) from the 3-try loop; `rootdev = makedev(6,0)` |
| `[dl] vfs_mountroot -> 0 in … ticks` | [TREE] `bsd_init.c:989` (DANDELION) | mockfs mounted on a device that does not exist — **[INF]** for the value `0`; see the two alternatives below |
| `[dl] root mount: success` | [TREE] `bsd_init.c:1000` | the walk found a `vfc_mountroot` and it returned 0 |
| `panic: rootvp not authenticated after mounting` | [TREE] `bsd_init.c:1103` | expected, because `rd=md0` is not set |

Alternatives and what they would mean:

| Log instead | Meaning |
|---|---|
| `vfs_mountroot: can't setup bdevvp` then `[dl] vfs_mountroot -> 19` | `setconf()` did not take the `makedev(6,0)` branch — read the lines above it |
| `mockfs_mountroot failed: <n>` | mockfs was reached and refused; `<n>` is the error from `mockfs_mountroot()`, only `ENOMEM` is possible in this path |
| `[dl] vfs_mountroot -> 19` with **no** `mockfs_mountroot failed:` | the vfsconf entry was skipped → `-DMOCKFS` did not happen (check the config, not the code) |
| the boot parks after three tries as it does today | the same as the previous row |

Step A is deliberately separate: it is the only step whose outcome depends on a *kernel*
change, and its whole value is one line of the log.

### Step B — firmware: ADT placeholder + `initrd` + `rd=md0` (no kernel change, no CI)

**What it tests**: whether the loader half of §2.1 works on this port.

| Line | Source | Meaning |
|---|---|---|
| firmware: `boot: entry 'xnu'` / `xnu: handing off 0 staged kext(s)` | UniXNU `Boot.c:118,92` | the entry is running |
| firmware: no `DARWINBOOT: ramdisk layout rejected` and no `… firmware retained` | [TREE] `DarwinBoot.c:513,509` | the blob was accepted and the branch happened |
| kernel: `Added memory device md0/rmd0 (…/…) at … for …` | [TREE] `memdev.c:710` (`printf`) | `/chosen/memory-map/RAMDisk` reached `mdevadd()`; `for` is the size in bytes |
| kernel: `BSD root: md0, major N, minor 0` | [TREE] `IOKitBSDInit.cpp:840` (`IOLog` → console) | `rd=md0` resolved; no `IOMedia` wait happened |
| kernel: `mockfs_mountroot: root device <n> bytes` | `zzz2-dandelion-mockfs-size.patch` | the device now advertises a size; `<n>` = the initrd's page-rounded length |
| kernel: `[dl] root mount: success` | [TREE] `bsd_init.c:1000` | `vfs_mountroot() == 0` with a real device |
| kernel: `load_init_program: …` then `panic` | [TREE] `bsd_init.c:1256` → `kern_exec.c` | the blob is not an arm64 Mach-O; **expected** for a zero-filled file |

Failure signatures worth naming:

* `mdevadd: devfs_make_node for block failed!` ([TREE] `memdev.c:690`) — devfs was not up when
  `IOFindBSDRoot()` ran; `mdevadd()` then returns -1 and `mdevlookup(0)` panics at `:854`.
* firmware `"XNU (dandelion)" returned: Unsupported` — ADT property and `initrd` disagree
  (`DarwinBoot.c:564-567`).
* `[dl] vfs_mountroot -> 19` **with** `Added memory device` present — the walk still found no
  filesystem, i.e. step A's config change did not reach the build.
* a panic inside `mdevstrategy`/`cluster_read` rather than `load_init_program` — the ramdisk's
  physical address is outside XNU's static mapping (`ml_static_ptovirt()`), see §6.

### Step C — `zzz2-dandelion-mockfs-size.patch` (kernel; may ride with step A)

**What it tests**: whether the exec rung can read anything at all. Its only visible effect is
the `mockfs_mountroot: root device <n> bytes` line and a file node of non-zero length. It
cannot change the mount outcome (the size is only consumed by `blockmap`/`read`/`getattr`,
[TREE] `mockfs_vnops.c:180-183,222,394-401`), so landing it with step A costs no extra
information; landing it later costs a second build. Recommendation: land it with step A.

### Step D — a real Mach-O as `kernel.macho.rd` (later, needs a macOS build)

The file node is exec'd as `/sbin/launchd`: `mockfs_lookup()` maps `sbin` → root and `launchd`
→ the file node ([TREE] `mockfs_vnops.c:120-128`), and `load_init_program()` tries
`/usr/appleinternal/sbin/launchd` then `/sbin/launchd` ([TREE] `kern_exec.c:6834-6842`).
A static arm64 Mach-O is the target; Apple's `launchd` is not reachable because it needs dyld
and the shared cache (`ROOTFS-PLAN.md` §6 R3 "honest limit").

---

## 5. What would falsify this reading

Each of these is a single observation that would prove a claim above wrong. They are listed so
the next log can be read against them instead of being interpreted after the fact.

| Claim | Observation that would kill it |
|---|---|
| mockfs is the only `vfc_mountroot` (§1.2) | `grep -n vfc_mountroot xnu/bsd/vfs/vfs_conf.c` shows a non-NULL entry outside `#if MOCKFS` |
| `options MOCKFS` in the machine file becomes `-DMOCKFS` (§1.4) | step A prints `[dl] vfs_mountroot -> 19` and no `mockfs_mountroot failed:` line, *and* the CI compile log shows `mockfs_vfsops.c` was never compiled |
| `bdevvp()` does not check `bdevsw` (§1.3) | step A prints `vfs_mountroot: can't setup bdevvp` |
| mockfs mounts a device with no driver ([INF]) | step A prints `mockfs_mountroot failed:` |
| `RAMDisk` in the ADT is what registers `md0` (§2.1) | step B prints `BSD root: md0 …` without `Added memory device md0/rmd0 …` before it |
| `si_devsize` is the size source (§1.2/zzz2) | step B prints `mockfs_mountroot: root device <n> bytes` with `<n>` ≠ the initrd length, or the exec panic is `ENOENT`/`EINVAL` instead of a Mach-O error |
| `bsd_rooted_ramdisk()` needs `rd=md0` (§3) | step B still panics `rootvp not authenticated after mounting` although `rd=md0` is in `args` |
| `IOLog()` reaches the eMMC log (§2.1) | `BSD root: md0` is missing but `Added memory device` is present — then read the panel, not the log |

---

## 6. Corrections to `ROOTFS-PLAN.md`, and what is still unverified

Three corrections, all from the tree:

1. **No blob needs to be linked into the kernel, and no `mdevadd()` call needs to be added to
   `bsd_init.c`.** `IOFindBSDRoot()` already calls `mdevadd()` from the ADT property
   ([TREE] `IOKitBSDInit.cpp:796-813`), and `DarwinBoot` already writes that property
   ([TREE] `DarwinBoot.c:634-639`). The plan's R1 "first step" moves the same work into the
   kernel for no benefit and adds a `files` entry and a link dependency.
2. **`pmd0=`/`vmd0=` boot arguments are not available**: `mdevinit()` is empty without
   `CONFIG_MEMDEV_INSECURE` ([TREE] `memdev.c:490,548`). The plan does not claim otherwise,
   but "let `mdevinit` parse the boot args" is a natural misreading of `MASTER:483`.
3. **`load_init_program()` cannot exec anything from mockfs as shipped**, because the file
   node's size is `mp->mnt_devvp->v_specinfo->si_devsize` ([TREE] `mockfs_fsnode.c:81`) and
   that field is only ever written in `spec_open()` ([TREE] `spec_vnops.c:421-456`), which the
   mountroot path never calls. The plan's §7 step 5 ("only then put a Mach-O in the array")
   skips this; `zzz2-dandelion-mockfs-size.patch` supplies it.

Unverified, and honest about it:

* **`ml_static_ptovirt()` over the DarwinBoot window** — [INF]. DarwinBoot allocates the kernel
  window from conventional memory (`AllocateWindow()`, `DarwinBoot.c:602`) and XNU converts the
  reported physical base with `ml_static_ptovirt()` before handing it to `mdevadd()`
  (`IOKitBSDInit.cpp:809`). That is Apple's own path on iPhone, but nothing here has measured
  that the static mapping covers this window on MT6762G. If it does not, the failure is a panic
  inside `mdevcopy`/`cluster_read`, not a mount error. Cheapest check if it happens: print
  `PE_state`/`ml_static_ptovirt()` of the value in `mdevadd()`.
* **Whether `devfs` is initialised before `IOFindBSDRoot()` runs** — [INF]; `mdevadd()` needs
  `devfs_make_node()` (`memdev.c:686`). The `mdevadd: devfs_make_node … failed!` printf is the
  test, and `IOKitBSDInit()` runs from `bsd_autoconf()` (`bsd_init.c:1261-1278`), i.e. after
  `bsd_init()`'s own devfs/vfs setup — expected to be fine.
* **Whether mockfs compiles for `MACHINE=dandelion`** — it has never been in a dandelion build.
  Step A answers this in CI, which is why step A is one config line and nothing else.
* **The `256 MiB` `darwinboot-memory-size`** (`bootshim/adt.c:164`) must hold kernel (14.8 MiB)
  + ADT (6 KiB) + ramdisk + trust cache. Fine for a stand-in ramdisk; a full IPSW-sized root
  would need the packed form (`PackedRamdisk.h`) or a bigger window — not this step.

---

## 7. The artifacts for this step

| Path | State | Why |
|---|---|---|
| `config/MASTER.arm64.dandelion` | edited, **not committed** | `options MOCKFS` (§1.4) |
| `patches/xnu/zzz2-dandelion-mockfs-size.patch` | new, **not committed**; `check_series.py` passes | the `si_devsize` fix (§6.3) |
| `docs/ROOTFS-STEP1.md` | this file | — |
| `bootshim/adt.c`, `Resources/UniXNU/kernel.macho.adt`, `Resources/UniXNU/UniXNU.toml`, `tools/mk_unixnu_esp.py` | **not touched** (§2.2 has the diffs) | firmware/ESP half, owned elsewhere |
