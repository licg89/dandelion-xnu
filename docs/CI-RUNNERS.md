# CI runners: what it takes to build this tree on `macos-15`

Status as of 2026-10-31: the two patch series entries this needs are in
`patches/xnu/` (`zzz7-*`, `zzz8-*`) and verified offline; the workflow still says
`runs-on: macos-14`, which is the Lead's file to change (§6 has the command).
Nothing here has been through GitHub Actions yet — §5 is explicit about what is
and is not verified.

## 1. Why the runner has to move

`macos-14` is retired on 2026-11-02. Until then it is on a brownout schedule that
cancels queued jobs with

    This job failed because of a scheduled macOS 14 runner image brownout

so the kernel build is already unreliable, and stops working entirely after the
retirement date.

## 2. What actually changed in the toolchain

From run `41107541053`, job `124241221262` (the run that was switched to
`macos-15`):

| | `macos-14` (what the tree was written for) | `macos-15` (what we get) |
|---|---|---|
| OS | macOS 14.x | macOS 15.8.2, build 24H232 |
| Xcode | 15.x present; the workflow's loop takes the newest it finds, starting at `Xcode_15.4` | `/Applications` holds `Xcode_16.0`…`Xcode_16.4` and `Xcode_26.0`…`Xcode_26.3` — **no Xcode 15.x at all** |
| compiler | Apple clang 15.x | Apple clang 17.0.0 (`clang-1700.0.13.5`), Xcode 16.4 / 16F6 |
| SDK | iPhoneOS17.x (ships with Xcode 15) | iPhoneOS18.5.sdk |

The workflow's "Select an Xcode whose clang can build this tree" step loops over
`Xcode_15.4 Xcode_15.3 Xcode_15.2 Xcode_15.1 Xcode_15.0`; on `macos-15` none of
those directories exists, so the loop falls through and the default Xcode (16.4)
is used. Pinning an older Xcode is therefore **not** an option on this image, and
clang 15 → clang 17 is unavoidable.

Everything up to the compiler works on `macos-15`: in that run `installhdrs`,
the libdispatch `os/` headers and `libfirehose_kernel.a` all built against
`iPhoneOS18.5.sdk`, and `make build` compiled `osfmk/`, `pexpert/`, `libkern/`,
`SETUP/`, `config/` and much of `bsd/`, `iokit/` and `security/` before stopping.

### The three classes of breakage clang 17 causes in this 2023 tree

`BUILD_WERROR=0` is passed to every build, so no warning is fatal here: all three
are hard errors, not promoted warnings.

| # | class | seen in | fixed by |
|---|---|---|---|
| 1 | unknown flag `-enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang` | earlier run | `zzz6-ci-clang-auto-var-init.patch` (flag dropped) |
| 2 | over-aligned array element: `uio_stackbuf_t` | `41107541053` | `zzz7-uio-stackbuf-alignment.patch` (§3) |
| 3 | DriverKit IIG glue: `IORPC` has no member `kernelContent` | `41107541053` | `zzz8-driverkit-iig-kernelcontent.patch` (§4) |

The complete set of compiler errors in that run is 16 lines: 7 of class 2 and 9
of class 3, listed below. `make` stopped after three sub-makes failed (`bsd`,
`security`, `iokit`); `bsd/kern`, `bsd/nfs` and `bsd/miscfs` were never compiled,
so classes 2 and 3 were not the last thing waiting in the build (§5).

## 3. Failure class 2: `uio_stackbuf_t` (7 sites seen, 36 sites in the tree)

    bsd/vfs/vfs_attrlist.c:881:27: error: size of array element of type 'uio_stackbuf_t' (aka 'char') (1 bytes) isn't a multiple of its alignment (8 bytes)
    bsd/vfs/vfs_attrlist.c:1850:29: (same)
    bsd/vfs/vfs_attrlist.c:3123:24: (same)
    bsd/vfs/vfs_attrlist.c:3477:24: (same)
    bsd/vfs/vfs_attrlist.c:4051:24: (same)
    security/mac_vfs_subr.c:170:24: (same)
    security/mac_vfs_subr.c:201:24: (same)

The definition is **not** in `bsd/sys/uio.h`; it is in `bsd/sys/uio_internal.h`:

```c
typedef char __attribute__((aligned(_Alignof(union iovecs)))) uio_stackbuf_t;
```

