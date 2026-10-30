# Root filesystem: what `vfs_mountroot()` needs on this port

Read-only investigation for the step after the timer interrupt. All `xnu/` citations are the
**patched** tree in `D:\aios\xnu` (`XNU 8796.141.3` + `patches/xnu/*.patch`), not upstream.
That tree is edited in place by other work, so line numbers are from 2026-10-30 21:0x;
re-grep the anchor name if a number has drifted.

## 0. The short version

| Question | Answer |
|---|---|
| Does the open-source tree have HFS+/APFS? | **No.** There is no `bsd/hfs` and no `bsd/apfs`; the only filesystem in the tree with a `vfc_mountroot` is **mockfs** (`xnu/bsd/vfs/vfs_conf.c:205`) |
| Does it have a memory disk (`md`)? | **Yes** — `xnu/bsd/dev/memdev.c`, declared `standard` in `xnu/bsd/conf/files:106`, instantiated by `pseudo-device mdevdevice` in `xnu/config/MASTER:483`. The three earlier notes in this repo that say "no md" (`docs/BOOT_STATUS.md:33`, `docs/ANALYSIS-2026-10-26.md:55`, `docs/FINDINGS-2026-10-26.md:103`) are wrong; what is missing is a *filesystem*, not the block device |
| Can a root device appear on its own? | **No.** Nothing in the current kernel ever registers an `IOMedia`; boot hangs in `IOService::waitForService()` (`xnu/iokit/bsddev/IOKitBSDInit.cpp:926`) |
| Cheapest way to a mounted root | `md` + `options MOCKFS` + `rd=md0`, then a static Mach-O as the file node — §6 R1, first step in §7 |
| What breaks right after a successful mount | `FSIOC_KERNEL_ROOTAUTH` panic unless `rd=md0` (`xnu/bsd/kern/bsd_init.c:1047-1067`), and `load_init_program()` panics if it cannot exec (`xnu/bsd/kern/kern_exec.c:6929`) |

## 1. The code path, `bsd_init()` → root vnode

`bsd_init()` is the `bsdinit_task` body; the mount section starts after `inittodr(0)`.

