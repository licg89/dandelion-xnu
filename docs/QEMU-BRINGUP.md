# Booting the dandelion kernel in QEMU: what works, what does not, how to hunt

Written 2026-10-31 20:xx, alongside `docs/QEMU-HUNT.md` (the plan) and
`docs/NEXT-SESSION.md` §0.-7…§0.-11 (what the corruption looks like).  Every
claim below is a line from a real run; where something does not work it says
so instead of hedging.

The port's problem is no longer a missing feature: the kernel boots, mounts a
root filesystem, reaches `devfs`, `load_init_program()` and the inside of
`execve`.  What stops it is a *moving* write-after-free (a 16-byte
`kalloc_type` element in the shared heap, plus corrupted `mockfs` structures).
No sanitizer is available for this tree, the phone gives no stacks, and every
experiment there costs a power cycle.  QEMU gives breakpoints, watchpoints and
unlimited reruns.

---

## 1. What this gets you, in one table

| | phone | QEMU |
|---|---|---|
| rerun a boot | manual power cycle, ~15 min | `tools/qemu/run.sh`, seconds |
| see the console | photographs of the panel, or pull `flashinfo` | PL011 on the terminal (`-dl_uart`), plus the trace ring read back over gdb |
| breakpoints | none | yes |
| watchpoints (**the point**) | none | yes, and `bt` at the write |
| hardware | MT6765: MSDC, DDP, GIC at MediaTek addresses | QEMU `virt`: PL011, GICv3 at 0x08000000, no eMMC, no panel |
| the corruption | reproduces, moving | **not yet reached**: see §6 |

## 2. Prerequisites (WSL Ubuntu, no macOS, no phone)

```
sudo apt install qemu-system-arm gcc-aarch64-linux-gnu gdb-multiarch
```

`qemu-system-aarch64` 10.2.1 is what this was written against, and
`gdb-multiarch` 17.1.  Nothing here needs the XNU tree to be buildable: the
kernel under test is `Resources/UniXNU/mach_kernel`, the binary CI produces.

## 3. The three pieces

### 3.1 A console: `-dl_uart` (patch `zzz22`)

`osfmk/kern/printf.c` mirrors every console character to the PL011 at PA
`0x09000000` when the command line contains `-dl_uart`.  Characters printed
before `arm_init()` has a pmap are held in a 2 KiB RAM ring and flushed from
`dandelion_uart_late_init()`, which `arm_init()` calls right after
`dandelion_emmc_enable()` -- the first point where `ml_io_map()` is usable.

The phone is unaffected: with no `-dl_uart` on its command line nothing in the
sink runs, and no device is touched.

### 3.2 A boot shim: `tools/qemu/shim`

QEMU's `-kernel` only loads an image and jumps to its entry point; it does not
know XNU's boot protocol.  `tools/qemu/shim` is a few hundred lines of AArch64
that does what DarwinBoot does:

