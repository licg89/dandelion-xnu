# TOUCH-PLAN — Novatek NT36525B bring-up on dandelion (MT6765)

Written 2026-11-01. Status: **plan + skeleton only**. Nothing here has been
executed on the phone; the code that goes with it is
`drivers/dandelion_touch.c` and it is untested by construction.

Tags as in `docs/DISPLAY-PLAN.md`: **[M]** measured on this phone and recorded in
this repository, **[V]** verified in a source tree checked out locally, **[D]**
derived, **[U]** unknown with the command that would read it.

---

## 0. Two corrections to what this repository said before

`docs/UEFI-DANDELION-PLAN.md` §2 has a row that reads "触控 | Novatek, I²C3
`0x1100F000` | 设备 DTB", and `docs/PROGRESS.md` names the controller
"NT36532E". Both are wrong in ways that would waste a session:

**(1) The touch controller is on SPI3, not I2C3.**

[V] Vendor kernel `dandelion-q-oss` @ `8974b91`, `arch/arm64/boot/dts/mediatek/
touchscreen.dtsi`, included by `dandelion.dts`:

```
&spi3 {
	touch@0 {
		compatible = "focaltech,fts","novatek,NVT-ts-spi","mediatek,himax_touch";
		reg = <0>;
		spi-max-frequency = <9600000>;
		interrupt-parent = <&pio>;
		interrupts = <13 0x02>;
		focaltech,reset-gpio = <&pio 174 0x01>;
		focaltech,irq-gpio   = <&pio 0 0x02>;
		novatek,reset-gpio   = <&pio 174 0x00>;
		novatek,irq-gpio     = <&pio 0 0x2001>;
		novatek,swrst-n8-addr    = <0x03F0FE>;
		novatek,spi-rd-fast-addr = <0x03F310>;
	};
};
```

[V] and the same tree's `dandelion_defconfig` selects the SPI driver, not an I2C
one: `CONFIG_TOUCHSCREEN_NT36XXX_HOSTDL_SPI=y`. The driver directory that gets
built is `drivers/input/touchscreen/mediatek/nt36xxx_spi/`, whose `.h` says
`NVT_SPI_NAME "NVT-ts"` — which is exactly the `[NVT-ts]` tag in the phone's
dmesg.

I2C3 at `0x1100f000` does exist on this device; it carries the external buck
regulators (`ext_buck_lp4@57`, `ext_buck_lp4x@50`), not touch.

**(2) The controller is an NT36525B, not an NT36532E.**

[V] The same dtsi annotates the node "`[IC Vendor]NOVATEK [IC Name]Nt36525b`",
and the touch driver's own firmware-info printer formats
`"[Vendor]…,[FW]0x%02x,[IC]nt36525b"`. The NT36525B is a TDDI part: the display
driver IC and the touch controller are the same die, which is why the panel
driver and the touch driver are both "nt36525b".

NT36532E is what `UniXNU/ShengNovatekTouch` targets. Same family, **different
memory map and different report format** — see §5.

The one thing the old notes got right: the controller is Novatek, and
`NVTCapacitiveTouchScreen` in `getevent -pl` is the driver's
`NVT_TS_NAME` string [V], so the Android-side evidence in
`docs/PROGRESS.md` §3 is real. It is just not stored as a raw log anywhere in
`docs/logs/` — the only copy of that reading is the sentence in `PROGRESS.md`.

## 1. Bus and pins

| item | value | source |
| --- | --- | --- |
| controller | SPI3 @ **`0x11013000`**, 4 KiB | [V] `mt6765.dts` `spi3: spi@11013000` |
| SPI3 interrupt | GIC SPI **129**, level-low | [V] same node |
| SPI mode | 0, 8 bits per word, full duplex, CS held for the whole transfer | [V] `nt36xxx.c` `nvt_ts_probe()`: `bits_per_word = 8`, `mode = SPI_MODE_0`, and it *rejects* a half-duplex master |
| clock | **9.6 MHz** max | [V] dtsi `spi-max-frequency = <9600000>` |
| chip select | `reg = <0>`, i.e. spi3.0 | [V] dtsi |
| reset | **GPIO 174**, active low (`novatek,reset-gpio`) | [V] dtsi; also `NVTTOUCH_RST_PIN 174` in the driver |
| interrupt pin | **GPIO 0** per `novatek,irq-gpio`; the driver does `client->irq = gpio_to_irq(ts->irq_gpio)` | [V] dtsi + `nt36xxx.c` |
| interrupt trigger | `IRQ_TYPE_EDGE_RISING` | [V] `nt36xxx.h` `INT_TRIGGER_TYPE` |
| reported resolution | 720 × 1600, 10 fingers, no keys | [V] `nt36xxx.h` `TOUCH_DEFAULT_MAX_*`, `TOUCH_MAX_FINGER_NUM`, `TOUCH_KEY_NUM 0` |
| **disagreement** | the dtsi also has `interrupts = <13 0x02>` (EINT 13, falling), while the Novatek properties point at GPIO 0 and the driver ignores `interrupts` entirely | [V] — see U1 |