| # | Site | What it does | What it needs on this port |
|---|---|---|---|
| 1 | `xnu/bsd/kern/bsd_init.c:803` | `bsd_autoconf()` → `kminit()` + pseudo-device inits + `IOKitBSDInit()` | `mdevinit()` runs here (pseudo-device), and `IOBSD` is published here (`xnu/iokit/bsddev/IOKitBSDInit.cpp:112`) — **both happen before the mount loop** |
| 2 | `xnu/bsd/kern/bsd_init.c:968` | `while (TRUE) { setconf(); vfs_mountroot(); }` | loops forever on any error return; no attempt counter |
| 3 | `xnu/bsd/kern/bsd_init.c:1250` | `setconf()` → `IOFindBSDRoot(rootdevice, …, &rootdev, &flags)` | the only place a *real* root device is chosen — blocks today (§2); the `makedev(6, 0)` fallback at `:1254` only fires on an error return |
| 4 | `xnu/iokit/bsddev/IOKitBSDInit.cpp:817-854` | `rd=mdN` branch: `mdevlookup(xchar)`, `goto iofrootx` | works only if `mdN` is already registered; panics at `:852` if the boot arg names a device that is not configured |
| 5 | `xnu/iokit/bsddev/IOKitBSDInit.cpp:794-816` | `/chosen/memory-map/RAMDisk` → `mdevadd(-1, base>>12, size>>12, 0)` (`:807`) | needs the loader to write that property; nothing writes it today |
| 6 | `xnu/bsd/kern/bsd_init.c:1259-1268` | `flags & 1` → `mountroot = netboot_mountroot`, else `mountroot = NULL` | `CONFIG_NETBOOT` is in `BSD_DEV` (`xnu/config/MASTER.arm64.dandelion:25`), so a failed match silently routes to netboot |
| 7 | `xnu/bsd/vfs/vfs_subr.c:1240` | `vfs_mountroot()` → `bdevvp(rootdev, &rootvp)` | fails `ENODEV` if `rootdev` is `NODEV` (`:2102`); **a block device is mandatory before any filesystem is tried** |
| 8 | `xnu/bsd/vfs/vfs_subr.c:1252-1265` | walks `vfsconf`, calls `vfc_mountroot` / `VFS_MOUNT` for entries that have one | with MOCKFS off, no entry qualifies (`devfs`/`nullfs`/`bindfs`/`routefs` all have `vfc_mountroot = NULL`), so it returns `ENODEV` at `:1398` |
| 9 | `xnu/bsd/vfs/vfs_subr.c:1294` | `vfs_init_io_attributes(rootvp, mp)` | issues `DKIOC*` ioctls; memdev answers some (`xnu/bsd/dev/memdev.c:403-465`) and the rest are non-fatal (`vfs_subr.c:1288-1293`) |
| 10 | `xnu/bsd/kern/bsd_init.c:1029` | `IOSecureBSDRoot(rootdevice)` | `CONFIG_SECURE_BSD_ROOT` is on (`SECURITY_BASE`); it waits 30 s for an `IOPlatformExpert` service (`IOKitBSDInit.cpp:1078-1082`) and can `mdevremoveall()` (`:1089-1091`) |
| 11 | `xnu/bsd/kern/bsd_init.c:1035` | `VFS_ROOT(mountlist.tqh_first, &init_rootvnode, …)` | panics if the fs cannot produce a root vnode |
| 12 | `xnu/bsd/kern/bsd_init.c:1047-1067` | non-OSX build: `require_rootauth = TRUE` → `VNOP_IOCTL(rootvnode, FSIOC_KERNEL_ROOTAUTH)` → `panic("rootvp not authenticated after mounting")` | skipped only when `bsd_rooted_ramdisk()` is true, i.e. boot arg `rd` = `md0…` (`bsd_init.c:444-460`) |
| 13 | `xnu/bsd/kern/bsd_init.c:1217` | `bsdinit_task()` → `load_init_program(p)` | tries `/usr/appleinternal/sbin/launchd.development`, then `/sbin/launchd` (`xnu/bsd/kern/kern_exec.c:6834-6842`), panics at `:6929` if both fail |

Two consequences worth stating plainly:

* **Mount is gated on a `dev_t`.** Even a pure in-kernel filesystem cannot be the root without a
  valid block device, because `bdevvp()` runs before the filesystem walk (step 7). "devfs as root"
  is therefore not a shortcut; it would need `vfs_mountroot()` itself to be bypassed.
* **`setconf()` never returns today.** The `DANDELION` branch keeps the wait loop alive instead of
  returning a device, so `vfs_mountroot()` is not even entered — the kernel is parked inside
  `IOFindBSDRoot()`.

## 2. Every blocking wait from the mount loop to a usable root vnode

No timer interrupt means: a thread blocked on a deadline is never woken, and `IOSleep()` is a
permanent park. Measured once already — `xnu/bsd/kern/bsd_init.c:925-929` records that an
`IOSleep()` diagnostic never printed its "after" line.