i.e. a 1-byte type that claims 8-byte alignment. Declaring one object of it is
legal; declaring an *array* of it is not, because an array element's size must be
a multiple of its alignment, and 1 is not a multiple of 8. clang 17 made that a
hard error where clang 15 accepted it. All 36 uses in the tree are arrays of the
form `uio_stackbuf_t uio_buf[UIO_SIZEOF(n)]` (12 files: `bsd/vfs/vfs_attrlist.c`
5, `vfs_syscalls.c` 10, `vfs_quota.c` 4, `bsd/nfs/nfs_serv.c` 5,
`bsd/kern/sys_generic.c` 4, `security/mac_vfs_subr.c` 2, and one each in
`vfs_vnops.c`, `vfs_subr.c`, `vfs_lookup.c`, `decmpfs.c`, `kpi_socket.c`,
`uipc_syscalls.c`), so the error is reported by whichever of them the build reaches
first — which is why only `vfs_attrlist.c` and `mac_vfs_subr.c` appear in the log,
while the other 29 sites sit in files the aborted sub-makes never compiled.

### The fix: keep the alignment, move it from the element type to the object

```c
#define uio_stackbuf_t char __attribute__((aligned(_Alignof(union iovecs))))
```

An over-aligned *object* is legal where an over-aligned array *element* is not,
so the property the type was created for survives, the size stays exact, and not
one of the 36 declaration sites changes.

Both properties the old typedef was trying to express are load-bearing:

* **1-byte element type.** `UIO_SIZEOF(n)` is a byte count and is used as the
  array dimension, so the element has to be a byte: `buf` must be exactly
  `UIO_SIZEOF(n)` bytes. It is also used directly as a byte count elsewhere
  (`nfs_serv.c` passes `UIO_SIZEOF(4)` to `kalloc_data`), so redefining
  `UIO_SIZEOF` in units of 8-byte elements is not available.
* **8-byte alignment.** `uio_createwithbuffer()` casts the buffer straight to
  `struct uio *` and `uio_init()` writes through it; `uio_addiov()` then writes
  `struct kern_iovec` / `struct user_iovec` fields (8-byte pointers and lengths)
  through a pointer derived from it (`bsd/kern/kern_subr.c:819-838`, `:1016-1033`).

### Alternatives rejected, and why

| alternative | verdict |
|---|---|
| Drop the alignment attribute (`typedef char uio_stackbuf_t;`) | Compiles, exact size, but the buffer is then aligned only by luck. Measured: as a struct member it lands at **offset 1**, and the first `uio_addiov()` writes an 8-byte pointer there. Rejected — it trades a compile error for a silent alignment fault. |
| Make the element type a struct with `aligned(8)` | Compiles and stays aligned, but `sizeof` of an element becomes 8, so every buffer is **8× too large**: 72 → 576 bytes for the common case, and the VLA in `kpi_socket.c:730` (dimension from `msg_iovlen`, up to `UIO_MAXIOV`) goes from ~16 KB to ~131 KB of kernel stack. It also needs a cast fix at `nfs_serv.c:773` (`char *uio_bufp = &uio_buf[0]`). Rejected. |
| Suppress the diagnostic | There is no flag for it; it is an error, not a warning in a `-W` group. |
| `UIO_STACKBUF(name, niov)` macro | This is what Apple did in xnu-10002.1.13 (macOS 14.4) for exactly this diagnostic: the typedef is deleted and all call sites become `UIO_STACKBUF(buf, n)` declaring a `uint64_t` array. Correct, upstream-blessed, and the fallback if the form above ever has to go — but it touches all 36 sites plus the `char *` alias in `nfs_serv.c`, and any site missed in a file that was not compiled locally is a fresh CI round trip. |

## 4. Failure class 3: DriverKit IIG glue / `IORPC::kernelContent` (9 errors)

    DriverKit/OSObject.iig.cpp:277:30: error: no member named 'kernelContent' in 'IORPC'
    DriverKit/OSObject.iig.cpp:305:30: error: no member named 'kernelContent' in 'IORPC'
    DriverKit/OSObject.iig.cpp:362:133: error: field designator 'kernelContent' does not refer to any field in type 'IORPC'
    DriverKit/OSObject.iig.cpp:425:133: error: field designator 'kernelContent' does not refer to any field in type 'IORPC'
    DriverKit/OSAction.iig.cpp:341:30: (no member named)
    DriverKit/OSAction.iig.cpp:374:30: (no member named)
    DriverKit/OSAction.iig.cpp:445:133: (field designator)
    DriverKit/OSAction.iig.cpp:522:133: (field designator)
    DriverKit/OSAction.iig.cpp:573:103: (field designator)

with the offending generated lines quoted by clang:

```c
IORPCMessage * msg = rpc.kernelContent;
IORPC rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach,
              .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl),
              .kernelContent = &buf.msg.content.__hdr };
```

### Where the name comes from

`*.iig.cpp` is generated at build time, so it cannot be edited. The rule is in
`iokit/DriverKit/Makefile`:

    $(GENERATED_HEADERS) : %.h : %.iig
        $(_v)$(IIG) --def $< --header $@ --impl $(patsubst %.h,%.iig.cpp,$@) --framework-name DriverKit ...

