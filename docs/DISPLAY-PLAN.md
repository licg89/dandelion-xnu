# DISPLAY-PLAN — DSI0 bring-up on dandelion (MT6765)

Written 2026-11-01. Status: **plan + skeleton only**. Nothing described here has
been executed on the phone; the code that goes with it is
`drivers/dandelion_display.c` and it is untested by construction.

This document answers three questions in order:

1. What do we already know about this panel, and where was each number measured?
2. What does XNU actually need for a usable framebuffer console — and what of
   that is impossible without Apple code?
3. What is the ladder from "the kernel can program DSI0" to "XNU's console
   renders into the framebuffer", with an acceptance check at every step?

Every register address, GPIO and timing value below is tagged:

- **[M]** measured on this phone and recorded in this repository
- **[V]** verified — read in a source tree that is checked out locally
- **[D]** derived — computed from quoted code, not observed on silicon
- **[U]** unknown — with the command that would read it

---

## 1. Where the port stands

The panel is **already lit** before XNU runs. LK programs DSI0, starts video mode
and leaves the panel scanning out of a DRAM buffer; the boot shim, the UEFI
console and the XNU console all just paint into that buffer. This is the single
most important fact in this document, because it changes what "bring-up" means:

| evidence | what it proves |
| --- | --- |
| `docs/NEXT-M3-GOP.md`, "Photo evidence: console reaches the panel": the panel shows BDS output, and a 5× scaled diagnostic drawn straight at `0x7EC50000` with a 2944-byte stride is *crisp*, while the GOP's own 720-pixel writes are sheared by 16 px per row | the panel really does scan out `0x7EC50000` with a 736-pixel pitch, programmed by LK, not by us |
| `docs/HARDWARE_ANALYSIS.md` §Framebuffer: `disphal_allocate_fb pa_start=0x7ec50000`, `stride → 2944`, `virtual_size → 736,4800` | same, from the Android side, with the buffer size |
| `bootshim/shim.c` `fb_probe()`: paints `0x7ec50000` for `0xfb0000` bytes and stops at the TEE boundary | the reserved window and its size, and that a non-secure write past it faults |
| `patches/xnu/dandelion-early-trace.patch`: `dandelion_fb_putc()` writes glyphs at `DANDELION_FB_PA + y*DANDELION_FB_STRIDE + x*4` | the kernel console already renders into that buffer |

So the ladder below is **not** "get the first pixels out". It is "take ownership
of a display that someone else switched on, without breaking it".

## 2. Hardware facts and their provenance

### 2.1 Addresses

| block | physical base | source |
| --- | --- | --- |
| `dispsys` / `mmsys_config` | `0x14000000` | `docs/UEFI-DANDELION-PLAN.md` §2 [M]; device DTS `mt6765.dts` [V] |
| `disp_mutex0` | `0x14001000` | `mt6765.dts` `disp_mutex0@14001000` [V] |
| `disp_ovl0` / `ovl0_2l` | `0x1400b000` / `0x1400c000` | `mt6765.dts` [V] |
| `disp_rdma0` | `0x1400d000` | `mt6765.dts` [V] |
| `disp_color0` / `ccorr0` / `aal0` / `gamma0` / `dither0` | `0x1400f000` … `0x14013000` | `mt6765.dts` [V] |
| **`dsi0`** | **`0x14014000`**, 4 KiB, GIC SPI **225**, level-low | `mt6765.dts` `dsi0@14014000` [V]; `docs/HARDWARE_ANALYSIS.md` [M]; LK `mt_reg_base.h` `DSI0_BASE` [V] |
| `mipi_tx0` (DSI PHY + PLL) | `0x11c80000` | `mt6765.dts` `mipi_tx0@11c80000` [V]; LK `mt_reg_base.h` `MIPI_TX0_BASE` [V] |
| `disp_pwm0` (backlight) | `0x1100e000` | `mt6765.dts` [V]; LK `mt_reg_base.h` [V] |
| `i2c6` (LCD bias regulator) | `0x1100d000`, GIC SPI 131 | `mt6765.dts` [V] |
| framebuffer | `0x7ec50000`, `0xfb0000` bytes, stride 2944, 736×1600 scanout, 720×1600 visible | [M], see §1 |

### 2.2 Which panel IC