| # | Site | Kind | Deadline | Status on DANDELION |
|---|---|---|---|---|
| 1 | `IOKitBSDInit.cpp:686-721` | `waitForMatchingService` for the `IOBSD` resource | 30 s | **patched out**: replaced by a 200 ms `CNTPCT` poll (would hang otherwise; the resource is always published at `:112`, so upstream never waits either) |
| 2 | `IOKitBSDInit.cpp:735-741` | `IOSleep(5 * 1000)` on the 2nd and later mount attempt | 5 s | **patched out** under `#if !defined(DANDELION)` |
| 3 | `IOKitBSDInit.cpp:922-926` | `IOService::waitForService(matching, &t)` — `matching` is `IOMedia` + `Content = Apple_HFS` (`:893-899`) | `ROOTDEVICETIMEOUT` = 120 s DEBUG / 60 s otherwise (`:58-60`) | **this is where the boot stops.** `SLEEPNOTIFYTO` → `IORecursiveLockSleepDeadline` (`xnu/iokit/Kernel/IOService.cpp:311,6500-6507`); no tick, no expiry, `while (!service)` at `:969` |
| 4 | `xnu/bsd/kern/bsd_init.c:1013-1025` | netboot failure loop: `IOSleep(i * 60 * 1000)` (`:1021`), doubling forever | 60 s, 120 s, … | only reachable if something returns `flags & 1`; also a permanent park |
| 5 | `xnu/bsd/kern/bsd_init.c:1070-1089` | `netboot_setup()` failure loop, same `IOSleep` pattern (`:1083`) | 60 s, 120 s, … | only on the netboot path |
| 6 | `IOKitBSDInit.cpp:1080` | `waitForMatchingService("IOPlatformExpert", 30 s)` inside `IOSecureBSDRoot` — **after** the mount, before `VFS_ROOT` | 30 s | `IOPlatformExpert::start()` does run on this port (DANDELION prints in `patches/xnu/non-apple-platform-expert-fallback.patch`), but nothing checks that the expert is *registered as a service*; `assert(pe)` at `:1082` is a panic if the wait comes back empty |
| 7 | `xnu/bsd/vfs/vfs_subr.c:1240-1298` | all filesystem-side work | — | synchronous for memdev (`xnu/bsd/dev/memdev.c:263-366` does the copy inline and calls `buf_biodone`), so no waits |

`IOSleep()` is not a spin here: `IOSleep()` → `delay_for_interval()`
(`xnu/iokit/Kernel/IOLib.cpp:1513-1516`) → `_clock_delay_until_deadline()`, which calls
`assert_wait_deadline()` + `thread_block()` whenever it is not spinning
(`xnu/osfmk/kern/clock.c:1584-1601`). Every `IOSleep()` in the boot path is therefore a hang until
the timer works — which is why the `DANDELION` diagnostics busy-wait on `CNTPCT` instead
(`xnu/bsd/kern/bsd_init.c:925-950`).

## 3. What the open-source tree does and does not contain

| Component | In tree? | Built for DANDELION? | Can it be a root? |
|---|---|---|---|
| HFS / HFS+ | **no** (`xnu/bsd/` has no `hfs` directory) | — | no |
| APFS | **no** (no `apfs` directory; `APFSIOC_*` only appears in ioctl tables, `xnu/bsd/vfs/vfs_syscalls.c:12740`) | — | no |
| devfs | yes (`xnu/bsd/miscfs/devfs/`) | yes — `devfs` is in `FILESYS_BASE` (`xnu/config/MASTER.arm64.dandelion:27`) | no: `vfc_mountroot = NULL` (`xnu/bsd/vfs/vfs_conf.c:130,146`) |
| nullfs | yes (`xnu/bsd/miscfs/nullfs/`) | **no** — `optional nullfs` (`xnu/bsd/conf/files:508-510`), `NULLFS` in no set | no: also `vfc_mountroot = NULL` (`vfs_conf.c:168`) |
| bindfs / routefs | yes | yes | no: `vfc_mountroot = NULL` (`vfs_conf.c:186,224`) |
| **mockfs** | yes (`xnu/bsd/miscfs/mockfs/`) | **no** — `optional mockfs` (`xnu/bsd/conf/files:155-157`), `options MOCKFS # <mockfs>` (`xnu/config/MASTER:115`), not in any set | **yes** — the only `vfc_mountroot` in the tree (`vfs_conf.c:205`) |
| memory disk (`md`) | yes (`xnu/bsd/dev/memdev.c`) | yes (`standard` in `bsd/conf/files:106`; `pseudo-device mdevdevice` in `config/MASTER:483`) | as a *device* only: `/dev/mdN` block+char nodes (`memdev.c:686-701`) |
| `IOMedia` / `IOBlockStorageDriver` | yes (IOKit storage stack) | yes, but **no provider ever publishes an `IOMedia`** | that is the missing link |
| NFS | only the **server**: `xnu/bsd/nfs/` has `nfs_serv.c`, `nfs_socket.c`, … but no `nfs_vfsops.c` / `nfs_vnops.c` | `nfs_syscalls.c` is `standard` (`xnu/bsd/conf/files:76`), the rest `optional nfsserver` (`:77-84`) | no — and this kills network root, see R6 |
| netboot (`CONFIG_NETBOOT`) | yes (`xnu/bsd/kern/netboot.c`) | yes (`BSD_DEV` set) | needs an `IONetworkInterface`; none exists (`netboot.c:598-603` fails `ENXIO`) |