and `makedefs/MakeInc.cmd:199` resolves `IIG` as

    export IIG := $(shell $(XCRUN) -sdk $(SDKROOT) -find iig)

i.e. **the iig tool that ships with the selected Xcode/SDK** — here 16.4's, which
is newer than this 2023 tree. Its emitted glue assumes an `IORPC` with a
`kernelContent` member (the in-kernel `IORPCMessage` for the message being
dispatched). Nothing in the tree names it: `grep -r kernelContent` over
`iokit/DriverKit/*.iig` and over the whole tree returns nothing.

The `IORPC` the generated code is compiled against is the **tree's** copy, not
the SDK's: the kernel build includes headers from the exported header tree
(`build/obj/EXPORT_HDRS/...`, visible in every include chain in the log) and from
the fake SDK root that `make installhdrs` fills *from the tree*; the log shows
`INSTALLHDR IORPC.h`, and `iokit/DriverKit/Makefile` installs `IORPC.h` both as
`DriverKit/IORPC.h` and into the Kernel.framework private headers. So the 2023
definition is what the 16.4-generated glue meets, which is why the fix has to be
in the tree.

So the fix belongs in the tree, not in the `.iig` files and not in the glue:

* add the member to `struct IORPC` in **both** copies of the header
  (`iokit/DriverKit/IORPC.h` and `iokit/IOKit/IORPC.h` — they are byte-identical
  and both end up in the kernel build; leaving them different would be an ODR
  violation on a type passed by value to every `Dispatch()`);
* it must be the **last** member: the generated initializer is a designated
  initializer and C++ requires designators in declaration order. Declared in the
  wrong position clang only warns (`-Wreorder-init-list`) and the generated code
  then reads an uninitialised pointer — verified, it segfaults;
* the two places where the *tree* builds an `IORPC` by hand must set it, because
  the generated `Dispatch()` dereferences it unconditionally:
  `IOUserServer::server()` (`iokit/Kernel/IOUserServer.cpp:3214`, where the value
  is already in scope as `message = IORPCMessageFromMach(msgin, false)`) and
  `IOUserServerUEXTTrap()` (`:3364`, same value, same name).

Rejected alternative: stop the kernel build from using the tree's DriverKit
headers so the SDK's newer `IORPC` is used. That changes the in-kernel DriverKit
ABI for the whole tree to chase one generated file, and the tree's own
`IOUserServer.cpp` builds `IORPC`s against the old layout.

## 5. What is verified, and what is not

Verified here (Windows + WSL, no macOS):

* `py -3.13 tools/ci/check_series.py` → `OK (42 patch(es) apply in order, no
  suspicious bytes)`, i.e. both new patches apply after all 40 existing ones and
  add no BOM/zero-width bytes.
* The series output is byte-identical to the tree the patches were generated from
  for all four touched files (so the patches carry the change and not something
  else).
* The clang-17 diagnostic is reproduced exactly, and the fix compiles, with
  clang 21 and gcc 15 (neither is Apple clang, but both implement the same rule):
  * old typedef → `error: size of array element of type 'uio_stackbuf_t' (aka
    'char') (1 bytes) isn't a multiple of its alignment (8 bytes)`, same text as
    CI, at `vfs_attrlist.c:881` and `kpi_socket.c:730`;
  * new definition, with the **actual** declaration lines copied out of
    `vfs_attrlist.c:881`, `kpi_socket.c:730` (the VLA), `uipc_syscalls.c:3564`
    and `nfs_serv.c:772`, compiles at `-std=gnu89/gnu99/gnu11/gnu17`, clean
    under `-Wall -Wextra -Werror`, with `sizeof` 72 / 56 (the VLA, `msg` null) /
    1080 / 120 and 8-byte alignment in every case.
* The generated-glue shape compiles and runs as C++ with the field added last
  (`-std=gnu++1z`, the mode `OTHER_IIG_CFLAGS` uses), and reproduces both CI
  error messages when it is absent.
* A scan for the same over-aligned-typedef pattern across the tree finds only
  `uio_stackbuf_t`: the others (`user64_*_t` with `aligned(8)` and size 8,
  `user32_off_t` with `aligned(4)` and size 8, `dbgwrap_thread_state_t` with
  `aligned(16)` and size 272) all have element sizes that are multiples of their
  alignment, so clang 17 accepts them.

Not verified — no macOS host and no CI run was made:

* The full kernel build. Nothing here has compiled the tree end to end.
* That Apple clang 17 accepts the `#define uio_stackbuf_t char
  __attribute__((aligned(...)))` form. The attribute sits in declaration-
  specifiers position, which is what makes it apply to the object rather than to
  the type; clang 21 and gcc 15 both do that, and neither warns under
  `-Wall -Wextra -Werror` (a silently ignored attribute would have warned), but
  the exact Apple build is untested.
* **`bsd/kern`, `bsd/nfs` and `bsd/miscfs` were never compiled in the failing
  run** (the `bsd` sub-make stopped in `bsd/vfs`), so another clang-17 hard error
  in those directories is possible. `zzz7` removes the one class that was
  certainly waiting there (29 of the 36 sites), but it cannot prove the absence
  of others.
* The other 15 generated `.iig.cpp` translation units: `iig` generated all 17
  headers in that run and only these two files produced diagnostics, but with
  `-j` the remaining ones may simply not have been compiled before `make`
  aborted. So "the glue needs nothing else" is bounded, not proven.
* The runtime behaviour of the `kernelContent` field. It is compile-verified and
  the two in-tree construction sites set it, but no DriverKit RPC was executed.
  If a future generated file reads it from an `IORPC` built somewhere else, that
  path will see `NULL`.

## 6. Flipping the workflow to `macos-15`

`.github/workflows/build-xnu.yml` is the Lead's file; this document does not
change it. The exact command, from the repository root (PowerShell, keeps the
file's CRLF endings and writes no BOM):

```powershell
cd D:\aios\dandelion-xnu
$p = (Resolve-Path .github/workflows/build-xnu.yml).Path
$t = [IO.File]::ReadAllText($p).Replace('runs-on: macos-14', 'runs-on: macos-15')
[IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($false)))
Select-String -Path .github/workflows/build-xnu.yml -Pattern 'runs-on:'   # must print one line: macos-15
```

Two things to change in the same commit while there:

* the "Select an Xcode whose clang can build this tree" step is a no-op on
  `macos-15` (no `Xcode_15.x` exists there). Either delete it or point it at a
  concrete `Xcode_16.x` so the compiler is pinned instead of "whatever the image
  defaults to";
* the comment above `runs-on:` says `macos-14` stays until the `macos-15` path
  can build this tree — that condition is what `zzz7`/`zzz8` are for, so the
  comment has to be rewritten, not just the runner name.

`patches/**` is itself a CI trigger path, so the commit carrying `zzz7`/`zzz8`
and the workflow flip should be pushed together — one build, not two.

## 7. If the next run still fails

`py -3.13 tools/ci/ci_log.py --run <id> --all` prints every `error:` line, all of
them, not the first 60. Worth checking in this order:

1. a class-2 error anywhere — that would mean the new definition did not take
   effect: check that `zzz7` applied and that `bsd/sys/uio_internal.h` still
   carries the `#define uio_stackbuf_t ...` line and not the old typedef;
2. any *other* `error: size of array element of type ... isn't a multiple of its
   alignment` — a second over-aligned typedef, same shape of fix;
3. `kernelContent` again — if clang says the field is missing, `zzz8` did not
   apply to the copy of `IORPC.h` the failing translation unit includes (check
   both `iokit/IOKit/IORPC.h` and `iokit/DriverKit/IORPC.h`);
4. errors in directories the previous run never reached. Expect this: the last
   run stopped in `bsd/vfs`, `security` and `iokit`, so anything after those is
   still unmeasured.

## 8. How these two patches were made

The tree at `D:\aios\xnu` is upstream plus the whole series, so a patch is the
diff between a snapshot taken before editing and the tree after editing:

```powershell
cd D:\aios\xnu
# 1. snapshot the pre-edit files somewhere outside the tree
# 2. edit the tree copies
py -3.13 D:\aios\dandelion-xnu\tools\make_patch.py `
    D:\aios\dandelion-xnu\patches\xnu\zzz7-uio-stackbuf-alignment.patch `
    "bsd/sys/uio_internal.h=<snapshot dir>/uio_internal.h"
py -3.13 D:\aios\dandelion-xnu\tools\make_patch.py `
    D:\aios\dandelion-xnu\patches\xnu\zzz8-driverkit-iig-kernelcontent.patch `
    "iokit/DriverKit/IORPC.h=<snapshot dir>/DriverKit_IORPC.h" `
    "iokit/IOKit/IORPC.h=<snapshot dir>/IOKit_IORPC.h" `
    "iokit/Kernel/IOUserServer.cpp=<snapshot dir>/IOUserServer.cpp"
# 3. the gate
cd D:\aios\dandelion-xnu
py -3.13 tools/ci/check_series.py
```

`zzz7` and `zzz8` sort after `zzz6`, which is what the series needs because
`tools/ci/apply-patches.sh` applies `patches/xnu/*.patch` in filename order.