The panel driver IC is a **Novatek NT36525B** — the same die that provides touch,
i.e. a TDDI part. This is not an inference from a marketing name: the vendor
kernel's own device configuration lists the panel drivers that are compiled in.

[V] `dandelion_defconfig` (Xiaomi kernel, `dandelion-q-oss` @ `8974b91`):

```
CONFIG_CUSTOM_KERNEL_LCM="nt36525b_vdo_hdp_boe_dijing ft8006s_vdo_hdp_boe_helitai \
                          hx83102d_vdo_hdp_boe_xinli nt36525b_vdo_hdp_boe_helitai \
                          nt36525b_vdo_hdp_panda_shengchao"
CONFIG_LCM_WIDTH="720"
CONFIG_LCM_HEIGHT="1600"
```

Five candidate panels, three of them NT36525B (BOE modules from three different
module houses: dijing, helitai, panda-shengchao), plus an FT8006S and an
HX83102D. The kernel picks between them at runtime with `lcm_compare_id()`.

**This is the first thing to settle, and it is cheap** — see §6, unknown U1.

### 2.3 Panel timing

[V] Vendor kernel `nt36525b_vdo_hdp_boe_helitai.c`, `lcm_get_params()`, verbatim:

| parameter | value | register it lands in |
| --- | --- | --- |
| mode | `SYNC_PULSE_VDO_MODE` (video, not command) | `DSI_MODE_CON.MODE_CON` |
| lanes | `LCM_FOUR_LANE` | `DSI_TXRX_CON.LANE_NUM` |
| format | `LCM_DSI_FORMAT_RGB888`, colour order RGB, MSB first, padding on LSB | `DSI_PSCON` |
| `vertical_sync_active` | 2 | `DSI_VSA_NL` (0x020) |
| `vertical_backporch` | 8 | `DSI_VBP_NL` (0x024) |
| `vertical_frontporch` | 10 | `DSI_VFP_NL` (0x028) |
| `vertical_active_line` | 1600 | `DSI_VACT_NL` (0x02c) |
| `horizontal_sync_active` | 4 | `DSI_HSA_WC` (0x050) |
| `horizontal_backporch` | 85 | `DSI_HBP_WC` (0x054) |
| `horizontal_frontporch` | 85 | `DSI_HFP_WC` (0x058) |
| `horizontal_active_pixel` | 720 | `DSI_SIZE_CON` / byte counts |
| `PLL_CLOCK` | 277 (MHz) | `MIPITX_PLL_CON0/1` |
| `ssc_disable` | 1 | `MIPITX_PLL_CON2/3` |
| `clk_lp_per_line_enable` | 0 | `DSI_HSTX_CKLP_WC` (0x064) |
| `packet_size` | 256 | `DSI_PSCON.DSI_PS_WC` |
| physical size | 68040 × 151200 µm, density 320 | ADT property, not a register |