### 3.1 mockfs is not a toy — it is the intended "root without a filesystem"

`mockfs_mountroot()` (`xnu/bsd/miscfs/mockfs/mockfs_vfsops.c:65-147`) builds three nodes: a root
directory, a `dev` node (mount point for devfs), and one regular file that is a *frontend for the
root device's contents* (`:54-63`). Its lookup maps names by type
(`xnu/bsd/miscfs/mockfs/mockfs_vnops.c:120-125`): `sbin` → the root node itself, `dev` → dev node,
`launchd` → the file node. That is exactly what `load_init_program()` asks for, so
`/sbin/launchd` resolves to "the first executable image on the root device". No directory tree, no
inode table, no fsck.

Caveats, all verifiable in the same files: `vnop_ioctl` is the generic `err_ioctl` stub
(`mockfs_vnops.c:422`), so **`FSIOC_KERNEL_ROOTAUTH` fails** and step 12 of §1 would panic unless
`rd=md0` is set; and the mount is deliberately not usable for anything else (`mockfs_unmount`
expects force-close, `mockfs_vfsops.c:166-174`).

## 4. The reference implementation: merlinx and friends

Quotes are from `Dracen-W/UniXNU-merlinx`, branch `merlinx-work`, read 2026-10-30.

`docs/devices/merlinx.md:7`:

> XNU mounts SystemOS, initproc runs, and launchd exec succeeds; Data volume and userland services are pending

`docs/devices/merlinx.md:19-21`:

> 64 MiB p33 image with the fixed BootShim, 3 MiB firmware volume, trimmed DTB and ramdisk
> NativeEmmc enumerates the userdata partition; the OS and SystemOS volumes appear; the owner path mounts the SystemOS cache carrier
> XNU reaches the root and the SystemOS cache … initproc and launchd exec complete

So the 64 MiB "ramdisk" in the boot image is the *payload/FAT* volume, not the root. The root is a
real APFS volume: `docs/guide/userdata.md:3`

> The userdata image supplies the root file system to XNU. It holds the unchanged `OS` and
> `Cryptex1,SystemOS` disk images from the target IPSW behind a manifest that the kernel reads.

and `docs/guide/userdata.md:41-50` says the kernel mounts it *by UUID*, with the ADT publishing
`/chosen/root-matching` (`IOProviderClass = IOMedia`), the native block driver publishing the
partition and "the partition scheme publishes the volumes".

Where do APFS and the driver come from? Not from the open-source tree:

* `docs/concepts/prelink.md:3` — "The prelinked kernel combines a kernel built from source with the
  original Apple kernelcache. … The original kernelcache provides the Apple kexts and platform
  services, which the source tree leaves to the kernelcache."
* `docs/xnu/patch-reference.md:57` — patch `offline-component-load-probes`: "Loads APFS, IOGPU and
  mobile graphics components with checkpoints".
* `docs/devices/merlinx.md:9` — `NativeEmmc`, a separate out-of-tree block driver repository.