### Power: touch is not independent of the display

[V] `nt36xxx.h`, the regulator fields of `struct nvt_ts_data`:

```c
struct regulator *pwr_vdd; /* IOVCC 1.8V */
struct regulator *pwr_lab; /* VSP +5V */
struct regulator *pwr_ibb; /* VSN -5V */
```

Those are the **panel's** supplies (the same TPS65132 bias rails and 1.8 V rail
that `docs/DISPLAY-PLAN.md` §2.4 lists). On a TDDI part this is expected — one
die, one supply set — and it has a hard consequence for the ladder: **touch
cannot be probed until the display bias is on.** Whoever brings up touch before
display will spend a session debugging a dead SPI bus.

It also has a reverse consequence: the display's own reset sequence (GPIO 45,
driven low twice, `docs/DISPLAY-PLAN.md` §2.4) may reset the touch section of the
die. That interaction is U4.

## 2. The protocol

[V] Novatek "host download" SPI protocol, as implemented in
`drivers/input/touchscreen/mediatek/nt36xxx_spi/`. It is a register window, not a
HID device:

- A register address is selected in two steps: send `0xff` followed by
  `(addr >> 15) & 0xff` and `(addr >> 7) & 0xff` (`nvt_set_page`), then address
  `addr & 0x7f` **inside** that 128-byte page.
- The first byte of a transfer is the index; its **top bit selects the
  direction**: `| 0x80` for a write, `& 0x7f` for a read (`SPI_WRITE_MASK` /
  `SPI_READ_MASK`).
- A read transfer is one byte longer than the data (`DUMMY_BYTES 1`), and the
  payload comes back starting at `rx[2]` — one index byte, one dummy byte
  (`CTP_SPI_READ`).
- Reads are retried up to five times before being reported as `-EIO`.

Addresses that matter, from the dtsi and from `nt36xxx.c` / `nt36xxx_mem_map.h`:

| name | address | what |
| --- | --- | --- |
| `SWRST_N8_ADDR` | **`0x03f0fe`** | software reset: `0x69` = bootloader reset, `0x55` = MCU reset, `0xaa` = MCU idle |
| `SPI_RD_FAST_ADDR` | **`0x03f310`** | fast-read mode; the bootloader reset clears it to 0 |
| `ENG_RST_ADDR` | `0x7fff80` | engineering reset, write `0x5a` |
| trim-ID window | `0x1f64e` | 7 bytes; `0x25 0x65 0x03` at bytes 4..6 = NT36525B |
| `EVENT_BUF_ADDR` | **`0x11a00`** | event buffer for the NT36525/NT36525B map |
| `EVENT_MAP_HOST_CMD` | `+0x50` | host command byte |
| `EVENT_MAP_HANDSHAKING` | `+0x51` | firmware handshake / sub-command |
| `EVENT_MAP_RESET_COMPLETE` | `+0x60` | reset-state machine byte |
| `EVENT_MAP_FWINFO` | `+0x78` | 17-byte firmware info block |
| `EVENT_MAP_PROJECTID` | `+0x9a` | project ID |

The reset-state machine the firmware publishes at `+0x60`:
`0xa0` init → `0xa1` baseline → `0xa2` baseline ready → `0xa3` normal run,
valid up to `0xaf`. The vendor driver polls it every 10 ms (10 polls for init,
50 afterwards).

**Good news, and it is worth stating because it changes the effort estimate:**
no firmware download is needed for normal operation. The vendor driver only runs
`nvt_update_firmware()` from the watchdog-recovery path (`NVT_TOUCH_WDT_RECOVERY`),
and a normal boot is: reset → read trim ID → read fw info → wait for normal run →
read reports. UniXNU's NT36532E driver, by contrast, implements a full host
download (ILM/DLM, CRC banks, boot-ready handshake) because that chip needs it.
We do not, as far as the vendor driver's own behaviour shows.