Note the arithmetic this implies **[D]**: 894 clocks per line (720+4+85+85) ×
1620 lines (1600+2+8+10) = 1,448,280 pixel clocks per frame. With 4 lanes at
`PLL_CLOCK` 277 MHz (data rate 554 Mbps/lane, RGB888), the pixel rate is
554e6 × 4 / 24 = 92.3 Mpix/s, i.e. **63.7 Hz**, not the 60.00 Hz the device tree's
`atag,videolfb-fps= <6000>` claims. Either the PLL runs a few percent fast on
purpose (common: margin for the panel's own minimum blanking) or `PLL_CLOCK` is
not the lane bit clock on this part. **[U] — resolve with D0: read
`MIPITX_PLL_CON0` back and compare with the value in §3.3.**

### 2.4 Power and GPIO sequence

[V] Vendor kernel `nt36525b_vdo_hdp_boe_helitai.c` `lcm_init()` and
`custom_common.dtsi` `&pio`:

| step | what | evidence |
| --- | --- | --- |
| 1 | LCD bias VPOS enable, **GPIO 168** high | `mtkfb_pins_lcd_bias_enp1` |
| 2 | 2 ms | `MDELAY(2)` |
| 3 | LCD bias VNEG enable, **GPIO 173** high | `mtkfb_pins_lcd_bias_enn1` |
| 4 | TPS65132 over **i2c6, address 0x3e**: register 0x00 ← 0x13, register 0x01 ← 0x13 | `lcm_init()` → `tps65132_write_bytes()`; `&i2c6 { tps65132_driver@3E }` |
| 5 | 2 ms + 9 ms | `MDELAY(2); MDELAY(9)` |
| 6 | panel reset **GPIO 45** low 1 ms, high 1 ms — and the vendor driver does this **twice** | `DTS_GPIO_STATE_LCM_RST_OUT0/1` |
| 7 | DCS init table (§3.4) | `init_setting[]` |
| 8 | backlight enable **GPIO 154**, brightness over DCS 0x51 | `mtkfb_pins_lcd_bl_out1`, `lcm_setbacklight_cmdq()` |
| — | 1.8 V panel rail on **GPIO 178** (`lcd_ldo18`) | pinctrl state exists; who drives it is **[U]** |

The TPS65132 is an I2C part on i2c6, *not* on the DSI side. That means the
display bring-up depends on a working I2C controller even if DSI itself is
inherited from LK — worth knowing before blaming DSI for a dark panel.

### 2.5 The DSI register map

[V] MediaTek LK, `platform/mt6765/include/platform/ddp_reg_dsi.h`, offsets from
`0x14014000`. The ones a bring-up needs:

```
0x000 DSI_START          bit0 DSI_START, bit16 VM_CMD_START
0x004 DSI_STATUS
0x008 DSI_INTEN
0x00c DSI_INTSTA         bit31 DSI_BUSY, bit12 BUFFER_UNDERRUN
0x010 DSI_COM_CON        bit2 DPHY_RESET, bit0 DSI_RESET
0x014 DSI_MODE_CON       bits1:0 MODE_CON (0 = command, 1 = sync pulse video)
0x018 DSI_TXRX_CON       bits5:2 LANE_NUM, bits1:0 VC_NUM
0x01c DSI_PSCON          bits18:16 PS_SEL, bits14:0 PS_WC
0x020 DSI_VSA_NL     0x024 DSI_VBP_NL
0x028 DSI_VFP_NL     0x02c DSI_VACT_NL
0x038 DSI_SIZE_CON
0x050 DSI_HSA_WC     0x054 DSI_HBP_WC
0x058 DSI_HFP_WC     0x05c DSI_BLLP_WC
0x060 DSI_CMDQ_CON
0x074 DSI_RX_DATA03 … 0x084 DSI_RX_RACK   (DCS read-back)
0x100 DSI_PHY_LCPAT  0x104 DSI_PHY_LCCON  0x108 DSI_PHY_LD0CON
0x110 DSI_PHY_TIMCON0  0x114 TIMCON1  0x118 TIMCON2  0x11c TIMCON3
0x130 DSI_VM_CMD_CON
0x200 DSI_CMDQ (command queue)
```

[V] MIPI TX, `ddp_reg_mipi.h`, offsets from `0x11c80000`:

```
0x00c MIPITX_LANE_CON    0x028 MIPITX_PLL_PWR
0x02c MIPITX_PLL_CON0    0x030 PLL_CON1    0x034 PLL_CON2
0x038 PLL_CON3           0x03c PLL_CON4    0x040 MIPITX_PHY_SEL0
```

## 3. What has to be written, and with which values

Everything in this section is **[D]**: computed with LK's own formulas
(`platform/mt6765/ddp_dsi.c`: `DSI_Config_VDO_Timing`, `DSI_PHY_clk_setting`,
`DSI_PHY_TIMCONFIG`) applied to the panel parameters in §2.3. The same
arithmetic is implemented, and commented with the same citations, in
`drivers/dandelion_display.c`. **None of it has been compared against the live
hardware yet** — that is exactly what step D0 is for.

### 3.1 Video-mode line counts

| register | value |
| --- | --- |
| `DSI_VSA_NL` | 2 |
| `DSI_VBP_NL` | 8 |
| `DSI_VFP_NL` | 10 |
| `DSI_VACT_NL` | 1600 |

### 3.2 Horizontal byte counts

LK computes these as `pixels × bytes_per_pixel − packet_overhead`, rounded up to
a multiple of 4; for `SYNC_PULSE_VDO_MODE` the overheads are 10 (sync), 10
(back porch) and 12 (front porch):

| register | formula | value |
| --- | --- | --- |
| `DSI_HSA_WC` | `align4(4×3 − 10)` = `align4(2)` | **4** |
| `DSI_HBP_WC` | `align4(85×3 − 10)` = `align4(245)` | **248** |
| `DSI_HFP_WC` | `align4(85×3 − 12)` = `align4(243)` | **244** |
| `DSI_BLLP_WC` | `85` is not used; BLLP is 0 for sync-pulse mode | **0** |

### 3.3 MIPI PLL and PHY timing

Data rate = `PLL_CLOCK × 2` = 554 Mbps per lane, which lands in LK's 500–1000
band: `pcw_ratio = 4`, `posdiv = 2`.

| register | value | how |
| --- | --- | --- |
| `MIPITX_PLL_CON4` | `0x00ff12e0` | literal from LK (`RG_DSI0_PLL_IBIAS = 0`) |
| `MIPITX_LANE_CON` | `0x3fff0180` then `0x3fff0080` | literal from LK (bandgap enable) |
| `MIPITX_PLL_CON0` | **`0x553b13b1`** | `pcw = 554×4/26` = 85 + 6/26; three fractional bytes = `3b 13 b1` |
| `MIPITX_PLL_CON1` | POSDIV = 2, then bit 0 = 1 (PLL enable) | LK writes the field, then enables |
| `MIPITX_PLL_CON2/3` | not written | `ssc_disable = 1`, so LK skips the spread-spectrum setup entirely |

PHY timings, with `ui = 1000/554 + 1 = 2` and `cycle_time = 8000/554 + 1 = 15`:

| register | fields | value |
| --- | --- | --- |
| `DSI_PHY_TIMCON0` | TRAIL 5, ZERO 10, PREP 4, LPX 5 | **`0x050a0405`** |
| `DSI_PHY_TIMCON1` | HS_EXIT 10, TA_GET 25, TA_SURE 7, TA_GO 20 | **`0x0a190714`** |
| `DSI_PHY_TIMCON2` | CLK_TRAIL 7, CLK_ZERO 26, SYNC 0, CONT_DET 0 | **`0x071a0000`** |
| `DSI_PHY_TIMCON3` | CLK_HS_EXIT 10, CLK_HS_POST 13, CLK_HS_PREP 4 | **`0x000a0d04`** |

### 3.4 The panel's own init sequence

This is the "init blob" the task expected to have to recover from the bootloader.
**It does not have to be recovered: it is in the vendor kernel source.** [V]
`nt36525b_vdo_hdp_boe_helitai.c`, `init_setting[]`, reproduced with the meaning of
each DCS command:

| cmd | data | meaning |
| --- | --- | --- |
| `0xff` | `23` | page select: backlight page |
| delay | 1 ms | |
| `0xfb` | `01` | vendor unlock |
| `0x07` | `20` | PWM frequency, 30 kHz band (a) |
| `0x08` | `07` | PWM frequency (b) |
| `0x09` | `02` | PWM frequency (c) |
| `0xff` | `10` | page select: main page |
| `0xfb` | `01` | vendor unlock |
| `0x35` | `00` | set tear on |
| `0x51` | `00` | write display brightness = 0 |
| `0x53` | `2c` | write control display |
| `0x55` | `00` | write CABC |
| `0x68` | `02 01` | vendor command (undocumented) |
| `0x29` | — | display on |
| `0x11` | — | sleep out |
| delay | 100 ms | |

Two things to note. The vendor driver sends **display-on before sleep-out**,
which is the reverse of the usual order; that is reproduced as-is, because the
panel is the authority on what it accepts. And the table is only correct for
whichever NT36525B module is fitted (U1).

There is a second, alternative source for the same information that is worth
knowing about: the LK tree we have locally (`svoboda18/lk`, `platform/mt6765/`)
contains a complete MTK DSI + DDP driver with the same programming order, but its
`k62v1_64_bsp` target ships `nt35695B` panels, **not** NT36525B — so LK gives the
*mechanism* (which register, in which order) while the vendor kernel gives the
*values* for this panel. Neither one alone is enough.

## 4. What XNU needs for a usable framebuffer console

### 4.1 What already works

`patches/xnu/non-apple-video-console-text.patch` forces `initialize_screen(NULL,
kPETextScreen)` when `PE_state.video.v_display` is set, and
`patches/xnu/dandelion-early-trace.patch` adds a glyph blitter that writes at
`0x7ec50000` with a 2944-byte stride. So the console path exists and is what
produces every kernel log photo we have. What it does **not** do is derive
anything from hardware: the address, the stride and the geometry are compile-time
constants, and it assumes LK left the panel running.

Consequences worth stating plainly:

- If LK ever stops programming the panel (a different LK build, a `fastboot boot`
  path, a resumed-from-suspend boot), the kernel console writes into DRAM that
  nothing scans out, and the boot looks silent even though it is fine. That is a
  debugging hazard, not a cosmetic one.
- There is no mode set, no vsync, no page flip, no backlight control from the
  kernel. Any user-space graphics is impossible in this state.

### 4.2 The IOMobileFramebuffer question, stated plainly

`IOMobileFramebuffer` is **not open source**. Apple publishes `IOGraphics`
(which contains `IOFramebuffer`, `IONDRVFramebuffer`, the I2C-over-DDC helper)
and `IOHIDFamily`, but the iOS display kext that the compositor actually talks to
is private: `apple-oss-distributions/IOGraphics` exists, `IOHIDFamily` exists, and
`apple-oss-distributions/IOMobileFramebuffer` is a 404. So:

- **Impossible without Apple code:** shipping the actual `IOMobileFramebuffer`
  kext, or depending on its private user client's exact ABI as documented by
  Apple. We will never have that.
- **Possible, and what the other projects do:** implement the *service interface*
  the compositor asks for. That interface is a user client that (a) reports a
  display mode, (b) hands out or maps a surface, (c) completes a flip and
  (d) delivers vsync. Everything above it — WindowServer, backboardd,
  CoreAnimation — only needs those four things plus a working GPU for the actual
  drawing.

Evidence for that reading:

- UniXNU's component `UniXNU/GopFramebuffer` describes itself as "UEFI GOP
  framebuffer and **MobileFramebuffer** integration. CPU and GPU scanout are
  supported. With `sheng-vsync=<Hz>`, a work-loop timer sends the
  **MobileFramebuffer vsync message** to the compositor." — i.e. they built a
  MobileFramebuffer, not a copy of Apple's kext.
- HakiOS's per-device driver set (`kernel/README.md` of `hakios-mt6768`) contains
  **no display file**: eMMC, touch, keys, Mali, SCP, CONNSYS. The README says the
  device drivers "hook into the common kernel (its `bsd/conf/files`, the input
  device `/dev/redwood-touch`, **the display driver**, `IOFindBSDRoot` …)". So
  their display path is one shared display driver in the common kernel, and their
  UI is drawn through panfrost/Metal (`hakios.mali.c` + `hakios.mali.cpp` +
  Mesa panfrost + a Metal driver). Their display "works, 720×1640, iOS at scale 2"
  with the CPU-side framebuffer as the scanout source and the GPU doing the
  drawing.
