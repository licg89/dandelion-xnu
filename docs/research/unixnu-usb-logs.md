# UniXNU boot/kernel log capture — mechanisms, prerequisites, and what transfers to an MT6762G XNU port

Research report. Subject: how the UniXNU project (AArch64 XNU on Android phones) captures boot and kernel
logs, and which of those mechanisms is reproducible on a MediaTek MT6762G phone with **no UEFI** (LK
bootloader only), **no working kernel drivers**, **no accessible UART**, and a **shim that runs at EL1 with
the MMU off before the kernel**.

All UniXNU claims below are taken from the pages and source files listed in §11. Where I am reasoning
rather than quoting, the sentence says so explicitly. Nothing here was verified on hardware; no flashing or
device commands were run.

---

## 0. Answer in one table

| Stage | Mechanism actually used by UniXNU | Transport | Needs UEFI? | Needs a working kernel driver? | Portable to MT6762G? |
|---|---|---|---|---|---|
| UEFI firmware | Advanced Logger RAM buffer + `AdvancedLogDxe` renders it as **OneSend QR frames on the panel** | optical (phone camera) | yes | no | only after a UEFI port |
| UEFI firmware | `EFI_SERIAL_IO_PROTOCOL` → real **UART** at 115200 8N1 | wire | yes | no | no UART on this board |
| UEFI firmware | `EFI_GRAPHICS_OUTPUT_PROTOCOL` text/menu | screen | yes | no | n/a |
| XNU entry (pre-MMU) | **Entry-probe colour bars** written straight to the framebuffer physical address | screen | no (loader does it) | **no** | **yes — best first step** |
| XNU kernel | `boot-progress=1/2` **diagnostic page** (48×120 text) + pulse bar; panic screen | screen | no | framebuffer only | yes, after a display driver |
| XNU kernel | `serial=3` console on UART | wire | no | UART driver | no |
| XNU kernel / userland | **USB CDC ACM gadget inside XNU** (giulia port), `/dev/giulia-debug` → `giulia-debugd` → PTY shell | USB | no | **yes** (USB device stack) | no (not without USB drivers) |
| Post-reset / persistent | **`SHLOG001` log window in the last 32 MiB of `userdata`**, written every 2 s by a kernel thread | eMMC/UFS | no | **yes** (block driver + bounded-window API) | yes, but it is the 2nd or 3rd project |
| Persistent read-back | **separate Linux "reader" image booted with `fastboot boot`**, exposing a **configfs CDC ACM shell**; host script pulls and decodes | USB | no | Linux-side only | expensive: no mainline MT6765 DTB |
| Persistent read-back | `BootReset`/`rebootbl`: PMIC SDAM reboot-reason cell + PSCI `SYSTEM_RESET` back to fastboot | — | no | register pokes only | yes (MTK equivalent must be found) |

The short version: **UniXNU does not have a USB log path in its firmware or its kernel entry stages.** Its
firmware stage uses QR codes over the panel, and its kernel-entry stage uses coloured bars written to the
framebuffer with the MMU off. USB only appears in two places: a full XNU USB device stack (giulia), and a
separate Linux image that exists purely to be a USB serial server. For the MT6762G, the two mechanisms that
need nothing you do not already have are the **framebuffer** and a **reset-surviving RAM ring**.

---

## 1. How UniXNU itself frames the problem

`docs/zh/porting/log-capture.md` splits capture into four stages, and the table is worth quoting because it
is the project's own statement of what each stage costs:

| Stage | Support required | Result saved |
|---|---|---|
| UEFI menu | GOP display, menu input, **Advanced Logger and QR plugin** | scanned `.log` file of the firmware |
| XNU entry | **UART or screen diagnostics** | serial log, or photo of the last checkpoint/panic |
| Storage | reserved log region, kernel writer, Linux reader | kernel message buffer + diagnostic snapshots |
| Repeat test | log read-back and return-to-bootloader | per-boot evidence directory |

And from `scripts/patch-xnu-entry-probe.py` in the sheng device repo, the operative sentence for any
phone-shaped device:

> "There is no serial console on the device and the kernel cannot draw before its own video console exists,
> so the early boot stages are reported by side channels."

That is the same constraint as the MT6762G. The sheng tablet (Snapdragon 8 Gen 2, the reference port) has
**no serial console either** — UART is not what makes UniXNU work.

---

## 2. Stage A — UEFI firmware

### A1. UART (`EFI_SERIAL_IO_PROTOCOL`)

UniXNU's boot manager writes to **the first `EFI_SERIAL_IO_PROTOCOL` instance in the handle database**.
Lines are prefixed `UNIXNU:` (boot manager) and `DARWINBOOT:` (the XNU loader). Host side is a plain
115200 8N1 terminal (`tio -b 115200 -d 8 -p none -s 1`, `picocom`, `screen -L`, `minicom`).

Prerequisites: a board with real UART pins brought out, a platform `SerialDxe`/`SbsaQemuSerialDxe`-style
driver in the firmware volume, and physical access to the pads. The docs note that *boards which support
USB device mode can expose a CDC ACM serial port* — but UniXNU ships **no** such driver: the only drivers
in `package/UniXNUPkg/Driver/` are `AdvancedLogDxe`, `UniXNUDxe`, `UniXNURamDiskDxe`. A firmware-stage USB
serial would have to come from the platform's Mu-Silicium tree. Do not plan on it.

Representative early output (from `docs/zh/guide/debugging.md`):