| step | value |
|---|---|
| parse the Mach-O staged at PA `0x48000000` (`-device loader`) | 10 file-backed segments |
| copy the segments to PA `0x4D004000` (+ zero `.bss`) | `physBase`, the phone's window base |
| entry | `0xFFFFFFF0073CB488` -> PA `0x4D3CB488` (the phone's log says `entry pa 0x4D3CB488`) |
| build an ADT | 2552 bytes at `0x4DF08000`, `deviceTreeP` = `virtBase + (adt - physBase)` |
| build `struct boot_args` | at `0x4DF0C000`, `memSize` 256 MiB, framebuffer = the panel geometry |
| ramdisk | `0x4E000000`, 16 KiB Mach-O, advertised as `/chosen/memory-map/RAMDisk` + `rd=md0` |
| jump | `x0` = boot_args PA, MMU/caches off, DAIF masked, EL1 |

`virtBase` is `VM_KERNEL_LINK_ADDRESS` (`0xFFFFFFF007004000`) and `physBase` is
`0x4D004000`.  They must agree modulo 2 MiB, because `start.s` builds 2 MiB
block mappings from both and a block descriptor drops the low 21 bits:
`0x4D004000 & 0x1fffff == 0xFFFFFFF007004000 & 0x1fffff == 0x4000`.  Get that
wrong and the kernel runs with a 0x4000 displacement between where it is and
where it thinks it is.

**The ADT is generated, not the shipped blob.**  `Resources/UniXNU/kernel.macho.adt`
describes the Redmi 9A: its `/arm-io/gic` says `0x0c000000`, which is where the
kernel takes the interrupt-controller base from
(`pexpert/arm/pe_identify_machine.c`).  QEMU's GICv3 is at `0x08000000`, so the
shim builds the same tree (`bootshim/adt.c` layout, node for node) with QEMU's
numbers, and drops the `emmc-log` node, whose `reg` the kernel would otherwise
map and drive.  Everything else -- `/chosen/dram-base`, `/cpus`, `/defaults`,
`darwinboot-memory-size` -- is identical to the phone's.

### 3.3 Reading the kernel's own console back: `tools/qemu/ringdump.py`

`printf.c` already appends every console character to a 256 KiB ring at PA
`0x5FC00000` (magic `XTX1`, then a u32 length, then the text) and paints the
framebuffer at `0x7EC50000`.  Both live in QEMU's RAM, and QEMU stops the guest
when a gdb client attaches, so `ringdump.py` attaches to the gdbstub, dumps
both ranges and decodes them -- **no kernel change needed**, which is why the
boot could be followed before `zzz22` existed.  `--fb` decodes the text out of
the framebuffer dump as well, `--registers` adds the CPU state.

The ring is not gated by anything and works on both machines, so it stays the
fallback when the serial console is misbehaving.

---

## 4. Running it

```bash
tools/qemu/build.sh                    # builds tools/qemu/shim/out/shim.elf
tools/qemu/run.sh                      # boot, serial log to tools/qemu/out/serial.log
tools/qemu/run.sh --stdout             # serial on the terminal
tools/qemu/run.sh --ring --fb --regs   # boot, then dump ring + framebuffer + CPU state
tools/qemu/run.sh --seconds 900        # a long boot (TCG is ~3x slower than the phone)
tools/qemu/run.sh --gdb                # stop at reset, wait for gdb-multiarch
tools/qemu/run.sh --kernel tools/qemu/out/mach_kernel.nogic
```

`--kernel` exists because a run often wants one probe neutralised without an
18-minute CI round: `tools/qemu/patchkernel.py --symbol <name> --out ...`
rewrites the first two instructions of a function to `mov w0,#0; ret`, which is
the same as gdb's `return 0` but applies before the guest starts.

### The serial log, verbatim

The shim's own lines are observed.  The kernel's half needs the `zzzz` build --
the PL011 sink lives in it -- so those lines are marked as expected; they are
already visible in the trace ring today, so the shape is not a guess.

```
=== dandelion QEMU shim ===
SHIM: timebase 62500000
SHIM: mach_kernel at 0000000048000000, kernel window 000000004d004000
SHIM: segments 10, entry fffffff0073cb488 -> pa 000000004d3cb488
SHIM: kernel end 000000004df08000, adt 000000004df08000, boot_args 000000004df0c000
SHIM: ramdisk 16384 bytes at 000000004e000000 (rd=md0)
SHIM: adt 2552 bytes, jumping to the kernel

                                        <- kernel output, once zzzz is built:
[dl] gates: uart=on qemu=on fb=off emmc=off gic-late=off trace=on
[dl] gates: cmdline: -v serial=3 debug=0x144 keepsyms=1 -noprogress wdt=-1 rd=md0 -dl_uart -dl_qemu
eMMC log: skipped (-dl_qemu: no MSDC0 here)
=== xnu: arm_init ===
...
```

Before that build existed, the same boot was read out of the ring with
`run.sh --ring`; that is what the gate line above was added for.

## 5. What does not work, and what was changed on the QEMU side only

Everything in this list is either a property of QEMU or a deliberate
deviation, never a change to the phone's path.

| thing | in QEMU | why / what instead |
|---|---|---|
| `GICD`/`GICR` at `0x0c000000` | nothing there | the ADT points the kernel at `0x08000000`, and `dandelion_gic_late_init()` is skipped under `-dl_qemu`. Without that skip it maps, reads, and data-aborts at `dandelion_gic_late_init+0x6c` (esr `0x96000010`) |
| MSDC0 at `0x11230000` | nothing there | `-dl_qemu` disables the eMMC log channel before it touches the controller. Without the gate it happens to read zeros: the hand-off word is at `0x5FD00000`, inside the extra 8 MiB `start.s` maps, so the magic check fails and the channel gives up -- luck, not a gate, which is why the gate exists |
| panel at `0x7ec50000` | RAM | `-dl_qemu` switches the framebuffer console and the probe bars off. It would "work" (write 19 MiB of pixels into RAM) but nothing reads it |
| trace ring at `0x5fc00000` | RAM | never gated, on either machine |
| `wdt` (interrupt-masked panic) | fires at `waitForSystemMapper()` (`machine_routines_common.c:897`, 30 ms against a 13.6 ms budget) | not a port bug: TCG is ~3x slower than the phone. The shim's command line carries `wdt=-1`, which is XNU's own switch |
| `-m 512` or less | the ring and the panel are outside RAM | use `-m 1024` or more; `run.sh` defaults to 2048 |
| `cpus=1` / `-smp 1` | one CPU | the port has one CPU's bring-up code; the ADT describes one CPU |
| the PL011 driver in `pe_serial.c` | not used | `VMAPPLE_UART` is not defined for this board, so the kernel does not drive a PL011 itself; `zzz22` writes the data register directly from the console path |
| `Res/+kernel.macho.adt` | not used | it describes MediaTek hardware; see §3.2 |

## 6. Where the boot gets to (and what is still missing)

Measured with `Resources/UniXNU/mach_kernel` (the 2026-10-31 build, before
`zzz22`) plus the GIC probe neutralised with `patchkernel.py`, and with the
ramdisk in place:

```
bsd_init: calling inittodr
[dl] root mount: entering loop
[dl] timerprobe: busy-wait spans=528172 elapsed=12655387 ticks=12500000 freq=62500000
[dl] IOFindBSDRoot: IOBSD resource absent after 200 ms (polls=101596)
Added memory device md0/rmd0 (01000000/0D000000) at FFFFFFF014400000 for 0000000000004000
BSD root: md0, major 1, minor 0
bsd_init: vfs_mountroot
mockfs_mountroot: root device 16384 bytes
[dl] vfs_mountroot -> 0 in 2545148 ticks (13000000 = 1 s)
[dl] root mount: success
DANDELION: platform expert: power domains SKIPPED (bring-up, see zzz21)
DANDELION: platform expert: uuid published
DANDELION: platform expert: configure
DANDELION: gic pidr2=0 arch=3 typer=0 gicr_typer=0
DANDELION: configure: done
```

So the whole early half of the phone's boot reproduces: `arm_init`,
`arm_vm_init`, `pmap_bootstrap`, the timer (ticks arrive -- `freq=62500000` is
QEMU's `CNTFRQ`), `bsd_init`, the root mount, `mockfs`, and the platform
expert.  The tick probe is real: `elapsed=12655387` against `ticks=12500000` is
200 ms at 62.5 MHz.

**Reached, and this is the point of the exercise:** with `IOSecureBSDRoot`
neutralised as well (see below), the boot walks on past `VFS_ROOT` and the
freed-element detector fires -- the phone's corruption, in QEMU:

```
[dl] root mount: success
[dl] zalloc: freed element was modified; re-zeroing and continuing (see docs/NEXT-SESSION.md)
```

Note that this particular binary (today's 10:48 UTC build) still carries the
*re-zeroing* variant of that report, and re-zeroing a 16-byte element that is
still live can itself disturb things -- the later `zzz20` only records.  Worth
knowing before believing the first hit.

### The wait that had to be removed to get there

After the platform expert's `configure: done` the boot does not stop in code --
it stops *waiting*.  Attaching gdb after 130 s of boot shows the only thing
running is the idle thread:

```
(gdb) bt 24
#0  0xfffffff0073d1468 in ml_get_timebase ()
#1  0xfffffff007295120 in processor_idle ()
#2  0xfffffff007295284 in idle_thread ()
#3  0xfffffff0073cd800 in Call_continuation ()
```

So `bsd_init` is blocked on a wait that nothing in QEMU completes, not
spinning: on the phone the same point goes on to `VFS_ROOT`.  The wait is
`IOSecureBSDRoot()` (iokit/bsddev/IOKitBSDInit.cpp:1125):

```c
pe = (IOPlatformExpert *) IOService::waitForMatchingService(matching, 30 * kSecondScale);
```

On this port the platform expert does match -- its `configure` runs and prints
`done` right before the boot goes quiet -- but the "wait until it has started"
half never completes, so the 30 s deadline never fires and `bsd_init` sleeps
forever.  `zzz10` already handles the *other* outcome (`pe == NULL`: print a
line and carry on); nothing handles "never returns".

For a QEMU run it is enough to neutralise the function before the guest starts:

```bash
py -3.13 tools/qemu/patchkernel.py --kernel Resources/UniXNU/mach_kernel \
    --symbol dandelion_gic_late_init --symbol IOSecureBSDRoot --mode ret \
    --out tools/qemu/out/mach_kernel.nogic2
tools/qemu/run.sh --kernel tools/qemu/out/mach_kernel.nogic2 --ring
```

That is what the run above used, and it is what carries the boot into
`VFS_ROOT` -> `devfs_kernel_mount` and on to the freed-element report.  A real
fix belongs in the port, not here: either why the platform expert never
finishes starting under QEMU, or a `zzz10`-style "do not wait for a service this
port cannot provide" gate.

## 7. Catching the writer

XNU's own detector is too late: `zalloc_validate_element` notices a dirty
element when it is handed out again, long after the write.  A watchpoint fires
on the write and `bt` names the writer.

```bash
tools/qemu/run.sh --gdb                     # QEMU waits for the debugger
gdb-multiarch -x tools/qemu/gdb.txt         # then: continue
# in another shell, or instead of the above:
gdb-multiarch -x tools/qemu/writewatch.py
```

`tools/qemu/writewatch.py` automates the recipe: break on
`kfree_ext(void *kheap, void *data, vm_size_t size)` (x1 = element, x2 = size),
keep the elements of the 16-byte size class, `finish` the free, then
`watch *(unsigned long *)$x1`.  It keeps at most three watchpoints alive,
dropping the oldest, and arms itself at `bsd_init` so the trap is not spent on
the thousands of early frees.  On a hit it prints the element's contents, the
registers and a 20-frame backtrace, and stops.

The by-hand version, and the reasons for each step (`kfree_ext` must be
*finished* before the watchpoint goes on, `awatch` instead of `watch` when the
read side matters too), are in `tools/qemu/gdb.txt`.

Two cautions that cost time to learn:

* a hit is either the bug or a legitimate reuse of the same address by the
  zone allocator; the backtrace tells you which, and `WATCH_SIZE`/`WATCH_AFTER`
  are the knobs for narrowing it;
* one gdb client at a time.  `ringdump.py` and the interactive session both
  want the gdbstub, so `detach` before you dump the ring.

## 8. Files

| file | what |
|---|---|
| `tools/qemu/build.sh` | build the shim (`aarch64-linux-gnu-gcc`, or `CC=clang`) |
| `tools/qemu/shim/start.S` | EL2 -> EL1 drop, DAIF, SCTLR/TCR, stack, `shim_main` |
| `tools/qemu/shim/shim.c` | Mach-O load, boot_args, ramdisk, jump |
| `tools/qemu/shim/adt_qemu.c` | the ADT for QEMU's virt machine |
| `tools/qemu/shim/shim.ld` | link at `0x40080000` |
| `tools/qemu/run.sh` | one boot; `--ring`, `--fb`, `--regs`, `--gdb`, `--kernel` |
| `tools/qemu/ringdump.py` | read the ring/framebuffer out of a live guest |
| `tools/qemu/patchkernel.py` | neutralise one function for a run |
| `tools/qemu/gdb.txt` | the gdb recipe |
| `tools/qemu/writewatch.py` | the `kfree_ext` -> watchpoint automation |
| `patches/xnu/zzz22-dandelion-qemu-console.patch` | `-dl_uart` sink and `-dl_qemu` gates |