- **[U]** the exact interface UniXNU's and HakiOS's display drivers expose was not
  read for this document (the repositories' file lists were not reachable from
  here at the time). Before implementing anything in this area, read
  `UniXNU/GopFramebuffer` and the common kernel's display driver. Do not design
  that ABI from scratch.

For *this* port, in order of increasing ambition:

1. A kernel framebuffer console that is derived from measured values instead of
   hardcoded constants (§4.1 fix). Cheap, no ABI risk.
2. A kernel-side "display" object that owns the panel: knows the mode, can set
   backlight, can blank, can flip between buffers. Still no user-space ABI.
3. A MobileFramebuffer-compatible user client (mode, surface, flip, vsync) so
   that a compositor can drive it. This is the one that needs the upstream
   interface study above, and it is worth exactly nothing until user space runs
   at all — which is blocked on the `devfs_kernel_mount` bug, not on display.

## 5. The ladder

Each step has an acceptance check that can be observed without a debugger, and
each step leaves the phone bootable. Steps D0 and D1 are read-only or
nearly-free; D2 onward write to hardware LK configured, so **D0's read-back is a
precondition for all of them** — without it we would be "fixing" a display that
is already correct and could not tell the difference.

### D0 — Read back the live DSI0 state (no writes)

Add a probe that, once, dumps the registers listed in `struct
dandelion_dsi_snapshot` (`drivers/dandelion_display.h`) to the eMMC log channel.
The accessor is trivial: after `ml_io_map()` the block is at a known virtual
address, and reading is a `volatile` load.