```
UNIXNU: UniXNU 0.1.0 starting as firmware boot option
UNIXNU: config: loaded \EFI\UniXNU\unixnu.toml
DARWINBOOT: loader v2 installed EL1
DARWINBOOT: inputs validated; exiting boot services
```

### A2. Advanced Logger + QR ("OneSend") — the actual firmware log path

This is the mechanism UniXNU uses on every board, UART or not.

Components:

* `package/UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf` + `AdvancedLogDxe.c` (16 KB), added to the
  DSC `[Components]` and the FDF firmware volume.
* Depends on `gAdvancedLoggerProtocolGuid`, `AdvLoggerPkg`, `MsGraphicsPkg`, `AdvancedLoggerAccessLib`,
  `QrEncoderLib`. It loads *after* `gAdvancedLoggerProtocolGuid` is installed, so the platform's Advanced
  Logger collector module must already be feeding it (that is where PEI/DXE/`DebugLib` output goes).
* `gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel` (ASCII) sets the exported file name prefix.
* Menu entry appears under **Tools → Advanced Logger**, enabled by `[payload] tools = true` in the device's
  `board.toml` (or a `[[menu]] action = "tools"` entry in hand-written `unixnu.toml`).

Wire format, read out of `AdvancedLogDxe.c`:

* Log text is read once through `AdvancedLoggerAccessLib`, wrapped in an envelope `"LMS1"` version 2:
  `name_len` (u16 @6), `mime_len` (u16 @8), `log_len` (u32 @10), CRC32 (u32 @14), then duplicated lengths,
  then file name, MIME string `text/plain`, then the log bytes. File name is
  `<PcdUniXNUDeviceModel>-YYYYMMDD-HHMMSS.log` from `gRT->GetTime()`.
* The envelope is cut into **720-byte blocks**; each frame is
  `'O','S', version 2 | session_id (u32) | sequence (u32) | block_count (u32) | block_len=720 (u16) | 0 (u16)
  | payload_len (u32) | payload_crc32 (u32)` = **28-byte header**, then 720 bytes, then a **CRC32 over the
  first 748 bytes** → 752-byte frame. Each frame is rendered as one QR symbol on the GOP.
* Frame rate: five gears, `5000 / 2000 / 1000 / 500 / 250 ms` per frame, initial gear = 1000 ms. Volume
  up/down changes gear; power or Enter/Escape leaves. If the log protocol is missing the plugin prints
  `Advanced Logger: no log`.
* Receiver is a **third-party phone app** ("OneSend / 扫传", `https://onesend.01mvp.com`), i.e. the host tool
  is somebody else's camera app — there is no UniXNU host decoder for this path.

Prerequisites: UEFI with GOP, the Advanced Logger collector, the two libraries, and a second device with a
camera. This is why it works on a phone with no UART.

### A3. Framebuffer (`EFI_GRAPHICS_OUTPUT_PROTOCOL`)

