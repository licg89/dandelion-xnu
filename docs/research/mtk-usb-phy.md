# MT6765 / MT6762G ("dandelion", Redmi 9A) — USB 2.0 device controller + PHY bring-up at register level

Research report. **Nothing here was executed on hardware.** Everything is derived from source code
(Linux mainline, U-Boot, the MT6765 vendor kernel) plus the device's own stock DTB that is lying in
this workspace (`D:\aios\stock_boot.dtb`, parsed locally). Claims I could not confirm from source are
marked **[unverified]**.

Target: get a bare-metal aarch64 shim (MMU off) to power the U2 PHY so the Mentor-MUSB core at
`0x11200000` reports VBUS, pulls up D+, and sees the host's reset.

---

## 0. TL;DR — why `DEVCTL` reads 0 today

Two independent facts explain the symptom:

1. **`MUSB_DEVCTL` (byte offset `0x60`) is not a controller-internal register.** Its `VBUS[4:3]` field
   and its `BDEVICE` bit are *driven by the PHY's UTMI comparator outputs*
   (`vbusvalid` / `avalid` / `sessend` / `iddig`). If the PHY is powered down, those outputs are 0,
   so `DEVCTL` reads `0x00` even though the MUSB register file itself is perfectly accessible.
   Register offsets: [`musb_regs.h`](https://github.com/torvalds/linux/blob/master/drivers/usb/musb/musb_regs.h)
   (`MUSB_DEVCTL 0x60`, `MUSB_FADDR 0x00`, `MUSB_POWER 0x01`, `MUSB_INTRUSB 0x0A`, `MUSB_INTRUSBE 0x0B`,
   `MUSB_INDEX 0x0E`, `MUSB_HWVERS 0x6C`); bit fields in
   [`musb_regs.h` DEVCTL block](https://github.com/torvalds/linux/blob/master/drivers/usb/musb/musb_regs.h)
   (`BDEVICE 0x80`, `VBUS 0x18`, `VBUS_SHIFT 3`, `HM 0x04`).
2. **All PHY reads returning 0 means the PHY register file is not clocked (or not powered).**
   The MT6765 U2 PHY is the MediaTek **"a60810"** IP; its control registers live in the second `reg`
   entry of the `usb0` node (`0x11cc0000`) and are accessed by the vendor driver only *after* it has
   enabled the USB clocks — see `usb_enable_clock()` / `usb_prepare_clock()` in
   [`usb20_phy.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_phy.c).
   The vendor driver even brackets single PHY register accesses with clock on/off
   (`usb_phy_check_in_uart_mode()`, `Charger_Detect_Init()`, `Charger_Detect_Release()`), which proves
   the PHY register file sits behind the USB clock gate.

So the fix order is: **clocks → (optional) USB IP reset → PHY power-on sequence → force device mode →
MUSB `POWER.SOFTCONN`**. Section 3 has the clock bits, section 4 the code, section 5 the readbacks.

---

## 1. Address map (from the device's own DTB)

Parsed from `D:\aios\stock_boot.dtb` (the stock boot image DTB of this handset):

| Base | Node | Compatible | Notes |
|---|---|---|---|
| `0x11200000` | `usb0@11200000` | `mediatek,mt6765-usb20` | MUSB core. `reg[0]` size `0x10000` |
| `0x11cc0000` | (same node, `reg[1]`) | — | **U2 PHY ("a60810") register file**, size `0x10000` |
| `0x11210000` | `usb_sif@11210000` | `mediatek,usb_sif` | second window, **not used by the vendor MUSB driver** |
| `0x11210000` | `usb1p_sif@11210000` | `mediatek,usb1p_sif` | same address — alias / USB1.1 SIF **[unverified]** |
| `0x10000000` | `topckgen@10000000` | `mediatek,mt6765-topckgen` | USB clock mux + gates |
| `0x10001000` | `infracfg_ao@10001000` | `mediatek,common-infracfg_ao`, `mediatek,mt6765-infracfg` | USB bus gate |
| `0x1000c000` | `apmixed@1000c000` | `mediatek,mt6765-apmixedsys` | 26 MHz "SSUSB" gate |
| `0x10003000` | `pericfg@10003000` | `mediatek,pericfg` | USB IP software reset (bit 29) |

Other `usb0` properties decoded from the same DTB (all relevant to bring-up):

```
reg         = <0x0 0x11200000 0x0 0x10000>, <0x0 0x11cc0000 0x0 0x10000>
interrupts  = <0x0 0x49 0x8>            /* GIC SPI 73, level-high */
mode        = <2>                        /* MUSB_PERIPHERAL (enum musb_mode: 0=undef,1=host,2=periph,3=otg) */
multipoint  = <1>;  num_eps = <16>
clocks      = <&infracfg_ao 8>, <&topckgen 0x76>, <&topckgen 0x20>
clock-names = "usb0", "usb0_clk_top_sel", "usb0_clk_univpll3_d4"
nvmem-cells = <&...>;  nvmem-cell-names = "efuse_idx107"   /* RG_USB20_INTR_CAL */
```

`mode = 2` means the board is **device-only** (no OTG role switch in this product), which is why the
shim never needs `DEVCTL.SESSION`.

**No `vusb` / `va12` supply properties exist on this node**, even though the vendor driver calls
`regulator_get(..., "vusb")` and `regulator_get(..., "va12")`
([`usb20.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20.c),
`musb_platform_init`). Consequence: **nothing in the OS enables the MT6357 `VUSB33` LDO** — it must be
left on by preloader/LK. If LK really killed the rail, no register poking will bring the PHY back and
the PMIC has to be re-programmed through `pwrap@1000d000` **[unverified, see §7]**.

---

## 2. The U2 PHY ("a60810") register map

### 2.1 How this relates to the generic `phy-mtk-tphy` driver

Upstream `phy-mtk-tphy.c` models MediaTek's "T-PHY" as *banks*
([`phy-mtk-tphy.c`](https://github.com/torvalds/linux/blob/master/drivers/phy/mediatek/phy-mtk-tphy.c),
same in [`u-boot`](https://github.com/u-boot/u-boot/blob/master/drivers/phy/phy-mtk-tphy.c)):

```c
/* V1 */  SSUSB_SIFSLV_V1_SPLLC 0x000 | _V1_U2FREQ 0x100 | _V1_CHIP 0x300
         SSUSB_SIFSLV_V1_U2PHY_COM 0x000            /* the U2 "COM" bank */
/* V2 */  SSUSB_SIFSLV_V2_MISC 0x000 | _V2_U2FREQ 0x100 | _V2_U2PHY_COM 0x300
```

MT6765 is **not** describable by those tables, and there is no `t-phy` node in this device tree at all:
MT6765 predates the upstream binding and its vendor driver talks to the PHY directly at
`usb0.reg[1]` (`0x11cc0000`). The register layout it uses is the **V1 "COM-at-offset-0" layout**
(`U2PHYDTM0` at `0x68`, `U2PHYDTM1` at `0x6C`), so:

* `com` bank base = `0x11cc0000` (not `+0x300` as in V2),
* `misc` bank (V2 only) does not exist → the `MR1_EFUSE_AUTO_LOAD_DIS` (V2 `0x04`) trick is unavailable,
* the U2 frequency-meter cluster is at **`0x700…0x71F`** in the vendor driver, *not* at `sif+0x100`
  as the upstream V1 model assumes. Do not blindly port `U2FREQ_*` offsets from `phy-mtk-tphy.c`.

`phy-mtk-t50.c` **does not exist** in mainline (verified against
[`drivers/phy/mediatek`](https://github.com/torvalds/linux/tree/master/drivers/phy/mediatek)); the
only other relevant files are `phy-mtk-ufs.c` (UFS PHY, unrelated IP) and `phy-mtk-xsphy.c`
(MT8195-era XS-PHY, different register file). For MT6765 the authoritative map is the vendor header
[`mtk-phy-a60810.h`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/mtk-phy-a60810.h)
(3126 lines, full bit-field + offset defines) together with
[`usb20_phy.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_phy.c).

### 2.2 Register table (`0x11cc0000` + offset)

Field names and bit positions are quoted from
[`mtk-phy-a60810.h`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/mtk-phy-a60810.h)
(sections `U3D_USBPHYACRx`, `U3D_U2PHYACR3/4`, `U3D_U2PHYDCRx`, `U3D_U2PHYDTMx`, `U3D_U2PHYDMONx`,
`U3D_U2PHYBC12C`, `U3D_REGFPPC`, `U3D_VERSIONC`). Offsets follow `struct u2phy_reg_a` in the same file.

| Off | Name | Interesting fields (bit) | Purpose |
|---|---|---|---|
| `0x00` | USBPHYACR0 | `USBPLL_FORCE_ON`(15) `PLL_FBDIV`(14:8) `PLL_PREDIV`(7:6) `INTR_EN`(5) `REF_EN`(4) `BGR_DIV`(3:2) `SIFSLV_CHP_EN`(1) `SIFSLV_BGR_EN`(0) | **PLL / bandgap / charge-pump enable**, PLL divider |
| `0x04` | USBPHYACR1 | `INTR_CAL`(23:19) `OTG_VBUSTH`(18:16) `VRT_VREF_SEL`(14:12) `TERM_VREF_SEL`(10:8) `MPX_SEL`(7:0) | **RX squelch/termination trim**, VBUS comparator threshold |
| `0x08` | USBPHYACR2 | `MAC_BANDGAP_EN`(17) `MAC_CHOPPER_EN`(16) `CLKREF_REV`(15:0) | analog reference |
| `0x10` | USBPHYACR4 | — (reserved in driver) | |
| `0x14` | USBPHYACR5 | `HSTX_SRCAL_EN`(15) `HSTX_SRCTRL`(14:12) `HS_100U_U3_EN`(11) `GBIAS_ENB`(10) | **HS TX slew / TX impedance trim** |
| `0x18` | USBPHYACR6 | `ISO_EN`(31) `PHY_REV`(31:24) `BC11_SW_EN`(23) `OTG_VBUSCMP_EN`(20) `DISCTH`(7:4) `SQTH`(3:0) | **BC1.1 off, VBUS comparator on, disconnect threshold**, PHY revision |
| `0x1C` | U2PHYACR3 | `PUPD_BIST_EN`(12) `EN_PU_DM`(11) `EN_PD_DM`(10) `EN_PU_DP`(9) `EN_PD_DP`(8) | DP/DM pull up/down override (test) |
| `0x20` | U2PHYACR4 | `DP_100K_MODE`(18) `DM_100K_EN`(17) `DP_100K_EN`(16) `GPIO_CTL`(9) `GPIO_MODE`(8) `TX_BIAS_EN`(5) | **GPIO/UART override; DP 100k mode** |
| `0x24` | U2PHYAMON0 | `GPIO_DM_O`(1) `GPIO_DP_O`(0) | monitor |
| `0x60` | U2PHYDCR0 | `PLL_FORCE_ON`(24) `PLL_STABLE`(25) `GATED_ENB`(29) `CDR_TST`(31:30) | **force PLL on / read PLL lock** |
| `0x64` | U2PHYDCR1 | `DRVVBUS`(23) `SW_PLLMODE`(19:18) `FORCE_TAP`(12) | VBUS drive (host only), PLL mode |
| `0x68` | U2PHYDTM0 | `FORCE_UART_EN`(26) `FORCE_USB_CLKEN`(25) `FORCE_DATAIN`(23) `FORCE_DM_PULLDOWN`(21) `FORCE_DP_PULLDOWN`(20) `FORCE_XCVRSEL`(19) **`FORCE_SUSPENDM`(18)** `FORCE_TERMSEL`(17) `FORCE_OPMODE`(16) `RG_DATAIN`(13:10) `RG_DMPULLDOWN`(7) `RG_DPPULLDOWN`(6) `RG_XCVRSEL`(5:4) **`RG_SUSPENDM`(3)** `RG_TERMSEL`(2) `RG_OPMODE`(1:0) | **UTMI TX control / suspend** |
| `0x6C` | U2PHYDTM1 | `CLK48M_EN`(23) `CLK60M_EN`(22) `UART_EN`(16) `IP_U2_PORT_POWER`(15) `FORCE_IP_U2_PORT_POWER`(14) **`FORCE_VBUSVALID`(13) `FORCE_SESSEND`(12) `FORCE_BVALID`(11) `FORCE_AVALID`(10) `FORCE_IDDIG`(9) `FORCE_IDPULLUP`(8)** `RG_VBUSVALID`(5) `RG_SESSEND`(4) `RG_BVALID`(3) `RG_AVALID`(2) `RG_IDDIG`(1) `RG_IDPULLUP`(0) | **the DEVCTL source** — VBUS/ID/AVALID forcing |
| `0x70` | U2PHYDMON0 | `PRBS7_BERTH`(7:0), `EOP_CTL`(19:16) | monitor |
| `0x74` | U2PHYDMON1 | `USB20_UART_O`(31) `AD_USB20_BVALID`(28) `USB20_IDDIG`(27) `AD_USB20_VBUSVALID`(26) `AD_USB20_SESSEND`(25) `AD_USB20_AVALID`(24) `LINE_STATE`(23:22) `HST_DISCON`(21) `TX_READY`(20) `RX_ERROR`(19) `RX_ACTIVE`(18) `RX_VALIDH`(17) `RX_VALID`(16) `DATA_OUT`(15:0) | **live monitor: what the PHY actually sees** |
| `0x78` | U2PHYDMON2 | `TXVALID_CNT` `RXACTIVE_CNT` `LB_BERCNT` | monitor |
| `0x7C` | U2PHYDMON3 | `PRBS7_ERRCNT` `PRBS7_DONE/LOCK/PASS` | monitor |
| `0x80` | U2PHYBC12C | `CHGDT_EN`(0) `CHGDT_COMP_EN`(2) `ISRC_DP`(7) `VDATSRC`(13) … | BC1.2 charger detect |
| `0x84` | U2PHYBC12C1 | `CHGDT_REV`(7:0) | |
| `0xE0` | REGFPPC | `USB20_REG`(0) `USB11_REG`(1) `CHGDT_REG`(2) `USB20_OTG_REG`(3) `USB11_OTG_REG`(4) | **register-file "PPC" enables** — prime suspect if the PHY reads 0 **[unverified meaning]** |
| `0xF0` | VERSIONC | `VERSION_CODE_REGFILE`(31:24) `USB11_VERSION_CODE`(23:16) `VERSION_CODE_ANA`(15:8) `VERSION_CODE_DIG`(7:0) | **liveness probe — must read non-zero** |
| `0xFC` | REGFCOM | `RG_PAGE`(31:24) `I2C_MODE`(16) | page select / test I/F |

Frequency meter cluster (used only for HS slew calibration), offsets as written by
`hs_slew_rate_cal()` in
[`usb20_phy.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_phy.c)
(`USBPHY_*32(0xF00 - 0x800, …)` etc.) and defined by `struct sifslv_fm_reg_a` /`U3D_FMCR0…FMMONR1`
in the header:

| Off | Name | Fields |
|---|---|---|
| `0x700` | FMCR0 | `LOCKTH`(31:28) `MONCLK_SEL`(27:26) `FM_MODE`(25) `FREQDET_EN`(24) `CYCLECNT`(23:0) |
| `0x704` | FMCR1 | `TARGET`(31:0) |
| `0x708` | FMCR2 | `OFFSET`(31:0) |
| `0x70C` | FMMONR0 | `USB_FM_OUT`(31:0) |
| `0x710` | FMMONR1 | `MONCLK_SEL_2`(9) `FRCK_EN`(8) `USBPLL_LOCK`(1) `USB_FM_VLD`(0) |

### 2.3 Values the vendor writes (i.e. the "required" values)

There is **no documented reset-value table** — MediaTek's header only lists fields. What can be
stated with certainty is what the driver *programs*, and therefore what a correct bring-up must read
back. All of this is in
[`usb20_phy.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_phy.c):
`usb_phy_poweron()`, `usb_phy_recover()`, `set_usb_phy_mode()`, `usb_rev6_setting()`,
`hs_slew_rate_cal()`, `usb_phy_tuning()`.

`usb_phy_poweron()` (comment: *"M17_USB_PWR Sequence 20160603.xls"*, i.e. the PHY team's official
power sequence):

```
udelay(50);                              /* "wait 50us for PHY 3.3V/1.8V stable" */
0x68 &= ~(1<<26);                        /* FORCE_UART_EN = 0            */
0x6C &= ~(1<<16);                        /* UART_EN = 0                  */
0x20 &= ~((1<<9)|(1<<8));                /* GPIO_CTL = 0, GPIO_MODE = 0  */
0x18 &= ~(1<<23);                        /* BC11_SW_EN = 0               */
0x20 |=  (1<<18);                        /* DP_100K_MODE = 1             */
0x20 &= ~((1<<16)|(1<<17));              /* DP_100K_EN = 0, DM_100K_EN = 0 */
0x18 |=  (1<<20);                        /* OTG_VBUSCMP_EN = 1           */
0x68 &= ~(1<<18);                        /* FORCE_SUSPENDM = 0           */
0x18 = (0x18 & 0x00FFFFFF) | (0x40<<24); /* PHY_REV = 0x40               */
udelay(800);
```

`usb_phy_recover()` (called from `mt_usb_enable()` right after clocks + `mdelay(10)`):

```
0x1C &= ~(1<<12);                        /* PUPD_BIST_EN = 0             */
0x68 &= ~(1<<26); 0x6C &= ~(1<<16);      /* UART off                     */
0x20 &= ~((1<<9)|(1<<8));                /* GPIO override off            */
0x68 &= ~(1<<18);                        /* FORCE_SUSPENDM = 0           */
0x68 &= ~((1<<6)|(1<<7));                /* DP/DM pulldown off           */
0x68 &= ~(3<<4);                         /* XCVRSEL = 00                 */
0x68 &= ~(1<<2);                         /* TERMSEL = 0                  */
0x68 &= ~(0xF<<10);                      /* DATAIN = 0                   */
0x68 &= ~((1<<20)|(1<<21)|(1<<19)|(1<<17)|(1<<23));  /* all FORCE_* off */
0x18 &= ~(1<<23); 0x18 |= (1<<20);       /* BC1.1 off, VBUSCMP on        */
0x18 = (0x18 & 0x00FFFFFF) | (0x40<<24); /* PHY_REV = 0x40               */
udelay(800);
set_usb_phy_mode(PHY_DEV_ACTIVE);        /* → 0x6C = 0x3F2F, see below   */
hs_slew_rate_cal();                      /* → 0x14[14:12]                */
/* efuse: RG_USB20_INTR_CAL = efuse_idx107 + 2  (0x04[23:19]) */
0x18 |= 0xF0;                            /* DISCTH = 0xF (max)           */
usb_phy_tuning();                        /* 0x04 VRT/TERM refs, 0x18[31:30] — DT only */
```

`set_usb_phy_mode()` — **this is the step that makes `DEVCTL` non-zero**:

| mode | resulting `0x6C` | meaning |
|---|---|---|
| `PHY_DEV_ACTIVE` | **`0x0000_3F2F`** | `IDPULLUP,IDDIG,AVALID,BVALID,VBUSVALID` = 1, `SESSEND` = 0, all `FORCE_*` = 1 |
| `PHY_HOST_ACTIVE` | `0x0000_3F2D` | same but `IDDIG` = 0 |
| `PHY_IDLE_MODE` | `0x0000_3F11` | `SESSEND` = 1, `VBUSVALID/AVALID/BVALID` = 0 |

The upstream T-PHY driver does the same thing with a subset of these bits
(`u2_phy_instance_power_on()`: set `VBUSVALID|AVALID`, clear `SESSEND`; u-boot identical), which is
why the vendor's full force-sequence is safer: the `FORCE_*` bits are what actually mux the register
values onto the UTMI status lines instead of the analog comparators.

### 2.4 The `+0x800` alias aperture

The `usb2jtag` helper in the same file writes `usb_phy_base + 0x800`, `+0x818`, `+0x820` for
`BGR_EN` (ACR0[0]), `BC11_SW_EN` (ACR6[23]) and `GPIO_CTL` (ACR4[9]) — the *same* fields the normal
driver writes at `0x00/0x18/0x20`. The same file then uses `0xF10 - 0x800` to reach `0x710`. Reading
this together: the PHY register file is ~`0x800` bytes and is **decoded twice**, at
`0x11cc0000+off` and `0x11cc0800+off` **[unverified — inferred from the two code paths]**. Practical
use: read both apertures when the PHY looks dead (`0x11cc00F0` vs `0x11cc08F0`); write only the
low one.

---

## 3. Clocks that must be on

Three blocks are involved, all identified by the device DT and by mainline `clk-mt6765.c`
([`clk-mt6765.c`](https://github.com/torvalds/linux/blob/master/drivers/clk/mediatek/clk-mt6765.c);
the vendor copy
[`clk-mt6765.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/clk/mediatek/clk-mt6765.c)
agrees bit-for-bit).

### 3.1 `infracfg_ao` @ `0x10001000` — the USB bus gate

```
CG group "ifr2":  set_ofs 0x80, clr_ofs 0x84, sta_ofs 0x90
GATE_IFR2(CLK_IFR_ICUSB, "ifr_icusb", "axi_ck", 8)
```

* **Enable**: write `0x0000_0100` to `0x10001084` (CG_CLR).
* **Disable**: write `0x0000_0100` to `0x10001080` (CG_SET).
* **Status**: `0x10001090` bit 8 — **1 = gated (off), 0 = running**.

Polarity is from `mtk_clk_gate_ops_setclr` in
[`clk-gate.c`](https://github.com/torvalds/linux/blob/master/drivers/clk/mediatek/clk-gate.c)
(`enable → mtk_cg_clr_bit`, `is_enabled → bit_is_cleared`).

The DT names this clock **`"usb0"`** (`clocks = <&infracfg_ao 8>`) and the vendor binding defines
`CLK_IFR_ICUSB 8` for that name, i.e. DT clock-ID 8 selects the `ICUSB` gate; independently of the
ID numbering, that gate's hardware bit is **bit 8** of the ifr2 group above. The vendor driver's
`devm_clk_get(&pdev->dev, "usb0")` and the mainline gate entry therefore describe the same
transistor.

### 3.2 `topckgen` @ `0x10000000` — mux + 48 M gate

```
usb_top_parents[] = { "clk26m", "univpll3_d4" };
MUX_GATE_CLR_SET_UPD(CLK_TOP_USB_TOP_SEL, "usb_top_sel", …,
                     CLK_CFG_5, CLK_CFG_5_SET, CLK_CFG_5_CLR,
                     /*shift*/16, /*width*/1, /*gate_shift*/23,
                     CLK_CFG_UPDATE, /*upd bit*/22);
CLK_CFG_5 = 0x90, CLK_CFG_5_SET = 0x94, CLK_CFG_5_CLR = 0x98, CLK_CFG_UPDATE = 0x004
```

* **Select parent 1 (`univpll3_d4`)**: set bit 16 of `0x10000090`, then write `(1<<22)` to
  `0x10000004` to commit.
* **Ungate**: write `(1<<23)` to `0x10000098` (CLR). Gate bit **0 = clock on, 1 = gated** — see
  `mtk_clk_mux_is_enabled()` in
  [`clk-mux.c`](https://github.com/torvalds/linux/blob/master/drivers/clk/mediatek/clk-mux.c)
  (`return (val & BIT(gate_shift)) == 0;`).
* Do **not** blind-write `CLK_CFG_5`: bits 0-7 (`sspm_sel`), 8-15 (`dxcc_sel`) and 24-31 (`spm_sel`)
  are other peripherals' clocks — read-modify-write only.

Additionally, `topckgen` has a "TOP1" gate register at **`0x104`** whose bits are *active-high*
(`GATE_TOP1` uses `mtk_clk_gate_ops_no_setclr_inv`, `enable → set bit`):

```
CLK_TOP_USB20_48M_EN   bit 8    (parent "usb20_48m_div" = usb20_192m/4 = 48 MHz)
CLK_TOP_UNIVPLL_48M_EN bit 9    (parent "univ_48m_div"  = usb20_192m/4 = 48 MHz)
```

Note the vendor clock driver has these two lines **commented out** (nobody in Linux ever enables
them), while mainline registers them. Since the vendor's own HS slew-rate calibration assumes a
**48 MHz** PHY reference (`#define FRA (48)` in `hs_slew_rate_cal()`), the 48 M gate at `0x104[8]` is
the most plausible "PHY reference clock" gate and a prime suspect for the dead PHY
**[inference, unverified]**.

### 3.3 `apmixedsys` @ `0x1000c000` — the "SSUSB 26M" gate

```
apmixed_cg_regs = { .set_ofs = 0x14, .clr_ofs = 0x14, .sta_ofs = 0x14 };
GATE_APMIXED(CLK_APMIXED_SSUSB26M, "apmixed_ssusb26m", "f_f26m_ck", 4)
ops = mtk_clk_gate_ops_no_setclr_inv   →  enable = set bit,  is_enabled = bit set
```

* **Enable**: `0x1000c014 |= (1<<4)`.  **Status**: bit 4 == 1 means running.
* Bits 5/6/7… of the same register gate `APPLL26M`, `MIPIC0_26M`, `MMSYS_F26M`, `UFS26M` — a blind
  write here can kill the camera/display 26 MHz. RMW only.
* In the vendor tree this gate is **commented out** (like the 48 M ones), i.e. preloader/LK is expected
  to leave it on.

### 3.4 What LK actually does (and what I could and could not verify)

I could not obtain MediaTek's LK source for MT6765 (no public mirror; the only public MT6765-family LK
tree I found is a port documentation/patch set,
[`oppo-a1k-lk`](https://github.com/calan152/oppo-a1k-lk), which touches
`platform/common/mt_musb/mt_musb.c` only for a QMU/ZLP fix — it confirms the file path and that LK
drives the *same* MUSB + QMU stack for fastboot, but contains no PHY init). Therefore:

* **Verified**: LK uses `platform/common/mt_musb/mt_musb.c` and the MUSB core for fastboot
  ([patch](https://github.com/calan152/oppo-a1k-lk/blob/main/patches/0001-oppo-a1k-cph1923-port.patch)).
* **Inferred [unverified]**: LK's USB init performs the same PHY "power-on / recover" writes as the
  vendor kernel (`usb20_phy.c`), because that file's sequence is literally the PHY team's
  `M17_USB_PWR Sequence 20160603.xls`, shared between LK and the kernel in MTK BSPs; and LK's
  `usb_enable_clock(false)` equivalent gates `ICUSB` + `usb_top_sel` on exit, which is exactly the
  state that produces "PHY reads 0".

### 3.5 If a bit is unknown — what to probe (read-only, safe)

| Question | Probe | Healthy answer |
|---|---|---|
| Is the USB bus clock on? | `r 0x10001090` | bit 8 == 0 |
| Is `usb_top_sel` ungated and on univpll3_d4? | `r 0x10000090` | bit 16 == 1, bit 23 == 0 |
| Is the 48 M USB gate on? | `r 0x10000104` | bit 8 == 1 (bit 9 too) |
| Is the 26 M SSUSB gate on? | `r 0x1000c014` | bit 4 == 1 |
| Is the PHY IP reset deasserted? | `r 0x10003000` | bit 29 == 0 |
| Is the PHY register file alive? | `r 0x11cc00F0` and `r 0x11cc08F0` | non-zero, equal-ish |
| Does the PHY see the 26 M/48 M ref? | `r 0x11cc0700/070C/0710` after enabling FM | `0x710` bit 1 (`USBPLL_LOCK`) set |

All of these are reads of AP-owned blocks; none of them can hang the bus on this SoC class as far as
the sources show. Do them **before** any write.

---

## 4. Concrete bring-up sequence (C pseudocode, aarch64, MMU off)

`PHY = 0x11CC0000`, `MUSB = 0x11200000`, `TOPCK = 0x10000000`, `INFRAAO = 0x10001000`,
`APMIX = 0x1000C000`, `PERICFG = 0x10003000`.

```c
#define R32(a)        (*(volatile uint32_t *)(uintptr_t)(a))
#define W32(a, v)     do { *(volatile uint32_t *)(uintptr_t)(a) = (v); __asm__ volatile("dsb sy" ::: "memory"); } while (0)
/* MUSB is a byte/half-word register file: never do 32-bit accesses there,
   a 32-bit write to 0x00 would also clobber POWER (0x01) and INTRTX (0x02). */
#define R8(a)         (*(volatile uint8_t  *)(uintptr_t)(a))
#define W8(a, v)      do { *(volatile uint8_t  *)(uintptr_t)(a) = (v); __asm__ volatile("dsb sy" ::: "memory"); } while (0)
#define W16(a, v)     do { *(volatile uint16_t *)(uintptr_t)(a) = (v); __asm__ volatile("dsb sy" ::: "memory"); } while (0)
#define SETCLR(a, c, s) do { uint32_t _v = R32(a); _v &= ~(uint32_t)(c); _v |= (uint32_t)(s); W32((a), _v); } while (0)
static inline void udelay(unsigned us);   /* your timer */

int usb_phy_bringup_device(void)
{
    /* ---------- Phase 1: clocks (idempotent, RMW only) ---------- */
    W32(INFRAAO + 0x84, 1u << 8);              /* CG_CLR: ICUSB (DT clock "usb0") ON      */
    SETCLR(TOPCK + 0x90, 0, 1u << 16);         /* usb_top_sel = univpll3_d4 (keep bits!)  */
    W32(TOPCK + 0x004, 1u << 22);              /* commit mux (CLK_CFG_UPDATE bit 22)     */
    W32(TOPCK + 0x98, 1u << 23);               /* ungate usb_top_sel (gate bit 0 = on)   */
    SETCLR(TOPCK + 0x104, 0, (1u << 8) | (1u << 9));   /* usb20_48m_en, univpll_48m_en   */
    SETCLR(APMIX + 0x14, 0, 1u << 4);          /* SSUSB26M ON (bits 5+ are camera!)      */
    udelay(50);

    /* ---------- Phase 2 (optional): USB IP soft reset ---------- */
    /* Vendor: "reset all of the USB IP, including PHY and MAC".
       Skip this while the MUSB is already configured - it resets the controller too. */
    SETCLR(PERICFG + 0x00, 0, 1u << 29);
    udelay(10000);
    SETCLR(PERICFG + 0x00, 1u << 29, 0);
    udelay(100);

    /* ---------- Phase 3: is the PHY register file alive? ---------- */
    if (R32(PHY + 0xF0) == 0 && R32(PHY + 0x8F0) == 0)
        return -1;                   /* clock gate, IP reset or VUSB33 rail - see section 7 */

    /* ---------- Phase 4: PHY power-on ("M17_USB_PWR") ---------- */
    udelay(50);
    SETCLR(PHY + 0x68, 1u << 26, 0);                  /* FORCE_UART_EN  = 0 */
    SETCLR(PHY + 0x6C, 1u << 16, 0);                  /* UART_EN        = 0 */
    SETCLR(PHY + 0x20, (1u << 9) | (1u << 8), 0);     /* GPIO_CTL/MODE  = 0 */
    SETCLR(PHY + 0x18, 1u << 23, 0);                  /* BC11_SW_EN     = 0 */
    SETCLR(PHY + 0x20, 0, 1u << 18);                  /* DP_100K_MODE   = 1 */
    SETCLR(PHY + 0x20, (1u << 16) | (1u << 17), 0);   /* DP/DM_100K_EN  = 0 */
    SETCLR(PHY + 0x18, 0, 1u << 20);                  /* OTG_VBUSCMP_EN = 1 */
    SETCLR(PHY + 0x68, 1u << 18, 0);                  /* FORCE_SUSPENDM = 0 */
    SETCLR(PHY + 0x18, 0xFFu << 24, 0x40u << 24);     /* PHY_REV        = 0x40 */
    udelay(800);
    /* expected here: PHY 0x18 == 0x4010_0000  (PHY_REV=0x40 + OTG_VBUSCMP_EN) */

    /* ---------- Phase 5: force device mode -> DEVCTL comes alive ---------- */
    /* vendor set_usb_phy_mode(PHY_DEV_ACTIVE):
       FORCE_IDPULLUP|FORCE_IDDIG|FORCE_AVALID|FORCE_BVALID|FORCE_SESSEND|FORCE_VBUSVALID = 1
       RG_IDPULLUP|RG_IDDIG|RG_AVALID|RG_BVALID|RG_VBUSVALID = 1, RG_SESSEND = 0
       -> DTM1 = 0x3F2F on a freshly powered PHY (host mode would be 0x3F2D) */
    W32(PHY + 0x6C, (R32(PHY + 0x6C) & ~0x00003F12u) | 0x00003F2Fu);
    udelay(10);

    /* vendor tail of usb_phy_recover(): disconnect threshold to max */
    SETCLR(PHY + 0x18, 0xF0u, 0xF0u);          /* DISCTH = 0xF  -> 0x18 == 0x4010_00F0 */

    /* ---------- Phase 6: MUSB side (device, no interrupts needed) ---------- */
    W8 (MUSB + 0x0A, 0xFF);                        /* INTRUSB  (W1C): clear all  */
    W8 (MUSB + 0x00, 0x00);                        /* FADDR = 0                  */
    W8 (MUSB + 0x0E, 0x00);                        /* INDEX = 0                  */
    W8 (MUSB + 0x60, 0x00);                        /* DEVCTL: no session (device) */
    W16(MUSB + 0x06, 0x0001);                      /* INTRTXE: EP0               */
    W8 (MUSB + 0x0B, 0xF7);                        /* INTRUSBE: vendor value     */
    W32(MUSB + 0xA4, 0x00000105);                  /* MTK USB_L1INTM: TX|USBCOM|DMA */
    W8 (MUSB + 0x01, 0x60);                        /* POWER = SOFTCONN|HSENAB    */
    udelay(1000);
    /* now poll MUSB+0x60 for 0x98 and MUSB+0x0A for RESET(0x04)/CONNECT(0x10) */
    return 0;
}
```

Notes:

* `MUSB + 0xA4` (`USB_L1INTM`) and `MUSB + 0x86/0x82` (`TXTOGEN`/`RXTOGEN`) are MediaTek extensions,
  not Mentor registers: `USB_L1INTS 0x00a0`, `USB_L1INTM 0x00a4`, `TX_INT_STATUS BIT(0)`,
  `RX_INT_STATUS BIT(1)`, `USBCOM_INT_STATUS BIT(2)`, `DMA_INT_STATUS BIT(3)` — see
  [`mediatek.c`](https://github.com/torvalds/linux/blob/master/drivers/usb/musb/mediatek.c);
  the vendor uses the same `0x105` mask in `musb_otg_reset_usb()`
  ([`usb20_otg_if.c`](https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_otg_if.c)).
* Every write needs a `dsb sy` (posted writes); read back after the group, not after each store, to
  keep the sequence fast.
* Phase 2 is optional and destructive to `MUSB` state — run it **before** Phase 6 or not at all.
* Phase 4/5 are the same register writes the vendor does from `usb_phy_poweron()` +
  `usb_phy_recover()`; if you want the full vendor order, call the slew calibration (§2.2, `0x700`
  cluster) and the efuse `INTR_CAL` (`0x04[23:19] = efuse_idx107 + 2`) between Phase 5 and Phase 6.

---

## 5. Verifying each step from software alone

Everything below is a single 32-bit read at a known address; no host needed.

| # | After step | Read | Pass looks like | Fail means |
|---|---|---|---|---|
| 1 | clocks | `0x10001090` | bit 8 == 0 (ICUSB running) | gate still off → MUSB/PHY register access meaningless |
| 2 | clocks | `0x10000104` | bit 8 == 1 (48 M gate on) | PHY may have no reference clock |
| 3 | clocks | `0x1000C014` | bit 4 == 1 (SSUSB26M on) | 26 M ref absent |
| 4 | clocks | `0x10000090` | bit 23 == 0 (ungated), bit 16 == 1 (parent) | clock mux wrong/off |
| 5 | reset | `0x10003000` | bit 29 == 0 | IP held in reset |
| 6 | **PHY alive** | `0x11CC00F0` | **non-zero** (`VERSION_CODE_*`) | register file dead: gate/rail/aperture (§7) |
| 7 | PHY power-on | `0x11CC0018` | `0x4010_0000` after Phase 4, `0x4010_00F0` after Phase 5 (DISCTH) | write dropped (dead PHY) or partially ignored |
| 8 | PHY power-on | `0x11CC0068` | bits 18, 26 == 0 (suspend/uart released) | still forced |
| 9 | force mode | `0x11CC006C` | **`0x0000_3F2F`** | forcing not applied |
| 10 | force mode | `0x11CC0074` | bit 26 (`AD_USB20_VBUSVALID`) reflects the real cable; bits 24/25 = `AVALID`/`SESSEND` | cable/rail problem if bit26 = 0 with a cable in |
| 11 | **MUSB** | `0x11200060` (`DEVCTL`) | **`0x98`** = `BDEVICE(0x80) | VBUS=11b(0x18)` | still 0 ⇒ PHY not driving UTMI status (back to #6-#9) |
| 12 | host sees pull-up | `0x11200060` | `DEVCTL` stays `0x98` after `POWER=0x60` | if it drops to 0x80, the DP pull-up is not actually on |
| 13 | host reset | `0x1120000A` (`INTRUSB`) | `0x04` (RESET) then `0x10` (CONNECT) | no reset ⇒ no `SOFTCONN`/pull-up, or D+/D- swapped |
| 14 | HS handshake | `0x11200001` (`POWER`) | `0x70` (`SOFTCONN|HSENAB|HSMODE`) after reset | stays `0x60` ⇒ chirp failed ⇒ PHY TX/RX not powered |
| 15 | EP0 alive | `0x11200000` (`FADDR`) | write 0, read 0 | core not responding |
| 16 | identity | `0x1120006C` (`HWVERS`) | Mentor version, e.g. `0x72`/`0x78` family | wrong base |

Interpretation shortcuts:

* **Access widths matter on the MUSB side.** `FADDR`, `POWER`, `INTRUSB`, `INTRUSBE`, `INDEX`,
  `DEVCTL` are 8-bit; `INTRTX/INTRRX/INTRTXE/INTRRXE/FRAME` are 16-bit. A 32-bit read at `0x00`
  returns `FADDR|POWER|INTRTX` packed and a 32-bit write there silently rewrites `POWER`.
* `DEVCTL == 0x98` proves **both** that the PHY's UTMI status lines reach the core and that VBUS is
  being forced; it is the single most useful checkpoint.
* If `DEVCTL` shows `0x80` only, the PHY *is* alive but `VBUSVALID`/`AVALID` are not asserted →
  re-check `0x6C` (step 9) and `0x18[20]` (step 7).
* If `INTRUSB` shows CONNECT but never RESET, the pull-up is present but the HS/FS transceiver is
  not driving SE0 correctly → `0x14` (slew/TX) and `0x00` (PLL/bias) are the next suspects.

---

## 6. Risks — what can hang the interconnect, and what to check first

Ordered by how likely they are to bite a bare-metal shim on this SoC:

1. **Writes to an un-clocked or un-decoded block.** Reads of a gated MTK block usually return `0`
   (which is why the PHY "looks empty"), but **writes can stall the peripheral bus** until the
   interconnect timeout fires. On MT6765 that surfaces as a SError/abort; under XNU that is a panic
   with no useful context. Keep every access inside `0x11cc0000–0x11cc00FF` (PHY), `0x11200000–0x112000FF`
   (MUSB), and the documented clock registers; do **not** walk the 64 KB `reg` windows.
2. **The `+0x800` PHY alias.** Driving `0x11cc08xx` is only known-good from the `usb2jtag` code path.
   Read it first; write it only if the low aperture is confirmed dead **and** the high one is alive.
3. **Read-modify-write discipline.** Three registers in this flow are shared with other subsystems:
   `0x10000090` (`CLK_CFG_5` also holds `sspm_sel`, `dxcc_sel`, `spm_sel`), `0x10000104` (other USB/UFS
   gates), `0x1000C014` (camera/display 26 M gates at bits 5-8). A blind `writel` there can kill the
   display or the camera 26 MHz and take the whole boot down later, silently.
4. **`PERICFG` bit 29 resets the entire USB IP, controller included.** Doing it after Phase 6 wipes
   `POWER`, `INTRUSBE`, `FADDR`, and any EP state; doing it while the MUSB is mid-transfer can hang
   the bus. Always RMW, always before configuring the core.
5. **Gating `ICUSB` back off.** Nothing in the shim should ever write `0x10001080`. If XNU later
   resumes/touches the MUSB with the gate off, accesses time out.
6. **DEVAPC / access-permission firewall.** MT6765 has a device-access-permission controller; a write
   from the wrong domain produces an immediate, silent reboot with no crash dump. This is the main
   reason not to "try" writes in `topckgen_ao` (`0x1001b000`), `sleep` (`0x10006000`), `scpsys`, or
   `pwrap` (`0x1000d000`) while exploring.
7. **`DEVCTL` reality distortion.** Forcing `VBUSVALID/AVALID/SESSEND/IDDIG` (Phase 5) makes the core
   believe a cable is present even if it is not. That is exactly what you want for bring-up, but do
   not use `DEVCTL` as a cable-presence signal afterwards — use PHY `0x74[26]` (`AD_USB20_VBUSVALID`).
8. **MMU off / Device memory.** With the MMU off all accesses are Device-nGnRnE: no speculation, but
   also no cache; every poll loop hits the bus. Add `dsb sy` after write groups and keep polling
   loops short (a 1 ms spin is ~thousands of bus reads).

**Check first, in this order:** `0x10001090[8]` → `0x10000104[8]` → `0x1000C014[4]` →
`0x10000090[23:16]` → `0x10003000[29]` → `0x11cc00F0`. The first five are one-line reads that need
no writes at all and will tell you whether the PHY is even reachable.

---

## 7. Unverified / open questions

1. **LK's exact MT6765 USB/PHY code is not public.** The sequence in §4 is the vendor *kernel*'s
   (which is the same PHY-team sequence LK uses), plus the LK-documented MUSB/QMU stack.
2. **What "LK shuts the PHY down" actually did.** Candidates, in order of plausibility for the
   all-zero PHY reads: (a) `topckgen` 48 M (`0x104[8]`) and/or `usb_top_sel` gate left off — fits
   "MUSB works, PHY reads 0" perfectly because MUSB only needs `ICUSB`; (b) `PERICFG[29]` left
   asserted (but then MUSB would be dead too, so unlikely); (c) `VUSB33` LDO disabled (nothing in
   Linux would re-enable it — the DT has no `vusb-supply`); (d) PHY put in `savecurrent` state only
   (that would *not* make registers read 0).
3. **`REGFPPC` (0xE0) semantics** — the bit names (`USB20_REG`, `USB11_REG`, `CHGDT_REG`, …) suggest
   per-block register-file enables, but no driver in the tree writes it. If `VERSIONC` reads 0 while
   clocks are on, try setting bit 0 and re-reading.
4. **`usb_sif@11210000` / `usb1p_sif@11210000`** — present in the DT, addressed by no driver in the
   vendor tree. Role unknown (possibly the UTMI/OTG "SIF" or a USB1.1 companion block).
5. **`univpll3_d4` frequency** — with MT6765's usual `univ2pll` for `usb20_192m = univpll*2/13 = 192 MHz`
   ⇒ `univpll = 1248 MHz` ⇒ `univpll3_d4 = univpll/20 = 62.4 MHz`. Plausible but not confirmed; it
   does not matter for enumeration as long as the mux/gates are on.
6. **Reset values** of the a60810 register file are not published anywhere I could find; §2.3 lists
   what drivers *program*, which is the only reproducible reference.
7. **The `+0x800` PHY aperture** is inferred from two code paths, not documented.

---

## 8. Sources

Linux (mainline, consulted at `master`):

* `drivers/phy/mediatek/phy-mtk-tphy.c` — T-PHY V1/V2 bank offsets, U2 init/power-on,
  `U2PHYDTM0/DTM1` bit definitions, slew calibration:
  <https://github.com/torvalds/linux/blob/master/drivers/phy/mediatek/phy-mtk-tphy.c>
* `drivers/clk/mediatek/clk-mt6765.c` — `ifr2_cg_regs` (0x80/0x84/0x90), `GATE_IFR2(CLK_IFR_ICUSB,…,8)`,
  `usb_top_sel` mux (`CLK_CFG_5`, shift 16, gate 23, update 22), `apmixed_cg_regs` (0x14) and
  `CLK_APMIXED_SSUSB26M`:
  <https://github.com/torvalds/linux/blob/master/drivers/clk/mediatek/clk-mt6765.c>
* `include/dt-bindings/clock/mt6765-clk.h` — clock IDs (`CLK_TOP_USB_TOP_SEL 118`,
  `CLK_TOP_UNIVPLL3_D4 32`, `CLK_IFR_ICUSB 0`, `CLK_APMIXED_SSUSB26M 12`):
  <https://github.com/torvalds/linux/blob/master/include/dt-bindings/clock/mt6765-clk.h>
* `drivers/clk/mediatek/clk-gate.c` and `clk-mux.c` — gate/mux polarity (`setclr` enable = write CLR
  register; `no_setclr_inv` enable = set bit; mux gate enabled when bit == 0):
  <https://github.com/torvalds/linux/blob/master/drivers/clk/mediatek/clk-gate.c>,
  <https://github.com/torvalds/linux/blob/master/drivers/clk/mediatek/clk-mux.c>
* `drivers/usb/musb/musb_regs.h` — MUSB offsets (`DEVCTL 0x60`, `POWER 0x01`, `INTRUSB 0x0A`,
  `HWVERS 0x6C`) and `DEVCTL` bit fields:
  <https://github.com/torvalds/linux/blob/master/drivers/usb/musb/musb_regs.h>
* `drivers/usb/musb/mediatek.c` — MTK glue, `USB_L1INTS 0xA0` / `USB_L1INTM 0xA4`, `TXTOGEN 0x86`,
  `RXTOGEN 0x82`, role switching that pokes `DEVCTL`:
  <https://github.com/torvalds/linux/blob/master/drivers/usb/musb/mediatek.c>

U-Boot:

* `drivers/phy/phy-mtk-tphy.c` — compact V1/V2 reference; `u2_phy_instance_init()` writes
  `DTM0(DTM0)`, `DTM1`, `ACR0/5/6`, and `u2_phy_instance_power_on()` sets `VBUSVALID|AVALID`,
  clears `SESSEND` in `U2PHYDTM1`:
  <https://github.com/u-boot/u-boot/blob/master/drivers/phy/phy-mtk-tphy.c>

MT6765 vendor kernel (Redmi 9A / dandelion tree):

* `drivers/misc/mediatek/usb20/mt6765/mtk-phy-a60810.h` — full a60810 register/field map:
  <https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/mtk-phy-a60810.h>
* `.../usb20_phy.c` — `usb_phy_poweron()`, `usb_phy_recover()`, `set_usb_phy_mode()`,
  `usb_rev6_setting()`, `hs_slew_rate_cal()`, clock bracket helpers:
  <https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_phy.c>
* `.../usb20.c` — clock names (`usb0`, `usb0_clk_top_sel`, `usb0_clk_univpll3_d4`), `mt_usb_enable()`
  ordering, `vusb`/`va12` regulators, `USB_L1INTM` masks:
  <https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20.c>
* `.../usb20_otg_if.c` — `musb_otg_reset_usb()`: `PERICFG+0x00` bit 29 pulse + `usb_phy_poweron()` +
  `L1INTM=0x105`, `INTRTXE=1`, `INTRUSBE=0xF7`:
  <https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/misc/mediatek/usb20/mt6765/usb20_otg_if.c>
* `drivers/clk/mediatek/clk-mt6765.c` (vendor copy) — same gate layout, with the USB 48 M / SSUSB26M
  gates commented out:
  <https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/drivers/clk/mediatek/clk-mt6765.c>
* `include/dt-bindings/clock/mt6765-clk.h` (vendor copy) — `CLK_IFR_ICUSB 8` (matches `clocks = <&infracfg_ao 8>`):
  <https://github.com/plmzaq2112/android_kernel_mt6765/blob/main/include/dt-bindings/clock/mt6765-clk.h>

LK:

* `platform/common/mt_musb/mt_musb.c` exists in the MT6762/6765 LK and carries fastboot's MUSB/QMU
  traffic (only the ZLP fix is public):
  <https://github.com/calan152/oppo-a1k-lk/blob/main/patches/0001-oppo-a1k-cph1923-port.patch>
* Fastboot behaviour of that port (not register-level, but confirms the stack):
  <https://github.com/calan152/oppo-a1k-lk/blob/main/docs/FASTBOOT.md>

Local (this workspace):

* `D:\aios\stock_boot.dtb` — the handset's own device tree, parsed for §1 (node names, `reg`,
  `interrupts`, `clocks`, `clock-names`, `mode`, absence of `vusb-supply`).
* `D:\aios\research\_dtbdump.py` — the FDT parser used for that (no `dtc` needed; run
  `python _dtbdump.py usb` / `... node usb0@` / `... all`).
* Local snapshots of the cited sources, kept for offline verification:
  `_a60810.h` (mtk-phy-a60810.h), `_usb20.c`, `_usb20_phy.c`, `_usb20_otg_if.c`,
  `_usb20_phy_debugfs.c`, `_mtk_musb_reg.h`, `_musb_core.h`, `_v_clk-mt6765.c`,
  `_v_mt6765-clk.h`, `_v_musb.h`, `_ml_clk-gate.c`, `_ml_clk-mux.c`, `_ml_musb_core.h`,
  `_cand_musb_regs.h`, `_lk.patch`. (No local `phy-mtk-t50.c`: that path 404s upstream.)