## 3. Reset and bring-up order

[V] `nt36xxx.c`, `nvt_ts_probe()` and the reset helpers, in the order the vendor
driver does it:

1. regulators: get them, then **disable** them by default (`nvt_ts_enable_regulator(false)`)
2. `gpio_request_one(reset, GPIOF_OUT_INIT_LOW, "NVT-tp-rst")`, `gpio_request_one(irq, GPIOF_IN, "NVT-int")`
3. `nvt_eng_reset()` — write `0x5a` to `0x7fff80`, wait 1 ms
4. `msleep(10)` — "need 10 ms delay after POR"
5. `nvt_ts_check_chip_ver_trim()` — up to five times: bootloader reset (`0x69` to
   `0x03f0fe`, 5 ms, clear `0x03f310`), then read 7 bytes at `0x1f64e`
6. `nvt_get_fw_info()` — 17 bytes at `EVENT_BUF + 0x78`; check
   `fw_ver + ~fw_ver == 0xff`, else retry three times and fall back to 720×1600
7. `nvt_read_pid()` — 2 bytes at `EVENT_BUF + 0x9a`
8. `nvt_boot_ready()` / CRC enable / `nvt_check_fw_reset_state(NORMAL_RUN)`
9. register the input device, then `client->irq = gpio_to_irq(irq_gpio)`

Note what is **absent**: the vendor driver does not drive the reset pin at probe
time at all (`NVT_TOUCH_SUPPORT_HW_RST 0`); it relies on the power-on reset and
only raises the line in `nvt_ts_resume()`. Our skeleton offers
`dandelion_nvt_hw_reset()` anyway, clearly marked as "not what the vendor driver
does", for the case where the controller is found in an unknown state after the
display's own reset.

## 4. The report

[V] `nvt_ts_work_func()`. One interrupt = one 66-byte read (65 bytes of payload
plus the index byte) from index 0 of the event-buffer page. Ten finger records
start at offset 1, six bytes each:

| byte | meaning |
| --- | --- |
| `+0` | bits 7..3 = finger id (1..10), bits 2..0 = status: 1 = enter or moving, 2 = moving |
| `+1` | X bits 11..4 |
| `+2` | Y bits 11..4 |
| `+3` | bits 7..4 = X bits 3..0, bits 3..0 = Y bits 3..0 |
| `+4` | contact area, 0..255 (0 reported as 1) |
| `+5` | pressure low byte; for the first two fingers the high byte is at `raw[63]` and `raw[64]` |

A record with id 0, id > 10, or status other than 1/2 is not a finger.
Coordinates outside the panel are dropped. Pressure is clamped to 1000
(`TOUCH_FORCE_NUM`).

Decoding this is the whole of the coordinate work — there is **no centroid
tracking, no baseline, no DSP** on this path. That is the single biggest
difference from the NT36532E reference in §5.

## 5. What UniXNU's driver gives us, and what it does not

`UniXNU/ShengNovatekTouch` (Apache-2.0; it is a submodule of `UniXNU/UniXNU-sheng`
at `drivers/ShengNovatekTouch`) is the closest public relative of this chip. Its
own header states the scope: *"NT36532E host-download protocol over an abstract
SPI transport. Register facts follow the study of the Linux nt36532e driver (no
code copied) … No board addresses or clocks here."*

**Borrowed (structure only):**

- The layering: a protocol module over an abstract transport
  (`struct nvt_bus { xfer, delay_ms }` in `NvtProtocol.h`), with all board
  addresses supplied by the caller. Our `struct dandelion_nvt_bus` is the same
  idea, with `reset_pin` and `irq_level` added because we need the GPIO path.
- The idea of selecting a memory map from the trim ID
  (`nvt_identify()` → `struct nvt_map`), and of keeping the map's fields named
  after the register function rather than raw numbers.
- Its diagnostic style for a failed download (stage counter, last wait value,
  CRC read-back) — worth copying if we ever do need a firmware download.

**Not borrowed (does not apply):**