The boot menu, the diagnostic page and the colour bars all go through GOP. `EFI_GRAPHICS_OUTPUT_PROTOCOL` is
listed as a *required service* in `docs/zh/porting/uefi-firmware.md`, together with
`EFI_SIMPLE_TEXT_IN_PROTOCOL`, `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, `EFI_BLOCK_IO_PROTOCOL`,
`EFI_SERIAL_IO_PROTOCOL` (marked "for boards with UART"), `EFI_ACPI_TABLE_PROTOCOL`,
`EFI_FIRMWARE_VOLUME2_PROTOCOL`.

---

## 3. Stage B — XNU entry and kernel

### B1. Entry-probe colour bars (the single most transferable technique)

`scripts/patch-xnu-entry-probe.py` (sheng repo, 20 KB) patches a **copy** of the kernel. There is no
console and no video driver yet, so it writes pixels to the framebuffer **physical** address with the MMU
off. The injected stub is:

```
  ldr x1, [x0, #0x28]      ; framebuffer base (physical)
  ldr x2, [x0, #0x38]      ; width
  ldr x3, [x0, #0x48]      ; stride / pitch
  mul x2, x2, x3
  lsr x2, x2, #4           ; 1/16 of a line's worth of bytes = one band
  bic x2, x2, #7
  mov x4, #<band>          ; band index = stage number
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #<lo>           ; 32 bpp pixel
  movk w6, #<hi>, lsl #16
1: stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
```

`x0` (stage 1) / `x20` (stage 2) is the loader's boot-args pointer; framebuffer **base** is at **+0x28**,
vertical extent at **+0x38**, pitch at **+0x48** — the code multiplies the last two and shifts right 4, so a
band is `height*pitch/16` bytes, i.e. exactly 1/16 of the frame. Bands are the 1/16-screen strips *below* the
green one that the loader paints. Semantics:

| Band | Colour | Painted by | Meaning |
|---|---|---|---|
| top | green | `DarwinEnter` after `ExitBootServices` | loader handed over, before the jump |
| 2nd | blue | probe stage 1 | kernel entry code ran, MMU still off |
| 3rd | magenta | probe stage 2 | kernel reached the instruction just before MMU-on |

Stage 3 is *time*, not colour: after the MMU is on, the patched kernel waits `--reset-delay N` seconds and
issues **PSCI `SYSTEM_RESET`**. If the phone reboots by itself after N seconds, the MMU came on. Stage 4 is
the same trick placed after an arbitrary symbol (`--reset-after _arm_vm_init --in _arm_init
--occurrence 1`), so one boot answers one yes/no question.

Every patch site is validated against its expected original instruction and the tool refuses a kernel that
does not match — the equivalent of a load-address check in a shim.

The code caves are `0xFFFFFFF00B9D0500/0560/05C0/0620` (0x60 bytes each) in the zero padding after
`_start`. Timebase is 19,200,000 Hz.

### B2. Diagnostic page and panic screen (needs a framebuffer mapping)

`boot-progress=1` (freeze: 30 s with no stage/storage progress, or a 120 s cap) and `boot-progress=2`
(continuous). The page is 48 rows × 120 columns of text; scales 3× on panels ≥ 2160×1400. Fields include
`time idle IRQ monitor lost root procs free requests verified4K failures integrity last PC TCR TTBR SCTLR`.
Row 47 is a separate **pulse bar** drawn by a high-priority thread once a second:
`P<turn> I<irq> M<monitor>/<seconds> B<bind>:<result> d<depth> @<base> lock<busy>/<render> PC<pc>`.
`boot-diag-resume=<s>` reclaims the panel after userland stops scanning out.

`[payload] panic_screen = true` runs `scripts/patch-xnu-entry-probe.py --panic-screen --no-reset`, which
hooks `_panic` and `_vinfo` and renders the format string **through the kernel's own framebuffer mapping**
using Mu-Silicium's `Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/Font.h`. `--no-reset`
keeps the screen up for a photograph. The first panic line is also copied to `boot_progress_panic_text`.

There is also a DT-based panic region (Apple's "embedded panic log", the XNU analogue of pstore/ramoops):
`CONFIG_XNU_DT_PANIC_REGION=y`, kernel reads `/chosen/embedded-panic-log-size` and the region's `reg`
property and validates before mapping. **Prerequisite: a reserved region that survives the reset and
something that reads it back.** UniXNU uses it for panic text, not as the general log transport.

### B3. XNU serial

`serial=3` + `debug=0x14e` + `kextlog=0xfff` + `-v` in `[payload] kernel_args`. UART only.

### B4. XNU-side USB CDC (only in the giulia port)

The OnePlus Ace 5 (SM8650) port has a working USB CDC path *inside XNU*, per
`UniXNU-OnePlus-Ace5/docs/USB_TERMINAL.md` and the giulia device page ("USB CDC 日志可用"). Details that
matter:

* Boot args `vermeer_usb=1` (enables CDС), `giulia_debug=1` (registers the device node), `giulia_mmio=1`.
* Data path: **host CMD → USB CDC → `/dev/giulia-debug` (root `0600`, single opener) → `giulia-debugd` →
  `forkpty()` → shell**. The daemon is ad-hoc signed under macOS + iPhoneOS SDK and trusted through an AMFI
  `CodeDirectory` hash list baked into the kernel patch — userland install is a separate, non-trivial step.
* Host sees `0x1209:0x0001`, iUSB serial `ACE5DBG001`, 115200 with DTR asserted.
* Protocol "2": commands `help ping status replay features info uptime memory ps bootargs display usb ufs
  keys.status audio.* gpu.* reg.* reboot.fastboot`, plus a `GSC1` screen-forwarding format (block with
  length, frame number, dimensions, CRC32; payload ≤ 1024 B; max image 7027840 B) and `GIULIA SHELL
  BEGIN/END` markers around raw PTY bytes.
* Known defect, quoted: the upstream USB driver **stops after a disconnect or bus reset following
  configuration**, usually requiring a phone reboot to reconnect. That is a real reliability cost of putting
  the gadget in the kernel.
* Host tool: `scripts/debug_terminal.py` (+ `scripts/debug-terminal.cmd`, `scripts/terminal-requirements.txt`,
  `tests/terminal/`), Python ≥ 3.10, pySerial ≥ 3.5, `--vid/--pid/--usb-serial` matching, sessions under
  `out/terminal/<timestamp>/`.

This is the only UniXNU log path where a **USB stack runs on XNU**. It required a functioning USB device
controller driver, a CDC ACM function, a userland daemon, code-signing trust for that daemon, and a shell.
None of that exists on a device with no working kernel drivers.

---

## 4. Stage C — persistent log (the part that survives a reset)

This is the closest thing UniXNU has to "USB logs", and it is really **storage + a throwaway Linux USB
serial server**.

### C1. Writer: `package/BootLog.c` (104 KB, sheng repo; assembled into `kits/sheng/`)

* Enabled by boot argument **`sheng-log=1`**; the rest of the arguments are
  `sheng-autoreset=<s of log time>`, `sheng-diag-export=<s>`, and `sheng-log-reserve=<MiB>` (default
  **1024 MiB** of `userdata` kept clear at the end).
* A kernel thread named `boot_log` runs every **2000 ms** and writes:
  * block 0 — the **header**, written *last* each round so a reader never trusts a slot the header does not
    count yet. 148 bytes: magic `"SHLOG001"`, `version=4`, `header_bytes`, `boot_id` (CNTPCT at start),
    `first_lba` (absolute 4096-byte LBA on LUN 0), `window_blocks`, `msg_first/msg_blocks/msg_bytes`,
    `slot_first/slot_blocks/slot_count/slots_written`, `uptime_s/rounds/write_errors/gpt_open/gpt_last/
    state`, `tag[32]`, `export_first/export_blocks/export_bytes/export_generation`, `export_below`,
    `diag_first/diag_bytes`.
  * blocks 1..32 — `copy_msgbuf()`: the XNU `msgbuf` (`msgbufp->msg_bufc`, `msg_size`, `msg_bufx`) unwrapped
    linearly into **128 KiB**.
  * blocks 33.. — text slots of **16 blocks (64 KiB)** in a ring; each slot is the same 48×120 text the
    diagnostic page would show plus `[reset]` probe lines. `slot_count = (window_blocks-33)/16` = 509 for an
    8192-block window.
  * the **export region**, 256 MiB *below* the window (`native_ufs_export_blocks()`), holding userland blobs
    (Metal pipeline caches) and `BootDiagExport.c` file dumps; two alternating halves so the header always
    names a wholly-written blob.
* Window = **the last 32 MiB of `userdata`** (8192 × 4096-byte blocks). All of it goes through a *bounded*
  API exported by the board block driver: `native_ufs_log_open(&first,&blocks,&why)`,
  `native_ufs_log_write(lba, nblocks, buf)`, `native_ufs_log_sync()`, `native_ufs_export_blocks()`,
  `native_ufs_export_write()`, `native_ufs_store_gpt_check()`. All are `__attribute__((weak_import))`, so
  the file links without them — but with `native_ufs_log_open` absent `boot_log_tick()` returns immediately.
* Safety rails: it waits for the block driver's root layout (`why == "no root layout"` → retry every 1 s,
  anything else → state 3 = stopped); after **3 consecutive write errors** it stops (state 3); every 30
  rounds it re-reads the GPT and **stops for good if the table changed** (state 4).
* Started from `boot_log_tick(now)`, called **once a second by the board diagnostic page's monitor**. A port
  that does not keep `BootProgress` must call it from its own timer, otherwise nothing is ever written.
* `sheng-autoreset=N` → when log time reaches N and the round synced, call `boot_reset_bootloader()`, which
  writes the bootloader reboot-reason cell and issues PSCI `SYSTEM_RESET`. Independently, the *first* tick
  (regardless of `sheng-log`) marks the cell and arms the hypervisor watchdog at `autoreset+60` seconds so a
  panicked kernel still comes back.

### C2. Reader: `package/linux-log/` (sheng repo)

`build-linux-log-boot.py` (63 lines) makes a plain **Android boot image, header v0**, from:

* a prebuilt mainline kernel + appended DTB (`ianchb/sm8550-mainline` release asset
  `boot_sheng_singleboot.img`),
* a gzip `newc` cpio initramfs containing `bin/busybox`, `init`, `bin/rebootbl`, `lib/modules/*.ko`
  (`nvmem-reboot-mode.ko`),
* command line `rdinit=/init console=tty0 loglevel=4 consoleblank=0`, SHA-1 digest patched at offset 576.

`init` (busybox `sh`) is the whole USB server:

```sh
mount -t configfs configfs /sys/kernel/config
G=/sys/kernel/config/usb_gadget/log
mkdir -p $G/strings/0x409 $G/configs/c.1 $G/functions/acm.usb0
echo 0x1d6b > $G/idVendor ; echo 0x0104 > $G/idProduct
echo shenglog          > $G/strings/0x409/serialnumber
echo sheng-log-reader  > $G/strings/0x409/product
ln -s $G/functions/acm.usb0 $G/configs/c.1/
echo $UDC > $G/UDC                    # first entry of /sys/class/udc
```

then finds the partition by `PARTNAME=userdata` in `/sys/class/block/*/uevent` and does

```sh
B=$(( $(cat /sys/class/block/$P/size) / 8 ))          # 512 B sectors → 4096 B blocks
dd if=/dev/$P of=/run/log.bin bs=4096 skip=$((B - 8192)) count=8192
say "ready: shell on USB serial"
while true; do setsid sh -c "exec sh -i <$T >$T 2>&1"; sleep 1; done   # T=/dev/ttyGS0
```

Nothing is mounted or written. Block devices are read-only. The shell is respawned forever on the gadget's
`/dev/ttyGS0`.

**What the host sees:** a CDC ACM device built by the kernel's `f_acm` function — a CDC control interface
(class `0x02`, subclass `0x02`, protocol `0x01`) plus a class `0x0a` data interface; the gadget's device
class is left at the interface level because `init` writes no `bDeviceClass`. VID/PID
**`0x1d6b:0x0104`** (Linux Foundation / "Multifunction Composite Gadget"), product string
`sheng-log-reader`, serial `shenglog`. It binds `cdc_acm` → `/dev/ttyACM0` on Linux,
`/dev/cu.usbmodem*` on macOS. The baud rate is nominal — the pipe is USB bulk, so throughput is not
115200-limited; the bottleneck is the shell + base64 protocol.

### C3. Host decoder: `scripts/read-sheng-log.py` (sheng repo, 235 lines)

* `HEADER = struct.Struct('<8sIIQQ8I6I32s')` (120 bytes) then `EXPORT '<4I'`, `EXPORT3 '<I'`, `DIAG4 '<II'`
  for versions 2/3/4 — matching the C struct field for field. `BLOCK = 4096`, `LOG_BLOCKS = 8192`.
* Port: opens raw (`tty.setraw`, `CLOCAL|CREAD`), no `pyserial`; auto-discovers `/dev/cu.usbmodem*` and waits
  `--wait` seconds, or takes `--port /dev/ttyACM0`.
* Framing: sends `stty -echo`, then wraps every command in markers **computed by the remote shell**
  (`echo @@B$((40+2))@@; …; echo @@E$((40+2))@@`) so the echoed command line never contains the marker, and
  uses `@` because it is not a base64 character.
* Transfer: `gzip -c` the 32 MiB image on the device into `/run/l.gz` (tmpfs), then **64 KiB pieces**, each
  with its own `md5sum` **and** `base64`, retried up to 6 times per piece, final whole-stream MD5 check. The
  docstring records why: a single 1.7 MB base64 stream over this console "lost or gained characters … often
  enough that four whole-stream attempts all failed". **That is the practical limit of a shell-over-CDC
  transport and worth remembering.**
* Output per boot: `<tag>-<boot id>/{log.bin,header.txt,msgbuf.txt,slots.txt,latest.txt}` plus
  `export/`+`manifest.json` when the header names one. Then it sends `/bin/rebootbl`.
* `--decode log.bin` decodes an already-saved window offline; `package/linux-log/check-linux-log-qemu.py`
  runs the whole thing against a synthetic GPT region in QEMU `virt` with virtio-blk — a host-side test that
  is worth cloning for any port.

### C4. Return to bootloader

`package/BootReset.c` + `BootResetCore.h` (sheng): the reboot reason is **one byte of PMIC SDAM** written
through the SPMI PMIC arbiter, then **PSCI `SYSTEM_RESET`**. Concrete facts from the header, for anyone
porting it:

* arbiter windows `core 0xc400000`, `chnls 0xc500000`, `obsrvr 0xc440000`, `cnfg 0xc42d000`, EE 0;
* version must be in `[0x70000000, 0x80000000)`; APID count from `core+0x04 & 0x7ff`; APID map at
  `core+0x2000+4*i` with PPID in bits 19:8; owner in `cnfg+4*apid` bits 2:0;
* SDAM PPID **0x071**, reboot cell **byte 0x48**, mask `0xfe`, value `2` = bootloader, bit 0 preserved;
* command word `(op<<27)|(byte_addr<<4)|(bc&7)`, `OP_EXT_WRITEL=0`, `OP_EXT_READL=1`, `STATUS.DONE=bit0`,
  error bits 1..3, 100000 polls.

`linux-log/rebootbl.cpp` is the Linux-side twin (uses the `reboot` syscall and the board DT's reboot-mode
cell) so the reader can hand the phone back to fastboot.

---

## 5. What the device pages say about MediaTek boards

| Device | SoC | Relevant facts |
|---|---|---|
| **merlinx** (Redmi Note 9) | MT6768/MT6769Z | UEFI boots from `p33` (Android `boot`, 64 MiB); **no UART**; evidence is the screen and flashing test images to `p33`; **`fastboot boot` is not supported** — "`usb_read failed with status e00002ed`", so you must `fastboot flash boot`; eMMC/SdCard DXE time out (`EFI_NOT_FOUND`) |
| **dandelion** (Redmi 9A) | **MT6765** (closest to MT6762G) | 8 core drivers done; direct boot crashes; **"blocked by having no UART"**; plan is to port Mu-Silicium UEFI for MT6765 first; README estimates **6–12 months** |
| **malachite** (POCO X7) | MT6878 | UEFI works, UFS works, XNU boots, rootfs mounts — and **USB: not implemented**; display is "only loader text on the LK framebuffer"; bring-up method is "the loader's log monitor core and log slots" |
| **a04br3** (Benesse tablet) | MT8167 | runs iPadOS springboard with `NativeMsdc` — a from-scratch **MediaTek MSDC (eMMC) driver** exists publicly; CPU-side framebuffer rendering works |

`malachite` is the honest picture of a MediaTek XNU port mid-bring-up: no USB, no touch, no GPU, and the
only output is text on the framebuffer LK already initialised. That is the state the MT6762G is in.

Note `0xE00002ED` is IOKit's `kIOReturnExclusiveAccess`, i.e. that error string was produced by a
macOS-hosted `fastboot` when LK refused/failed the download — so treat "this LK has no `fastboot boot`" as a
**device-specific thing to test before designing around it**, not as an MTK law.

---

## 6. Prerequisite matrix

| Mechanism | Firmware changes | Kernel/kext | Reserved memory / partitions | Host side |
|---|---|---|---|---|
| UART console | `SerialDxe` in FV; UART pads wired | XNU UART driver for `serial=` | — | USB-serial adapter, `tio`/`picocom` |
| AdvancedLogDxe QR | INF in DSC+FDF, `AdvLoggerPkg`/`MsGraphicsPkg`/`QrEncoderLib`/`AdvancedLoggerAccessLib`, `PcdUniXNUDeviceModel`, `[payload] tools` | — | Advanced Logger buffer (platform-configured) | Android/iOS phone + OneSend scanner app |
| Colour bars / entry probe | — | patched kernel copy, framebuffer base/stride known | — | eyes / camera |
| Diagnostic page + panic screen | — | framebuffer mapping, `GopFramebuffer` component, `--panic-screen` patch | optional DT panic region (`embedded-panic-log-size` + `reg`) | camera |
| XNU USB CDC (giulia) | — | **USB device controller driver + CDC ACM function + `/dev` node + signed daemon** | — | `debug_terminal.py`, pySerial |
| `SHLOG001` log window | — | block driver exposing `native_ufs_log_*` + `BootLog.c` + a 1 Hz tick | **last 32 MiB of `userdata`**, 1 GiB reserve by default, 256 MiB export region below it | reader image |
| Linux reader | — | *(Linux, not XNU)*: kernel + DTB + UDC + `configfs` + `f_mass_storage`-free `acm` | — | `read-sheng-log.py` |
| Return to fastboot | — | PMIC arbiter addr + reboot cell + PSCI | — | `fastboot devices` |

---

## 7. Transfer analysis for MT6762G + LK, no UEFI, no drivers, EL1 shim

### 7.1 Hard constraints (as given, plus what the sources imply)

1. **No UEFI.** Everything in §2 is out until someone ports Mu-Silicium to MT6765 — dandelion's own estimate
   is 6–12 months of work before UniXNU even starts, and merlinx shows the intermediate state (UEFI boots,
   storage does not).
2. **LK only.** LK already initialises DRAM, eMMC and (usually) the DSI panel and backlight to draw the boot
   logo. That is a large asset: **the framebuffer is live and scanning out when your shim gets control.**
3. **No working kernel drivers.** So §4's writer cannot be `BootLog.c` as-is: it depends on
   `native_ufs_log_open/write/sync` from the board block driver. The *format* transfers; the *substrate* does
   not.
4. **No UART.** §2.1 and §3.3 are dead.
5. **EL1 shim with MMU off** is exactly the execution context of UniXNU's entry probe (§3.1). This is the
   single most reusable asset you have.
6. **USB on MT6762G**: MediaTek's MT67xx family implements USB device mode on the Inventra/MUSB core;
   mainline carries the glue as `drivers/usb/musb/mediatek.c`. **Verify in your BSP** (`MUSB_` register
   macros, `drivers/musb/`, `mtk-musb`) before committing to a shim-side gadget — I could not confirm the
   MT6765/MT6762 USB IP from a primary source without the vendor tree.
7. **Mainline Linux has no MT6762/MT6765 support.** Verified today: `arch/arm64/boot/dts/mediatek/mt6765.dtsi`
   returns 404 on `torvalds/linux` master; the directory has 192 files and the `mt67xx` entries are
   `mt6755`, `mt6779`, `mt6795`, `mt6797` — and grepping `mt6755.dtsi` and `mt6797.dtsi` for `usb` returns
   **nothing**. So UniXNU's "boot a Linux reader image" leg would require writing a DTB *and* a USB device
   driver *and* a storage driver from scratch on MT6762G. **This is the finding that most changes the plan:
   the Linux reader is not a cheap read-back path on this SoC.**

### 7.2 What maps cleanly, and what does not

| UniXNU piece | Transfers? | Why |
|---|---|---|
| Entry-probe colour bars → shim stage codes | **yes, day one** | needs only a framebuffer base/stride; MMU off is already the shim's state |
| `boot-progress` 48×120 diag page | yes, later | needs a font blitter + the LK framebuffer, not a display driver |
| Pulse bar / heartbeat counter | **yes, day one** | a counter on screen that must keep incrementing is the cheapest hang detector |
| Timed-reset as a signal (`PSCI SYSTEM_RESET` after N s) | yes | MTK supports PSCI; alternatively `WDT_SWRST` in TOPRGU |
| `SHLOG001` header + slot ring format | yes (format only) | write it from the shim into a raw region; `read-sheng-log.py --decode` already parses it |
| `native_ufs_log_*` writer | no | needs the eMMC driver; a polled PIO MSDC writer is the shim-side substitute |
| Linux reader + configfs CDC ACM | not now | no mainline DTB; would be a separate bring-up project |
| XNU CDC ACM gadget | no | no USB device stack |
| AdvancedLogDxe QR | no | no UEFI |
| `BootReset` PMIC SDAM cell | yes, after mapping | the *shape* (one scratch byte = reboot reason, then PSCI reset) is exactly MTK's model; the addresses are MT6357-class, not PM8550 |

---

## 8. Ranked recommendation

Assumptions: one engineer, no prior MTK bare-metal work, LK source and a working fastboot on the device, the
shim already loads and runs. Effort figures are ranges, not commitments.

### Rank 1 — Framebuffer text console + stage codes from the shim *(≈3–6 engineer-days)*

Zero new drivers. LK leaves DSI/panel/backlight programmed and a DRAM framebuffer scanning out, exactly as
`malachite` describes ("only loader text on the LK framebuffer"). The shim prints `SHIM <stage> <pc> <esr>
<far>` plus a 1 Hz heartbeat whose **number must keep changing**; a frozen counter is a hang, a missing line
is a crash site. Add UniXNU's colour-bar trick as a coarse binary channel that works even if the text path is
broken.

This is the only mechanism that is available today, and it is the direct analogue of what UniXNU actually
uses on its flagship device.

### Rank 2 — Reset-surviving RAM ring, read back by the shim on the next boot *(≈1–2 engineer-weeks)*

The cheapest *persistent* log with no drivers at all: reserve a few MiB of DRAM above the kernel's load
address, have the shim write a `SHLOG001`-style header + ring into it, reset, and on the next boot have the
shim **print the previous boot's tail to the framebuffer** (Rank 1) or hand it to whatever reader exists.

This is a hypothesis that must be tested first, cheaply: MTK's own `mrdump`/minidump machinery relies on
DRAM contents surviving a watchdog reset, and UniXNU's `embedded-panic-log` region relies on the same idea —
but MTK's preloader does DRAM (EMI) calibration each boot, and it *may* clobber the region you choose. The
1-day experiment is: write a magic word to a candidate PA, reset via `WDT_SWRST`/`PSCI SYSTEM_RESET`, read it
back in the shim, print `MAGIC OK`/`MAGIC LOST`. Try 2–3 candidate addresses (top of DRAM, just below the
kernel load address, and inside a region the kernel's memory map excludes).

Payoff: a full text log of the *previous* boot with no storage driver and no USB.

### Rank 3 — Raw reserved region on eMMC written by a polled MSDC writer *(≈3–5 engineer-weeks)*

Take §4's format verbatim — 4096-byte blocks, `SHLOG001` header written last, 128 KiB msgbuf, 64 KiB slots,
2 s period, 3-error stop — but write it from the shim with a **polled PIO MSDC** driver instead of
`native_ufs_log_*`. `mouseos/NativeMsdc` (from the a04br3 port) is a public MediaTek MSDC driver to read
against. Reserve the region at the **end of `userdata`** the way UniXNU does, so you cannot corrupt the root
image, and keep the GPT re-check-before-write rule.

Read-back, in increasing cost:

* **(a) Android/adb**, if the phone still boots a rooted Android: `dd` the region out. Hours of work.
* **(b) MTK BROM/preloader download mode** (mtkclient / SP Flash Tool readback): the BROM speaks USB with no
  device-side software at all, so it can dump a partition or a raw range with the phone held in BROM. Needs a
  supported loader/DA and a key-combo or test point; verify before relying on it.
* **(c) A Linux reader image** as in §4.2 — only if someone writes a MT6762G DTB + MUSB + storage, which per
  §7.1(7) is its own multi-month project. Do not put this on the critical path.

### Rank 4 — USB CDC ACM gadget inside the shim *(≈6–12 engineer-weeks, high risk)*

If you genuinely want logs over USB with no OS, the shim has to own the controller: MTK USB PHY/clock
(generally `USB` + `USB_L1`/`USB2` PHY register blocks, `U2PHY`/`U3PHY` in the vendor DTS), MUSB core init,
endpoint 0 control transfers, `GET_DESCRIPTOR`/`SET_CONFIGURATION`, then a CDC ACM function with two bulk
endpoints, then a TX ring the rest of the shim can print into. Also needs VBUS/charger detection so you know
a host is attached. Enumerating at all is the milestone; a TX-only ACM is the realistic scope (no shell, no
command channel). This is a genuinely large piece of bare-metal USB work and it competes with Rank 3 for the
same weeks, while producing *less* log capacity.

### Rank 5 — UEFI port + reuse AdvancedLogDxe and the QR exporter *(≈6–12 months)*

Buys you the *entire* UniXNU firmware toolbox — QR log export, GOP menu, `fastboot boot` payloads,
`boot-progress`, panic screen — but it is the dandelion estimate and it must come before anything else works.
Worth doing eventually, wrong to do first.

### Rank 6 — XNU-side CDC ACM like giulia *(months, after the kernel runs)*

Correct long-term answer (it is what makes giulia usable day to day), but it needs a USB device stack in
XNU, a `/dev` node, a signed daemon and a shell, and it inherits the known "driver stops after a bus reset"
defect. Not a bring-up tool.

### Explicitly not recommended

* **UART**: not routed on this board; nothing to be done in software.
* **pstore/ramoops on the XNU side**: interesting and eventually right (UniXNU already ports Apple's
  `embedded-panic-log` DT contract), but it requires a working kernel, a DT reservation the kernel honours,
  and a read-back path — i.e. Ranks 2+3 must exist first anyway. Implement Rank 2 so that it can *become* the
  ramoops region later.

---

## 9. The specific first implementation step

**Make the shim a framebuffer console and prove it on the current boot path. Two days.**

1. **Recover the framebuffer geometry once.** Three ways, in order of preference: (a) if your LK passes a
   DT/`bootargs` framebuffer node, read it; (b) read the display controller's OVL/RDMA source-address
   register for the boot-logo layer out of the MTK `mmsys`/`disp` block (base from your DTS) and take
   width/height/format from the panel's `lcm` parameters; (c) instrument LK's `video_printf`/`mtk_disp` path
   once and print the pointer. Then hardcode `{base, width, height, stride, bpp=32}` in the shim.
2. **Write the blitter** — 8×16 font (reuse `SiliconPkg/Library/FrameBufferSerialPortLib/Font.h` from
   Mu-Silicium, the same font UniXNU's panic screen uses), `volatile` 32 bpp stores, `dsb sy` after each
   line. No caching concerns because the MMU is off.
3. **Print, every second, on the top line:**
   `SHIM t=<cntpct/cntfrq> pc=<addr> el=<n> sp=<sp>` and on the second line the last stage marker
   `STAGE=<n> NAME=<...>`. Read the counter from `cntpct_el0`/`cntfrq_el0` (MTK timebase 19.2 MHz — the same
   constant UniXNU bakes into `patch-xnu-entry-probe.py`); do not trust a delay loop.
4. **Add the colour-bar channel** in the top 1/16 of the screen, exactly as §3.1: bar index = stage, one
   distinct 32 bpp colour per stage (white = shim entered, blue = MMU-off code reached, magenta = about to
   jump to the kernel). This survives a broken font path and is photographable at a distance.
5. **Add a controlled reset** so a boot is a self-contained experiment: after N seconds, or when the stage
   reaches a chosen value, issue **PSCI `SYSTEM_RESET`** (or `WDT_SWRST` in TOPRGU if PSCI is not wired yet).
   "The phone rebooted by itself after N s" then answers one question per boot, which is precisely how
   UniXNU's entry-probe stage 3 works.
6. **Immediately define the log record format** you will reuse for Ranks 2 and 3 — copy §4.1's `SHLOG001`
   header field-for-field (magic/version/`boot_id` from CNTPCT/`first_lba`/`window_blocks`/`msg_first`/
   `msg_blocks`/`msg_bytes`/`slot_first`/`slot_blocks`/`slot_count`/`slots_written`/`uptime_s`/`rounds`/
   `write_errors`/`state`/`tag[32]`), header-written-last, 4096-byte blocks. Then `read-sheng-log.py
   --decode` already works on your files with zero changes, and `check-linux-log-qemu.py` is a ready-made
   host-side test harness for the reader leg.

Only after that: run the Rank 2 magic-word survival experiment, then start the polled MSDC writer.

---

## 10. Open questions to resolve before committing to Rank 3/4

1. Does this LK implement `fastboot boot`? (merlinx's MT6768 does not.) If it does, a throwaway reader image
   becomes possible for *debugging* even without mainline support.
2. Which DRAM ranges does the preloader/EMI calibration preserve across `WDT_SWRST` and across
   `PSCI SYSTEM_RESET`? (Rank 2 experiment.)
3. Which USB IP is it — MUSB or DWC3 — and is a UDC/PHY already initialised by LK (which would let even a
   kernel-mode gadget start from a live controller)?
4. Is there an unused or oversized `expdb`-class partition in the GPT that could serve as the reserved log
   region instead of the tail of `userdata`? (Verify the actual GPT; do not assume.)
5. Does the device expose BROM/preloader download mode (mtkclient-compatible), giving a driver-free read-back
   path for Rank 3?
6. Which PMIC/RTC scratch word does LK read as "boot into fastboot"? Port `BootResetCore.h` to it rather than
   inventing one.

---

## 11. Sources

Documentation (all fetched 2026-10-27):

* https://unixnu.com/zh/devices/ — device index (SoC + repo per device)
* https://unixnu.com/zh/porting/log-capture — four-stage capture procedure, QR export, persistent log, manual/automated cycle
* https://unixnu.com/zh/guide/debugging — log sources table, `serial=3`, diagnostic page, pulse bar, colour bars, entry probe, panic screen, `scripts/debug_terminal.py`
* https://unixnu.com/zh/porting/uefi-firmware — required services (incl. `EFI_SERIAL_IO_PROTOCOL`), DSC/FDF integration, `PcdUniXNUDeviceModel`
* https://unixnu.com/zh/devices/sheng-unattended-test — reader image inputs, `boot_sheng_singleboot.img`, `rebootbl`, `read-sheng-log.py` usage, 32 MiB `userdata` window, CDC shell
* https://unixnu.com/zh/devices/giulia — "USB CDC 日志可用"
* https://unixnu.com/zh/devices/dandelion — MT6765, no UART, 6–12 month UEFI estimate
* https://unixnu.com/zh/devices/merlinx — MT6768, no UART, `fastboot boot` refused (`e00002ed`), p33 boot partition
* https://raw.githubusercontent.com/UniXNU/UniXNU/main/docs/zh/devices/malachite.md — MT6878: UEFI+UFS+XNU ok, **USB not implemented**, LK-framebuffer text only
* https://raw.githubusercontent.com/UniXNU/UniXNU/main/docs/zh/devices/a04br3.md — MT8167, `NativeMsdc` eMMC driver
* https://raw.githubusercontent.com/UniXNU/UniXNU/main/docs/concepts/userdata.md — `userdata` window manifest, `sheng-log-reserve` (1024 MiB default), data-persist keys

Source code:

* https://github.com/UniXNU/UniXNU — `package/UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.{c,inf}`; `scripts/debug_terminal.py`, `scripts/debug-terminal.cmd`, `tests/terminal/`
* https://github.com/UniXNU/UniXNU-sheng — `package/BootLog.c`, `package/BootReset.c`, `package/BootResetCore.h`, `package/linux-log/{init,build-linux-log-boot.py,rebootbl.cpp,check-linux-log-qemu.py}`, `scripts/read-sheng-log.py`, `scripts/run-sheng-unattended.sh`, `scripts/patch-xnu-entry-probe.py`
* https://github.com/tycykp/UniXNU-OnePlus-Ace5/blob/main/docs/USB_TERMINAL.md — XNU CDC ACM protocol 2, `/dev/giulia-debug`, `giulia-debugd`, GSC1 frames, `reboot.fastboot`
* https://github.com/torvalds/linux/tree/master/drivers/usb/musb — `mediatek.c` (MediaTek MUSB glue)
* https://github.com/torvalds/linux/tree/master/arch/arm64/boot/dts/mediatek — 192 files; no `mt6765.dtsi` (404), and `mt6755.dtsi`/`mt6797.dtsi` declare no USB node

---

### Appendix — files downloaded for inspection

`D:\aios\research\_sources\` holds local copies of the sources quoted above (`BootLog.c`,
`read-sheng-log.py`, `linux-log/{init,build-linux-log-boot.py}`, `BootReset.c`, `BootResetCore.h`,
`patch-xnu-entry-probe.py`, `AdvancedLogDxe.{c,inf}`, and the MediaTek device pages). Scratch material, not
a deliverable.