They also keep a **mockfs fixture** alive for bring-up: `docs/xnu/patch-reference.md:89-97` lists
`mockfs-intermediate-lookup`, `mockfs-memory-device-width`, `mockfs-statfs`,
`mockfs-typed-allocation`, `mockfs-test-file` ("Adds `launchd` and `probe.bin` to the test file
system"), and `docs/guide/build-kernel.md:93` — `XNU_SOURCE_MOCKFS | Adds the mockfs test file`.

Three other ports show the same two rungs:

* `docs/devices/clover.md:11` — "An eMMC-backed mockfs root runs a static EL0 test init."
* `docs/devices/kebab.md:13` — "an HFS root and execution of a static test init in EL0. The file
  named `/sbin/launchd` in that test root is the custom test program."
* `docs/devices/fajita.md:15` — the state we are about to enter: "the verbose console ends on
  `Waiting on ... IOMedia ... Apple_HFS`; no storage driver yet".

## 5. What "success" would look like in the log

The port already has trace marks at the entry and the success path
(`xnu/bsd/kern/bsd_init.c:877,982`); the observable sequence for a real mount is:

```
[dl] root mount: entering loop
[dl] IOFindBSDRoot: IOBSD resource matched (polls=N)
BSD root: md0, major X, minor 0
mockfs_mountroot: ...            (nothing today; add a printf in the patch)
[dl] root mount: success
load_init_program: attempting to load /sbin/launchd
```

## 6. Options, ranked by cost

Costs are engineering estimates for this tree, not promises. "Files" lists what actually changes.

### R1 — md + `options MOCKFS` + an embedded static Mach-O (recommended)

* **What ships**: no `CONFIG_MEMDEV_INSECURE` needed — call `mdevadd()` from the existing
  `DANDELION` block in `bsd_init.c` (`:874-967`, where `dandelion_gic_late_init()` already runs)
  over a page-aligned blob linked into the kernel, boot with `rd=md0`, then let
  `mockfs_mountroot()` mount it and `load_init_program()` exec the blob.
* **Files**: `config/MASTER.arm64.dandelion` (+`options MOCKFS`); one new
  `patches/xnu/dandelion-root-md.patch` (`mdevadd()` + a `printf` in `setconf()` and in
  `mockfs_mountroot()`); the test image itself, either linked in (a new source file needs a `files`
  entry — pattern: `xnu/iokit/conf/files.arm64:7` lists `iokit/Kernel/arm/AppleARMSMP.cpp`) or
  delivered from the payload through `/chosen/memory-map/RAMDisk`
  (`xnu/iokit/bsddev/IOKitBSDInit.cpp:794-816`, the path UniXNU's DarwinBoot fills:
  [concepts/payload.md:72](https://github.com/Dracen-W/UniXNU-merlinx/blob/merlinx-work/docs/concepts/payload.md));
  plus `rd=md0` on the command line
  that already carries `-v keepsyms=1 debug=0x144` (`docs/LOG-CHANNEL.md:78`). No
  `.github/workflows` change: `build-xnu.yml` already builds `MACHINE=dandelion,
  KERNEL_CONFIG=DEVELOPMENT` on a `macos-14` runner, and `patches/**` + `config/**` already trigger
  it (`.github/workflows/build-xnu.yml:22-30,44-47`).
* **Unlocks**: `vfs_mountroot() == 0`, `rootvnode` set, `/dev` mountable, and an EL0 program
  running — the same rung merlinx's own mockfs fixture and the clover/kebab ports report.
* **Risk**: `mockfs` is off by default and less travelled; the `rd=md0` dependency for ROOTAUTH is
  easy to forget; the blob must be a real arm64 Mach-O.
* **First step**: §7.

### R2 — mount-only (does not exec anything)

* Same as R1 but stop at "[dl] root mount: success"; keep the existing `load_init_program` panic
  (or add a `DANDELION` return before it).
* **Cost**: ~1 day, one config line + one small patch. **Unlocks**: proves the whole
  `bdevvp → vfc_mountroot → VFS_ROOT → mnt_flag MNT_ROOTFS` chain on this port, which is the part
  that is untested today. **Do this first**, then R1 is only "add a blob".

### R3 — a read-only filesystem of your own (tar/ustar or flat image)

* **What ships**: a `vfsops` + `vnop` set with `vfc_mountroot` (mockfs is the template:
  `xnu/bsd/miscfs/mockfs/` is 1 249 lines including headers), reading a packed image from the md
  device.
* **Files**: new `bsd/miscfs/<fs>/` (not possible via `patches/xnu/*.patch` alone — it needs a
  `bsd/conf/files` entry and a config option), so `tools/ci/apply-board.sh` grows a directory copy.
* **Cost**: 1–2 weeks for read-only lookup/read/readdir/getattr. **Unlocks**: a real directory
  tree (`/sbin`, `/etc`) from a ramdisk, useful for a stand-in init and for tests.
* **First step**: copy `xnu/bsd/miscfs/mockfs/` into a `patches`-supplied `bsd/miscfs/tarfs/`,
  rename, and make `lookup`/`read` walk a ustar header chain from the md image; keep `mockfs` as
  the fallback so R1 keeps working.
* **Honest limit**: it still cannot run Apple's userland — `launchd` needs dyld, libSystem and the
  shared cache from an actual iOS root, none of which are in this tree.

### R4 — import Apple's filesystem code the way UniXNU does (prelink)

* Fetch an IPSW `KernelCache`, keep `__PRELINK_INFO`, graft the APFS/HFS kext segments into our
  kernel, plus the trust cache so AMFI accepts them.
* **Files**: a new prelink tool (UniXNU ships `kits/XnuPrelink/emit_prelink.py`), trust-cache
  plumbing in the payload, plus a storage driver (R5). **Cost**: weeks, plus inputs that
  "are not distributed" (`docs/devices/merlinx.md:45`). **Unlocks**: a real APFS root, i.e. the
  merlinx end state. **Not** reachable from source alone.
* **First step**: dump `__PRELINK_INFO` out of one IPSW kernelcache and check whether an APFS kext
  bundle is present with its CD hash — if that step fails, the whole option is off the table.

### R5 — an XNU-side eMMC driver + APFS/HFS

* The "real" answer: `IOBlockStorageDriver` → `IOMedia` on the MT6765 MSDC controller
  (`0x11230000`, already measured, `docs/MERLINX-COMPARISON.md:39`), then a filesystem.
* **Cost**: XL — the driver needs DMA + interrupts + timeouts, i.e. it needs the timer work first,
  and it still does not give a filesystem. Referenced by merlinx as a separate repository
  (`docs/devices/merlinx.md:9`).
* **First step**: nothing before the timer; after that, a standalone probe that reads sector 0 over
  CMD17/CMD18 and prints it, with no IOKit attachment yet.
* **Not possible without writing a driver.** Say so and move on.

### R6 — network root (`CONFIG_NETBOOT`)

* `netboot_mountroot()` (`xnu/bsd/kern/netboot.c:584-660`) picks an interface, configures IPv4 from
  `/chosen`, then mounts the image. Two hard blockers, both in the source: the NFS path is
  `kNetBootImageTypeNFS` → `error = ENOTSUP`, "deprecated" (`netboot.c:651-656`), and there is no
  NFS *client* in this tree anyway (§3); the HTTP path calls `netboot_setup()` and needs an
  `IONetworkInterface` that nobody publishes (`netboot.c:598-603` fails `ENXIO`).
* **Cost**: XL and pointless before R1. **Not possible without a NIC driver**, and not possible at
  all with the shipped netboot/NFS code.
* **First step**: none — there is no network family to build on: `xnu/iokit/Families/` contains only
  `IONVRAM` and `IOSystemManagement`, and `IONetworkInterface` appears solely as a `metaCast` string
  in `IOKitBSDInit.cpp:941,964`.

### R7 — stop before the mount and declare the milestone here

* Zero cost, but the kernel's failure path is `while (TRUE) { setconf(); … }`
  (`bsd_init.c:968-1026`): even "stopping" costs a patch that parks deliberately and loudly.
* **Unlocks**: nothing new; it only turns an accidental hang into a documented one. Worth doing as
  a *fallback* if the timer work regresses, not as a goal.
* **First step**: one `DANDELION` guard in the loop that prints the `errno` once, calls
  `dandelion_trace_mark()` and then parks on `CNTPCT` instead of spinning on `setconf()`.

## 7. Recommended first step, concretely

R2 → R1, in this order, each verifiable from the kernel log alone:

1. **Turn mockfs on.** In `config/MASTER.arm64.dandelion`, add an untagged option next to the other
   `options` lines (untagged options are kept for every configuration; the set lists are read from
   the `#  NAME = [ … ]` lines by `SETUP/config/doconf:222`):

   ```
   options   MOCKFS		# mockfs_mountroot: the only vfc_mountroot in this tree
   ```

   `patches/xnu/ci-arm64-config.patch` already edits those set lines, so this is within the
   project's conventions.
2. **Register md0 before `setconf()`.** In the existing `DANDELION` block in `bsd_init.c` (the one
   that already calls `dandelion_gic_late_init()`, lines 874-967), add a page-aligned image array
   and one call:

   ```c
   extern dev_t mdevadd(int devid, uint64_t base, unsigned int size, int phys);

   static const unsigned char test_init[] __attribute__((aligned(4096))) = { /* Mach-O */ };

   mdevadd(-1, ((uintptr_t)test_init) >> 12, (sizeof(test_init) + 4095) >> 12, 0);
   ```

   `base` and `size` are **page counts**, not bytes (`xnu/bsd/dev/memdev.c:160-169`), pages are
   assumed to be 4 KiB (`:161-162`) — which is what this port builds (`docs/MMU_GRANULE.md:3`) — and
   `phys = 0` means "`base` is a kernel virtual address" (`memdev.c:307-313`), which is what a blob
   linked into the kernel needs. Declaring `mdevadd()` locally is enough; `IOKitBSDInit.cpp:63` does
   the same.
3. **Boot with `rd=md0`.** This makes `IOFindBSDRoot()` take the md branch
   (`IOKitBSDInit.cpp:817-854`) and, more importantly, makes `bsd_rooted_ramdisk()` true
   (`bsd_init.c:444-460`), which skips the `FSIOC_KERNEL_ROOTAUTH` panic
   (`bsd_init.c:1047-1067`). Add `rd=md0` where the command line is already set; the port parses
   boot args through `PE_state.bootArgs->CommandLine` (`xnu/pexpert/arm/pe_bootargs.c:11`, filled
   in `xnu/pexpert/arm/pe_init.c:406`).
4. **Expect**: `Added memory device md0/rmd0 …` from `mdevadd()`
   (`xnu/bsd/dev/memdev.c:710-711`), then `BSD root: md0, major N, minor 0`
   (`IOKitBSDInit.cpp:838`), then `[dl] root mount: success`. If instead
   `mockfs_mountroot failed: 22` prints, `rvp` was not a memdev — check step 2's `phys` argument.
5. **Only then** put a Mach-O in the array. Until it exists, `load_init_program()` panics by design
   (`kern_exec.c:6929`) — that is a *success* signal for step 4, not a regression.
6. **Do not "fix" the wait by returning early.** `setconf()` maps a failed match to
   `flags | 1` and `rootName = "en0"` (`IOKitBSDInit.cpp:967-971`), which sets
   `mountroot = netboot_mountroot` (`bsd_init.c:1259-1268`) and lands in the netboot `IOSleep`
   loop. The correct change is to supply a `dev_t`, not to stop waiting.

Then re-check `xnu/bsd/kern/bsd_init.c:1029` (`IOSecureBSDRoot`): if the port's platform expert ever
implements `SecureRootName`, a `kIOReturnNotPrivileged` answer calls `mdevremoveall()`
(`IOKitBSDInit.cpp:1089-1091`) and the root device disappears *after* a successful mount.

## 8. Open questions

| Question | How to answer |
|---|---|
| Does the command line actually reach the kernel on this port? | print `PE_boot_args()` next to the existing `[dl]` marks; `docs/LOG-CHANNEL.md:78` implies it does |
| Is `MOCKFS` the only `vfc_mountroot` after our config edits? | `grep -n vfc_mountroot xnu/bsd/vfs/vfs_conf.c` — expect line 205 only |
| Does `mdevadd()` survive to `setconf()`? | `mdevadd()` prints `Added memory device md0/rmd0 …` (`xnu/bsd/dev/memdev.c:710-711`) — grep the kernel log block |
| Is a real `IOMedia` reachable without a driver? | no: nothing in the tree publishes one; `docs/devices/fajita.md:15` is the same state on another SoC |
| Is `iokit/Kernel/arm/DandelionPlatformExpert.cpp` actually compiled? | it appears in no `files` list (`xnu/iokit/conf/files.arm64:7` lists only `AppleARMSMP.cpp` for that directory) and nothing includes it, so it looks like dead code — check before relying on it for the platform expert |