**Acceptance:** the log contains a dump in which `DSI_VACT_NL == 1600`,
`DSI_VSA_NL == 2`, `DSI_MIPITX_PLL_CON0` is *some* value, and the values are
stable across two boots. Then compare against §3: the fields that already match
are proof that our arithmetic is right; the ones that differ are the real
information, and they tell us which of the [U] items in §6 to chase first.

**Why first:** it costs one flash cycle, it cannot break anything, and it
converts half of this document from [D] to [V].

### D1 — A full-screen colour from the kernel

Fill the visible area with a solid colour from XNU (D1 is `dandelion_fb_fill()`
in the skeleton), then paint four quadrant colours.

**Acceptance:** a photograph shows a solid, evenly coloured screen; the quadrant
boundaries are straight vertical/horizontal lines, not diagonal. Diagonal edges
mean the stride is wrong; a colour that appears shifted or wrapped means the
buffer is smaller than assumed.

### D2 — Re-program what is already programmed (idempotent write)

Write §3.1–§3.3 into DSI0 and the MIPI TX block, with the same values LK used
(confirmed by D0), then `DSI_START`. Do **not** touch the panel's own state.

**Acceptance:** the screen keeps showing the D1 picture, `DSI_INTSTA.DSI_BUSY`
returns to 0 within the poll window, `BUFFER_UNDERRUN` never sets, and D0's dump
after the write is identical to D0's dump before it. Any difference means our
understanding of a field is wrong — stop and read §3 again rather than
proceeding.