- Its memory map. NT36532E's `EVENT_BUF_ADDR` is not `0x11a00`; the NT36525B map
  is `nt36xxx_mem_map.h`'s `NT36525_memory_map`, and the trim table row is
  `{0xff,0xff,0xff,0x25,0x65,0x03}` with mask `{0,0,0,1,1,1}`.
- Its data path. `NvtTouchCore.h` reads the raw **40 × 60 int16 mutual-capacitance
  matrix** per frame (a "THP" frame of 257 + 5160 bytes) and does its own
  baseline, delta, peak finding, centroid, palm rejection and slot tracking in
  software, at 120 Hz, with an adaptive low-pass filter and 12 slots. On the
  NT36525B the firmware already does all of that and hands us ten decoded finger
  records. Copying the matrix path here would be strictly more work and worse.
  (It is, however, a very good reference for what "good touch feel" requires if
  we ever have to do it ourselves.)
- Its firmware download, for the reason in §2.

## 6. How a touch event reaches XNU

`IOHIDFamily` **is** open source — `apple-oss-distributions/IOHIDFamily` exists,
unlike `IOMobileFramebuffer` (404). So the standard route is available to us. What
a driver has to do, in the order the family expects:

1. **A transport driver** that owns the hardware: maps SPI3 (`ml_io_map()`), takes
   the EINT as an interrupt (or polls, which is fine for a first test), and
   implements register access. That is the whole of §2 and §3.
2. **Report → event translation.** Two shapes are possible:
   - *HID device:* publish an `IOHIDDevice` with a synthesized report descriptor
     (digitizer page, multi-touch) and let `IOHIDEventDriver` do the translation.
     Cheaper to write, but it means inventing a report descriptor for a device
     that is not HID — the mapping is ours either way.
   - *Event service:* subclass `IOHIDEventService`/`IOHIDEventDriver` and dispatch
     `IOHIDEvent`s directly — a digitizer event with a finger child event per
     contact, carrying the transducer type (finger), the X/Y ranges (0..719,
     0..1599), pressure and the per-finger id. This is what an embedded touch
     driver normally does, and it is what our decoded report maps onto naturally.
3. **Interrupt delivery.** The EINT must become a GIC interrupt. On this port that
   means the GPIO/EINT block at `0x10005000` plus the GIC SPI for the EINT; the
   GIC side of the port is already working (`docs/PROGRESS.md`: Group 1 enabled,
   timer PPI delivered), but no EINT has ever been wired up here — **[U]**, see U1.
4. **Above that, nothing.** iOS's HID event system (`IOHIDEventSystemClient`,
   backboardd) is user space and consumes the events without further kext work.

The alternative, and the one HakiOS actually took: **a BSD character device.**
[V] `hakios-mt6768`, `kernel/README.md` lists their device drivers and says they
hook into the common kernel's "input device `/dev/redwood-touch`". So their touch
never enters IOHIDFamily at all — it is a `/dev` node that something in user
space reads. That is much less work and much less ABI risk, but it means the
consumer has to be ours (or patched), not iOS's own event system.

For dandelion, the honest recommendation is: **do the BSD device first** (ladder
T4), because it can be tested without user space at all, and treat the
IOHIDFamily driver (T5) as the step that only becomes verifiable once
`load_init_program()` works — which is blocked on the memory-corruption bug, not
on touch.

## 7. The ladder

### T0 — Read-only: does the chip answer?

Map SPI3, select the trim-ID page, read 7 bytes. No resets, no writes.

**Acceptance:** the log shows bytes 4..6 = `0x25 0x65 0x03`. Anything else means
either the bus or the pin mux is wrong, or the controller is held in reset — and
then the answer is the pin mux (`0x10005000`), not the protocol.

### T1 — Reset and identify from scratch

Engineering reset, 10 ms, bootloader reset, trim ID, firmware info, wait for
normal run (§3 steps 3–8).

**Acceptance:** `fw_ver + ~fw_ver == 0xff`, `abs_x_max == 720`, `abs_y_max == 1600`,
`sensor counts` plausible (the vendor fallback is 18 × 32), and a non-zero project
ID. This is the step that proves we can bring the controller up without Android.

### T2 — One report, decoded

Read 66 bytes and decode. Touch a known corner.

**Acceptance:** a finger in the top-left reports `x < 50, y < 50`; the
bottom-right reports near `719, 1599`; lifting the finger yields
`DANDELION_TOUCH_ERR_EMPTY` (no record with status 1/2). Orientation matters:
if the axes are swapped, the report is still correct and the *mapping* is wrong —
fix it in one place, not in the decoder.

