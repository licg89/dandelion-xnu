# The first user-space process: what `load_init_program` needs, and what we lack

Written 2026-10-31 by the `minimal-init` task, against kernel `92253d8` and XNU
`xnu-8796.141.3` in `D:\aios\xnu` (already patched). Every claim below is either
a line number in that tree, a byte in a file this document ships, or a log line
in `docs/logs/`. Where I could not verify something it says so 鈥?in particular
**nothing here has been run on the phone.**

The page is in three parts:

* [Part A](#part-a--the-artifact) 鈥?`tools/mk_init_macho.py`, the smallest arm64
  Mach-O this kernel will exec, and what its `--verify` proves byte by byte.
* [Part B](#part-b--every-gate-between-the-last-log-line-and-initproc) 鈥?the
  ordered gate list from `load_init_program:` to `initproc running`, which of
  them our patches already neutralise, which still refuse an unsigned hand-made
  image, and the smallest step sequence that gets there.
* [Part C](#part-c--what-hakios-does-and-the-three-things-we-lack) 鈥?what
  `oosexgang/hakios-xnu-kernel` actually does to boot iOS 16.5 user space on a
  MediaTek/Qualcomm phone, and the three things dandelion does not have yet.
  See also [`LANDSCAPE.md`](LANDSCAPE.md) for the ecosystem map.

## 0. The state this starts from

`docs/NEXT-SESSION.md` 搂0.-6, log `docs/logs/kernel-log-2026-10-31-bsd-init-done.txt`,
last line 3487:

```
bsd_init: bsd_do_post - doneload_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
```

(from here on, `launchd.development`).  `bsd_init` completed; `bsdinit_task()`
(`bsd/kern/bsd_init.c:1230-1258`) reached `load_init_program(p)` at `:1256` and
that function is now inside its first exec attempt.

The root filesystem is mockfs over a 4 KiB RAM disk handed over by DarwinBoot
([`ROOTFS-STEP1.md`](ROOTFS-STEP1.md) 搂2.1).  mockfs is not a filesystem in any
useful sense: it fabricates **three** vnodes and maps three names onto them
(`bsd/miscfs/mockfs/mockfs_vnops.c:106-144`):

| name looked up in `/` | result | code |
|---|---|---|
| `sbin` | the root vnode itself (so `/sbin` is `/`) | `:120-121` |
| `dev` | the devfs mountpoint node | `:122-123` |
| `launchd` | the **file node**: the whole root device as one `VREG` | `:124-125` |
| anything else | `ENOENT` | `:126-128` |

So the **only** path that can ever name an executable on this root is
`/sbin/launchd`, and what it names is the entire contents of
`Resources/UniXNU/kernel.macho.rd` (4 KiB of zeros today).

---

## Part A 鈥?the artifact

### A.1 What it is

`tools/mk_init_macho.py` is a pure-Python generator (standard library only) that
emits a static `MH_EXECUTE` AArch64 Mach-O:

| bytes | what |
|---|---|
| `0x000` | `mach_header_64`, `flags = 0` (no `MH_DYLDLINK`, no `MH_PIE`) |
| `0x020` | `LC_SEGMENT_64 __PAGEZERO` 鈥?`vmaddr 0`, `vmsize 0x100000000`, `filesize 0`, prot `NONE` |
| `0x068` | `LC_SEGMENT_64 __TEXT` 鈥?`vmaddr 0x100000000`, `vmsize 0x4000`, `fileoff 0`, `filesize = file size`, `R+X`, one section `__TEXT,__text` |
| `0x100` | `LC_SEGMENT_64 __DATA` 鈥?`vmaddr 0x100004000`, `vmsize 0x4000`, `fileoff 0`, `filesize 0` (pure zero-fill), `RW` |
| `0x148` | `LC_UNIXTHREAD` 鈥?`ARM_THREAD_STATE64`, count 68, all registers 0 except `pc` = entry |
| `0x268` | `LC_BUILD_VERSION` 鈥?`PLATFORM_IOS`, minos 13.0, sdk 17.0 |
| `0x280` | `LC_SYMTAB` 鈥?one symbol `_start` = entry (for `tools/macho_sym.py`, ignored by the kernel) |
| `0x2a0` | the payload, 8 instructions |

```
movz  x1, #0x5000                 ; x1 = 0x1_0000_5000
movk  x1, #0x0001, lsl #32
movz  x0, #0x6e69                 ; x0 = "initproc", little-endian
movk  x0, #0x7469, lsl #16
movk  x0, #0x7270, lsl #32
movk  x0, #0x636f, lsl #48
str   x0, [x1]                    ; [0x100005000] = 0x636f7270746 96e69
1: b  1b                          ; spin
```

No syscalls: there is no libc, no dyld and no `svc` in this image.  The beacon
lives in `__DATA` because `__TEXT` is R+X by the loader's own rule (below), so it
cannot hold a writable value.

The image is **padded to 16384 bytes** by default.  That is not cosmetic 鈥?see
the `resid` gate in A.2.

### A.2 Why each load command looks exactly like that

Each row cites the check that consumes it, in `bsd/kern/mach_loader.c` unless
noted.  The same table is in the script's docstring, next to the encoder.

| requirement | where it is enforced | what we do |
|---|---|---|
| `magic == MH_MAGIC_64` | `parse_machfile:809-812` | `0xFEEDFACF` |
| `cputype & ~CPU_ARCH_MASK == cpu_type() & ~CPU_ARCH_MASK` | `:834-837` | `CPU_TYPE_ARM64 = 0x0100000C` |
| `grade_binary(cputype, cpusubtype)` non-zero | `:839-843`, `bsd/dev/arm/kern_machdep.c:84-103` | `CPU_SUBTYPE_ARM64_ALL` grades 9 on both `ARM64_V8` and `ARM64E` hosts |
| `filetype == MH_EXECUTE` (and `depth == 1`) | `:847-851` | yes |
| a **static** image (no `MH_DYLDLINK`) is legal | `:852-869`: `#if !(DEVELOPMENT || DEBUG) return LOAD_FAILURE;` | flags 0. This is a **build-configuration gate**, see B.1 |
| `header + sizeofcmds` fits the file **and** a page-rounded read must not run short | `:895-900`, `:910-923` (`resid` 鈫?`LOAD_BADMACHO`) | the file is a whole 16 KiB page, so `round_page(32+632)=16384` reads back in full |
| `__PAGEZERO` raises `min_offset` to 4 GiB | `:2220-2291`, then `load_machfile:639-647` + `osfmk/vm/vm_map.c:20620-20635` | required for **every** 64-bit arm64 image; without it `EBADMACHO` |
| exactly one segment maps file offset 0, with `filesize > 0`, and it is `R+X` | `:1154-1162`, `:1209-1218` | `__TEXT` only |
| segment `fileoff` is aligned to `PAGE_MASK_64` **and** the map page mask | `:2175-2185` | 16 KiB process pages on arm64; `__TEXT` at 0, `__DATA` has `filesize 0` |
| a non-pagezero segment must not have `vmaddr == 0` | `:2293-2298` | `__TEXT` at `0x100000000`, `__DATA` at `0x100004000` |
| a segment with `filesize 0` becomes anonymous zero-fill | `:2431-2468`, `map_segment:1850-1854` | that is how `__DATA` is writable without occupying file bytes |
| entry point must be inside a `R+X` segment | `:1012-1027` (`validentry`), set at `:2503-2512` | `pc` inside `__TEXT` |
| `LC_UNIXTHREAD`, not `LC_MAIN` | `:1222-1242` vs `:1243-1255` | `load_main:2670` sets `needs_dynlinker = TRUE`, and `:1461-1464` then fails unless an `LC_LOAD_DYLINKER` brought a dyld along. `load_unixthread:2733-2806` sets `entry_point` and `unixproc` directly 鈥?that is the whole reason this image uses the older command |
| thread flavor `ARM_THREAD_STATE64` (6) with count **68** | `osfmk/arm64/status.c:1992-2012` (`thread_entrypoint`), `:1846-1855` (`thread_userstack`) | both return `KERN_INVALID_ARGUMENT` otherwise and the load dies |
| `sp == 0` | `osfmk/arm64/status.c:1854` reads the **32-bit** `sp` field for flavor 6 (a cast-to-`arm_thread_state32_t` bug in this tree) | with sp 0 the kernel reports "no custom stack" and `create_unix_stack` (`bsd/kern/kern_exec.c:6648-6699`) allocates the default one; `thread_setuserstack` (`:1745`) fixes sp afterwards |
| other commands are ignored | `:1442-1445` | `LC_BUILD_VERSION` and `LC_SYMTAB` are free |

Deliberately absent: `LC_CODE_SIGNATURE`, `LC_LOAD_DYLINKER`, `LC_MAIN`,
`LC_UUID`, `LC_ENCRYPTION_INFO*`.  The first of those is the interesting one 鈥?B.1 explains why its absence does not fail today.

### A.3 `--verify`: what is actually checked, and how

`--verify` re-reads the file **from disk** and re-parses it as the kernel does,
from scratch (no shared state with the writer), asserting 16 properties; each
message names the tree check it mirrors.  It was written so a mistake in the
writer cannot hide behind the same variable.  `--verify-only PATH` verifies an
arbitrary image (this is how the negative tests below were run).

```
$ py -3.13 tools/mk_init_macho.py --out out/kernel.macho.rd --verify --tree-tools
wrote out\kernel.macho.rd: 16384 bytes, entry 0x1000002a0, 6 load commands (632 bytes), 8 instructions
verify: re-reading out\kernel.macho.rd
  ok  magic MH_MAGIC_64 (parse_machfile:809)
  ok  cputype 0x0100000c is CPU_TYPE_ARM64
  ok  cpusubtype ARM64_ALL, ABI64 bit set
  ok  filetype MH_EXECUTE
  ok  flags 0x0: static, not slid
  ok  round_page(header+sizeofcmds) fits the file (mach_loader.c:917)
  ok  section __TEXT,__text at 0x1000002a0 (file 0x2a0, 32 bytes)
  ok  LC_UNIXTHREAD ARM_THREAD_STATE64/68, pc 0x1000002a0, sp 0
  ok  ncmds 6, sizeofcmds 632, walk lands exactly at the end
  ok  __PAGEZERO covers [0, 0x100000000) => min_offset >= 4 GiB
  ok  __TEXT fileoff 0 filesize 16384, __DATA zero-fill 16384 bytes
  ok  entry 0x1000002a0 inside R+X __TEXT
  ok  __DATA writable at 0x100004000+16384, disjoint from __TEXT
  ok  payload: 8 instructions, stores 0x636f727074696e69 at 0x100005000 (in __DATA), then b .
  ok  beacon bytes are the ASCII 'initproc'
  ok  LC_SYMTAB: _start = 0x1000002a0
verify: 16 checks passed on 16384 bytes
  ok  tools/macho_sym.py resolves _start at 0x1000002a0
verify: OK
```

Two independent layers, on purpose:

1. **Structural** 鈥?header, every load command, segment arithmetic, the
   page-zero extent, the page-alignment rules, the entry-point range.
2. **Semantic** 鈥?the payload is *disassembled back* into `(mnemonic, fields)`
   by a small decoder in the same file, and the verifier then asserts that the
   five `movz`/`movk` instructions really do produce `x1 = 0x100005000` and
   `x0 = 0x636f727074696e69`, that the single `str` really does store `x0` at
   `[x1]`, that the address is inside `__DATA`, and that the last instruction is
   a self-branch.  A wrong bit pattern in any of them fails the build.

`--tree-tools` additionally runs the repository's own Mach-O reader,
`tools/macho_sym.py`, twice, and requires it to resolve the entry point:

```
macho_sym.py <file>            ->  0x00000001000002a0 _start
macho_sym.py <file> 0x1000002a0 ->  0x00000001000002a0  _start+0x0
```

That tool unpacks the header with `"<IiiIIIII"` and walks the load commands to
find `LC_SYMTAB`, so a clean `_start` here proves the header *and* the command
walk decode the way this document says they do.  (Consequence worth knowing:
`macho_sym.py` exits with `no LC_SYMTAB` on any image without a symbol table 鈥?which is why the generator emits one even though the kernel ignores it.)

Negative tests, run against deliberately corrupted copies
(`--verify-only`, all exit 1):

| corruption | verifier says |
|---|---|
| magic byte 0 zeroed | `magic 0xfeedfa00 != MH_MAGIC_64` |
| `pc` moved outside `__TEXT` | `entry 0x200000000 is outside __TEXT ...: load_segment:2503-2512 leaves validentry 0` |
| `__PAGEZERO` `vmsize` 0x1000 | `no __PAGEZERO reaching 4 GiB: load_machfile:640-646 requires ...` |
| `sp = 0x1234` | `sp != 0: status.c:1854 reads the 32-bit sp field for flavor 6 ...` |
| thread count 64 | `thread count 64 != 68: thread_entrypoint returns KERN_INVALID_ARGUMENT` |
| thread flavor 5 | `thread flavor 5 != ARM_THREAD_STATE64` |

### A.4 What A is **not**

* Not run.  No phone, no emulator: whether the kernel accepts this image is
  the round described in B.3.
* The beacon is unobservable through the log (no syscall, no console).  It
  exists to make "did user space run" answerable *under a debugger* or by a
  later rung that reads it; the log-based acceptance signal is the *absence* of
  a load failure, see B.3.
* `--payload trap` builds a `BRK #0` variant instead (same layout) for the case
  where a fault report is more useful than a silent loop.
* The bridge to the *real* userland in Part C is not implemented, and should not
  be: this image is a probe.

---

## Part B 鈥?every gate between the last log line and `initproc running`

### B.1 The gates, in the order the kernel reaches them

The path is `load_init_program()` 鈫?`load_init_program_at_path()` 鈫?`execve()` 鈫?`__mac_execve()` 鈫?`namei` 鈫?`exec_check_permissions` 鈫?`exec_activate_image` 鈫?`exec_mach_imgact` 鈫?`load_machfile` 鈫?`parse_machfile` 脳4/3 passes 鈫?`activate_exec_state` 鈫?`create_unix_stack` 鈫?`process_signature`.  Line numbers are `bsd/kern/kern_exec.c` unless noted.

| # | gate | code | verdict for an unsigned, static, hand-made image |
|---|---|---|---|
| 1 | path resolution | `namei` in `__mac_execve`; mockfs `mockfs_vnops.c:106-144` | **PASS only for `/sbin/launchd`.** `/usr/appleinternal/sbin/launchd.development` cannot resolve: `usr` is `ENOENT` |
| 2 | `vnode_isreg` | `:6222-6224` | PASS 鈥?the file node is `VREG` (`mockfs_fsnode.c:338`) |
| 3 | at least one `x` bit | `:6244-6246` | PASS 鈥?mockfs returns `va_mode = VREAD\|VWRITE\|VEXEC` = `0o7` (`mockfs_vnops.c:178`) |
| 4 | `va_data_size != 0` | `:6249-6251` (`ENOEXEC`) | PASS once the image is installed; the current 4 KiB of zeros is non-zero, so this is not the blocker it looks like |
| 5 | `vnode_authorize(KAUTH_VNODE_EXECUTE)` | `:6289-6291` | PASS as uid 0 on a `0o7` file |
| 6 | `mac_vnode_check_exec` | `:6276-6281` | PASS 鈥?no code-signing policy is registered on this port (see gate 7); `CONFIG_MACF` is not in `config/MASTER.arm64.dandelion`, and with no policy the hook allows |
| 7 | **AMFI `vnode_check_signature` upcall** | not in this tree; on iOS it is a MACF policy in the AMFI kext | **ABSENT** 鈥?nothing sets `CS_SIGNED`. This is the difference from HakiOS, which *implements* this as a stub (C.2). It does not fail here because of gate 9 |
| 8 | header read tolerates a short file | `:2210-2220` 鈥?`vn_rdwr(PAGE_SIZE)` then `memset` the residue | PASS (a short file is zero-filled, not an error) |
| 9 | **unsigned image + global CS enforcement** | `mach_loader.c:1456-1459`: `if (!got_code_signatures && cs_process_global_enforcement()) ret = LOAD_FAILURE;` | **PASS.** `cs_process_global_enforcement()` (`bsd/kern/kern_cs.c:362-366`) returns `cs_process_enforcement_enable`, whose default is 0 unless `CONFIG_ENFORCE_SIGNED_CODE` (`kern_cs.c:94-100`), which appears nowhere in this tree's configuration. This is *the* gate that would normally refuse an unsigned binary, and it is already open |
| 10 | platform binary / entitlements | `task_set_platform_binary` at `:1672-1688` from `load_result.platform_binary`, which only `load_code_signature` sets (`mach_loader.c:3540`) | PASS (not a requirement 鈥?the task is simply not a platform binary). No entitlement is *required* for anything on this path |
| 11 | static-vs-dyld | `mach_loader.c:852-869` | **PASS only because this is a `DEVELOPMENT` build** (`#if !(DEVELOPMENT \|\| DEBUG)` 鈫?`LOAD_FAILURE`). A `RELEASE` kernel refuses every static executable |
| 12 | `needs_dynlinker 鈬?LC_LOAD_DYLINKER` | `:1461-1464` | PASS 鈥?`LC_UNIXTHREAD` never sets it (`:2670` does, and only `LC_MAIN`/driverkit reach it) |
| 13 | Mach-O structural gates | A.2 above, ~15 checks | PASS by construction, verified byte-level |
| 14 | hard page zero for 64-bit arm64 | `load_machfile:639-647` | PASS 鈥?`__PAGEZERO` |
| 15 | `activate_exec_state` 鈫?`thread_setstatus` + `thread_setentrypoint` | `:850-891` | PASS 鈥?flavor/count/pc as verified |
| 16 | `create_unix_stack`, copyout of argv strings | `:1648-1664`, `:1722-1746` | PASS (needs memory only) |
| 17 | `process_signature` | `:7329-7585` | PASS 鈥?no `CS_SIGNED` in `ip_csflags` (`:7432`), then `task_get_task_access_port` fails for a task with no iOS-style access port and the function returns **0** (`:7441-7451`), i.e. "no taskgated, carry on". `machine_task_process_signature` (`osfmk/arm64/machine_task.c:261-302`) only looks at x18 entitlements, which are absent |

**Nothing else refuses an unsigned hand-made Mach-O on this port.**  The one
gate that would is #9, and it is off; #11 is on but is satisfied by the
DEVELOPMENT configuration; #7 is missing but only *matters* when something asks
"is this signed" and acts on the answer (Part C).

Gates our existing patches touch, for the record:

| patch | what it changes | relevance here |
|---|---|---|
| `non-apple-trustedcache.patch` | `osfmk/kern/startup.c`: skip `trust_cache_init()` under `DANDELION` | removes a kext-dependent init at boot; **not** on the exec path |
| `non-apple-closed-source.patch` | `CONFIG_APPLE_AMCC` option + `osfmk/conf/files.arm64` | build-only; nothing to do with exec |
| `zzz10-dandelion-secure-bsd-root.patch` | `IOSecureBSDRoot`: no platform expert 鈫?treat as unsupported | lets `bsd_init` pass `bsd_init.c:1068`; already proven by the log |
| (no patch) | `bsd_init.c:1086-1106`: `rootvp not authenticated` panic | not reached, because `bsd_rooted_ramdisk()` is true with `rd=md0` |

**No source change is needed for Part B.**  In particular `zzz12-*.patch` is
*not* required, which keeps `patches/**` (and therefore CI) untouched by this
work.

### B.2 Which path is tried first, and whether a boot argument can redirect it

`load_init_program()` (`kern_exec.c:6874-6930`) has two stages:

1. **`launchdsuffix`** boot argument (`:6887-6916`, `DEBUG || DEVELOPMENT`
   only).  If set to `""`/`release` it tries `/sbin/launchd` and **panics** on
   failure (`:6902`); otherwise it builds
   `/usr/appleinternal/sbin/launchd.<suffix>` (`:6904-6905`).
   **It cannot name another path** 鈥?the prefix is hard-coded.
2. the compile-time table (`:6834-6842`, walked at `:6918-6927`):

```c
static const char * init_programs[] = {
#if DEBUG
	"/usr/appleinternal/sbin/launchd.debug",
#endif
#if DEVELOPMENT || DEBUG
	"/usr/appleinternal/sbin/launchd.development",
#endif
	"/sbin/launchd",
};
```

The log line names `launchd.development`, so **the build has `DEVELOPMENT || DEBUG`
and not `DEBUG`** 鈥?that is not a boot argument talking, it is
`KERNEL_CONFIG=DEVELOPMENT` (`.github/workflows/build-xnu.yml:49`) selecting the
first surviving table entry.  (It also independently proves gate #11 above is
open, since it is the same `DEVELOPMENT` preprocessor condition.)

The loop at `:6919-6927` prints `attempting to load <path>` *before* each try and
continues on `ENOENT`, then panics
(`:6929`) if all of them failed.  So the expected continuation of our log is a
**second** `attempting to load /sbin/launchd`, and that is the path mockfs *can*
resolve.

Can a boot argument point the loader at our file instead?  Three candidate
mechanisms, all checked:

| candidate | exists? | can it reach our file? |
|---|---|---|
| `init=` / `-init=` | **no such boot argument** 鈥?the only `PE_parse_boot_argn` calls near this code are `rd` (`bsd_init.c:452`, `IOKitBSDInit.cpp:751/1106`) and `launchdsuffix` (`kern_exec.c:6890`) | 鈥?|
| `launchdsuffix=鈥 | yes (`kern_exec.c:6890`) | **no** 鈥?suffix only, always under `/usr/appleinternal/sbin/launchd.` |
| make `/usr/鈥 resolvable | would need mockfs to name a node `usr` (`mockfs_vnops.c:120-128`) | would be a kernel patch, and buys nothing: `/sbin/launchd` is already reachable |

**Conclusion: the cheapest path is the last entry of the built-in table,
`/sbin/launchd`, which mockfs already maps to the file node.**  No boot-arg
change, no kernel patch.  What `tools/round.ps1` sends stays
`-v keepsyms=1 debug=0x144 rd=md0`.

One thing to keep in mind while reading the next log: the log currently stops
*after* the first `attempting to load` line and *before* the second one, and the
eMMC log block is only ~154 KiB of its 256 KiB (`docs/LOG-CHANNEL.md`), so it
did not run out of room.  Two explanations remain, and the round in B.3
distinguishes them by itself:

* the first `execve` does not return `ENOENT` (a silent death inside it 鈥?this
  port has the established failure mode "panic in a context where the console
  write cannot complete", `docs/PROGRESS.md` 搂浜?2), or
* the console writer lost the rest.

If the new log shows `attempting to load /sbin/launchd` then the first
explanation is dead and the probe is running normally.

### B.3 The smallest step sequence

Everything below is one round; only `flashinfo` is reflashed (the kernel and
`DarwinBoot` are unchanged), and the ADT already has the `RAMDisk` placeholder
(that is why `md0` exists today, [`ROOTFS-STEP1.md`](ROOTFS-STEP1.md) 搂2.2).

```powershell
# 1. build the image and verify it (byte level + the tree's own parser)
py -3.13 tools/mk_init_macho.py --out out/kernel.macho.rd --verify --tree-tools

# 2. install it where the ESP builder looks, and rebuild the FAT volume.
#    kernel.macho.rd is already in LAYOUT (tools/mk_unixnu_esp.py:73), so this
#    only swaps the file that goes into it.
copy out\kernel.macho.rd Resources\UniXNU\kernel.macho.rd
py -3.13 tools/mk_unixnu_esp.py --out out\unixnu-esp.img
py -3.13 tools/mk_unixnu_esp.py --check out\unixnu-esp.img   # kernel.macho.rd: 16384 bytes

# 3. flash the ESP only (0..30000 sectors; the log blocks behind it survive)
fastboot flash flashinfo out\unixnu-esp.img
fastboot reboot

# 4. read the kernel log back (Android + adb, per docs/LOG-CHANNEL.md), then
py -3.13 tools\read_log_block.py out\klog.bin --header --scan > out\kernel-boot.log
```

`tools/round.ps1` wraps 3鈥? if the phone is already in the right state.

**Acceptance signals, in the log order they will appear:**

| line | meaning |
|---|---|
| `mockfs_mountroot: root device 16384 bytes` | the new initrd's size reached the file node (was `4096`) |
| `Added memory device md0/rmd0 鈥?for 0x4000` | DarwinBoot wrote the new size into `/chosen/memory-map/RAMDisk` (`0x4000` bytes = 4 pages) |
| `load_init_program: attempting to load /sbin/launchd` | the first attempt returned `ENOENT` as expected and the loop advanced 鈥?**this is also the answer to the open question in B.2** |
| *no* `load_init_program: failed loading /sbin/launchd: errno N` | the exec got past gates 1鈥?3 |
| *no* `panic("Process 1 exec of /sbin/launchd failed, errno N")` (`kern_exec.c:6929`) | **`initproc` is running the payload** 鈥?the positive result is silence after that line |
| `panic: 鈥?Process 1 exec of /sbin/launchd failed, errno 88` | `EBADMACHO`: a structural gate (A.2) 鈥?re-read the verify output |
| `errno 86` (`EBADARCH`) | cputype/cpusubtype grading (`mach_loader.c:834-843`) |
| `errno 12` (`ENOMEM`) | `load_segment`/`map_segment` ran out (unlikely: 32 KiB of VA) |
| `errno 85` (`EBADEXEC`) | `exec_activate_image` iteration limit / non-Mach-O |
| no new line at all | the round told us nothing; then instrument `load_init_program_at_path` (Lead's call, it needs a patch) |

If the beacon matters, it needs a debugger or a later rung; the loop is
deliberate.

---

## Part C 鈥?what HakiOS does, and the three things we lack

`oosexgang/hakios-xnu-kernel` (HakiOS) runs **Darwin 16.5 user space** on a
POCO X5 Pro (`xnu-8796.121.2`, same generation as our `xnu-8796.141.3`).  The
repository contains no Apple binary; the user space is unpacked from an IPSW the
user supplies.  Everything below is from their published docs and scripts:

* README: <https://github.com/oosexgang/hakios-xnu-kernel>
* Architecture: <https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/concepts/architecture.md>
* Policy stand-ins review (line-level): <https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/driver-review/policy-glue.md>
* IPSW unpacking: <https://github.com/oosexgang/hakios-xnu-kernel/blob/main/sysimage/unpack.sh>
* Boot arguments: <https://github.com/oosexgang/hakios-xnu-kernel/blob/main/configs/boot-args.lst>

### C.1 IPSW 鈫?bootable user space, concretely

Their `sysimage/unpack.sh` (needs `python3`, `7z` 鈮?23 for APFS, `cc`) takes
*iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw* (exactly 6 165 698 198 bytes;
the script refuses anything else unless `ANY_IPSW=1`) and produces:

| output | from | notes |
|---|---|---|
| `rootfs/` | `096-04088-077.dmg`, the 4.7 GB APFS root | `sysimage/ipsw-member.py` lifts the member out of the IPSW, `sysimage/select-system.py` chooses a **subset** (`system.files` / `system.manifest`), `7z x` extracts it; two fonts are shrunk in place (`font_trim.py`: AppleColorEmoji, PingFang) so the image fits |
| `cryptex/` | `096-04666-078.dmg`, the 2.7 GB OS cryptex | minus `com.apple.dyld`; **LZBITMAP-compressed files are left empty by 7z** and finished by `unzbm` built from `xnu-deps/build/lzbitmap/` |
| `20F66__iPhone10,3_6/` | the same cryptex | the **dyld shared cache**, 43 files, `.symbols` left out |
| `cryptex-app/` + `cryptex-app.dmg` | `096-03536-083.dmg` (10 MB) | the App cryptex, reached through `/System/Cryptexes/App` |
| `var.skel` | `sysimage/mkvarskel.py` | the skeleton of `/private/var` |
| `DeviceTree.d22ap.im4p` | `Firmware/all_flash/` | the loader's identity |

Then `userland/build.sh` (45 KB) assembles the system image from that layout: the
root filesystem goes into an image that the kernel mounts read-only, `/private/var`
comes from a writable partition, and the dyld shared cache is where dyld expects
it.  Their `prebuilt/userland/` also ships a **tiny raw-syscall user space**
(`hello`, `init`, `stage0`, `libmini/mini.c` + `libmini/start.S`), which is what
`make qemu-open` runs 鈥?*"the open kernel + a placeholder loader + a tiny
raw-syscall user space, in QEMU"*.  That is the same rung as our Part A, and the
same reason to do it first: they needed it too, to know the exec path worked
before there was a user space worth executing.

### C.2 How they get past the gates in B.1

They do **not** pass `cs_enforcement_disable` or `amfi_get_out_of_my_way` 鈥?neither
appears in `configs/boot-args.lst`.  Instead they replace the kexts that would
otherwise be missing or refuse, with in-kernel stand-ins under `xnu/files/`
(per their `policy-glue` review):

| Apple component | their file | behaviour |
|---|---|---|
| AMFI | `xnu/files/libkern/amfi/miniamfi.c` | `vnode_check_signature` sets `CS_SIGNED`, and `CS_PLATFORM_BINARY` for ad-hoc signatures, then returns 0 鈥?**no validation** |
| AMFI entitlements | `xnu/files/libkern/amfi/miniamfi_entitlements.cpp` | reads entitlements out of the program's *own* signature blob; no trust evaluation |
| Trust cache / OSEntitlements | same file | "not found"/"unsupported" everywhere except the three `鈥ithProc` queries |
| Sandbox | `xnu/files/bsd/kern/kern_minimac.c` | every request except `container_path_for_pid` returns 0 |
| AppleKeyStore | `xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp` | "no passcode, unlocked"; wrap/unwrap is a reversible XOR |
| network stack | `xnu/files/iokit/Kernel/MiniIONetworkStack.cpp` | minimal service |

Boot arguments switch the drivers; `xnu/hakios.patch` carries the rest (one APSL
2.0 patch against their XNU).

**Mapping onto our list:** gates #7 (AMFI signature) and #10 (platform binary /
entitlements) are exactly what `miniamfi.c` fills in; gate #9 (global enforcement)
they leave at its default, like us.  For **our probe** none of that matters 鈥?the
probe is not asked to be a platform binary, and nothing consults a signature on
that path.  For **running Darwin user space** it matters a great deal: dyld, launchd
and the daemons do ask, and on this port a missing AMFI is a missing *answer*, not
a refusal 鈥?the same shape of bug as `zzz11`'s NULL `g_crypto_funcs`.  If we ever
get to C.3 step 3, `miniamfi.c` is the reference for what to link in, not something
to copy blindly (their own review lists its weaknesses: ad-hoc signatures are
trusted, entitlements are unvalidated).

### C.3 The three things we lack (and the reference for each)

| # | missing | why it is required | reference in HakiOS | cost signal |
|---|---|---|---|---|
| 1 | **a real root device**: a read-only block device holding ~2.5 GB of user space | mockfs is three vnodes and an executable; it cannot hold `/System/Library`, cannot be looked up beyond three names, and the 4 KiB RAM disk cannot be 2.5 GB. iOS user space needs **APFS** (their root is an APFS volume) plus UFS/eMMC | `bsd/dev/hakios.ufs.c` (`/dev/rufs_*`, 0640 root:operator), `bsd/miscfs/apfs/apfs_vfsops.c`, boot args `ufs=`, `root_apfs`, `ufs_write=1`, and `kern.hakios_switch_root` (root or pid 1, once) | largest item; they wrote an APFS implementation and a UFS driver |
| 2 | **an IPSW鈫抜mage toolchain** | nothing on our side can turn an IPSW into a root filesystem; it needs APFS extraction (7z 鈮?23), LZBITMAP decompression, dyld-cache placement, and a file-selection policy | `sysimage/unpack.sh`, `sysimage/ipsw-member.py`, `sysimage/select-system.py`, `sysimage/mkvarskel.py`, `font_trim.py`, then `userland/build.sh` | medium; it is scripts, but they are tuned to one IPSW |
| 3 | **display / touch / GPU drivers inside XNU** | user space that cannot draw is not user space; SpringBoard needs `IOMobileFramebuffer` and CoreAnimation needs a GPU driver. We have none 鈥?the console is eMMC writes (`patches/xnu/native-emmc-serial.patch`), and the panel is driven by LK/UniXNU before hand-off | `drivers/` (per subsystem), `docs/driver-review/{gpu-display,input}.md`, `iokit/Kernel/MiniIOMobileFramebuffer.cpp`, `hakios.gpu.cpp`, `tools/mesa/` (panfrost/freedreno) | large; MT6765 is Mali-G72, MT6768 is G52 鈥?both in panfrost's range, but the register work is per-SoC |

Two more facts worth having in the plan, both from their repository:

* **The loader writes almost nothing.** By default the system lives in RAM for
  the session; the only writes are the loader's log (a few KB) and, with
  `ufs_write=1`, a partition the user creates.  Our flashing story
  (`flashinfo` ESP + RAM disk) is not the limiting factor.
* **Every driver is an `options`/`conf/files` entry plus a boot argument**, i.e.
  exactly the shape our `config/MASTER.arm64.dandelion` + `patches/xnu/zzz*`
  already have.  Their `configs/common_defconfig` 鈫?`hakios-config.py` 鈫?  `config/MASTER.hakios` pipeline is the model for "kernel feature on/off by
  config, behaviour by boot-arg".

Legal note, since it is easy to get wrong: HakiOS ships no Apple file, downloads
none, and decrypts nothing.  Their own `LEGAL.md`/`COMPLIANCE.md` say so, and the
same rule applies to us: **this document cites paths and methods only.**  No
Apple binary, no vendored firmware, no key goes into `dandelion-xnu`.

---

## What would falsify this document

1. The round in B.3 produces `attempting to load /sbin/launchd` and then a
   `failed loading 鈥?errno 88` 鈥?then a structural gate in A.2 is wrong
   somewhere, and `--verify` is the place to look first (it is verified against
   the source, not against the kernel).
2. The round produces no second line at all 鈥?then the B.2 open question is the
   real blocker and the next step is instrumentation of
   `load_init_program_at_path`, not the artifact.
3. Someone reads `cs_process_global_enforcement()` from a debugger (or finds
   `CONFIG_ENFORCE_SIGNED_CODE` in the generated `.config`) as non-zero 鈥?then
   gate #9 closes, and an unsigned image needs `cs_enforcement_disable=1` (only
   honoured when `PE_i_can_has_debugger()` is true, `kern_cs.c:165-176`) or an
   AMFI stand-in per C.2.

## Artifacts

| path | what |
|---|---|
| `tools/mk_init_macho.py` | the generator, `--verify`, `--verify-only`, `--tree-tools`, `--payload trap` |
| `out/kernel.macho.rd` | the built image (16384 bytes), untracked |