### D3 — Program DSI0 from scratch

The real test of independence from LK: reset the DSI block
(`DSI_COM_CON.DSI_RESET` / `DPHY_RESET`), program PLL, PHY timing, video timing,
`TXRX_CON`, `PSCON`, start video mode, then send §3.4 over the command queue and
wait for the firmware's reset-state byte to reach normal-run.

**Acceptance:** after a reset-and-reprogram cycle the panel shows the D1 picture
again. This is the first step that proves we could survive an LK that does not
touch the panel.

Open items for this step, all listed in the skeleton's `TODO(D3)`:
`DSI_TXRX_CON.LANE_NUM` encoding (LK writes `(1 << lanes) - 1` = `0xf` for four
lanes, but only on the ULPS path), `DSI_PSCON` pixel-format field encoding for
RGB888, and the MIPI TX impedance calibration that LK reads from the efuse words
at `0x11c50190` / `0x11c50194` / `0x11c50198` — the last one is per-die data and
must be read, never invented.

### D4 — Own the power and backlight

Bias GPIOs (168, 173), the TPS65132 writes on i2c6 @ `0x3e`, the panel reset
(GPIO 45), backlight enable (GPIO 154) and `disp_pwm0` at `0x1100e000`.

**Acceptance:** brightness changes from the kernel in at least four steps and the
panel survives a full off/on cycle (bias down, 100 ms, bias up, reset, init
table) without a flash.

### D5 — Console geometry from hardware, not from `#define`

Replace the constants in `pexpert/pexpert/arm64/DANDELION.h` usage with values
read at runtime (or at least with values that a probe validated), and fill
`PE_state.video` so that the console's own idea of width/height/stride is right.

**Acceptance:** kernel text is crisp at 2944-byte stride with no shearing, and the
same kernel binary prints correctly after D3's from-scratch reprogramming (i.e.
the console does not depend on LK's leftovers).

### D6 — A display service for user space

Mode query, surface map, flip, vsync — the four things §4.2 identified. **Do not
start this before reading `UniXNU/GopFramebuffer` and HakiOS's common display
driver**, and note that it is useless until `load_init_program()` works, which is
blocked on the memory-corruption bug, not on anything in this document.

**Acceptance:** a test program (run from the mockfs, or a boot-arg-gated kernel
thread before user space exists) can map the framebuffer, flip two buffers and
count vsync ticks at ~60 Hz.

## 6. Unknowns, and the command that reads each one

