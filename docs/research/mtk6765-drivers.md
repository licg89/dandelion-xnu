# MT6762G / MT6765 (Redmi 9A "dandelion") — register-level driver extraction for a bare-metal EL1 shim

Date: 2026-10-27. Target: Xiaomi Redmi 9A (MT6762G), LK bootloader, shim at EL1 with **MMU off**, no cache maintenance available, polled I/O only.

All register values below are quoted from source that I cloned and read locally. Every claim is tagged:

- **[V]** verified — read directly in the tree, file+line quoted.
- **[D]** derived — computed from quoted code (bitfield math), not observed on silicon.
- **[U]** unverified — could not confirm from source; do not trust blindly.

---

## 0. Sources (what was actually cloned)

### 0.1 Vendor Linux kernel — FOUND

| item | value |
|---|---|
| repo | `https://github.com/MiCode/Xiaomi_Kernel_OpenSource` |
| branch | `dandelion-q-oss` |
| commit | `8974b91` — *"Kernel: Xiaomi kernel changes for Redmi 9C, Redmi POCO C3 and Redmi 9A Android Q"* |
| kernel | 4.19 (Android Q) |
| local | `D:\aios\research\refs\mt6765-vendor` (shallow + blobless; 67,792 files in tree) |

This is the Redmi 9A (dandelion) / MT6762G vendor tree. **[V]**

### 0.2 MediaTek LK (little kernel) for MT6765 — FOUND

| item | value |
|---|---|
| repo | `https://github.com/svoboda18/lk` |
| branch | `main`, commit `37ec8bc` — *"hack: skip oem unlock check when unlocking bootloader"* |
| local | `D:\aios\research\refs\lk-svoboda18` |

Critically, this tree contains a genuine **`platform/mt6765/`** port plus `k62*` targets (`target/k62v1_64_bsp`, `k62v1_32_bsp`, `k62mv1_32_bsp`, …). `k62` is the MediaTek codename for the MT6765/MT6762 family, so this is *the same bootloader code base that runs on dandelion before the shim*. **[V]**

> Provenance note: `lk-svoboda18` was already present under `refs/` from earlier work in this workspace rather than cloned by this session; I verified it in place (`git log -1` → `37ec8bc`, `platform/mt6765/` present, LK source read directly). Same for `refs/oppo-a1k-lk`, which I inspected and rejected.

Other LK trees checked and rejected: `l33tnoob/lk_mtk` (7,859 files, no mt676x targets — older platform), `calan152/oppo-a1k-lk` (only a port patch + CI files, no LK source).

> Note: the MTK LK files carry a MediaTek confidentiality header. They are public on GitHub; treat as reference data, not as code you can relicense.

### 0.3 Path corrections vs. the original brief

The paths guessed in the brief are wrong for this tree. Actual locations:

| brief said | actually is |
|---|---|
| `drivers/mmc/host/mtk-sd.c` | **`drivers/mmc/host/mediatek/ComboA/`** — `sd.c` (main engine), `msdc_reg.h`, `mtk_sd.h`, `mtk_sd_misc.c`, `msdc_tune.c`, `autok.c`, `mt6765/msdc_cust.{c,h}`, `mt6765/msdc_io.h`. (`drivers/mmc/host/mtk-sd.c` also exists but is the mainline-style driver, **not** the one this device uses.) |
| `drivers/usb/musba/musb_hcd.c` | `drivers/misc/mediatek/usb20/` (MUSB-based): `mt6765/usb20.c`, `mt6765/usb20_phy.c`, `mt6765/mtk-phy-a60810.h`, `mtk_musb_reg.h` |
| `drivers/misc/mediatek/usb20/*` | correct, but the SoC-specific parts are under `usb20/mt6765/` |
| `arch/arm64/boot/dts/mediatek/mt6765.dtsi` | `arch/arm64/boot/dts/mediatek/mt6765.dts` (a `.dts`, not `.dtsi`) + `cust_mt6765_msdc.dtsi` + `dandelion.dts` |
| watchdog | two drivers exist; the device uses **`drivers/watchdog/mediatek/wdt/common/wdt_v2/mtk_wdt_v2.c`** (`CONFIG_MEDIATEK_WATCHDOG=y`), not `drivers/watchdog/mtk_wdt.c` |