### T3 — Interrupt driven

Take the EINT, count interrupts.

**Acceptance:** zero interrupts over ten seconds with nothing touching the panel,
and a monotonic count while dragging. This is also the step that settles U1: log
which pin asserted.

### T4 — A BSD device node

Publish `/dev/dandelion-touch` with a fixed-size record per report (count, then
`{id, status, x, y, weight, pressure}` per finger), read from a kernel thread on
the interrupt.

**Acceptance:** a reader gets one record per touch change, and the coordinates
match T2's.

### T5 — IOHIDFamily

An `IOHIDEventService`/`IOHIDEventDriver` subclass publishing digitizer events.

**Acceptance (kernel-side only, until user space runs):** the service appears in
`ioreg -l`, publishes the right X/Y ranges and transducer type, and its event
callback fires on a touch. **Final acceptance needs user space**, and is therefore
downstream of the `devfs_kernel_mount` bug.

### T6 — Behaviour

Debounce, palm rejection, and the mapping to the panel's 720×1600 portrait
coordinates including any rotation. Note that the firmware already does
significant filtering (§4 has no software filtering at all), so this step is about
matching Android's feel, not about making touch usable.

## 8. Unknowns

| # | unknown | why it matters | how to read it |
| --- | --- | --- | --- |
| U1 | which pin actually asserts: GPIO 0 (`novatek,irq-gpio`) or EINT 13 (`interrupts`) | without it there is no interrupt path, only polling | `adb shell cat /proc/interrupts \| grep -i -e nvt -e touch -e 13:` while touching the panel; the vendor driver's probe also prints `novatek,irq-gpio=…` — `adb shell dmesg \| grep -i "NVT-ts"` |
| U2 | whether the controller survives the display's own reset (GPIO 45) | TDDI: the display's init may reset the touch section, so ordering matters | do a T0 read immediately after a display reset and compare with a T0 read before it |
| U3 | the firmware's report rate and whether it scans without a host command | a controller that never reports looks identical to a broken bus | count T3 interrupts per second; if zero, read `EVENT_BUF + 0x50` (host command) back |
| U4 | the exact 1.8 V / bias rail sequencing for touch vs display | §1: touch shares the panel's supplies | `adb shell dmesg \| grep -i -e regulator -e lcm` at boot; the vendor driver logs its regulator enables |
| U5 | whether a firmware download is ever needed on this unit | it changes the effort from days to weeks | the vendor driver logs a firmware update only on WDT recovery: `adb shell dmesg \| grep -i "fw update\|update_firmware"` — if the phone never logs it, we never need it either |
| U6 | the panel module variant (see `docs/DISPLAY-PLAN.md` U1) | touch and display are one die, so the variant question is shared | same command as DISPLAY-PLAN U1 |

## 9. References

- `docs/DISPLAY-PLAN.md` — the panel side of the same die, and the shared power rails
- `docs/PROGRESS.md` §3 — the only record of the phone's `getevent -pl` and `[NVT-ts]` reading
- `docs/UEFI-DANDELION-PLAN.md` §2 — the row this document corrects
- `drivers/dandelion_touch.c`, `drivers/dandelion_touch.h` — the skeleton this plan describes
- Xiaomi vendor kernel `MiCode/Xiaomi_Kernel_OpenSource`, branch `dandelion-q-oss` @ `8974b91`: `arch/arm64/boot/dts/mediatek/touchscreen.dtsi`, `mt6765.dts` (`spi3`), `arch/arm64/configs/dandelion_defconfig`, and `drivers/input/touchscreen/mediatek/nt36xxx_spi/` (`nt36xxx.c`, `nt36xxx.h`, `nt36xxx_mem_map.h`) — GPL-2.0, read for facts, no code copied
- [UniXNU/ShengNovatekTouch](https://github.com/UniXNU/ShengNovatekTouch) (Apache-2.0) — NT36532E, `NvtProtocol.h`, `NvtTouchCore.h`, `README.md`
- [oosexgang/hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) — `kernel/README.md`: touch as `/dev/redwood-touch`, OmniVision TD4160 over SPI, firmware sent by the loader
- [apple-oss-distributions/IOHIDFamily](https://github.com/apple-oss-distributions/IOHIDFamily) — published; this is the family a real driver plugs into