| # | unknown | why it matters | how to read it |
| --- | --- | --- | --- |
| U1 | which of the five panel modules is fitted | the DCS init table, the PWM frequency bytes and possibly the porches differ per module | `adb shell dmesg \| grep -i "lcm_compare_id\|nt36525b\|ft8006s\|hx83102d"` — the vendor LCM driver prints `[LCM]lcm_compare_id,nt36525b id0 = 0x..,id1 = 0x.., id2 = 0x..` at boot. Also `adb shell getprop \| grep -i lcm` |
| U2 | the live `MIPITX_PLL_CON0/1` and `DSI_PHY_TIMCON0..3` | decides whether §3.3 is right, and whether 63.7 Hz or 60 Hz is real | step D0 (our own kernel probe); the Android side does not expose these |
| U3 | `DSI_TXRX_CON.LANE_NUM` and `DSI_PSCON` encoding as LK leaves them | D3 cannot be written without them | step D0 |
| U4 | MIPI TX impedance calibration words | wrong impedance = marginal HS signalling, which looks like random corruption | read the efuse words at `0x11c50190`/`94`/`98` — from the kernel (D0 probe) or, if the vendor kernel exposes efuse, `adb shell cat /proc/efuse` if present **[U]** |
| U5 | who drives `lcd_ldo18` (GPIO 178) | a missing 1.8 V rail is a dead panel with no error message | `adb shell cat /sys/kernel/debug/pinctrl/*/pinmux-pins \| grep 178` if the vendor kernel has pinctrl debugfs; otherwise D0-style probe reading the GPIO mode register at `0x10005000` + pin |
| U6 | whether LK actually leaves DSI in `SYNC_PULSE_VDO_MODE` | D2's idempotent rewrite assumes the same mode | step D0, `DSI_MODE_CON` bits 1:0 |
| U7 | whether the 63.7 Hz implied by `PLL_CLOCK = 277` matches the panel's real frame rate | a mismatch shows up as a slowly rolling tear or a flicker | `adb shell dumpsys SurfaceFlinger \| grep -i refresh` on Android, or count DSI frame-done interrupts in the kernel |

Two things this document does **not** claim: that the DCS table in §3.4 is the
right one for the fitted module (U1), and that any of §3 has ever been compared
with the hardware (U2). Both are one flash cycle away.

## 7. References

- `docs/NEXT-M3-GOP.md` — the measured GOP/panel facts and the stride story
- `docs/HARDWARE_ANALYSIS.md` — addresses, framebuffer geometry, `disphal_allocate_fb`
- `docs/UEFI-DANDELION-PLAN.md` — the port's phase plan, DDP base, panel name
- `bootshim/shim.c`, `bootshim/fb.h` — the framebuffer the shim and console use
- `pexpert/pexpert/arm64/DANDELION.h` — where the board's display constants live today
- `patches/xnu/non-apple-video-console-text.patch`, `patches/xnu/dandelion-early-trace.patch` — the existing console path
- `drivers/dandelion_display.c`, `drivers/dandelion_display.h` — the skeleton this plan describes
- MediaTek LK `svoboda18/lk` @ `37ec8bc`: `platform/mt6765/ddp_dsi.c`, `platform/mt6765/include/platform/ddp_reg_dsi.h`, `ddp_reg_mipi.h`, `mt_reg_base.h` (register maps and PHY formulas)
- Xiaomi vendor kernel `MiCode/Xiaomi_Kernel_OpenSource`, branch `dandelion-q-oss` @ `8974b91`: `arch/arm64/configs/dandelion_defconfig`, `arch/arm64/boot/dts/mediatek/mt6765.dts`, `custom_common.dtsi`, and the panel driver `nt36525b_vdo_hdp_boe_helitai.c`
- [UniXNU/GopFramebuffer](https://github.com/UniXNU/GopFramebuffer) — "UEFI GOP framebuffer and MobileFramebuffer integration", CPU and GPU scanout, vsync message
- [oosexgang/hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) — README (display works, 720×1640, Metal via panfrost) and `kernel/README.md` (no per-device display driver; a shared one in the common kernel)
- [apple-oss-distributions/IOGraphics](https://github.com/apple-oss-distributions/IOGraphics), [IOHIDFamily](https://github.com/apple-oss-distributions/IOHIDFamily) — published; `IOMobileFramebuffer` is not