Device config (`D:\aios\research\stock_kernel.config`, extracted from the phone's own kernel — pre-existing artifact, not re-verified against the phone) says: `CONFIG_MEDIATEK_WATCHDOG=y`, `CONFIG_MSDC…` vendor stack on, `CONFIG_MTK_EMMC_CQ_SUPPORT=y`, `CONFIG_USB_MTK_HDRC=y`, `CONFIG_USB_MTK_OTG=y`, `CONFIG_MTK_GIC_EXT=y`, `CONFIG_MTK_DEVAPC=y`, `CONFIG_MTK_UART_USB_SWITCH` **not set**, `CONFIG_MTK_HW_FDE` **not set**. **[V]** (config only)

---

## 1. MSDC / eMMC — polled PIO write to an arbitrary LBA

### 1.1 Physical addresses

From `arch/arm64/boot/dts/mediatek/mt6765.dts` **[V]**:

```
1999: 	msdc0:msdc@11230000 {
2000: 		compatible = "mediatek,msdc";
2001: 		reg = <0 0x11230000 0 0x10000>;
2002: 		interrupts = <GIC_SPI 79 IRQ_TYPE_LEVEL_LOW>;
```

```
2025: 	msdc0_top@11cd0000 {
2026: 		compatible = "mediatek,msdc0_top";
2027: 		reg = <0 0x11cd0000 0 0x1000>;
```

```
2156: 	msdc0_pad_macro@11cd0000 {
2157: 		compatible = "mediatek,msdc0_pad_macro";
2158: 		reg = <0 0x11cd0000 0 0x10000>;
```

Cross-checked in LK `platform/mt6765/include/platform/mt_reg_base.h` **[V]**:

```
258: #define MSDC0_BASE (0x11230000)
259: #define MSDC0_TOP_BASE (0x11CD0000)
262: #define MSDC1_BASE (0x11240000)
```

So for the shim:

| block | base | notes |
|---|---|---|
| MSDC0 controller | **0x1123_0000** | all `MSDC_*` / `SDC_*` / `EMMC_*` registers |
| MSDC0 top / pad macro | **0x11CD_0000** | pad & DS/clock trim block, *not* the controller registers |
| MSDC0 IO pad config | **0x1000_2000** | `MSDC0_IO_PAD_BASE` (drive strength, tdsel/rdsel, pull) |
| GPIO mode | **0x1000_5000** | `MSDC0_GPIO_MODE0 = +0x3f0`, `MODE1 = +0x400` |

### 1.2 Controller register map

`drivers/mmc/host/mediatek/ComboA/msdc_reg.h` **[V]**:

```
20: #define OFFSET_MSDC_CFG                 (0x0)
21: #define OFFSET_MSDC_IOCON               (0x04)
22: #define OFFSET_MSDC_PS                  (0x08)
23: #define OFFSET_MSDC_INT                 (0x0c)
24: #define OFFSET_MSDC_INTEN               (0x10)
25: #define OFFSET_MSDC_FIFOCS              (0x14)
26: #define OFFSET_MSDC_TXDATA              (0x18)
27: #define OFFSET_MSDC_RXDATA              (0x1c)
28: #define OFFSET_SDC_CFG                  (0x30)
29: #define OFFSET_SDC_CMD                  (0x34)
30: #define OFFSET_SDC_ARG                  (0x38)
31: #define OFFSET_SDC_STS                  (0x3c)
32: #define OFFSET_SDC_RESP0                (0x40)
36: #define OFFSET_SDC_BLK_NUM              (0x50)
38: #define OFFSET_SDC_CSTS                 (0x58)
42: #define OFFSET_EMMC_CFG0                (0x70)
43: #define OFFSET_EMMC_CFG1                (0x74)
44: #define OFFSET_EMMC_STS                 (0x78)
45: #define OFFSET_EMMC_IOCON               (0x7c)
57: #define OFFSET_MSDC_PATCH_BIT0          (0xb0)
58: #define OFFSET_MSDC_PATCH_BIT1          (0xb4)
59: #define OFFSET_MSDC_PATCH_BIT2          (0xb8)
66: #define OFFSET_MSDC_PAD_TUNE0           (0xf0)
68: #define OFFSET_MSDC_DAT_RDDLY0          (0xf8)
73: #define OFFSET_MSDC_VERSION             (0x114)
74: #define OFFSET_MSDC_ECO_VER             (0x118)
84: #define OFFSET_EMMC50_CFG0              (0x208)
85: #define OFFSET_EMMC50_CFG1              (0x20c)
89: #define OFFSET_SDC_FIFO_CFG             (0x228)
```

`SDC_RESP1..3` are `0x44 / 0x48 / 0x4c`. **[V]**

> **Name mapping (important).** The brief asks for `MSDC_RSP` and `MSDC_STA`. **Those names do not exist in this driver.** The legacy MT6577-era driver used `MSDC_RSP`/`MSDC_STA`; this IP calls them:
> - `MSDC_RSP` → **`SDC_RESP0..SDC_RESP3`** at `0x40..0x4c`
> - `MSDC_STA` → **`SDC_STS`** at `0x3c` (busy/status) and **`MSDC_PS`** at `0x08` (pin state)
> - `MSDC_DAT` → **`MSDC_TXDATA` (0x18) / `MSDC_RXDATA` (0x1c)** (PIO FIFO ports, not a shared DAT register)
> - `EMMC_CFG` → **`EMMC_CFG0` (0x70) / `EMMC_CFG1` (0x74)** **[V]**

### 1.3 Bit fields

**MSDC_CFG (0x00)** — `msdc_reg.h:514-533` **[V]**:

```
514: #define MSDC_CFG_MODE                   (0x1  << 0)     /* RW */
515: #define MSDC_CFG_CKPDN                  (0x1  << 1)     /* RW */
516: #define MSDC_CFG_RST                    (0x1  << 2)     /* A0 */
517: #define MSDC_CFG_PIO                    (0x1  << 3)     /* RW */
518: #define MSDC_CFG_CKDRVEN                (0x1  << 4)     /* RW */
519: #define MSDC_CFG_BV18SDT                (0x1  << 5)     /* RW */
520: #define MSDC_CFG_BV18PSS                (0x1  << 6)     /* R  */
521: #define MSDC_CFG_CKSTB                  (0x1  << 7)     /* R  */
522: #define MSDC_CFG_CKDIV                  (0xfff << 8)    /* RW */
523: #define MSDC_CFG_CKDIV_BITS             (12)
524: #define MSDC_CFG_CKMOD                  (0x3  << 20)    /* W1C */
525: #define MSDC_CFG_CKMOD_BITS             (2)
526: #define MSDC_CFG_CKMOD_HS400            (0x1  << 22)    /* RW */
```

**SDC_CFG (0x30)** — `msdc_reg.h:604-611` **[V]**:

```
604: #define SDC_CFG_SDIOINTWKUP             (0x1  << 0)     /* RW */
605: #define SDC_CFG_INSWKUP                 (0x1  << 1)     /* RW */
606: #define SDC_CFG_WRDTOC                  (0x1fff  << 2)  /* RW */
607: #define SDC_CFG_BUSWIDTH                (0x3  << 16)    /* RW */
608: #define SDC_CFG_SDIO                    (0x1  << 19)    /* RW */
609: #define SDC_CFG_SDIOIDE                 (0x1  << 20)    /* RW */
610: #define SDC_CFG_INTATGAP                (0x1  << 21)    /* RW */
611: #define SDC_CFG_DTOC                    (0xffUL << 24)  /* RW */
```

**SDC_CMD (0x34)** — `msdc_reg.h:614-624` **[V]**:

```
614: #define SDC_CMD_OPC                     (0x3f << 0)     /* RW */
615: #define SDC_CMD_BRK                     (0x1  << 6)     /* RW */
616: #define SDC_CMD_RSPTYP                  (0x7  << 7)     /* RW */
617: #define SDC_CMD_DTYP                    (0x3  << 11)    /* RW */
618: #define SDC_CMD_RW                      (0x1  << 13)    /* RW */
619: #define SDC_CMD_STOP                    (0x1  << 14)    /* RW */
620: #define SDC_CMD_GOIRQ                   (0x1  << 15)    /* RW */
621: #define SDC_CMD_BLKLEN                  (0xfff << 16)   /* RW */
622: #define SDC_CMD_AUTOCMD                 (0x3  << 28)    /* RW */
623: #define SDC_CMD_VOLSWTH                 (0x1  << 30)    /* RW */
624: #define SDC_CMD_ACMD53                  (0x1UL << 31)   /* RW */
```

**SDC_STS (0x3c)** — `msdc_reg.h:627-633` **[V]**:

```
627: #define SDC_STS_SDCBUSY                 (0x1  << 0)     /* RW  */
628: #define SDC_STS_CMDBUSY                 (0x1  << 1)     /* RW  */
629: #define SDC_STS_CMD_WR_BUSY             (0x1  << 16)    /* W1C */
630: #define SDC_STS_DAT_TMO_TYPE            (0x3  << 17)    /* RO  */
631: #define SDC_STS_CMD_TMO_TYPE            (0x3  << 19)    /* RO  */
632: #define SDC_STS_DVFS_LEVEL              (0xf  << 27)    /* RO  */
633: #define SDC_STS_SWR_COMPL               (0x1  << 31)    /* RO  */
```

**MSDC_INT / MSDC_INTEN (0x0c / 0x10)** — the bits a polling driver cares about, `msdc_reg.h:580-596` **[V]**:

```
580: #define MSDC_INT_CMDRDY                 (0x1  << 8)     /* W1C */
581: #define MSDC_INT_CMDTMO                 (0x1  << 9)     /* W1C */
582: #define MSDC_INT_RSPCRCERR              (0x1  << 10)    /* W1C */
584: #define MSDC_INT_XFER_COMPL             (0x1  << 12)    /* W1C */
586: #define MSDC_INT_DATTMO                 (0x1  << 14)    /* W1C */
587: #define MSDC_INT_DATCRCERR              (0x1  << 15)    /* W1C */
```

**MSDC_FIFOCS (0x14)** — `msdc_reg.h:599-601` **[V]**:

```
599: #define MSDC_FIFOCS_RXCNT               (0xff << 0)     /* R  */
600: #define MSDC_FIFOCS_TXCNT               (0xff << 16)    /* R  */
601: #define MSDC_FIFOCS_CLR                 (0x1UL << 31)   /* RW */
```

**MSDC_IOCON (0x04)** — used only for DDR/HS400 sampling here; for a single-rate PIO port program it as **0x0** (see §1.6). Relevant bits, `msdc_reg.h:543-562` **[V]**: `RSPL (1<<1)`, `R_D_SMPL (1<<2)`, `DDLSEL (1<<3)`, `DDR50CKD (1<<4)`, `W_D_SMPL (1<<8)`, `R_D0SPL..R_D7SPL (1<<16..23)`.

**MSDC_PS (0x08)** — `msdc_reg.h:565-570` **[V]**: `CDEN (1<<0)`, `CDSTS (1<<1)`, `CDDEBOUNCE (0xf<<12)`, `DAT (0xff<<16)`, `CMD (1<<24)`, `WP (1<<31)`.

**EMMC_CFG0 / EMMC_CFG1 (0x70 / 0x74)** — boot-partition engine, `msdc_reg.h:654-663` **[V]**:

```
654: #define EMMC_CFG0_BOOTSTART             (0x1  << 0)     /* W  */
655: #define EMMC_CFG0_BOOTSTOP              (0x1  << 1)     /* W  */
656: #define EMMC_CFG0_BOOTMODE              (0x1  << 2)     /* RW */
657: #define EMMC_CFG0_BOOTACKDIS            (0x1  << 3)     /* RW */
658: #define EMMC_CFG0_BOOTWDLY              (0x7  << 12)    /* RW */
659: #define EMMC_CFG0_BOOTSUPP              (0x1  << 15)    /* RW */
662: #define EMMC_CFG1_BOOTDATTMC            (0xfffff << 0)  /* RW */
663: #define EMMC_CFG1_BOOTACKTMC            (0xfffUL << 20) /* RW */
```

**This is a negative result worth flagging:** the kernel driver touches **only `EMMC_IOCON.BOOTRST`** and never programs `EMMC_CFG0/1`. Grepping the whole ComboA driver for `EMMC_CFG0|EMMC_CFG1|EMMC_STS` yields exactly two hits, both `EMMC_IOCON` **[V]**:

```
sd.c:907: MSDC_CLR_BIT32(EMMC_IOCON, EMMC_IOCON_BOOTRST);
sd.c:909: MSDC_SET_BIT32(EMMC_IOCON, EMMC_IOCON_BOOTRST);
```

i.e. the eMMC boot *hardware* engine (`EMMC_CFG0.BOOTSTART`) is only used by preloader/LK, not by Linux. For the shim, **do not touch EMMC_CFG0/1** — leave whatever LK left there; just do normal CMD24/25 data transfers.

### 1.4 Clock: source, gating, and divider

**Source.** `mt6765/msdc_cust.h` **[V]**:

```
#define MSDCPLL_FREQ            800000000
#define MSDC0_SRC_0             260000
#define MSDC0_SRC_1             (MSDCPLL_FREQ/2)
```
so MSDC0 sources are **26 MHz** (`MSDC0_SRC_0`) or **400 MHz** (`MSDC0_SRC_1` = MSDCPLL/2).

`cust_mt6765_msdc.dtsi` selects the 400 MHz one and 8-bit bus **[V]**:

```
&msdc0 {
	index = /bits/ 8 <0>;
	clk_src = /bits/ 8 <MSDC0_CLKSRC_400MHZ>;
	bus-width = <8>;
	max-frequency = <200000000>;
	cap-mmc-highspeed;
	mmc-ddr-1_8v;
	mmc-hs200-1_8v;
	mmc-hs400-1_8v;
	no-sd;
	no-sdio;
	non-removable;
	host_function = /bits/ 8 <MSDC_EMMC>;
	bootable;
	clocks = <&infracfg_ao CLK_IFR_MSDC0_SRC>,
		<&infracfg_ao CLK_IFR_MSDC0>,
		<&infracfg_ao CLK_IFR_FAES_FDE>;
	clock-names = "msdc0-clock", "msdc0-hclock", "msdc0-aes-clock";
};
```

**Clock gates to enable (infracfg_ao @ 0x1000_1000).** From `drivers/clk/mediatek/clk-mt6765.c` **[V]**:

```
890: 	GATE_IFR3(CLK_IFR_MSDC0, "ifr_msdc0", "msdc5hclk", 2),
923: 	GATE_IFR5(CLK_IFR_MSDC0_SELF, "ifr_msdc0sf", "msdc50_0_ck", 0),
928: 	GATE_IFR5(CLK_IFR_MSDC0_SRC, "ifr_msdc0_clk", "msdc50_0_ck", 9),
```

with the register offsets **[V]**:

```
793: static const struct mtk_gate_regs ifr3_cg_regs = {
794: 	.set_ofs = 0x88,
795: 	.clr_ofs = 0x8c,
796: 	.sta_ofs = 0x94,
...
805: static const struct mtk_gate_regs ifr5_cg_regs = {
806: 	.set_ofs = 0xc0,
807: 	.clr_ofs = 0xc4,
808: 	.sta_ofs = 0xc8,
```

`GATE_IFR3`/`GATE_IFR5` both use `&mtk_clk_gate_ops_setclr` (`clk-mt6765.c:835, 862`), i.e. "write 1 to `set_ofs` to ungate". So for a cold shim:

```
infracfg_ao_wr(0xC0, 1 << 9);   /* CLK_IFR_MSDC0_SRC  (msdc0 bus clock from msdcpll)  */
infracfg_ao_wr(0x88, 1 << 2);   /* CLK_IFR_MSDC0      (msdc5hclk, APB hclk)          */
```
then read back `0xC8` / `0x94` to confirm the bits are 1. **[D]** (derived from gate table + ops; not traced on silicon)

Also confirm the top-level mux `CLK_TOP_MSDC50_0_SEL` is set to the PLL path. `clk-mt6765.c` **[V]**:

```
644: 	MUX_CLR_SET_UPD(CLK_TOP_MSDC50_0_SEL, "msdc50_0_sel",
645: 	msdc50_0_parents, CLK_CFG_3, CLK_CFG_3_SET, CLK_CFG_3_CLR,
```
```
270: #define CLK_CFG_5 0x90
```
(`CLK_CFG_3` is `0x84`–`0x8c` in the same block; **[U]** — I did not read its numeric value. The vendor's own clock dump in `mt6765/msdc_cust.c:541-545` prints `topckgen_base + 0x70` with the expected pattern `bit[1:0]=01b, bit[7]=0, bit[10:8]=001b, bit[15]=0, bit[18:16]=001b, bit[23]=0`, which is a practical "is the MSDC0 mux sane" check to reuse.)

There is also a **runtime sanity read** the vendor does, useful as an assertion in the shim (`mt6765/msdc_cust.c:554-570`) **[V]**:

```
" infracfg_ao [0x%p]=0x%x(should bit[2]=0b,bit[4]=0b)\n",   infracfg_ao_base + 0x94
" infracfg_ao [0x%p]=0x%x(should bit[10:9]=00b)\n",        infracfg_ao_base + 0xc8
```
i.e. `0x10001094` bits 2 and 4 must read 0, and `0x100010c8` bits 10:9 must read 00, for MSDC0 to be clocked correctly.

**Divider math.** `MSDC_CFG.CKDIV` (`[19:8]`) plus `MSDC_CFG.CKMOD` (`[21:20]`). Verbatim from `sd.c:816-830` **[V]**:

```
	} else if (hz >= hclk) {
		mode = 0x1; /* no divisor */
		div  = 0;
		sclk = hclk;
	} else {
		mode = 0x0; /* use divisor */
		if (hz >= (hclk >> 1)) {
			div  = 0;         /* mean div = 1/2 */
			sclk = hclk >> 1; /* sclk = hclk / 2 */
		} else {
			div  = (hclk + ((hz << 2) - 1)) / (hz << 2);
			sclk = (hclk >> 2) / div;
		}
	}
```

So with `hclk = 400 MHz`, for `hz < 200 MHz`:

> **sclk = 100 MHz / CKDIV**, and `CKDIV = ceil(400 MHz / (4·hz))` — i.e. **`CKDIV = round(100 MHz / hz)`**.

**[D]** Worked values for a bootloader:

| target sclk | CKMOD | CKDIV (decimal / hex) | MSDC_CFG[21:8] |
|---|---|---|---|
| 400 MHz | 1 (bypass) | 0 | `0x001000` |
| 200 MHz | 0 | 0 | `0x000000` |
| **50 MHz** | 0 | **2** (`0x002`) | `0x000200` |
| **26 MHz** | 0 | **4** (`0x004`) | `0x000400` |
| **20 MHz** | 0 | **5** (`0x005`) | `0x000500` |
| **12.5 MHz** | 0 | **8** (`0x008`) | `0x000800` |
| **400 kHz** (ident/card-init) | 0 | **250** (`0x0FA`) | `0x00FA00` |

Note the table is *requested* → register value; the resulting sclk is `100 MHz / CKDIV`, so e.g. "26 MHz" lands on **25 MHz** (400 MHz is not divisible by 26 MHz). 200 MHz is the ceiling in mode 0; only mode 1 (bypass) reaches the full 400 MHz. **[D]**

Recommendation for a polled PIO port: **26 MHz or 20 MHz, mode 0, 8-bit**. At 50 MHz a byte-polled FIFO loop risks underrun on a cache-off path; 26 MHz is 4× slower than the hardware's capability but immune to poll-loop jitter. **[D]** (engineering judgement, not from source)

The clock-stable handshake is mandatory and is a simple poll (`sd.c:446-484`) **[V]**:

```
460: 		msdc_retry(!(MSDC_READ32(MSDC_CFG) & MSDC_CFG_CKSTB), retry, cnt, host->id);
...
472: 		MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_CKDIV, div);
473: 		msdc_retry(!(MSDC_READ32(MSDC_CFG) & MSDC_CFG_CKSTB), retry, cnt, host->id);
```

Sequence: write CKMOD+CKDIV with a "dummy" `div+1`, poll **CKSTB (bit 7) == 1**; then write the real `CKDIV`, poll **CKSTB == 1** again; then soft-reset the controller. **[V]**

### 1.5 Reset, bus width, init

Reset is `MSDC_CFG.RST` (bit 2, self-clearing) + FIFO clear + INT clear — `mtk_sd.h:584-603` **[V]**:

```
584: #define msdc_reset(id) \
586: 		int retry = 3, cnt = 1000; \
587: 		MSDC_SET_BIT32(MSDC_CFG, MSDC_CFG_RST); \
588: 		msdc_retry(MSDC_READ32(MSDC_CFG) & MSDC_CFG_RST, retry, \
...
592: #define msdc_clr_int() \
594: 		u32 val = MSDC_READ32(MSDC_INT); \
595: 		MSDC_WRITE32(MSDC_INT, val); \
...
598: #define msdc_reset_hw(id) \
600: 		msdc_reset(id); \
601: 		msdc_clr_fifo(id); \
602: 		msdc_clr_int(); \
```
FIFO clear = set `MSDC_FIFOCS.CLR` (bit 31) and poll until it self-clears (`sd.c:442-443`) **[V]**.

Bus width — `sd.c:914-935` **[V]**, with `MSDC_BUS_1BITS=0, 4BITS=1, 8BITS=2` (`mtk_sd.h:127-129`):

```
917: 	u32 val = MSDC_READ32(SDC_CFG);
919: 	val &= ~SDC_CFG_BUSWIDTH;
...
929: 	case MMC_BUS_WIDTH_8:
930: 		val |= (MSDC_BUS_8BITS << 16);
933: 	MSDC_WRITE32(SDC_CFG, val);
```
→ **8-bit = write `2 << 16` into SDC_CFG[17:16].** Yes, the bootloader/driver really does run 8-bit on msdc0 (`cust_msdc.c` in LK: `8, /* data pins */`). **[V]**

Hardware init order from `sd.c:937-1026` **[V]** (abbreviated, this is the exact order a shim should mirror):

```
945: 	MSDC_SET_FIELD(MSDC_CFG, MSDC_CFG_MODE, MSDC_SDMMC);   /* MSDC_SDMMC == 1 */
953: 	msdc_reset_hw(host->id);
956: 	MSDC_CLR_BIT32(MSDC_PS, MSDC_PS_CDEN);                 /* card detect off */
963: 	MSDC_CLR_BIT32(MSDC_INTEN, MSDC_READ32(MSDC_INTEN));    /* mask all ints */
964: 	MSDC_WRITE32(MSDC_INT, MSDC_READ32(MSDC_INT));          /* clear all ints */
977: 	MSDC_SET_BIT32(SDC_CFG, SDC_CFG_SDIO);                  /* mandatory, see comment */
983: 	MSDC_CLR_BIT32(SDC_CFG, SDC_CFG_SDIOIDE);
986: 	msdc_set_smt(host, 1);
988: 	msdc_set_driving(host, &host->hw->driving);
990: 	msdc_set_pin_mode(host);
995: 	MSDC_SET_FIELD(MSDC_PATCH_BIT0, MSDC_PB0_DETWR_CRCTMO, 1);
998: 	MSDC_SET_FIELD(SDC_CFG, SDC_CFG_DTOC, DEFAULT_DTOC);    /* DEFAULT_DTOC == 3 */
1000: 	msdc_set_buswidth(host, MMC_BUS_WIDTH_1);
```

**PIO mode is not set in `init_hw`.** It is toggled by the DMA/PIO switch macros, `sd.c:157-162` **[V]**:

```
157: 	MSDC_CLR_BIT32(MSDC_CFG, MSDC_CFG_PIO); },
...
161: #define msdc_dma_on()           MSDC_CLR_BIT32(MSDC_CFG, MSDC_CFG_PIO)
162: #define msdc_dma_off()          MSDC_SET_BIT32(MSDC_CFG, MSDC_CFG_PIO)
```
→ **PIO = `MSDC_CFG` bit 3 = 1**; DMA = bit 3 = 0. For the shim: **set `MSDC_CFG_PIO`** (and never build a GPD/BD descriptor). Also note `sd.c:5133` sets PIO unconditionally in one resume path. **[V]**

### 1.6 Pad / IO settings for msdc0 (0x11CD0000 and 0x10002000)

`mt6765/msdc_cust.h` **[V]**:

```
#define MSDC0_IO_PAD_BASE       (msdc_io_cfg_bases[0])  /* 0x10002000 */
#define MSDC0_GPIO_MODE0       (MSDC_GPIO_BASE + 0x3f0)
#define MSDC0_GPIO_MODE1       (MSDC_GPIO_BASE + 0x400)
#define MSDC0_GPIO_MODE_TRAP       (MSDC_GPIO_BASE + 0x6f0)
#define MSDC0_GPIO_IES     (MSDC0_IO_PAD_BASE + 0x10)
#define MSDC0_GPIO_SMT     (MSDC0_IO_PAD_BASE + 0x80)
#define MSDC0_GPIO_TDSEL   (MSDC0_IO_PAD_BASE + 0x90)
#define MSDC0_GPIO_RDSEL   (MSDC0_IO_PAD_BASE + 0x70)
#define MSDC0_GPIO_DRV     (MSDC0_IO_PAD_BASE + 0)
#define MSDC0_GPIO_PUPD   (MSDC0_IO_PAD_BASE + 0x30)
#define MSDC0_GPIO_R0   (MSDC0_IO_PAD_BASE + 0x50)
#define MSDC0_GPIO_R1   (MSDC0_IO_PAD_BASE + 0x60)
```

Pin-mux (`mt6765/msdc_cust.c:827-842`) **[V]** — this is the one to copy verbatim-ish. It is conditional on a trap register:

```
827: void msdc_set_pin_mode(struct msdc_host *host)
828: {
829: 	if (host->id == 0) {
830: 		if ((MSDC_READ32(MSDC0_GPIO_MODE_TRAP) & (0x3 << 13)) == 0) {
831: 			MSDC_SET_FIELD(MSDC0_GPIO_MODE0,
832: 				0x777777 << 8, 0x222222);
833: 			MSDC_SET_FIELD(MSDC0_GPIO_MODE1,
834: 				0x777777 << 0, 0x222222);
835: 		} else {
836: 			MSDC_SET_FIELD(MSDC0_GPIO_MODE0,
837: 				0x777777 << 8, 0x111111);
838: 			MSDC_SET_FIELD(MSDC0_GPIO_MODE1,
839: 				0x777777 << 0, 0x111111);
840: 		}
841: 	}
```

So: **read `0x100056F0` (MODE_TRAP); if bits [14:13]==0 use mux value 2, else 1**, applied to 6 pins in `MODE0[25:8]` and 6 pins in `MODE1[23:0]`. **[V]**

Drive strength — `msdc_set_driving_by_id` (`mt6765/msdc_cust.c:968-990`) **[V]** uses `MSDC0_GPIO_DRV` bitfields:
`ds_drv = [17:15]`, `rst_drv = [20:18]`, `cmd_drv = [11:9]`, `clk_drv = [8:6]`, `dat_drv = [14:12]`. The DT default (`cust_mt6765_msdc.dtsi`) is `drive-strength = <3>` for cmd/dat/clk/rst/ds on the default (non-HS200/400) node. **[V]**

Pull config — `msdc_pin_config_by_id` (`mt6765/msdc_cust.c:1028-1068`) **[V]**, with the vendor's own decode table in the comment above it:

```
1016: /* msdc pin config
1017:  * MSDC0
1018:  * PUPD/R1/R0
1019:  * 0/0/0: High-Z
1020:  * 0/1/0: Pull-up with 50Kohm
1021:  * 0/0/1: Pull-up with 10Kohm
1022:  * 0/1/1: Pull-up with 50Kohm//10Kohm
1023:  * 1/0/0: High-Z
1024:  * 1/1/0: Pull-down with 50Kohm
1025:  * 1/0/1: Pull-down with 10Kohm
1026:  * 1/1/1: Pull-down with 50Kohm//10Kohm
1027:  */
```
and for the "pull-up" case (what LK/kernel use at boot):
```
1042: 		} else if (mode == MSDC_PIN_PULL_UP) {
1046: 			MSDC_SET_FIELD(MSDC0_GPIO_PUPD, 0xFFF, 0x401);
1047: 			MSDC_SET_FIELD(MSDC0_GPIO_R0, 0xFFF, 0xBFE);
1048: 			MSDC_SET_FIELD(MSDC0_GPIO_R1, 0xFFF, 0x401);
```
Comment: `cmd:pu10k, clk:pd50k, dat:pu10k, rstb:pu10k, dsl:pd50k`. **[V]**

> **msdc0_top at 0x11CD0000 is *not* where the pad settings live.** In this tree `MSDC0_TOP_BASE` is only used for **HS200/HS400 delay-line tuning** (`msdc_tune.c`, `autok.c`, `autok_dvfs.c` — `host->base_top`), i.e. `EMMC50_PAD_*_TUNE`-style registers. The kernel maps it at `mt6765/msdc_cust.c:1243-1250` **[V]**:
> ```
> 1244: 	if (host->hw->host_function == MSDC_EMMC) {
> 1245: 		np = of_find_compatible_node(NULL, NULL, "mediatek,msdc0_top");
> 1246: 		host->base_top = of_iomap(np, 0);
> ```
> For a single-rate PIO port you do **not** need 0x11CD0000 at all: the pads are in `0x10002000` / GPIO `0x10005000`, and sampling/delay registers you need are in the controller (`MSDC_PAD_TUNE0` @0xF0, `MSDC_DAT_RDDLY0` @0xF8, `EMMC50_CFG0` @0x208, `MSDC_IOCON` @0x04). **[V]** for the mapping; the claim "not needed" is **[D]**.

### 1.7 Exact CMD sequence and response checking

Response-type encoding (`msdc_rsp[]`, `sd.c`) **[V]** — this is the raw `SDC_CMD.RSPTYP` value, not the MMC core enum:

```
	0,                      /* RESP_NONE */
	1,                      /* RESP_R1 */
	2,                      /* RESP_R2 */
	3,                      /* RESP_R3 */
	4,                      /* RESP_R4 */
	1,                      /* RESP_R5 */
	1,                      /* RESP_R6 */
	1,                      /* RESP_R7 */
	7,                      /* RESP_R1b */
```

So **RSPTYP: 0=none, 1=48-bit with CRC (R1/R5/R6/R7), 2=136-bit (R2), 3=48-bit no CRC (R3), 4=R4, 7=R1b.**

Command issue is two writes, arg first — `mtk_sd.h:562-566` **[V]**:

```
562: #define sdc_send_cmd(cmd, arg) \
563: 	do { \
564: 		MSDC_WRITE32(SDC_ARG, (arg)); \
565: 		MSDC_WRITE32(SDC_CMD, (cmd)); \
566: 	} while (0)
```

`SDC_CMD` word construction — comment and code, `sd.c:1486-1492` **[V]**:

```
1486: 	/* rawcmd :
1487: 	 * vol_swt << 30 | auto_cmd << 28 | blklen << 16 | go_irq << 15 |
1488: 	 * stop << 14 | rw << 13 | dtype << 11 | rsptyp << 7 | brk << 6 |
1489: 	 * opcode
1490: 	 */
1492: 	rawcmd = opcode | msdc_rsp[resp] << 7 | host->blksz << 16;
```
and the data-type/stop bits, `sd.c:1494-1596` **[V]**:
- `MMC_READ_SINGLE_BLOCK` / tuning: `rawcmd |= (1 << 11);`  (DTYP=1, single block)
- `MMC_WRITE_BLOCK`: `rawcmd |= ((1 << 11) | (1 << 13));`  (DTYP=1, RW=1)
- `MMC_READ_MULTIPLE_BLOCK`/`WRITE_MULTIPLE`: `rawcmd |= (2 << 11);` + `(1<<13)` for write; `(1<<28)` for AUTOCMD12
- `MMC_STOP_TRANSMISSION`: `rawcmd |= (1 << 14); rawcmd &= ~(0x0FFF << 16);`

**Busy-before-issue** (`sd.c:1603-1617`) **[V]** — poll before *every* command:

```
1603: 	if (opcode == MMC_SEND_STATUS) {
1604: 		while (sdc_is_cmd_busy()) {
...
1610: 	} else {
1611: 		while (sdc_is_busy()) {
```
with (`mtk_sd.h:548-549`) **[V]**:
```
548: #define sdc_is_busy()           (MSDC_READ32(SDC_STS) & SDC_STS_SDCBUSY)
549: #define sdc_is_cmd_busy()       (MSDC_READ32(SDC_STS) & SDC_STS_CMDBUSY)
```

**Completion / response** — `msdc_command_resp_polling`, `sd.c:1660` and `1698-1709` **[V]**:

```
1660: 	u32 cmdsts = MSDC_INT_CMDRDY | MSDC_INT_RSPCRCERR | MSDC_INT_CMDTMO;
...
1698: 		intsts = MSDC_READ32(MSDC_INT);
1699: 		if ((intsts & cmdsts) != 0) {
1705: 			intsts &= cmdsts;
1707: 			MSDC_WRITE32(MSDC_INT, intsts);      /* W1C */
1708: 			break;
```

Response fetch — `sd.c:1741-1779` **[V]**:

```
1741: 		switch (host->cmd_rsp) {
1742: 		case RESP_NONE:
1743: 			break;
1744: 		case RESP_R2:
1745: 			*rsp++ = MSDC_READ32(SDC_RESP3);
1746: 			*rsp++ = MSDC_READ32(SDC_RESP2);
1747: 			*rsp++ = MSDC_READ32(SDC_RESP1);
1748: 			*rsp++ = MSDC_READ32(SDC_RESP0);
1749: 			break;
1750: 		default: /* Response types 1, 3, 4, 5, 6, 7(1b) */
1751: 			*rsp = MSDC_READ32(SDC_RESP0);
```

Error decode — `sd.c:1781-1800` **[V]**:
- `MSDC_INT_RSPCRCERR` → `-EILSEQ`; the driver **does not** reset the hw for R1B/CMD13, resets otherwise.
- `MSDC_INT_CMDTMO` → `-ETIMEDOUT`; resets hw except for R1B/CMD13.
- Additionally, the driver inspects **R1 card-status error bits** for CMD13/CMD25 (`sd.c:1752-1773`) **[V]**: `R1_WP_VIOLATION`, `R1_OUT_OF_RANGE`. (These macros live in `mtk_sd.h`; I did not extract their numeric values — **[U]**. Standard SD/MMC R1 bits: out-of-range = bit 31, WP violation = bit 26.)

**Sequence to program** (eMMC, 8-bit, polled PIO write of one 512-byte block at LBA `n`) **[D]** (composition derived from the quoted code; not executed):

| step | CMD | arg | RSPTYP | SDC_CMD value (computed) | check |
|---|---|---|---|---|---|
| 1 | CMD0 GO_IDLE_STATE | 0 | 0 | `0x00000000` | CMDRDY only |
| 2 | CMD1 SEND_OP_COND | OCR (e.g. `0x40FF8080` = 3.2–3.4 V, sector mode) | 3 | `0x00000181` | CMDRDY, then resp[0] bit31 (busy) == 1; loop until set |
| 3 | CMD2 ALL_SEND_CID | 0 | 2 | `0x00000102` | CMDRDY; read RESP3..RESP0; CID[0] must be `0x15` (eMMC MID) or plausible |
| 4 | CMD3 SET_RELATIVE_ADDR | `RCA<<16` | 1 | `0x00000083` | CMDRDY; resp[0][31:16] must equal RCA |
| 5 | CMD9 SEND_CSD | `RCA<<16` | 2 | `0x00000109` | CMDRDY; CSD[0][7:6] = CSD_STRUCTURE, CSD[1][27:22] = READ_BL_LEN (9 ⇒ 512 B) |
| 6 | CMD7 SELECT_CARD | `RCA<<16` | 1 | `0x00000087` | CMDRDY; resp[0][12:9] = current state, expect 3 (tran) after CMD13 |
| 7 | CMD16 SET_BLOCKLEN | 512 | 1 | `0x00000090` | CMDRDY |
| 8 | CMD13 SEND_STATUS | `RCA<<16` | 1 | `0x0000008D` | resp[0][12:9] == 4 (tran) or 5 (data); retry until not busy; **check R1 error bits** |
| 9 | CMD24 WRITE_BLOCK | `n*512` | 1 | `0x00002898` (=`24\|0x80\|0x800\|0x2000`) with `BLKLEN=512` → **`0x02002898`** | CMDRDY, **then** stream 512 B into `MSDC_TXDATA` |
| 10 | poll MSDC_INT | — | — | — | `XFER_COMPL` (bit 12) ⇒ done; `DATTMO`/`DATCRCERR` ⇒ fail |

Notes that matter:
- **eMMC uses byte addressing**: `arg = LBA * 512` for CMD24/25 as long as the address fits 32 bits. Above 4 GiB you must go through CMD23 + `SDC_BLK_NUM` and sector-mode (`SDC_CMD.AUTOCMD[29:28]`), which is why the vendor driver implements `MSDC_AUTOCMD23` (`sd.c:1504-1521`) **[V]**.
- For multi-block writes the vendor sets `AUTOCMD12` (`rawcmd |= (1 << 28)`) so the controller emits CMD12 itself; in a bare PIO port it is simpler and safer to use **single-block CMD24** or send **CMD25 + explicit CMD12** (`0x0000408C`, note BLKLEN cleared) **[D]**.
- `SDC_CFG.DTOC` must be non-zero before any data command: `MSDC_SET_FIELD(SDC_CFG, SDC_CFG_DTOC, DEFAULT_DTOC)` with `DEFAULT_DTOC == 3` (`mt6765/msdc_cust.h`) ⇒ timeout counted in units of 2^20 sclk **[V]**.

### 1.8 PIO write inner loop (byte/word traffic)

`sd.c:2453-2472` **[V]** — this is the whole PIO TX algorithm:

```
2453: 			if (left >= MSDC_FIFO_SZ && msdc_txfifocnt() == 0) {
2454: 				count = MSDC_FIFO_SZ >> 2;
2455: 				do {
2456: 					msdc_fifo_write32(*ptr);
2457: 					ptr++;
2458: 				} while (--count);
2459: 				left -= MSDC_FIFO_SZ;
2460: 			} else if (left < MSDC_FIFO_SZ &&
2461: 				   msdc_txfifocnt() == 0) {
2462: 				while (left > 3) {
2463: 					msdc_fifo_write32(*ptr);
2464: 					ptr++;
2465: 					left -= 4;
2466: 				}
2467: 				u8ptr = (u8 *) ptr;
2468: 				while (left) {
2469: 					msdc_fifo_write8(*u8ptr);
2470: 					u8ptr++;
2471: 					left--;
2472: 				}
```

with (`mtk_sd.h:114, 605-612`) **[V]**:

```
114: #define MSDC_FIFO_SZ            (128)
605: #define msdc_txfifocnt()        ((MSDC_READ32(MSDC_FIFOCS) & MSDC_FIFOCS_TXCNT) >> 16)
609: #define msdc_fifo_write32(v)    MSDC_WRITE32(MSDC_TXDATA, (v))
610: #define msdc_fifo_write8(v)     MSDC_WRITE8(MSDC_TXDATA, (v))
```

So: **FIFO is 128 bytes (32 words)**; wait for `TXCNT==0`, then blast 32 words; a 512-byte block = 4 such bursts. `msdc_fifo_write8` on `MSDC_TXDATA` is a **byte write to 0x18** — the IP packs bytes, so a byte-granular store works if your MMU-off code does a `strb` to `0x11230018`. **[V]** for the macro; byte-lane behaviour on hardware is **[D]** (very likely correct, but PIO is normally done 32-bit).

The overall write loop also polls the interrupt register every iteration and bails on `DATTMO|DATCRCERR` (`sd.c:2374-2382`) **[V]**.

---

## 2. USB — U2 PHY (a60810) + clock gates, for a CDC-ACM console

### 2.1 Addresses

Kernel DTS **[V]**:

```
1978: 	usb0@11200000 {
1979: 		compatible = "mediatek,mt6765-usb20";
1980: 		reg = <0 0x11200000 0 0x10000>,
1981: 		<0 0x11CC0000 0 0x10000>;
1982: 		interrupts = <GIC_SPI 73 IRQ_TYPE_LEVEL_LOW>;
1983: 		mode = <2>;
1984: 		multipoint = <1>;
1985: 		num_eps = <16>;
1986: 		clocks = <&infracfg_ao CLK_IFR_ICUSB>,
1987: 		<&topckgen CLK_TOP_USB_TOP_SEL>,
1988: 		<&topckgen CLK_TOP_UNIVPLL3_D4>;
1989: 		clock-names = "usb0",
1990: 		"usb0_clk_top_sel",
1991: 		"usb0_clk_univpll3_d4";
```
```
1994: 	usb_sif@11210000 {
1995: 		compatible = "mediatek,usb_sif";
1996: 		reg = <0 0x11210000 0 0x10000>;
```

LK confirms **[V]** (`platform/mt6765/include/platform/mt_reg_base.h`):
```
248: // APB Module usb2
249: #define USB_BASE (0x11200000)
251: // APB Module usb_sif
252: #define USBSIF_BASE (0x11CC0000)
272: #define ICUSB_BASE (0x11270000)
```

| block | base | what |
|---|---|---|
| MUSB controller | **0x1120_0000** | MUSB register file (`MUSB_POWER`, `MUSB_DEVCTL`, endpoints…) |
| USB2 PHY ("SIF" / a60810) | **0x11CC_0000** window, PHY regs at **+0x800** | analogue U2 PHY |
| USB SIF (kernel naming) | 0x1121_0000 | sideband/charger detect |
| ICUSB (USB1.1) | 0x1127_0000 | second PHY, unused for our console |

**The +0x800 is the trap.** Kernel accessor, `drivers/misc/mediatek/usb20/mtk_musb.h:31-38` **[V]**:

```
31: #define USBPHY_READ32(offset) \
32: 	readl((void __iomem *)(((unsigned long)\
33: 		mtk_musb->xceiv->io_priv)+0x800+offset))
36: #define USBPHY_SET32(offset, mask) \
37: 	USBPHY_WRITE32(offset, (USBPHY_READ32(offset)) | (mask))
38: #define USBPHY_CLR32(offset, mask) \
39: 	USBPHY_WRITE32(offset, (USBPHY_READ32(offset)) & (~(mask)))
```

`io_priv` is the second `reg` entry = 0x11CC0000, so **`USBPHY_xxx32(0x68)` really hits `0x11CC0868`.**

LK instead uses **byte** accesses at `USB20_PHY_BASE + offset` (`platform/mt6765/mt_musb/mt_musbphy.c:112-119`) **[V]**:

```
112: #define USBPHY_READ8(offset)        readb(USB20_PHY_BASE+offset)
113: #define USBPHY_WRITE8(offset, value)    writeb(value, USB20_PHY_BASE+offset)
116: #define USBPHY_SET32(offset, mask)     USBPHYWRITE32(offset, USBPHYREAD32(offset) | mask)
```
(I could **not** find the `USB20_PHY_BASE` definition itself — it is not in `mt_reg_base.h`; **[U]**. It must be 0x11CC0800 for the two trees to agree, since LK's byte `0x6a` is the byte-1 lane of the kernel's word `0x68`.)

> **Consequence for the shim:** write **32-bit** registers at `0x11CC0800 + off`, or byte registers at `0x11CC0800 + byte_off`. Mixing them up (e.g. hitting 0x11CC0000 instead of 0x11CC0800) will silently write to the SIF block and the PHY will never come up.

### 2.2 PHY power-on sequence (device/CDC-ACM mode)

`drivers/misc/mediatek/usb20/mt6765/usb20_phy.c:575-628` **[V]**, quoted with the vendor's own register table in the comment:

```
575: void usb_phy_poweron(void)
...
583: 	/* wait 50 usec for PHY3.3v/1.8v stable. */
584: 	udelay(50);
586: 	/*
587: 	 * force_uart_en	1'b0		0x68 26
588: 	 * RG_UART_EN		1'b0		0x6c 16
589: 	 * rg_usb20_gpio_ctl	1'b0		0x20 09
590: 	 * usb20_gpio_mode	1'b0		0x20 08
591: 	 * RG_USB20_BC11_SW_EN	1'b0		0x18 23
592: 	 * rg_usb20_dp_100k_mode 1'b1		0x20 18
593: 	 * USB20_DP_100K_EN	1'b0		0x20 16
594: 	 * RG_USB20_DM_100K_EN	1'b0		0x20 17
595: 	 * RG_USB20_OTG_VBUSCMP_EN	1'b1	0x18 20
596: 	 * force_suspendm		1'b0	0x68 18
597: 	 */
599: 	/* force_uart_en, 1'b0 */
600: 	USBPHY_CLR32(0x68, (0x1 << 26));
601: 	/* RG_UART_EN, 1'b0 */
602: 	USBPHY_CLR32(0x6c, (0x1 << 16));
603: 	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
604: 	USBPHY_CLR32(0x20, ((0x1 << 9) | (0x1 << 8)));
606: 	/* RG_USB20_BC11_SW_EN, 1'b0 */
607: 	USBPHY_CLR32(0x18, (0x1 << 23));
609: 	/* rg_usb20_dp_100k_mode, 1'b1 */
610: 	USBPHY_SET32(0x20, (0x1 << 18));
611: 	/* USB20_DP_100K_EN 1'b0, RG_USB20_DM_100K_EN, 1'b0 */
612: 	USBPHY_CLR32(0x20, ((0x1 << 16) | (0x1 << 17)));
614: 	/* RG_USB20_OTG_VBUSCMP_EN, 1'b1 */
615: 	USBPHY_SET32(0x18, (0x1 << 20));
617: 	/* force_suspendm, 1'b0 */
618: 	USBPHY_CLR32(0x68, (0x1 << 18));
620: 	/* RG_USB20_PHY_REV[7:0] = 8'b01000000 */
621: 	USBPHY_CLR32(0x18, (0xFF << 24));
622: 	USBPHY_SET32(0x18, (0x40 << 24));
624: 	/* wait for 800 usec. */
625: 	udelay(800);
```

Then the "recover" step, which is the one that actually puts the PHY into **device** mode (`usb20_phy.c:715-828`) **[V]**:

```
751: 	/* clean PUPD_BIST_EN */
755: 	USBPHY_CLR32(0x1c, (0x1 << 12));
758: 	USBPHY_CLR32(0x68, (0x1 << 26));
760: 	USBPHY_CLR32(0x6C, (0x1 << 16));
762: 	USBPHY_CLR32(0x20, (0x1 << 9));
763: 	USBPHY_CLR32(0x20, (0x1 << 8));
766: 	USBPHY_CLR32(0x68, (0x1 << 18));
769: 	USBPHY_CLR32(0x68, ((0x1 << 6) | (0x1 << 7)));
772: 	USBPHY_CLR32(0x68, (0x3 << 4));
775: 	USBPHY_CLR32(0x68, (0x1 << 2));
777: 	USBPHY_CLR32(0x68, (0xF << 10));
782: 	USBPHY_CLR32(0x68, ((0x1 << 20) | (0x1 << 21) | (0x1 << 19) | (0x1 << 17) | (0x1 << 23)));
786: 	USBPHY_CLR32(0x18, (0x1 << 23));
788: 	USBPHY_SET32(0x18, (0x1 << 20));
791: 	usb_rev6_setting(0x40);
794: 	udelay(800);
797: 	set_usb_phy_mode(PHY_DEV_ACTIVE);
799: 	hs_slew_rate_cal();
802: 	efuse_val = (get_devinfo_with_index(107) & (0x1f<<0)) >> 0;
...  (RG_USB20_INTR_CAL[4:0] <- efuse 107[4:0], bits [23:19] of reg 0x04)
822: 	/* disc threshold to max, RG_USB20_DISCTH[7:4], dft:1000, MAX:1111 */
823: 	USBPHY_SET32(0x18, (0xf0<<0));
```

**The device-mode register write** — `set_usb_phy_mode`, `usb20_phy.c:531-557` **[V]**:

```
	case PHY_DEV_ACTIVE:
	/* VBUSVALID=1, AVALID=1, BVALID=1, SESSEND=0, IDDIG=1, IDPULLUP=1 */
		USBPHY_CLR32(0x6C, (0x10<<0));
		USBPHY_SET32(0x6C, (0x2F<<0));
		USBPHY_SET32(0x6C, (0x3F<<8));
```
i.e. **reg 0x6C: clear bit4, OR-in `0x2F`, OR-in `0x3F00`** — this is the OTG control byte that advertises a device (D+ pull-up) and hard-wires VBUSVALID/AVALID/BVALID.

Note `CONFIG_MTK_UART_USB_SWITCH` is **not set** on this device, so the entire `force_uart_en` / `RG_UART_EN` dance is a no-op in practice (all `if (in_uart_mode) return;` short-circuits are compiled out). **You must still clear them once** in case LK left the port in UART-download mode. **[D]**

### 2.3 Clock gates for USB

From `drivers/clk/mediatek/clk-mt6765.c` **[V]**:

- `infracfg_ao` ICUSB gate (this is the `usb0` clock in the DT):
```
869: 	GATE_IFR2(CLK_IFR_ICUSB, "ifr_icusb", "axi_ck", 8),
```
```
787: static const struct mtk_gate_regs ifr2_cg_regs = {
788: 	.set_ofs = 0x80,
789: 	.clr_ofs = 0x84,
790: 	.sta_ofs = 0x90,
```
with `mtk_clk_gate_ops_setclr` (`clk-mt6765.c:835`), so: **`write 1<<8 to 0x1000_1080`** (set), status in `0x1000_1090` bit 8. **[D]** (derived)

- `topckgen` USB mux + 48 M gate:
```
673: 	MUX_CLR_SET_UPD(CLK_TOP_USB_TOP_SEL, "usb_top_sel",
674: 		usb_top_parents, CLK_CFG_5, CLK_CFG_5_SET, CLK_CFG_5_CLR,
675: 		16, 1, 23, CLK_CFG_UPDATE, 22),
```
```
270: #define CLK_CFG_5 0x90
271: #define CLK_CFG_5_SET 0x94
272: #define CLK_CFG_5_CLR 0x98
288: #define CLK_CFG_UPDATE 0x004
```
```
761: 	GATE_TOP1(CLK_TOP_USB20_48M_EN, "usb20_48m_en", "usb20_48m_div", 8),
```
```
705: static const struct mtk_gate_regs top1_cg_regs = {
706: 	.set_ofs = 0x104,
707: 	.clr_ofs = 0x104,
708: 	.sta_ofs = 0x104,
```
```
732: 		.ops = &mtk_clk_gate_ops_no_setclr_inv,	\
```

`GATE_TOP1` is the **inverted** variant (`no_setclr_inv`), and its enable path is `mtk_cg_clr_bit_no_setclr()` (`drivers/clk/mediatek/clk-gate.c:213-220`) **[V]**:

```
213: static int mtk_cg_enable_no_setclr(struct clk_hw *hw)
214: {
215: 	mtk_cg_clr_bit_no_setclr(hw);
```
→ **for topckgen `0x1000_0104`, bit 8 = 0 means "clock on"**; bit 8 = 1 is power-down. Do **not** blind-write 1.

- Reference clock: the DT asks for `CLK_TOP_UNIVPLL3_D4` (univpll3 / 4) as the USB parent, muxed by `CLK_TOP_USB_TOP_SEL` into `CLK_CFG_5[16]`. `clk_prepare/musb_clk_top_sel`'s whole job is `clk_set_parent(musb_clk_top_sel, musb_clk_univpll3_d4)` (`usb20_phy.c:276-282`) **[V]**. In a shim: **just verify** `CLK_CFG_5` bit 16 = 1 (i.e. the parent is univpll3_d4) rather than reprogramming PLLs; LK already set this up for fastboot/download mode.

**Practical bring-up order for the shim:** ungate ICUSB → ungate USB20_48M (bit 8=0 @0x10000104) → `usb_phy_poweron` → `usb_phy_recover` → configure MUSB device controller (`MUSB_POWER.SOFTCONN`, `MUSB_DEVCTL`) → run the CDC-ACM gadget. **[D]**

### 2.4 MUSB controller registers (for the CDC-ACM side)

`drivers/misc/mediatek/usb20/mtk_musb_reg.h` **[V]** (standard Mentor MUSB offsets, byte/halfword):

```
208: #define MUSB_FADDR		0x00	/* 8-bit */
209: #define MUSB_POWER		0x01	/* 8-bit */
211: #define MUSB_INTRTX		0x02	/* 16-bit */
212: #define MUSB_INTRRX		0x04
213: #define MUSB_INTRTXE		0x06
214: #define MUSB_INTRRXE		0x08
215: #define MUSB_INTRUSB		0x0A	/* 8 bit */
216: #define MUSB_INTRUSBE		0x0B	/* 8 bit */
217: #define MUSB_FRAME		0x0C
218: #define MUSB_INDEX		0x0E	/* 8 bit */
219: #define MUSB_TESTMODE		0x0F	/* 8 bit */
228: #define MUSB_DEVCTL		0x60	/* 8 bit */
243: #define MUSB_TXFIFOSZ		0x62	/* 8-bit (see masks) */
244: #define MUSB_RXFIFOSZ		0x63	/* 8-bit (see masks) */
```

Politeness note: this is a *full* device controller with 16 EPs, a QMU DMA engine and a 4 KB+ FIFO RAM. Driving CDC-ACM bare-metal from scratch means implementing `musb_gadget.c`/`musb_gadget_ep0.c` behaviour (EP0 SETUP/STATUS staging, `MUSB_INDEX` banking, `MUSB_CONFIGDATA` readback). A pragmatic alternative if the goal is *just a console*: use **UART** (the DT `bootargs` already has `console=ttyS0,921600n1`), or a **polled EP0-only vendor bulk** implementation, rather than a spec-complete CDC-ACM. **[D]** — engineering advice, outside the source.

---

## 3. WATCHDOG (TOPRGU @ 0x10007000)

### 3.1 Which driver is live

Two drivers exist in the tree:

- `drivers/watchdog/mtk_wdt.c` — mainline-style; its match table **does not** include mt6765 (`mtk_wdt.c:507-510`: `mediatek,mt6589-wdt`, `mediatek,mt8163-rgu`, `mediatek,mt8173-wdt`). **[V]**
- `drivers/watchdog/mediatek/wdt/common/wdt_v2/mtk_wdt_v2.c` — match table **includes the dandelion compatible string** **[V]**:
```
55: 	{ .compatible = "mediatek,toprgu", },
56: 	{ .compatible = "mediatek,mt6765-toprgu", },
```
and the DTS has `toprgu@10007000 { compatible = "mediatek,mt6765-toprgu"; ... }` **[V]**. With `CONFIG_MEDIATEK_WATCHDOG=y`, **`mtk_wdt_v2.c` is the live driver.** (Its symbol set is `mtk_wdt_mode_config()`, `mtk_wdt_restart()`, `MTK_WDT_MODE` etc.)

### 3.2 Register map and magic values

Authoritative table from LK `platform/mt6765/include/platform/mtk_wdt.h` **[V]** (identical offsets in the kernel driver):

```
#define MTK_WDT_BASE            TOPRGU_BASE                /* 0x10007000 */
#define MTK_WDT_MODE            (MTK_WDT_BASE+0x0000)
#define MTK_WDT_LENGTH          (MTK_WDT_BASE+0x0004)
#define MTK_WDT_RESTART         (MTK_WDT_BASE+0x0008)
#define MTK_WDT_STATUS          (MTK_WDT_BASE+0x000C)
#define MTK_WDT_INTERVAL        (MTK_WDT_BASE+0x0010)
#define MTK_WDT_SWRST           (MTK_WDT_BASE+0x0014)
#define MTK_WDT_SWSYSRST        (MTK_WDT_BASE+0x0018)
#define MTK_WDT_NONRST_REG      (MTK_WDT_BASE+0x0020)
#define MTK_WDT_NONRST_REG2     (MTK_WDT_BASE+0x0024)
#define MTK_WDT_REQ_MODE        (MTK_WDT_BASE+0x0030)
#define MTK_WDT_REQ_IRQ_EN      (MTK_WDT_BASE+0x0034)
#define MTK_WDT_DEBUG_CTL       (MTK_WDT_BASE+0x0040)
```
```
/*WDT_MODE*/
#define MTK_WDT_MODE_KEY            (0x22000000)
#define MTK_WDT_MODE_DUAL_MODE      (0x0040)
#define MTK_WDT_MODE_IRQ_LEVEL_EN   (0x0020)
#define MTK_WDT_MODE_AUTO_RESTART   (0x0010)
#define MTK_WDT_MODE_IRQ            (0x0008)
#define MTK_WDT_MODE_EXTEN          (0x0004)
#define MTK_WDT_MODE_EXT_POL        (0x0002)
#define MTK_WDT_MODE_ENABLE         (0x0001)

/*WDT_LENGTH*/
#define MTK_WDT_LENGTH_TIME_OUT     (0xffe0)
#define MTK_WDT_LENGTH_KEY          (0x0008)

/*WDT_RESTART*/
#define MTK_WDT_RESTART_KEY     (0x1971)

/*WDT_SWRST*/
#define MTK_WDT_SWRST_KEY       (0x1209)

/*WDT_SWSYSRST*/
#define MTK_WDT_SWSYS_RST_KEY       (0x88000000)
```

So the "magic values like 0x1209" from the brief are:
- **`0x1209`** → `WDT_SWRST` (0x14) key, **immediate SoC reset** (this is the reset path, not the pet path).
- **`0x1971`** → `WDT_RESTART` (0x08) key, **the pet/kick**.
- **`0x22000000`** → `WDT_MODE` (0x00) must always be OR-ed in on every write or the write is ignored.
- **`0x88000000`** → `WDT_SWSYSRST` key (per-subsystem reset).

### 3.3 Disable / pet — the exact code

**Disable** (LK `platform/mt6765/mtk_wdt.c:51-58`) **[V]** — 4 lines, copy verbatim:

```
void mtk_wdt_disable(void)
{
	u32 tmp;

	tmp = DRV_Reg32(MTK_WDT_MODE);
	tmp &= ~MTK_WDT_MODE_ENABLE;       /* disable watchdog */
	tmp |= (MTK_WDT_MODE_KEY);         /* need key then write is allowed */
	DRV_WriteReg32(MTK_WDT_MODE, tmp);
}
```
→ `*(u32*)0x10007000 = (old & ~1) | 0x22000000;`

**Pet** (LK `platform/mt6765/mtk_wdt.c:218-224`) **[V]**:

```
void mtk_wdt_restart(void)
{
	/* Reset WatchDogTimer's counting value to time out value */
	/* ie., keepalive() */
	DRV_WriteReg32(MTK_WDT_RESTART, MTK_WDT_RESTART_KEY);
}
```
→ `*(u32*)0x10007008 = 0x1971;`  **No key field, no read-modify-write. This is the one-liner the shim needs, called at ≤½ the timeout period.**

**Timeout encoding** (LK `mtk_wdt_set_time_out_value`) **[V]**:

```
	/*
	* TimeOut = BitField 15:5
	* Key      = BitField  4:0 = 0x08
	*/
	/* sec * 32768 / 512 = sec * 64 = sec * 1 << 6 */
	timeout = (unsigned int)(value * (1 << 6));
	timeout = timeout << 5;
	DRV_WriteReg32(MTK_WDT_LENGTH, (timeout | MTK_WDT_LENGTH_KEY));
```
→ `0x10007004 = (seconds << 11) | 0x8`. The vendor kernel does the same with `reg = WDT_LENGTH_TIMEOUT(timeout << 6) | WDT_LENGTH_KEY;` where `WDT_LENGTH_TIMEOUT(n) ((n) << 5)` (`drivers/watchdog/mtk_wdt.c:52, 258`). **[V]**

**What LK leaves armed when it jumps to the kernel — this is the state the shim inherits** (`mtk_wdt.c:319-345`) **[V]**:

```
void mtk_wdt_init(void)
{
	mtk_wdt_mode_config(FALSE, FALSE, FALSE, FALSE, FALSE);   /* all off first */
	/* mark stage */
	mtk_wdt_mark_stage(RGU_STAGE_LK);
	/* Setting timeout 10s */
	mtk_wdt_set_time_out_value(10);
	...
#ifdef WDT_ENABLED
	mtk_wdt_mode_config(TRUE, TRUE, TRUE, FALSE, TRUE);
	mtk_wdt_restart();
#endif
```
`mtk_wdt_mode_config(dual, irq, exten, ext_pol, enable)` per its signature at `mtk_wdt_v2.c:203` — so LK arms **dual-mode + IRQ + external-reset, 10 s timeout**, and pets it. Expect the watchdog to be **running with a ~10 s timeout** when your shim starts. Either pet it every ≤5 s or disable it; **do not assume it is off**.

**Status decoding** (to log *why* the last boot died) — `WDT_STATUS` @0x0C **[V]**:

```
#define MTK_WDT_STATUS_HWWDT_RST    (0x80000000)
#define MTK_WDT_STATUS_SWWDT_RST    (0x40000000)
#define MTK_WDT_STATUS_IRQWDT_RST   (0x20000000)
#define MTK_WDT_STATUS_DEBUGWDT_RST (0x00080000)
#define MTK_WDT_STATUS_SPMWDT_RST   (0x0001)
```
**Caveat from the vendor:** "WDT_STATUS will be cleared to zero after writing to WDT_MODE, so we backup it in WDT_NONRST_REG" (`drivers/watchdog/mtk_wdt.c:198-202`) **[V]**. So **read 0x1000700C *before* you write 0x10007000**, or you lose the reset reason.

Also note `WDT_NONRST_REG2` (0x24) carries the boot-stage marker in bits [31:30] / [29:28] (`RGU_STAGE_PRELOADER=1, LK=2, KERNEL=3`) — a nice way for the shim to prove "the machine really did come from LK". **[V]**

If you want a deliberate reboot instead of a hang: `*(u32*)0x10007014 = 0x1209;` then spin (`mtk_wdt_sw_reset` does exactly that, `mtk_wdt.c:226-240`). **[V]**

---

## 4. GIC — does the vendor init differ from mainline?

### 4.1 Kernel side: **no, not materially**

`drivers/irqchip/irq-gic.c` `gic_dist_init()` **[V]** — this is the *entire* function:

```
470: static void gic_dist_init(struct gic_chip_data *gic)
471: {
472: 	unsigned int i;
473: 	u32 cpumask;
474: 	unsigned int gic_irqs = gic->gic_irqs;
475: 	void __iomem *base = gic_data_dist_base(gic);
476:
477: 	writel_relaxed(GICD_DISABLE, base + GIC_DIST_CTRL);
478:
479: 	/*
480: 	 * Set all global interrupts to this CPU only.
481: 	 */
482: 	cpumask = gic_get_cpumask(gic);
483: 	cpumask |= cpumask << 8;
484: 	cpumask |= cpumask << 16;
485: 	for (i = 32; i < gic_irqs; i += 4)
486: 		writel_relaxed(cpumask, base + GIC_DIST_TARGET + i * 4 / 4);
487:
488: 	gic_dist_config(base, gic_irqs, NULL);
489:
490: 	writel_relaxed(GICD_ENABLE, base + GIC_DIST_CTRL);
491: }
```

and `gic_cpu_init()` **[V]**:

```
525: 	gic_cpu_config(dist_base, NULL);
527: 	writel_relaxed(GICC_INT_PRI_THRESHOLD, base + GIC_CPU_PRIMASK);
528: 	gic_cpu_if_up(gic);
```
with `gic_cpu_if_up` **[V]**:
```
463: 	bypass = readl(cpu_base + GIC_CPU_CTRL);
464: 	bypass &= GICC_DIS_BYPASS_MASK;
466: 	writel_relaxed(bypass | mode | GICC_ENABLE, cpu_base + GIC_CPU_CTRL);
```

I diffed this file against `torvalds/linux` **v4.19** `drivers/irqchip/irq-gic.c` (downloaded to `D:\aios\research\refs\mainline-irq-gic-v4.19.c`). **[V]** The differences are **API-refactor-level only** — the vendor file is from an older lineage:

| vendor | mainline v4.19 |
|---|---|
| `static_key_true(&supports_deactivate)` | `static_branch_likely(&supports_deactivate_key)` |
| no `isb()` barriers | `isb()` added after `GICD_CTLR`/`GICC_CTLR` writes |
| no `gic_check_gicv2()` | `gic_check_gicv2()` reads `GIC_CPU_IDENT` and validates `0x02043B` |
| `static struct irq_chip gic_chip` | `static const struct irq_chip gic_chip` |

**There is no MediaTek-specific MMIO in `gic_dist_init()`/`gic_cpu_init()`**: no extra register writes before `GICD_CTLR`, none before/between `GICD_IGROUPR` (which is written inside generic `gic_dist_config()`), no DCM poke, no DEVAPC unlock. **[V]** A grep of the whole vendor `irq-gic.c` for `mtk|MTK|DCM|devapc|SYNC` returns **zero hits**. **[V]**

### 4.2 Where MediaTek *does* differ: the polarity-inversion block (INT_POL_CTL)

The MTK-specific piece is **not** in the GIC driver — it is a companion "sysirq"/polarity block at **0x10200A80**, declared in the DTS as its own interrupt controller **[V]**:

```
370: 	sysirq: intpol-controller@10200a80 {
371: 		compatible = "mediatek,mt6577-sysirq";
372: 		interrupt-controller;
373: 		#interrupt-cells = <3>;
374: 		interrupt-parent = <&gic>;
375: 		reg = <0 0x10200a80 0 0x50>;
376: 	};
```
and the vendor GIC is at `gic: interrupt-controller@0c000000` (`mt6765.dts:356`) — **GICv2, dist base 0x0C000000, CPU base 0x0C002000**. **[V]**

LK programs exactly that block, and its init is a plain GICv2 init **plus** polarity inversion. `dev/gic/mt_gic_v2.c:41-92` **[V]**:

```
41: static void mt_gic_cpu_init(void)
42: {
43: 	DRV_WriteReg32(GIC_CPU_BASE + GIC_CPU_PRIMASK, 0xF0);
44: 	DRV_WriteReg32(GIC_CPU_BASE + GIC_CPU_CTRL, 0x1);
45: 	dsb();
46: }
48: static void mt_gic_dist_init(void)
49: {
50: 	unsigned int i;
51: 	unsigned int cpumask = mt_irq_get_target();
53: 	DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CTRL, 0);
58: 	for (i = 32; i < (MT_NR_SPI + 32); i += 16) {
59: 		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CONFIG + i * 4 / 16, 0);
60: 	}
65: 	for (i = 32; i < (MT_NR_SPI + 32); i += 4) {
66: 		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_TARGET + i * 4 / 4, cpumask);
67: 	}
72: 	for (i = 0; i < NR_IRQ_LINE; i += 4) {
73: 		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_PRI + i * 4 / 4, 0xA0A0A0A0);
74: 	}
79: 	for (i = 0; i < NR_IRQ_LINE; i += 32) {
80: 		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_ENABLE_CLEAR + i * 4 / 32, 0xFFFFFFFF);
81: 	}
83: 	dsb();
85: 	DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CTRL, 1);
86: }
```

Differences vs. mainline, all benign for a shim:
1. **LK sets every SPI to level-triggered (all `GICD_ICFGR = 0`)**, whereas mainline's `gic_dist_config()` writes `GICD_INT_DEF_PRI` + `GICD_ICFGR` defaults too but via `gic_dist_config`; net effect similar.
2. LK does **not** write `GICD_IGROUPR` at all (GICv2 secure/non-secure grouping) because it runs in the secure world; the kernel only touches it through `gic_dist_config`/`gic_set_type`. **A non-secure EL1 shim cannot change IGROUPR meaningfully** — leave it. 
3. LK sets **all priorities to `0xA0A0A0A0`**; kernel uses `GICC_INT_PRI_THRESHOLD` (`0xF0`) in PMR. For the shim, PMR=0xF0 is the safe value (matches both).

The **only** genuinely MediaTek-specific register is the polarity inversion (`mt_gic_v2.c:114-136`) **[V]**:

```
125: 	offset = (irq - GIC_PRIVATE_SIGNALS) & 0x1F;
126: 	reg_index = (irq - GIC_PRIVATE_SIGNALS) >> 5;
127: 	if (polarity == 0) {
128: 		value = DRV_Reg32(INT_POL_CTL0 + (reg_index * 4));
129: 		value |= (1 << offset); // always invert the incoming IRQ's polarity
130: 		DRV_WriteReg32((INT_POL_CTL0 + (reg_index * 4)), value);
```
with, from `platform/mt6765/include/platform/mt_irq.h` **[V]**:
```
50: #define INT_POL_CTL0  (MCUCFG_BASE + 0xA80)
78: #define MT_NR_SPI   (241)//(224)
79: #define NR_IRQ_LINE  (GIC_PPI_OFFSET + MT_NR_PPI + MT_NR_SPI)
```
→ **`INT_POL_CTL0 = 0x10200A80`**, 241 SPIs, ~5 PPIs. This matches the kernel's `sysirq` node exactly, and the kernel's `drivers/irqchip/irq-mtk-sysirq.c` (also present, 11,901 bytes) is the driver that owns it at runtime. **[V]**

**Bottom line for the shim:** you do **not** need to re-init the GIC. LK already left it enabled (GICD/GICC), with all SPIs masked, level-triggered, target = this CPU, priorities 0xA0. If you need interrupts at all, the minimum is: `GICC_PMR = 0xF0` (if you changed it), `GICD_ISENABLER` for your IRQ, `GICC_CTLR.EnableGrp1`. If you stay polled (recommended with MMU off), **touch nothing**. **[D]**

### 4.3 DCM / GIC_SYNC_DCM — negative result

The tree has a full MediaTek DCM framework (`drivers/misc/mediatek/base/power/dcm_v1/`, incl. `mt6765/mtk_dcm_autogen.c`, `mtk_dcm_internal.c`) and even a DCM copy inside LK (`platform/mt6765/mtk_dcm.c`, `mtk_dcm_autogen.c`). **However:**

```
$ git grep -n "GIC_SYNC_DCM" -- drivers/misc/mediatek/base/power
$ echo $?
1
```
→ **`GIC_SYNC_DCM` does not exist in this 4.19 MT6765 tree** (it is an MT6797/MT6755-era symbol). **[V]** The DCM code here deals with EMI/CONN/INFRA/FMEM clock gating (`dcm_infracfg_ao_emi_indiv`, `sync_dcm_set_ccifreq`, …), not with a GIC clock-domain synchroniser. So: **no `GIC_SYNC_DCM` handling to port.** **[V]**

### 4.4 DEVAPC

`CONFIG_MTK_DEVAPC=y`; the driver is `drivers/misc/mediatek/devapc/mt6765/devapc.c` (36,199 B) and the DTS node is **[V]**:

```
875: 	devapc@10207000 {
876: 		compatible = "mediatek,devapc";
877: 		reg = <0 0x10207000 0 0x1000>,
878: 		<0 0x1000e000 0 0x1000>;
879: 		interrupts = <GIC_SPI 142 IRQ_TYPE_LEVEL_LOW>;
880: 		clocks = <&infracfg_ao CLK_IFR_DEVICE_APC>;
```

What it actually does in the kernel is **violation logging + a debugfs/sysfs interface**, not permission programming. Its only unconditional boot-time MMIO write is **[V]**:

```
499: 	mt_reg_sync_writel(0x80000000, DEVAPC_PD_INFRA_APC_CON);
```
(plus per-violation status clearing at lines 505, 730-762). There is **no "unlock everything" sequence at probe**; the slave permission table is established earlier (preloader/LK) and the kernel merely reports violations (`devapc_vio_count`, AEE trigger). **[V]** for the absence of a kernel-side unlock.

> **Practical warning:** because the permission map is set before Linux, if the shim touches a peripheral that LK/kernel *never* touched, and that slave is masked for the AP domain, you can get a silent bus hang or a DEVAPC violation IRQ (SPI 142) rather than a data abort. MSDC0, the USB PHY and TOPRGU are all AP-owned and used by LK, so the four areas in this report are safe. **[D]** — this is reasoning from the architecture, not from a read of the permission table (which I did **not** extract; **[U]**).

---

## 5. Reuse checklist (what to copy, what to change for MMU-off)

### 5.1 MSDC — copy `cust_msdc.c` + the PIO half of `sd.c`

| # | Action | Source |
|---|---|---|
| 1 | Copy the register `#define`s (offsets + bitfields) | `mt6765-vendor/drivers/mmc/host/mediatek/ComboA/msdc_reg.h` — 1,178 lines, self-contained, **no kernel deps** |
| 2 | Copy the clock-divider math | `sd.c:763-846` (`msdc_set_mclk`) or `lk-svoboda18/platform/common/storage/mmc/msdc.c:1962-2087` (`msdc_config_clock`) — **the LK one is already freestanding** |
| 3 | Copy pad/pinmux | `mt6765/msdc_cust.c:827-842` (`msdc_set_pin_mode`), `:968-1018` (driving), `:1028-1068` (pull) — or LK `platform/mt6765/msdc_io.c` (38 KB, no kernel deps) |
| 4 | Copy the PIO TX loop | `sd.c:2453-2472` (kernel) or LK `msdc_pio_write` (`msdc.c:1468-1613`) |
| 5 | Copy the CMD word builder | `sd.c:1486-1597` — but the LK equivalent (`msdc_send_cmd`, `msdc.c:709-907`) is **already free of `struct mmc_command`** and is the better template |
| 6 | **Change:** replace `MSDC_READ32/WRITE32` | they are `__raw_readl`/`mt_reg_sync_writel` (`mtk_sd.h:499-502`). With MMU off, a plain `*(volatile u32*)` works because the addresses are physical already. **Drop `mt_reg_sync_writel`'s `dsb`** — it is a barrier helper you cannot call; use `dsb sy` inline or nothing (Device-nGnRnE ordering via `volatile` is usually enough for a polled driver, but add `dsb` after `SDC_CMD` writes if you see lost commands) |
| 7 | **Change:** `uffs()` in `MSDC_SET_FIELD` | `mtk_sd.h:531-537` uses `uffs(field)-1` to derive the shift. In freestanding code, **precompute the shifts as literals** (e.g. CKDIV→8, CKMOD→20, DTOC→24, BUSWIDTH→16). Do not port `uffs()` |
| 8 | **Change:** remove `msdc_retry()`/`msdc_dump_info()` on failure | `mtk_sd.h:568-582` calls `msdc_dump_info` + `WARN_ON` on retry exhaustion. Replace with a plain bounded loop; a `WARN_ON` in a shim is a hang |
| 9 | **Change:** bus width | start at **1-bit**, then `SDC_CFG[17:16]=2` for 8-bit *only after* CMD7 returns tran-state. Vendor does 1-bit in `init_hw` (`sd.c:1000`) then `set_ios` widens it |
| 10 | **Do not** port | DMA/GPD/BD (`msdc_dma_*`), CQHCI/CMDQ (`cmdq_hci.c`, `EMMC51_CFG0`), autok/tuning (`autok.c` 207 KB, `msdc_tune.c`), HW-FDE (`mtk_ufs_hw_fde.c`), regulator/PMIC (`msdc_ldo_power`), runtime PM |
| 11 | **Do not** touch | `EMMC_CFG0/1`, `0x11CD0000` (only needed for HS200/400 tuning) |

Suggested shim config for MSDC0 (all **[D]**): `MSDC_CFG = MODE(1) | PIO(1) | CKDIV(4) | CKMOD(0)` → 26 MHz; `SDC_CFG = BUSWIDTH(2)<<16 | DTOC(3)<<24`; `MSDC_IOCON = 0`; `SDC_CFG.DTOC` non-zero; INTEN = 0 (pure polling).

### 5.2 USB — copy the PHY sequences, keep the MUSB stack out

| # | Action | Source |
|---|---|---|
| 1 | Copy `usb_phy_poweron()` | `mt6765/usb20_phy.c:575-628` — 20 lines of `USBPHY_CLR32/SET32` + 2 `udelay`. Kernel deps: none beyond the macro |
| 2 | Copy `usb_phy_recover()` **minus** the efuse/tuning tail | `:715-794` is pure PHY writes; `:799-825` calls `hs_slew_rate_cal()`, `get_devinfo_with_index(107)` (efuse!) and `usb_phy_tuning()` — **drop them or stub**; they only trim analogue accuracy |
| 3 | Copy `set_usb_phy_mode(PHY_DEV_ACTIVE)` | `:531-548` — 3 writes to 0x6C |
| 4 | **Change:** the accessor base | `USBPHY_xxx32(off)` → `0x11CC0800 + off`. The kernel derives it from `xceiv->io_priv`; hardcode it |
| 5 | **Change:** `udelay()` | wire to your GPT/timer; the sequence needs 50 µs + 800 µs + 800 µs |
| 6 | **Change:** clock gates | `ICUSB` = `1<<8 @0x10001080`; `USB20_48M` = **clear** bit 8 @`0x10000104`; verify `CLK_CFG_5[16]==1` @`0x10000090` |
| 7 | **Do not** port | QMU DMA (`musb_qmu.c`), charger/BC1.2 (`Charger_Detect_*`), `usb2jtag`, `usb_boost`, dual-role/OTG state machine (`usb20_otg_if.c`, 42 KB), `mtk_dual_role.c` |
| 8 | Decide | full MUSB gadget (`usb20/musb_gadget*.c`, 100 KB+, needs `struct musb`, DMA, workqueues) vs. **polled EP0 + 1 bulk EP** hand-rolled from `mtk_musb_reg.h` offsets. For a console, the latter is ~300 lines |

### 5.3 WATCHDOG — the easiest win, copy 2 functions

| # | Action | Source |
|---|---|---|
| 1 | Copy `mtk_wdt_disable()` | LK `platform/mt6765/mtk_wdt.c:51-58` — literally `MODE = (MODE & ~1) | 0x22000000` |
| 2 | Copy `mtk_wdt_restart()` (pet) | LK `:218-224` — `0x10007008 = 0x1971` |
| 3 | Copy `mtk_wdt_set_time_out_value()` | LK `:205-215` — `0x10007004 = (sec<<11) \| 8` |
| 4 | **Change:** read `WDT_STATUS` (0x0C) **first** | writing `WDT_MODE` clears it (vendor comment, `drivers/watchdog/mtk_wdt.c:198-202`) |
| 5 | **Change:** assume it is **armed at 10 s** on entry | `mtk_wdt_init()` arms dual+IRQ+ext reset, `mtk_wdt.c:319-345` |
| 6 | Add | stage marker via `WDT_NONRST_REG2` (0x24) bits [31:30] = 3 (kernel stage) to prove provenance |
| 7 | **Do not** port | `wdt_v2.c`'s driver plumbing (notifier, `watchdog_device`, PM ops), `wdk/`, `ext_wd_drv.h`, FIQ glue |

### 5.4 GIC — mostly "don't"

| # | Action | Source |
|---|---|---|
| 1 | **Do nothing** if polled | kernel `gic_dist_init`/`gic_cpu_init` contain no MTK-specific MMIO (`irq-gic.c:470-531`) |
| 2 | If you must take an IRQ | re-use LK's init verbatim: `lk-svoboda18/dev/gic/mt_gic_v2.c:41-92` — GICD_CTLR=0, ICFGR=0, ITARGETSR, IPRIORITYR=0xA0A0A0A0, ICENABLER=all-1, GICD_CTLR=1; GICC_PMR=0xF0, GICC_CTLR=1 |
| 3 | Polarity | if an IRQ is active-low and misbehaves, set the bit in `INT_POL_CTL0` (`0x10200A80 + (irq-32)/32*4`, bit `(irq-32)&31`) — `mt_gic_v2.c:114-136` |
| 4 | **Do not** write `GICD_IGROUPR` | it is a secure-world (TZ) resource; from NS-EL1 it is typically RAZ/WI and writing it can fault |
| 5 | **Do not** look for `GIC_SYNC_DCM` | it does not exist in this tree (`git grep` exit 1 in `drivers/misc/mediatek/base/power`) |
| 6 | **Do not** port | `irq-mtk-sysirq.c` (the kernel driver for the above block, 11.9 KB) unless you need dynamic polarity; `drivers/misc/mediatek/ext_gic/mtk-gic-extend.c` (22 KB) is sleep/FIQ-mask plumbing |

### 5.5 Cross-cutting MMU-off notes **[D]**

1. **`__raw_readl/__raw_writel` → `*(volatile u32*)`.** Every vendor macro bottoms out in these (`mtk_sd.h:499-502`); with MMU off the virtual address *is* the physical address, so the substitution is exact. But `mt_reg_sync_writel` adds a `dsb`; keep an explicit `dsb sy` after multi-register sequences (notably after `SDC_ARG`→`SDC_CMD`, and after each MSDC clock change).
2. **Device memory attributes.** With MMU off, all accesses are Device-nGnRnE by default on ARMv8 — which is exactly what you want for these peripherals, but it also means **no unaligned accesses** and **no read-back caching**. The MSDC PIO byte path (`msdc_fifo_write8`) needs byte stores, fine; the `u32` PIO path needs 4-byte alignment of your buffer.
3. **No `udelay`/`jiffies`.** The three USB delays (50/800/800 µs) and every MSDC poll timeout (`msdc_retry`'s `cnt=1000`, `mdelay(1)`) must be re-based on the arch counter (`CNTPCT_EL0`) or the GPT. The vendor's `msdc_retry` semantics are "1000 spins then 1 ms then retry, 3 retries" — replace with a deadline loop.
4. **Clocks are already on** when you arrive from LK, and LK sets MSDC0 to the 400 MHz PLL path and enables the gates. **First action in the shim should be reads, not writes** — dump `MSDC_CFG`, `SDC_CFG`, `MSDC_CFG[19:8]` and the infracfg_ao gate status, and *then* decide whether to re-init. This avoids fighting state LK set up for its own fastboot/partition writes.
5. **Exception levels.** LK runs at EL3/EL1-secure on this platform for its TZ interactions (`tz_cross`, `sec_policy`, SMC IDs in `platform/mt6765/include/platform/plat_smc_id_table.h`). Your shim at **EL1 non-secure, MMU off** cannot call those SMCs or touch secure-only blocks (e.g. `GICD_IGROUPR`, some efuse, DEVAPC permission tables). **[D]** — architectural inference, listed as a caution.

---

## 6. What I could **not** verify (be careful here)

| item | status |
|---|---|
| `USB20_PHY_BASE` literal | **[U]** — used by `mt_musbphy.c` but not defined in `mt_reg_base.h`; inferred to be `0x11CC0800` from the kernel accessor `io_priv + 0x800` with `io_priv = 0x11CC0000`. **Confirm by reading `0x11CC0868` on target and checking it is non-zero/garbage-sane.** |
| `CLK_CFG_3` numeric offset (MSDC50_0_SEL mux) | **[U]** — only `CLK_CFG_5 = 0x90` was extracted (line 270). Same block, so likely `0x84`–`0x8c`, unconfirmed |
| `R1_WP_VIOLATION` / `R1_OUT_OF_RANGE` values | **[U]** — macros exist in `mtk_sd.h`, numeric values not extracted |
| Actual CMD `rawcmd` values in §1.7 | **[D]** — computed from the bit layout in `sd.c:1486-1597`; not observed in a register dump |
| `CKDIV` table in §1.4 | **[D]** — arithmetic from `sd.c:816-830` with `hclk = 400 MHz`; depends on the mux actually selecting MSDCPLL/2 |
| Gate-bit polarity for `GATE_TOP1` (inverted) | **[V]** for the ops table (`no_setclr_inv` + `mtk_cg_enable_no_setclr`→`clr_bit`), **[D]** for the operational conclusion "bit=0 means on". Verify by reading `0x10000104` before/after |
| DEVAPC permission table contents | **[U]** — I read that the kernel does *not* program it at probe, but did not extract the preloader/LK table, so "MSDC0/USB/TOPRGU are AP-accessible" is inference |
| Whether the svoboda18 LK matches dandelion's **exact** LK build | **[U]** — it has `platform/mt6765` + `k62*` targets, but Xiaomi may ship a different target folder / patch level. Register-level content is MTK-standard and matches the kernel's `mt6765.dts` addresses exactly, so it is a sound reference regardless |
| MT6762G vs MT6765 differences | **[U]** — MT6762G ("dandelion") is the same die family; all addresses above come from `mt6765.dts`/`platform/mt6765` and are what both LK and the vendor kernel use for this device. No MT6762-specific register divergence was found, but I did not diff a second SoC DTS |
| `stock_kernel.config` provenance | pre-existing artifact in `D:\aios\research\`, used only for `CONFIG_*` cross-checks; not re-derived from the phone in this session |

---

## 7. Local file map (for follow-up reading)

```
D:\aios\research\
├─ mtk6765-drivers.md                     ← this report
├─ refs\
│  ├─ mt6765-files.txt                    ← full 67,792-path listing of the vendor kernel
│  ├─ mainline-irq-gic-v4.19.c            ← baseline used for the §4.1 diff
│  ├─ mt6765-vendor\                      ← MiCode/Xiaomi_Kernel_OpenSource @ 8974b91 (dandelion-q-oss)
│  │  ├─ drivers\mmc\host\mediatek\ComboA\   (sd.c, msdc_reg.h, mtk_sd.h, mtk_sd_misc.c, autok.c, mt6765\*)
│  │  ├─ drivers\mmc\host\mtk-sd.c           (mainline-style variant, for contrast)
│  │  ├─ drivers\misc\mediatek\usb20\        (mtk_musb_reg.h, mt6765\usb20_phy.c, mt6765\usb20.c, mt6765\mtk-phy-a60810.h)
│  │  ├─ drivers\watchdog\                   (mtk_wdt.c + mediatek\wdt\common\wdt_v2\mtk_wdt_v2.c)
│  │  ├─ drivers\irqchip\                    (irq-gic.c, irq-mtk-sysirq.c)
│  │  ├─ drivers\clk\mediatek\clk-mt6765.c   (gate tables + CLK_CFG_* offsets)
│  │  ├─ drivers\misc\mediatek\ext_gic\mtk-gic-extend.c
│  │  ├─ drivers\misc\mediatek\devapc\mt6765\devapc.c
│  │  └─ arch\arm64\boot\dts\mediatek\       (mt6765.dts, cust_mt6765_msdc.dtsi, dandelion.dts)
│  └─ lk-svoboda18\                       ← svoboda18/lk @ 37ec8bc  (MT6765 LK!)
│     ├─ platform\mt6765\                    (cust_msdc.c, msdc_io.c, mtk_wdt.c, interrupts.c, platform.c, pll.c,
│     │                                      mt_musb\mt_musbphy.c, include\platform\{msdc_reg.h,mtk_wdt.h,mt_irq.h,mt_reg_base.h})
│     ├─ platform\common\storage\mmc\msdc.c  ← freestanding MSDC driver (best template)
│     ├─ dev\gic\mt_gic_v2.c                 ← GICv2 init + INT_POL_CTL0
│     └─ target\k62v1_64_bsp\                ← MT6765 board target
```

Reproduction: `git clone --depth 1 --filter=blob:none --branch dandelion-q-oss https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git`, then `git config core.protectNTFS false` (the tree contains `drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c`, a Windows reserved name that breaks a full checkout) and materialise only the paths above with `git checkout HEAD -- <path>`.
