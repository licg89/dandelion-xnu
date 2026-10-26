# MT6765 (Redmi 9A / dandelion) — polled-PIO eMMC text log from a bare-metal EL1 shim

Register-level, implementable design. Target: MediaTek MT6762G/MT6765, MSDC0 → eMMC.
Author's stance: everything that follows is either (a) quoted from a public source that was actually
read while writing this, or (b) explicitly marked **UNVERIFIED** / **ASSUMPTION**. Nothing was tested on
hardware; no phone was touched.

---

## 0. Executive summary — the numbers you need

| Item | Value | Confidence |
|---|---|---|
| MSDC0 register base | `0x1123_0000`, 64 KiB window | **verified** (stock DTB `/msdc@11230000`) |
| MSDC0 "top"/pad control | `0x11CD_0000` (only needed for HS200/HS400 tuning — *not* for this design) | verified (DTB `/msdc0_top@11cd0000`) |
| PIO data ports | `MSDC_TXDATA = 0x18`, `MSDC_RXDATA = 0x1C` (32-bit or 8-bit accesses) | **verified** (vendor `msdc_reg.h` + NativeMsdc) |
| PIO select | `MSDC_CFG[3] = MSDC_CFG_PIO = 1` | **verified** (3 independent drivers) |
| FIFO depth / threshold | 128 B / 64 B; `MSDC_FIFOCS[7:0]` = RX bytes, `[23:16]` = TX bytes, `[31]` = clear | **verified** (`mtk_sd.h`: `MSDC_FIFO_SZ 128`) |
| MSDC0 source clock | `clk_src = 1` → `MSDCPLL/2` = **400 MHz** (MSDCPLL is defined as 800 MHz) | **verified** (`mt6765/msdc_cust.h`, DTB `clk_src`) |
| Clock divider for identify (≤400 kHz) | `CKMOD=0`, `CKDIV=260` → 400 MHz/(4·260) = **384.6 kHz** (exactly 400 kHz if MSDCPLL is the 416 MHz variant) | derived |
| Clock divider for data (~12.5 MHz) | `CKMOD=0`, `CKDIV=8` → **12.5 MHz** (13 MHz if 416 MHz PLL) | derived |
| eMMC RCA | **1** (Linux hard-codes it for eMMC; LK does the same) | **verified** (`core/mmc.c`: `card->rca = 1;`) |
| Block size | 512 B, sector (LBA) addressing once OCR bit 30 = 1 | verified |
| Scratch area | GPT partition named `flashinfo`, 16 MiB, addressed by LBA | per task statement; LBA must be discovered at run time |
| Minimum viable milestone | single-block CMD24 write of one 512-B sector + read back from Android | §6 |

**Design decision that dominates the effort:** do **not** re-implement the clock tree, pad muxing or
DMA. The shim runs after LK has already used this controller to load the very image being executed, so
MSDC0's gate is open, the pads are muxed, `MSDC_CFG.CKSTB` reads 1, and the card is powered and in the
transfer state. The shim only has to (1) slow the bus clock down, (2) re-identify the card from CMD0 so it
is in a known state, (3) do PIO.

---

## 1. MSDC register map, reset, clocking, PIO configuration

### 1.1 Where the numbers come from

Three independent sources agree on the register layout, which is why I trust it:

1. **MediaTek's own MT6765 driver** shipped in Xiaomi's dandelion kernel release — this is *the* driver
   that runs on this exact phone (`CONFIG_MMC_MTK_PRO=y` in `D:\aios\research\stock_kernel.config`,
   `# CONFIG_MMC_MTK is not set`):
   [`drivers/mmc/host/mediatek/ComboA/msdc_reg.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/msdc_reg.h),
   [`mtk_sd.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/mtk_sd.h),
   [`sd.c`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/sd.c),
   [`mt6765/msdc_cust.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/mt6765/msdc_cust.h),
   [`mt6765/msdc_cust.c`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/mt6765/msdc_cust.c).
2. **Linux upstream** [`drivers/mmc/host/mtk-sd.c`](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/mtk-sd.c)
   — same offsets, plus the authoritative comment block that documents the `SDC_CMD` bit layout.
3. **mouseos/NativeMsdc** (MT8167, same MSDC IP family) — the only public implementation I found that
   does **pure polled PIO for eMMC block I/O** and says so:
   [`NativeMsdc.h`](https://github.com/mouseos/NativeMsdc/blob/master/NativeMsdc.h),
   [`NativeMsdc.cpp`](https://github.com/mouseos/NativeMsdc/blob/master/NativeMsdc.cpp). Its header states
   the register usage came from `bsd/dev/a04br3_msdc.c` of
   [xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3).

Cross-check of the *platform* facts against the phone's own boot DTB
(`D:\aios\stock_boot.dtb`, dumped with `D:\aios\research\_msdc_dtb.py`):

```
/msdc@11230000   reg 0x11230000 len 0x10000  index 0  clk_src 1  host_function 0 (EMMC)
                 bus-width 8  max-frequency 200 MHz  non-removable  bootable
                 cap-mmc-highspeed, mmc-ddr-1_8v, mmc-hs200-1_8v, mmc-hs400-1_8v, no-sd, no-sdio
                 clock-names "msdc0-clock", "msdc0-hclock", "msdc0-aes-clock"
/msdc0_top@11cd0000  reg 0x11cd0000 len 0x1000
```

So: **eMMC is MSDC0 at 0x1123_0000**, and it is the only eMMC-capable controller (`msdc@11250000` is
`disabled`, `msdc@11240000` is the SD slot with `no-mmc`).

### 1.2 Register map (offsets from 0x1123_0000)

Offsets and bit positions are from `msdc_reg.h`; the "used?" column says whether *this* design touches it.

| Off | Name | Key fields (this IP) | Used? |
|---|---|---|---|
| `0x00` | `MSDC_CFG` | `[0] MODE` (1=SD/MMC), `[1] CKPDN`, `[2] RST` (self-clearing), `[3] **PIO**`, `[4] CKDRVEN`, `[5] BV18SDT`, `[6] BV18PSS` (RO), `[7] CKSTB` (RO), `[19:8] **CKDIV**` (12 bits), `[21:20] **CKMOD**`, `[22] CKMOD_HS400`, `[24:23] START_BIT`, `[25] SCLK_STOP_DDR`, `[27] DVFS_IDLE`, `[28] DVFS_HW`, `[30] DVFS_EN`, `[31] RE_TRIG` | **RMW** |
| `0x04` | `MSDC_IOCON` | `[0] SDR104CKS`, `[1] RSPL` (cmd response sample edge), `[2] R_D_SMPL`, `[5] R_D_SMPL_SEL`, `[8] W_D_SMPL`, `[9] W_D_SMPL_SEL`, `[16..23] R_D0..D7SPL` | inherit (see §1.7) |
| `0x08` | `MSDC_PS` | `[16..23] DAT[0..7] pin level`, `[24] CMD`, `[1] CDSTS`, `[0] CDEN` | read `[16]` = DAT0 for busy check |
| `0x0C` | `MSDC_INT` | W1C status; `[8] CMDRDY`, `[9] CMDTMO`, `[10] RSPCRCERR`, `[12] XFER_COMPL`, `[13] DXFER_DONE`, `[14] DATTMO`, `[15] DATCRCERR`, `[3] ACMDRDY`, `[19] DMAPRO`, `[17/18] BDCS/GPDCS ERR` | **polled + W1C** |
| `0x10` | `MSDC_INTEN` | same bit map, enable | **write 0** |
| `0x14` | `MSDC_FIFOCS` | `[7:0] RXCNT` (bytes), `[23:16] TXCNT` (bytes), `[31] CLR` | **read + clear** |
| `0x18` | `MSDC_TXDATA` | PIO write port (32-bit; 8-bit works for tails) | **write** |
| `0x1C` | `MSDC_RXDATA` | PIO read port | read (for GPT/EXT_CSD) |
| `0x30` | `SDC_CFG` | `[1] INSWKUP`, `[14:2] WRDTOC` (13 bits, busy timeout, unit 2^20 sclk), `[17:16] BUSWIDTH` (0=1-bit,1=4-bit,2=8-bit), `[19] SDIO`, `[20] SDIOIDE`, `[21] INTATGAP`, `[31:24] DTOC` (8 bits, data timeout, unit 2^20 sclk) | **RMW** |
| `0x34` | `SDC_CMD` | command word, see §1.3 | **write** |
| `0x38` | `SDC_ARG` | 32-bit argument | **write** |
| `0x3C` | `SDC_STS` | `[0] SDCBUSY`, `[1] CMDBUSY`, `[16] CMD_WR_BUSY` (W1C), `[31] SWR_COMPL` | **poll** |
| `0x40..0x4C` | `SDC_RESP0..3` | `RESP0` = bits 31:0 of the response; for R2: `RESP3`=127:96 … `RESP0`=31:0 | **read** |
| `0x50` | `SDC_BLK_NUM` | block count for the *next* data command | **write** |
| `0x64` | `SDC_ADV_CFG0` | `[14] ENDBIT_CHECK`, `[15] INDEX_CHECK`, `[20] SDC_RX_ENH_EN` | inherit |
| `0x7C` | `EMMC_IOCON` | `[0] BOOTRST` — **do not touch** (see §1.7) | no |
| `0x98/0x9C` | `MSDC_DMA_CTRL/CFG` | DMA engine | no (must stay idle) |
| `0xB0/0xB4/0xB8` | `MSDC_PATCH_BIT0/1/2` | undocumented platform patch bits; `PB0[30]`=DETWR_CRCTMO, `PB1[7]`=BUSY_CHECK_SEL, `PB2[1]`=SUPPORT64G, `PB2[28]`=CFGCRCSTS | inherit |
| `0x114` | `MSDC_VERSION` | RO version | read for sanity |
| `0x118` | `MSDC_ECO_VER` | RO ECO | read for sanity |
| `0x208` | `EMMC50_CFG0` | `[0] PADCMD_LATCHCK`, `[3] CRC_STS_EDGE`, `[4] CRC_STS_SEL`, `[9] CMD_RESP_SEL`, `[11..] ENDBIT_CNT` | inherit |

**Do not** confuse `0x18/0x1C` with the FIFO *count* register (`0x14`). The mapping PIO→`0x18`/`0x1C` is
confirmed three ways: `msdc_reg.h` (`OFFSET_MSDC_TXDATA 0x18`, `OFFSET_MSDC_RXDATA 0x1c`), `mtk_sd.h`
(`msdc_fifo_write32(v) → MSDC_WRITE32(MSDC_TXDATA, v)`), and NativeMsdc (`kMsdcTxData = 0x18,
kMsdcRxData = 0x1c`).

### 1.3 `SDC_CMD` (0x34) — the command word

Documented verbatim in `mtk-sd.c`:

```
/* rawcmd :
 * vol_swt << 30 | auto_cmd << 28 | blklen << 16 | go_irq << 15 |
 * stop << 14 | rw << 13 | dtype << 11 | rsptyp << 7 | brk << 6 | opcode
 */
```

and identical in the MT6765 vendor driver (`sd.c`, `msdc_command_start()`), whose masks are
`SDC_CMD_OPC (0x3f<<0)`, `SDC_CMD_BRK(1<<6)`, `SDC_CMD_RSPTYP(7<<7)`, `SDC_CMD_DTYP(3<<11)`,
`SDC_CMD_RW(1<<13)`, `SDC_CMD_STOP(1<<14)`, `SDC_CMD_BLKLEN(0xfff<<16)`, `SDC_CMD_AUTOCMD(3<<28)`.

| bits | field | values |
|---|---|---|
| 5:0 | opcode | |
| 6 | brk | 0 |
| 9:7 | rsptyp | 0 = none, 1 = R1/R5/R6/R7, 2 = R2 (136-bit), 3 = R3 (OCR), 7 = R1b |
| 12:11 | dtype | 0 = no data, 1 = single block, 2 = multiple blocks |
| 13 | rw | 1 = write (host→card), 0 = read |
| 14 | stop | 1 for CMD12 (vendor also clears the blklen field for CMD12) |
| 15 | go_irq | 0 |
| 27:16 | blklen | 512 |
| 29:28 | autocmd | 0 = none, 1 = auto CMD12, 2 = auto CMD23 |
| 30 | volswth | 0 |
| 31 | acmd53 | 0 |

Pre-computed words for this design (matches NativeMsdc's `static_assert`s where they overlap):

| CMD | Word | Meaning |
|---|---|---|
| CMD0 GO_IDLE_STATE | `0x00000000` | no response |
| CMD1 SEND_OP_COND | `0x00000181` | R3 |
| CMD2 ALL_SEND_CID | `0x00000102` | R2 |
| CMD3 SET_RELATIVE_ADDR | `0x00000083` | R1 |
| CMD6 SWITCH | `0x00000386` | R1b (vendor uses R1b; `msdc_rsp[RESP_R1b] = 7`) |
| CMD7 SELECT_CARD | `0x00000087` | R1 |
| CMD8 SEND_EXT_CSD | `0x02000888` | R1 + single block read, 512 B |
| CMD9 SEND_CSD | `0x00000109` | R2 |
| CMD12 STOP_TRANSMISSION | `0x0000438C` | R1b + stop bit |
| CMD13 SEND_STATUS | `0x0000008D` | R1 |
| CMD16 SET_BLOCKLEN | `0x00000090` | R1 |
| CMD24 WRITE_BLOCK | `0x02002898` | R1 + single, write, 512 B |
| CMD25 WRITE_MULTIPLE_BLOCK | `0x02003099` | R1 + multi, write, 512 B |

R2 (CMD2/CMD9) read order, from the vendor's `msdc_command_resp_polling()`:
`resp[0]=SDC_RESP3, resp[1]=SDC_RESP2, resp[2]=SDC_RESP1, resp[3]=SDC_RESP0`
(i.e. `RESP3` holds the most-significant 32 bits; CID byte 0 = manufacturer ID = `RESP3 >> 24`).

### 1.4 Reset / bring-up sequence (ordered, with what to poll)

Preconditions the shim must **verify**, not assume:

```c
/* 1. Is the controller clocked at all?  LK opened msdc0-clock + msdc0-hclock.
 *    If CKSTB never sets, everything else is pointless: report on the panel and bail. */
if (!(rd(MSDC_CFG) & MSDC_CFG_CKSTB)) -> fatal "no MSDC clock"
```

Then:

| # | Action | Poll / expect |
|---|---|---|
| 1 | `wr(MSDC_INTEN, 0)` — pure polling, no IRQs wanted | — |
| 2 | `wr(MSDC_INT, 0xFFFF_FFFF)` — clear all W1C bits | — |
| 3 | Slow the clock **first**: `cfg = rd(MSDC_CFG)`; clear `CKMOD|CKDIV`; set `CKDIV = 260`, `CKMOD = 0`; also set `MODE = 1` | `MSDC_CFG[7] CKSTB == 1` within 20 ms (`msdc_clk_stable()` uses retries of 1000 × 3 with a 20 ms bound in `msdc_ungate_clock()`) |
| 4 | Soft reset: `MSDC_CFG |= RST` | `RST` self-clears (`msdc_reset_hw()` in both Linux and vendor driver) |
| 5 | FIFO clear: `MSDC_FIFOCS |= CLR` | `CLR` self-clears |
| 6 | Re-check `CKSTB` (reset can drop/re-lock the divider) | must be 1 |
| 7 | `MSDC_CFG |= PIO` (bit 3) — select the FIFO data path | — |
| 8 | `SDC_CFG` RMW: `BUSWIDTH = 0` (1-bit), `DTOC = 0xF0`, `WRDTOC = 0x1FFF` | — |
| 9 | Sanity read `MSDC_VERSION` (0x114), `MSDC_ECO_VER` (0x118) | non-zero, print to panel |
| 10 | Card bring-up (CMD0/1/2/3/9/7/16/8), §2 | see §2 |
| 11 | *Optional*: raise the clock (CKDIV→8) and re-CMD13 before the first data transfer | `CKSTB`, then CMD13 OK |

`MSDC_CFG.PIO` must be set **before** `SDC_CMD` is written for a data command — the data path is chosen
when the command starts. NativeMsdc does exactly that inside its `HostSession` constructor
(`wr(kMsdcCfg, cfg | kCfgPio)`), and its destructor restores the inherited value.

### 1.5 Clock: deriving the source without touching topckgen

The vendor driver does **not** read topckgen to learn the frequency; it uses a compile-time table indexed
by the DT `clk_src` property (`mt6765/msdc_cust.h`, `mt6765/msdc_cust.c`):

```c
#define MSDCPLL_FREQ   800000000
#define MSDC0_SRC_0    260000            /* 26 MHz / 100 */
#define MSDC0_SRC_1    (MSDCPLL_FREQ/2)  /* = 400 MHz */
u32 hclks_msdc0[] = { MSDC0_SRC_0, MSDC0_SRC_1 };
u32 *hclks_msdc_all[] = { hclks_msdc0, hclks_msdc1, hclks_msdc3 };
#define msdc_get_hclk(id, src)  hclks_msdc_all[id][src]
```

and the DTB says `/msdc@11230000 { clk_src = 1; }` → **hclk = 400 MHz**.
The mux path is also known: `CLK_TOP_MSDC50_0_SEL` selects `msdc50_0_sel` whose parents are
`{clk26m, msdcpll_ck, syspll2_d2, syspll4_d2, univpll1_d2, syspll1_d2, univpll_d5, univpll1_d4}`
(MT6765 clock driver, `D:\aios\research\_v_clk-mt6765.c` lines 633–642, 798–800), and MSDCPLL lives in
apmixedsys at `0x027C/0x0280/0x0288`.

**Divider formula** (identical in Linux `msdc_set_mclk()` and vendor `sd.c`), for non-DDR mode
(`CKMOD = 0`):

```
if (hz >= hclk/2)  div = 0;                  sclk = hclk/2
else               div = ceil(hclk / (4*hz)); sclk = hclk / (4*div)
```

`CKDIV` is the register field `[19:8]`; `CKMOD` is `[21:20]`; both are written in one RMW of
`MSDC_CFG`. (Linux builds the same field as `(mode << 12) | div` relative to bit 8 — consistent.)

**Recommended values, chosen so they are safe for *both* plausible PLL rates.** Note the vendor clock
name list contains `AD_MSDCPLL_416M_CK` (`_v_clk-mt6765.c` line 137), i.e. this platform has a 416 MHz
MSDCPLL variant for DVFS — so do **not** assume exactly 400 MHz:

| Purpose | `CKDIV` | sclk @400 MHz | sclk @416 MHz | Spec limit |
|---|---|---|---|---|
| Identify (CMD0..CMD16, CMD8) | **260** | 384.6 kHz | 400.0 kHz | ≤ 400 kHz ✔ |
| Data (CMD24/25) | **8** | 12.5 MHz | 13.0 MHz | ≤ 26 MHz (default speed) ✔ |
| Optional faster (only after CMD6 HS_TIMING=1) | 4 | 25 MHz | 26 MHz | needs high-speed mode |

Throughput at 12.5 MHz, 1-bit: **1.56 MB/s** → a 1 MiB log costs ~0.7 s of bus time. That is why 1-bit
mode is acceptable for v1 and 8-bit/DDR is pure optimisation (§6, M5).

**If you want to *measure* instead of trusting the table** (recommended once, on the bench): issue one
CMD17/CMD18 read of N blocks at a known `CKDIV`, time it with `CNTPCT_EL0`, and solve
`sclk ≈ 8 × bytes / seconds`. With N = 1 MiB and one sample you get ~1 % accuracy; that is enough to
catch a wrong PLL assumption. This needs no topckgen knowledge at all.

**What to probe if you insist on reading the clock tree**: `topckgen_base + 0x70` (the vendor's own
register dump prints exactly this and documents the expected bit patterns:
`bit[1:0]=01b, bit[7]=0, bit[10:8]=001b, bit[15]=0, bit[18:16]=001b, bit[23]=0` —
`mt6765/msdc_cust.c`, `msdc_dump_clock_sts_core()`), plus apmixed `0x027C..0x0288` for MSDCPLL. This is
strictly optional and I would skip it: the table + a one-off measurement is less code and less risk.

### 1.6 Register values that make PIO work

Nothing exotic. PIO is selected by one bit and driven through two registers; the rest is timeouts and bus
width:

```c
/* MSDC_CFG: MODE=1 (SD/MMC), PIO=1, CKMOD=0, CKDIV as above.
 * Everything else (CKDRVEN, BV18SDT, DVFS_EN, ...) must be preserved. */
cfg = rd(MSDC_CFG);
cfg &= ~(MSDC_CFG_CKMOD | MSDC_CFG_CKDIV);
cfg |= MSDC_CFG_MODE | MSDC_CFG_PIO | (260u << 8);
wr(MSDC_CFG, cfg);

/* SDC_CFG: 1-bit bus, generous hardware timeouts; keep SDIO/INSWKUP/INTATGAP as inherited. */
sdc = rd(SDC_CFG);
sdc &= ~SDC_CFG_BUSWIDTH;          /* 0 = 1 bit */
sdc &= ~(SDC_CFG_DTOC | SDC_CFG_WRDTOC);
sdc |= (0xF0u << 24);              /* DTOC   : 240 x 2^20 sclk  (~20 s @12.5 MHz) */
sdc |= (0x1FFFu << 2);             /* WRDTOC : 8191 x 2^20 sclk (~11 min @12.5 MHz) */
wr(SDC_CFG, sdc);

/* Per transfer: SDC_BLK_NUM = block count; FIFOCS |= CLR and wait for CLR to drop. */
```

Why those timeout fields: `DTOC` is the card data timeout (`SDC_CFG[31:24]`, max 255, unit 2^20 sclk —
`msdc_set_timeout()`), `WRDTOC` is the busy/programming timeout (`SDC_CFG[14:2]`, max 8191 —
`msdc_set_busy_timeout_ms()`). The vendor default for `DTOC` is `DEFAULT_DTOC = 3` (`msdc_cust.h`), which
is only ~0.25 s at 12.5 MHz — too tight for a write path that we want to fail *slowly and diagnosably*.
They are hardware backstops; our software deadlines (§3) are the primary limit.

Note the vendor sets `SDC_CFG.SDIO` (bit 19) unconditionally in `msdc_init_hw()` with the comment
"it's must otherwise sdio cmd5 failed". For an eMMC-only path it should be irrelevant — but because it is
unconditional in the driver that actually runs on this phone, **do not clear it**; only touch
`BUSWIDTH`, `DTOC`, `WRDTOC`.

### 1.7 What *not* to touch (and why)

* **`MSDC_PATCH_BIT0/1/2` (0xB0/0xB4/0xB8), `EMMC50_CFG0` (0x208), `MSDC_IOCON` (0x04),
  `MSDC_PAD_TUNE*`, `SDC_ADV_CFG0`, `EMMC50_PAD_*`** — these hold pad sampling edges, end-bit checks,
  64 GiB support, CRC-status sampling and other per-platform tuning set by LK. The vendor defaults are
  known (`MSDC_PB0_DEFAULT_VAL 0x403C0007`, `MSDC_PB1_DEFAULT_VAL 0xFFFA0349`, `MSDC_PB2_DEFAULT_*` in
  `mt6765/msdc_cust.h`) but LK may have adjusted them. **Read, save, restore, never blindly write.**
  Exception: if you ever have to re-init from a cold state, those two defaults are the best available
  reference values.
* **`EMMC_IOCON[0] BOOTRST`** — the vendor's own pin-config comments warn "Don't toggle RST to prevent
  from entering boot mode" (`mt6765/msdc_cust.c`, `msdc_pin_config_by_id()`, id 0). Asserting RST_n on
  MTK eMMC parts can make the next boot enter eMMC boot mode. Do not use it for error recovery; the
  supported recovery is CMD0 + re-init.
* **`MSDC_CFG.BV18SDT` / voltage switching** — eMMC on this board is 1.8 V I/O (`VIO18`) with 3.0 V VCC
  (`VEMC`), both already up; there is nothing to switch.
* **`MSDC_CFG.CKDRVEN` (bit 4)** and **`MSDC_CFG.DVFS_*` (bits 27–31)** — undocumented enough that
  guessing is worse than inheriting. **UNVERIFIED**: what CKDRVEN=0/1 implies on this IP.
* **The DMA engine** — leave `MSDC_DMA_CTRL`/`MSDC_DMA_CFG` alone. If a previous stage left DMA active,
  stop it (`DMA_CTRL |= STOP`, poll `DMA_CFG.STS == 0`) before touching the FIFO; NativeMsdc treats "DMA
  engine did not stop" as a permanent quarantine.

---

## 2. eMMC command sequence to be ready to write at an arbitrary LBA

### 2.1 Reference for the sequence

Linux's attach path is the cleanest statement of the sequence and is what I follow
(`drivers/mmc/core/mmc.c`, `mmc_attach_mmc()` / `mmc_init_card()`, and `core/mmc_ops.c`):

```
cmd0 (GO_IDLE, no response)                     -> delay 1 ms
cmd1 (SEND_OP_COND, arg = ocr|BIT(30), R3)      -> repeat until resp & BUSY(bit31) == 0
cmd2 (ALL_SEND_CID, R2)                         -> 128-bit CID
cmd3 (SET_RELATIVE_ADDR, arg = rca<<16, R1)     -> RCA = 1
cmd9 (SEND_CSD, arg = rca<<16, R2)              -> 128-bit CSD
cmd7 (SELECT_CARD, arg = rca<<16, R1)
cmd8 (SEND_EXT_CSD, read 512 B)
```

with two details worth quoting:

* CMD1 is retried as a *poll*: `__mmc_poll_for_busy(host, MMC_OP_COND_PERIOD_US /*4 ms*/,
  MMC_OP_COND_TIMEOUT_MS /*1000*/)` and "busy" is `resp[0] & MMC_CARD_BUSY`. Linux additionally notes
  (eMMC v5.1 §6.4.3) that CMD1 must be re-issued in the idle state, and after the first response it sets
  `cmd->arg = resp[0] | BIT(30)` — i.e. **bit 30 in the argument is the "I want sector addressing"
  request**, and its echo in the response is CCS.
* `card->rca = 1;` — RCA is a constant 1 for eMMC. **This is what makes the "fast path" of §2.4
  possible**: an inherited controller can be used immediately with `arg = 1 << 16`.

### 2.2 Step-by-step (polled, with success criteria)

`T` = software deadline from `CNTPCT_EL0`. Every step has one.

| Step | SDC_CMD | ARG | Response | Success looks like | On failure |
|---|---|---|---|---|---|
| 0 | — | — | — | `SDC_STS.SDCBUSY == 0` (T=100 ms) | host busy → reset controller |
| 1 | `0x00000000` CMD0 | 0 | none | `CMDRDY` within T=100 ms; no RSPCRCERR expected | retry once, else abort |
| 2 | `0x00000181` CMD1 | `0x401F0000` (see note) | R3 | first: `RESP0 & 0x8000_0000` (busy). Repeat every 10 ms. Ready when busy=0; **note `RESP0 & 0x4000_0000` = CCS (1 ⇒ LBA addressing)**; OCR voltage bits `[23:15]` non-zero | T = 1 s (Linux uses 1 s / 4 ms) → error |
| 3 | `0x00000102` CMD2 | 0 | R2 | `RESP3>>24` = valid manufacturer ID (0x11/0x13/0x15/0x45/0x90 are common); all four RESP regs non-zero/non-0xFFFFFFFF | T=100 ms → re-init from CMD0 |
| 4 | `0x00000083` CMD3 | `0x0001_0000` | R1 | `RESP0 & 0xFFFF_0000 == 0x0001_0000` (RCA echoed) and status has no error bits; state becomes STBY (3) | T=100 ms → CMD0 |
| 5 | `0x00000109` CMD9 | `0x0001_0000` | R2 | `RESP3[31:30]` = CSD_STRUCTURE (2 for v4.x/512 B, 3 for >2 GiB); `READ_BL_LEN = (RESP2 >> 16) & 0xF` should be 9 (512 B) | T=100 ms |
| 6 | `0x00000087` CMD7 | `0x0001_0000` | R1 | state = TRAN (4), `READY_FOR_DATA` = 1 | T=100 ms → CMD0 |
| 7 | `0x00000090` CMD16 | 512 | R1 | no error bits | optional (see note) |
| 8 | `0x02000888` CMD8 | 0 | R1 + 512 B data | 512 B in RX FIFO, `EXT_CSD[192]` (rev) sane; **`EXT_CSD[179] & 0x7 == 0`** (user area selected!); `EXT_CSD[212..215]` = sector count; `EXT_CSD[33] & 1` = cache on | T=1 s |
| 9 | `0x0000008D` CMD13 | `0x0001_0000` | R1 | state = TRAN, READY_FOR_DATA = 1 → **ready to write at any LBA** | T=100 ms |

Notes:
* **CMD1 argument.** The OCR voltage field is bits 23:15 for 2.7–3.6 V in 100 mV steps (bit 15 = 2.7–2.8 V,
  bit 23 = 3.5–3.6 V) plus bit 7 = 1.65–1.95 V (`MMC_VDD_165_195`). This platform declares
  `MSDC_OCR_AVAIL = MMC_VDD_28_29|29_30|30_31|31_32|32_33` (`mt6765/msdc_cust.h`) = `0x001F0000`, and
  Linux sends `(card_ocr & host_ocr_avail) | BIT(30)`, so the value the vendor driver effectively uses here is
  **`0x401F0000`**. The widely-copied `0x40FF8080` also works (it advertises the full 2.7–3.6 V range *and*
  the 1.65–1.95 V window; the latter is meaningless for a 3.0 V `VEMC` part). Either way, bit 30 = "I want
  sector addressing" and the response's bit 30 = CCS is what actually matters.
* **CMD16**: Linux does not send it for eMMC (block length comes from CSD `READ_BL_LEN`, normally 512);
  U-Boot does. It is legal in TRAN state and harmless, so send it as a defensive measure — but do not set
  the `blklen` field in `SDC_CMD` for CMD16 (only `SDC_ARG = 512`), because `SDC_CMD[27:16]` describes a
  *data phase*, and CMD16 has none.
* **CMD8/EXT_CSD is not optional in practice** — `EXT_CSD[179]` bit 0..2 (`PARTITION_ACCESS`) tells you
  *which* physical partition subsequent LBAs refer to. If a previous stage left the boot partition
  selected, your "flashinfo" writes would silently corrupt `boot1`/`boot2`. The vendor driver refuses to
  continue in that case (`sd.c`: `if (ext_csd[179] & 7) -> "no capacity or a boot/RPMB partition
  selected"` in NativeMsdc's bring-up). Fix with CMD6 (§2.5) or abort.
* `EXT_CSD[212..215]` = `SEC_COUNT`, little-endian; `EXT_CSD[33]` bit 0 = `CACHE_CTRL`;
  `EXT_CSD[32]` = `FLUSH_CACHE` (write-only); `EXT_CSD[183]` = `BUS_WIDTH`, `[185]` = `HS_TIMING`
  (`NativeMsdc.cpp` `hostBringUp()`, `hostLogMode()`).

### 2.3 Decoding R1 and polling "ready for data"

R1 (CMD13) bit assignments — layout per JEDEC JESD84-B51 §6.13; the individual bits I could confirm in
the sources actually read are flagged:

| Bit | Name | Confirmed in |
|---|---|---|
| 31 | `OUT_OF_RANGE` | vendor `sd.c` (`R1_OUT_OF_RANGE`) |
| 30 | `ADDRESS_MISALIGN` | — (spec) |
| 29 | `BLOCK_LEN_ERROR` | — (spec) |
| 28 | `ERASE_SEQ_ERROR` | — (spec) |
| 27 | `ERASE_PARAM` | — (spec) |
| 26 | `WP_VIOLATION` | vendor `sd.c` |
| 25 | `CARD_IS_LOCKED` | — (spec) |
| 24 | `LOCK_UNLOCK_FAILED` | — (spec) |
| 23 | `COM_CRC_ERROR` | — (spec) |
| 22 | `ILLEGAL_COMMAND` | vendor `sd.c` (`if (status & (1 << 22)) /* illegal command */`) |
| 21 | `CARD_ECC_FAILED` | — (spec) |
| 20 | `CC_ERROR` | — (spec) |
| 19 | `ERROR` | — (spec) |
| 18 | `UNDERRUN` | — (spec) |
| 17 | `OVERRUN` | — (spec) |
| 16 | `CID/CSD_OVERWRITE` | — (spec) |
| 15 | `WP_ERASE_SKIP` | — (spec) |
| 14 | `CARD_ECC_DISABLED` | — (spec, informational) |
| 13 | `ERASE_RESET` | — (spec) |
| 12:9 | `CURRENT_STATE` | `mtk_sd.h`: `CARD_CURRENT_STATE(x) = (x & 0x1E00) >> 9` |
| 8 | `READY_FOR_DATA` | `mtk_sd.h`: `CARD_READY_FOR_DATA (1<<8)`; Linux `mmc_ready_for_data()` |
| 7 | `SWITCH_ERROR` | — (spec; Linux `mmc_switch_status_error()`) |
| 6 | `EXCEPTION_EVENT` | — (spec) |
| 5 | `APP_CMD` | — (spec) |
| 3 | `AKE_SEQ_ERROR` (SD only) | — (spec) |

States: 0 Idle, 1 Ready, 2 Ident, 3 Stby, **4 Tran**, 5 Data(read), **6 Rcv(write data)**, **7 Prg**,
8 Dis, 9 Boot.

Two masks are useful:

```c
#define R1_READY_FOR_DATA  (1u << 8)
#define R1_STATE(s)        (((s) >> 9) & 0xFu)
#define R1_STATE_TRAN      4u
#define R1_STATE_PRG       7u

/* "something is really wrong" — bits 31..19, 18..16, 15, 13, 7.
 * Note this includes CARD_IS_LOCKED(25) and CARD_ECC_DISABLED(14 is excluded).
 * NativeMsdc uses 0xfdffa080 (same minus CARD_IS_LOCKED); either is fine. */
#define R1_FATAL_MASK      0xFFFFA080u
```

**Polling rules** (this is the part that decides whether you hang or not):

1. *Before* issuing a command with data or an R1b command, wait for the **host** to be idle:
   `SDC_STS.SDCBUSY == 0` (100 ms). For CMD12/CMD13 only `SDC_STS.CMDBUSY` must be clear — this is what
   lets you poll the card's programming state while DAT0 is still busy. Confirmed twice: vendor
   `msdc_command_start()` (`if (opcode == MMC_SEND_STATUS) wait cmd_busy else wait sdc_busy`) and
   NativeMsdc `busyMask(opcode)`.
2. *Before* issuing CMD24/CMD25, wait for the **card** to be ready: `CMD13` until
   `R1_STATE == TRAN && READY_FOR_DATA`. Linux's `mmc_poll_for_busy()` defines busy as
   `!mmc_ready_for_data(status)` and gives up with "Card stuck being busy!" after the timeout — copy that
   shape exactly, including the 1 ms-ish poll interval (Linux ramps `udelay` 32 µs → 32 ms).
3. After CMD24/25 + data, the card goes TRAN → RCV → **PRG** → TRAN. `READY_FOR_DATA` is 0 while PRG.
   Poll CMD13 every ~1 ms up to ~5 s (a 512 B write is typically ≪ 50 ms; the 5 s bound covers bad
   blocks / GC). Do **not** use `MSDC_PS` DAT0 alone as the completion signal — use CMD13, and treat
   `MSDC_PS[16]` as a cross-check (`(MSDC_PS >> 16) & 1`; the vendor's `msdc_stop_and_wait_busy()`
   polls `MSDC_PS & 0x10000`).

### 2.4 Optional but cheap: the "inherit and verify" fast path

If the card is already selected with RCA=1 (it is, if LK did the last I/O — and Linux/XNU both use RCA 1),
you can skip §2.2 entirely:

```
CMD13(1<<16) -> expect TRAN + READY_FOR_DATA   (T=20 ms)
CMD8         -> EXT_CSD; check [179]&7 == 0, [212..215] sector count
CMD24/25     -> write
```

NativeMsdc is built entirely on this assumption ("The controller is inherited from the boot firmware as
it was left (card in the transfer state, bus width, clock and sampling unchanged); this driver never
resets or retunes it") and validates it with exactly that CMD13. **Recommendation:** implement this path
*first* (it is ~40 lines and derisks everything except the FIFO), log to the panel what it saw, and keep
the full CMD0-based re-init as the fallback/repair path. If the CMD13 fast path works, you never need to
know or care what clock/width state LK left.

Risk if you rely on it blindly: if LK left the card in a boot partition or in HS400, CMD13 may still
answer while writes go somewhere unexpected. Hence the `EXT_CSD[179]` check.

### 2.5 CMD6 write-byte: cache flush and partition select

Both are `CMD6` with `arg = (0b11 << 24) | (index << 16) | (value << 8) | 0` (write-byte mode;
`__mmc_switch()` in `core/mmc_ops.c` builds exactly this with `MMC_SWITCH_MODE_WRITE_BYTE = 3`).

| Purpose | index | value | arg | Timeout |
|---|---|---|---|---|
| **Flush the eMMC write cache** | 32 (`FLUSH_CACHE`) | 1 | `(3u<<24)|(32u<<16)|(1u<<8)` = `0x03200100` | up to 30 s (Linux `CACHE_FLUSH_TIMEOUT_MS 30000`) |
| Select user area | 179 (`PART_CONFIG`) | value from EXT_CSD with bits 2:0 cleared | `(3<<24)|(179<<16)|(v<<8)` | 300 ms…1 s |
| Bus width 8-bit (optional) | 183 (`BUS_WIDTH`) | 2 | `(3<<24)|(183<<16)|(2<<8)` | then `SDC_CFG.BUSWIDTH=2` |

CMD6 is R1b. Since the shim's hardware busy-check is off (`MSDC_PATCH_BIT1[7]` inherited = 0), treat it
exactly like any R1 command and then poll CMD13 until the switch completes (Linux does the same, with
`send_status=true`). **Flush the card cache after the last log write**; otherwise a power cut can lose
the tail even though your write "succeeded" — `CACHE_CTRL` is enabled by Android's kernel
(`EXT_CSD[33] & 1`), and NativeMsdc calls this out: writes "return only when the card is back in the
transfer state. Forced unit access … adds a FLUSH_CACHE when the card cache is on, so the data is on the
medium when the request completes."

---

## 3. `msdc_write_sectors()` — MMU-off, EL1, polled PIO

### 3.1 Environment assumptions (all checkable)

* AArch64 EL1, `SCTLR_EL1.M == 0`. With the MMU off there is no D-cache to maintain and all accesses are
  strongly ordered, so no cache maintenance is needed anywhere in this file.
  (**UNVERIFIED** caveat for the future: if you ever enable the MMU, any buffer handed to the FIFO must
  be cleaned with `DC CVAC` + `DSB` first — NativeMsdc's `cacheCleanInvalidate()` exists for that reason.)
* `CNTPCT_EL0` runs and `CNTFRQ_EL0` is programmed (LK does this). If `CNTFRQ_EL0 == 0`, fall back to a
  loop counter; **the design must not busy-wait forever on a broken timer**.
* Interrupts masked (`DAIF.I = 1`) and `MSDC_INTEN = 0` — we never take an IRQ.
* Physical addresses; `MSDC0_BASE` is an `(uintptr_t)` constant.
* Data buffers are physically contiguous and 4-byte aligned (a `memcpy`'d staging buffer satisfies this;
  the FIFO needs 32-bit writes for the bulk and the block length is 512, so no byte tails occur).

### 3.2 The code

```c
/* msdc_pio_log.c — polled PIO single/multi-block writes for MT6765 MSDC0.
 * Built for -ffreestanding, no MMU, no cache, no interrupts, no allocation. */

#include <stdint.h>
#include <stddef.h>

#define MSDC0_BASE          0x11230000u

/* ---- registers ---------------------------------------------------------- */
#define R_MSDC_CFG          0x000
#define R_MSDC_IOCON        0x004
#define R_MSDC_PS           0x008
#define R_MSDC_INT          0x00C
#define R_MSDC_INTEN        0x010
#define R_MSDC_FIFOCS       0x014
#define R_MSDC_TXDATA       0x018
#define R_MSDC_RXDATA       0x01C
#define R_SDC_CFG           0x030
#define R_SDC_CMD           0x034
#define R_SDC_ARG           0x038
#define R_SDC_STS           0x03C
#define R_SDC_RESP0         0x040
#define R_SDC_RESP1         0x044
#define R_SDC_RESP2         0x048
#define R_SDC_RESP3         0x04C
#define R_SDC_BLK_NUM       0x050
#define R_MSDC_VERSION      0x114
#define R_MSDC_ECO_VER      0x118

/* ---- fields ------------------------------------------------------------- */
#define CFG_MODE            (1u << 0)
#define CFG_CKPDN           (1u << 1)
#define CFG_RST             (1u << 2)
#define CFG_PIO             (1u << 3)
#define CFG_CKSTB           (1u << 7)
#define CFG_CKDIV(d)        (((d) & 0xFFFu) << 8)
#define CFG_CKDIV_MASK      (0xFFFu << 8)
#define CFG_CKMOD(m)        (((m) & 0x3u) << 20)
#define CFG_CKMOD_MASK      (0x3u << 20)

#define STS_SDCBUSY         (1u << 0)
#define STS_CMDBUSY         (1u << 1)

#define INT_CMDRDY          (1u << 8)
#define INT_CMDTMO          (1u << 9)
#define INT_RSPCRCERR       (1u << 10)
#define INT_XFER_COMPL      (1u << 12)
#define INT_DATTMO          (1u << 14)
#define INT_DATCRCERR       (1u << 15)
#define INT_CMD_ALL         (INT_CMDRDY | INT_CMDTMO | INT_RSPCRCERR)
#define INT_DAT_ALL         (INT_XFER_COMPL | INT_DATTMO | INT_DATCRCERR)

#define FIFOCS_RXCNT(x)     ((x) & 0xFFu)
#define FIFOCS_TXCNT(x)     (((x) >> 16) & 0xFFu)
#define FIFOCS_CLR          (1u << 31)
#define FIFO_DEPTH          128u          /* mtk_sd.h: MSDC_FIFO_SZ */

#define SDC_BUSWIDTH_MASK   (3u << 16)
#define SDC_DTOC_MASK       (0xFFu << 24)
#define SDC_WRDTOC_MASK     (0x1FFFu << 2)

/* ---- command words (SDC_CMD) -------------------------------------------- */
#define CMDW(opc, rsp, dtype, rw, blklen, extra) \
    ((uint32_t)(opc) | ((uint32_t)(rsp) << 7) | ((uint32_t)(dtype) << 11) | \
     ((uint32_t)(rw) << 13) | ((uint32_t)(blklen) << 16) | (uint32_t)(extra))
#define CMD0_W   0x00000000u
#define CMD1_W   CMDW(1, 3, 0, 0, 0, 0)                       /* 0x00000181 R3      */
#define CMD2_W   CMDW(2, 2, 0, 0, 0, 0)                       /* 0x00000102 R2      */
#define CMD3_W   CMDW(3, 1, 0, 0, 0, 0)                       /* 0x00000083 R1      */
#define CMD6_W   CMDW(6, 7, 0, 0, 0, 0)                       /* 0x00000386 R1b     */
#define CMD7_W   CMDW(7, 1, 0, 0, 0, 0)                       /* 0x00000087 R1      */
#define CMD8_W   CMDW(8, 1, 1, 0, 512, 0)                     /* 0x02000888 read512 */
#define CMD9_W   CMDW(9, 2, 0, 0, 0, 0)                       /* 0x00000109 R2      */
#define CMD12_W  (CMDW(12, 7, 0, 0, 0, (1u << 14)))           /* 0x0000438C R1b+stop*/
#define CMD13_W  CMDW(13, 1, 0, 0, 0, 0)                      /* 0x0000008D R1      */
#define CMD16_W  CMDW(16, 1, 0, 0, 0, 0)                      /* 0x00000090 R1      */
#define CMD24_W  CMDW(24, 1, 1, 1, 512, 0)                    /* 0x02002898 W1      */
#define CMD25_W  CMDW(25, 1, 2, 1, 512, 0)                    /* 0x02003099 Wmulti  */
#define CMD17_W  CMDW(17, 1, 1, 0, 512, 0)                    /* 0x02000891 R1      */
#define CMD18_W  CMDW(18, 1, 2, 0, 512, 0)                    /* 0x02001092 Rmulti  */

/* ---- R1 bits ------------------------------------------------------------ */
#define R1_READY_FOR_DATA   (1u << 8)
#define R1_STATE(s)         (((s) >> 9) & 0xFu)
#define R1_STATE_TRAN       4u
#define R1_STATE_PRG        7u
#define R1_FATAL_MASK       0xFFFFA080u

/* ---- errors ------------------------------------------------------------- */
enum { MSDC_OK = 0, MSDC_ETMO = -1, MSDC_ECRC = -2, MSDC_EBUSY = -3,
       MSDC_ESTATE = -4, MSDC_EARG = -5, MSDC_ENOCLK = -6 };

/* ---- tiny register layer ------------------------------------------------ */
static inline uint32_t rd(uint32_t off)   { return *(volatile uint32_t *)(uintptr_t)(MSDC0_BASE + off); }
static inline void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)(MSDC0_BASE + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");      /* one barrier per register write */
}
static inline void wr_nf(uint32_t off, uint32_t v)  /* FIFO bursts: no per-word barrier */
{ *(volatile uint32_t *)(uintptr_t)(MSDC0_BASE + off) = v; }

/* ---- time ---------------------------------------------------------------
 * Deadlines only, so microsecond resolution is plenty.  Written to work for any
 * CNTFRQ (32.768 kHz .. 1 GHz) and to survive CNTFRQ_EL0 == 0, because a shim that
 * busy-waits on a broken timer is a brick.  Pair every loop with the deadlines below
 * *and* an iteration cap if you do not trust the previous stage's timer setup. */
static inline uint64_t now_us(void)
{
    uint64_t c = 0, f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
    if (!f) return c / 10ULL;                            /* unknown: assume 10 MHz */
    return (c / f) * 1000000ULL + ((c % f) * 1000000ULL) / f;
}
#define DEADLINE_MS(ms)  (now_us() + (uint64_t)(ms) * 1000ULL)
static inline int expired(uint64_t dl) { return now_us() > dl; }
static void delay_ms(uint32_t ms) { uint64_t dl = DEADLINE_MS(ms); while (!expired(dl)) { } }

/* ---- host primitives ---------------------------------------------------- */
static int wait_sts_clear(uint32_t mask, uint32_t ms)
{
    uint64_t dl = DEADLINE_MS(ms);
    while (rd(R_SDC_STS) & mask) if (expired(dl)) return MSDC_EBUSY;
    return MSDC_OK;
}

static int fifo_clear(void)
{
    uint64_t dl = DEADLINE_MS(20);
    wr(R_MSDC_FIFOCS, rd(R_MSDC_FIFOCS) | FIFOCS_CLR);
    while (rd(R_MSDC_FIFOCS) & FIFOCS_CLR) if (expired(dl)) return MSDC_ETMO;
    return MSDC_OK;
}

/* Issue one command word and poll MSDC_INT for its completion.
 * r1: receives RESP0 for R1/R3 (may be NULL). For R2 callers read RESP3..RESP0. */
static int cmd(uint32_t cmdw, uint32_t arg, uint32_t *r1, uint32_t ms)
{
    uint64_t dl;
    int rc;

    /* CMD12/CMD13 may run while DAT0 is busy; everything else needs the whole controller idle. */
    rc = wait_sts_clear(((cmdw & 0x3Fu) == 12u || (cmdw & 0x3Fu) == 13u) ? STS_CMDBUSY : STS_SDCBUSY, 100);
    if (rc) return rc;

    wr(R_MSDC_INT, INT_CMD_ALL);                 /* W1C: drop stale flags before arming */
    wr(R_SDC_ARG, arg);
    wr(R_SDC_CMD, cmdw);

    dl = DEADLINE_MS(ms);
    for (;;) {
        uint32_t st = rd(R_MSDC_INT) & INT_CMD_ALL;
        if (st) {
            wr(R_MSDC_INT, st);
            if (st & INT_CMDTMO)    return MSDC_ETMO;     /* no response at all */
            if (st & INT_RSPCRCERR) return MSDC_ECRC;     /* response CRC bad */
            if (r1) *r1 = rd(R_SDC_RESP0);
            return MSDC_OK;
        }
        if (expired(dl)) return MSDC_ETMO;
    }
}

/* CMD13 until the card is in TRAN with READY_FOR_DATA, or the deadline expires. */
static int wait_card_ready(uint32_t rca, uint32_t ms)
{
    uint64_t dl = DEADLINE_MS(ms);
    for (;;) {
        uint32_t r1 = 0;
        int rc = cmd(CMD13_W, rca << 16, &r1, 50);
        if (rc == MSDC_OK) {
            if (r1 & R1_FATAL_MASK) return MSDC_ESTATE;
            if (R1_STATE(r1) == R1_STATE_TRAN && (r1 & R1_READY_FOR_DATA)) return MSDC_OK;
            /* PRG or RCV: keep polling */
        }
        if (expired(dl)) return MSDC_ETMO;
        delay_ms(1);
    }
}

/* Push `bytes` (multiple of 4) into the TX FIFO, honouring FIFOCS.TXCNT. */
static void fifo_push(const uint8_t *p, uint32_t bytes)
{
    uint32_t i;
    for (i = 0; i < bytes; i += 4) {
        uint32_t w;
        __builtin_memcpy(&w, p + i, 4);         /* little-endian: byte 0 goes out first */
        wr_nf(R_MSDC_TXDATA, w);
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- the function you asked for ----------------------------------------- */

#define MSDC_MAX_XFER   65536u          /* bytes per CMD24/CMD25; 128 sectors        */
#define MSDC_MULTI      1               /* 0 => one CMD24 per 512 B (first bring-up) */

int msdc_write_sectors(uint32_t lba, const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;

    if (!data || bytes == 0 || (bytes & 511u)) return MSDC_EARG;

    while (bytes) {
        uint32_t chunk = (bytes > MSDC_MAX_XFER) ? MSDC_MAX_XFER : (uint32_t)bytes;
#if !MSDC_MULTI
        chunk = 512u;
#endif
        uint32_t blocks = chunk >> 9;
        uint32_t resp = 0;
        int rc;

        /* --- 0. host must be idle before we program the block count ---------- */
        rc = wait_sts_clear(STS_SDCBUSY, 100);
        if (rc) return rc;

        /* --- 1. program the transfer ---------------------------------------- */
        rc = fifo_clear();
        if (rc) return rc;
        wr(R_SDC_BLK_NUM, blocks);
        wr(R_MSDC_INT, INT_DAT_ALL);   /* a stale XFER_COMPL would end the data loop at once */
        if (!(rd(R_MSDC_CFG) & CFG_PIO)) wr(R_MSDC_CFG, rd(R_MSDC_CFG) | CFG_PIO);

        /* --- 2. the write command (single or multi block) -------------------- */
        rc = cmd(blocks > 1 ? CMD25_W : CMD24_W, lba, &resp, 100);
        if (rc) goto recover;
        if (resp & R1_FATAL_MASK) { rc = MSDC_ESTATE; goto recover; }

        /* --- 3. data phase: keep the FIFO fed, watch INT ---------------------- */
        {
            uint32_t left = blocks << 9, off = 0;
            int done = 0;
            uint64_t dl = DEADLINE_MS(1000);

            for (;;) {
                uint32_t st = rd(R_MSDC_INT) & INT_DAT_ALL;
                if (st) {
                    wr(R_MSDC_INT, st);                 /* W1C */
                    if (st & INT_DATTMO)   { rc = MSDC_ETMO; goto recover_data; }
                    if (st & INT_DATCRCERR){ rc = MSDC_ECRC; goto recover_data; }
                    if (st & INT_XFER_COMPL) done = 1;
                }
                if (done && left == 0) break;

                if (left && FIFOCS_TXCNT(rd(R_MSDC_FIFOCS)) == 0) {
                    uint32_t burst = left < FIFO_DEPTH ? left : FIFO_DEPTH;
                    fifo_push(p + off, burst);          /* vendor policy: only when empty */
                    off  += burst;
                    left -= burst;
                }
                if (expired(dl)) { rc = MSDC_ETMO; goto recover_data; }
            }

            /* --- 4. let the FIFO drain so the card really has every byte ------ */
            dl = DEADLINE_MS(500);
            while (FIFOCS_TXCNT(rd(R_MSDC_FIFOCS)) != 0) {
                if (expired(dl)) { rc = MSDC_ETMO; goto recover_data; }
            }
        }

        /* --- 5. ended the multi-block transfer ------------------------------- */
        if (blocks > 1) {
            rc = cmd(CMD12_W, 0, &resp, 250);
            if (rc) goto recover;
        }

        /* --- 6. wait until the card has finished programming ----------------- */
        rc = wait_card_ready(1, 5000);            /* RCA 1; 5 s covers slow blocks */
        if (rc) goto recover;

        lba   += blocks;
        p     += blocks << 9;
        bytes -= blocks << 9;
        continue;

recover_data:
        /* A failed data phase leaves the card in RCV: a STOP is mandatory, or the
         * next command is rejected with ILLEGAL_COMMAND forever. */
        if (blocks > 1) (void)cmd(CMD12_W, 0, &resp, 250);
        (void)fifo_clear();
recover:
        (void)fifo_clear();
        (void)wr(R_MSDC_INT, 0xFFFFFFFFu);
        return rc;
    }
    return MSDC_OK;
}
```

Not shown, because they are tables rather than code: `msdc_host_init()` (§1.4) and `msdc_card_init()` /
`msdc_card_probe()` (§2.2, or the §2.4 fast path), plus `msdc_save_state()`/`msdc_restore_state()` around
`MSDC_CFG`, `SDC_CFG` and `MSDC_INTEN` so the next stage inherits what LK left.

### 3.3 The read path you also need (GPT + EXT_CSD)

`msdc_write_sectors()` alone cannot locate `flashinfo`: you need CMD8 (EXT_CSD) and CMD17/CMD18 (GPT).
The read path is the same skeleton with the data direction reversed — push nothing, drain
`MSDC_RXDATA` while `FIFOCS[7:0]` (RXCNT) says bytes are available:

```c
/* Read `blocks` (1..128) 512-byte blocks into dst.  Returns 0 or a MSDC_E* code. */
int msdc_read_sectors(uint32_t lba, void *dst, uint32_t blocks)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t left, off = 0, resp = 0;
    int rc, done = 0;
    uint64_t dl;

    if (!dst || !blocks || blocks > 128) return MSDC_EARG;
    if ((rc = wait_sts_clear(STS_SDCBUSY, 100))) return rc;
    if ((rc = fifo_clear())) return rc;
    wr(R_SDC_BLK_NUM, blocks);
    wr(R_MSDC_INT, INT_DAT_ALL);
    if (!(rd(R_MSDC_CFG) & CFG_PIO)) wr(R_MSDC_CFG, rd(R_MSDC_CFG) | CFG_PIO);

    rc = cmd(blocks > 1 ? CMD18_W : CMD17_W, lba, &resp, 100);
    if (rc) goto out;
    if (resp & R1_FATAL_MASK) { rc = MSDC_ESTATE; goto stop; }

    left = blocks << 9;
    dl = DEADLINE_MS(1000);
    for (;;) {
        uint32_t st = rd(R_MSDC_INT) & INT_DAT_ALL;
        if (st) {
            wr(R_MSDC_INT, st);
            if (st & INT_DATTMO)    { rc = MSDC_ETMO; goto stop; }
            if (st & INT_DATCRCERR) { rc = MSDC_ECRC; goto stop; }
            if (st & INT_XFER_COMPL) done = 1;
        }
        if (done && left == 0) break;

        for (;;) {
            uint32_t n = FIFOCS_RXCNT(rd(R_MSDC_FIFOCS));
            if (!n) break;
            if (n > left) n = left;                 /* never read past the request */
            if ((n & 3u) == 0 && left >= n) {
                uint32_t i, w;
                for (i = 0; i < n; i += 4) {
                    w = rd(R_MSDC_RXDATA);
                    __builtin_memcpy(d + off, &w, 4);
                    off += 4;
                }
                left -= n;
            } else {                                 /* tail: byte access */
                uint32_t i;
                for (i = 0; i < n; i++)
                    d[off++] = *(volatile uint8_t *)(uintptr_t)(MSDC0_BASE + R_MSDC_RXDATA);
                left -= n;
            }
        }
        if (expired(dl)) { rc = MSDC_ETMO; goto stop; }
    }
    if (blocks == 1) return MSDC_OK;

stop:
    if (blocks > 1) (void)cmd(CMD12_W, 0, &resp, 250);
out:
    (void)fifo_clear();
    (void)wr(R_MSDC_INT, 0xFFFF_FFFFu);
    return rc;
}
```

Use it for: CMD8 (one block, then select `[179]`, `[192]`, `[212..215]`, `[32]`, `[33]`), then GPT LBA 1 +
the entry array (e.g. 32 blocks), then every verification read during bring-up.

### 3.4 Where delays are actually required

| Where | Minimum | Why |
|---|---|---|
| after CMD0, before CMD1 | **1 ms** | Linux `__mmc_go_idle()` does `mmc_delay(1)`; the spec wants ≥74 clocks, i.e. 200 µs at 400 kHz |
| between CMD1 polls | **4 ms** (Linux) / 10 ms (fine) | Linux `MMC_OP_COND_PERIOD_US`; polling faster just wastes bus time |
| after writing `CKDIV` | none, but poll `CKSTB` | `msdc_clk_stable()` polls `CKSTB` with a 20 ms bound |
| after CMD3 → CMD9/CMD7 | none | R1 completion is enough |
| between CMD13 polls while PRG | **1 ms** | Linux ramps 32 µs → 32 ms; 1 ms is a good fixed compromise for a shim |
| inside the FIFO loop | **none — do not insert any** | at 12.5 MHz, 128 B drains in 82 µs; a `delay_ms(1)` per burst would underrun the card |
| after the last byte, before CMD13 | none, but wait `TXCNT == 0` first | otherwise the card is still receiving |
| after a failed data phase | CMD12 immediately, then 1 ms before CMD13 | the card needs the stop to leave RCV |

Three real "gotchas" that the code above encodes:
1. **`SDC_CMD[27:16]` must be 512 even for multi-block.** The block *count* goes in `SDC_BLK_NUM`, the
   block *length* in the command word. Vendor and Linux both do this.
2. **Only refill when `TXCNT == 0`.** That is the vendor's own policy
   (`else if (left < MSDC_FIFO_SZ && msdc_txfifocnt() == 0)` / `if (left >= MSDC_FIFO_SZ && msdc_txfifocnt() == 0)`).
   Refilling on "room available" also works and is what NativeMsdc does (`room = FIFO_DEPTH - used`), but
   the empty-FIFO rule is the one MediaTek's own driver exercises.
3. **Clearing `MSDC_INT` before each command**: `MSDC_INT` is W1C and accumulates; a stale
   `XFER_COMPL` from a previous aborted transfer makes the data loop exit immediately and you silently
   write garbage. Note the vendor clears only the bits it saw, and re-reads after clearing.

---

## 4. Verifying each stage from software alone; failure containment

### 4.1 Expected values, stage by stage

| Stage | Check | Pass looks like | Typical failure signature |
|---|---|---|---|
| Controller alive | `MSDC_CFG[7]` | 1 | 0 → clock gate off / wrong base (check `0x1123_0000`) |
| Register block right | `MSDC_VERSION` (0x114), `MSDC_ECO_VER` (0x118) | non-zero, stable across two reads | 0xFFFFFFFF → bus/clock dead; 0x0 → gate off |
| Reset | `MSDC_CFG[2]` after set | clears within a few µs | stuck → abandon (needs a real reset) |
| Clock change | `MSDC_CFG[7]` after `CKDIV` write | 1 within 20 ms | 0 → divider write rejected / source off |
| CMD0 | `INT[8]` CMDRDY | set, no CRC/TMO | `CMDTMO` → no card / wrong bus width / clock too fast |
| **CMD1** | `RESP0` | first: bit31 set (busy). Then bit31=0, **bit30=1 (CCS ⇒ LBA addressing)**, voltage bits `[23:15]` non-zero. `RESP0 == 0xC0FF8080`-ish is the classic eMMC value (any value with bit31=0 and bit30=1 and a sane voltage window passes) | bit31 never clears → card not powered, or CMD0/clock wrong; bit30=0 → ≤2 GiB byte-addressed card (then CMD24 arg must be a *byte* offset!) |
| **CMD2** | `RESP3>>24` | manufacturer ID ∈ {0x11,0x13,0x15,0x45,0x90,…}; `RESP3..RESP0` not all 0/0xFFFFFFFF | `RSPCRCERR` → sampling edge wrong (only at high clock); all-zero → no response captured |
| **CMD3** | `RESP0` | `(RESP0 & 0xFFFF0000) == 0x00010000` | mismatch → RCA not accepted → CMD0 again |
| CMD9 | `RESP3[31:30]` | CSD_STRUCTURE = 2 (v4.x) or 3 (>2 GiB); `READ_BL_LEN=9` | 0 or 3 in a ≤2 GiB part is odd → log it, continue |
| CMD7 | CMD13 | state 4, READY_FOR_DATA=1 | state 3 → CMD7 didn't take |
| CMD16 | `RESP0` | no fatal bits | `BLOCK_LEN_ERROR` (bit 29) |
| CMD8 | 512 B in FIFO, `EXT_CSD[192]` | rev ∈ 5..8; `[179]&7 == 0`; `[212..215]` = plausible sector count (~30 GB / 512 = 61e6) | `DATTMO` → RX path/timing; `[179]&7 != 0` → **stop, wrong partition** |
| **CMD24** | `INT` sequence | `CMDRDY` (with R1 state RCV or TRAN, no fatal bits) → data loop → `XFER_COMPL` → CMD13 back to TRAN + READY_FOR_DATA | `INT[15] DATCRCERR` = card rejected the data (CRC-status token bad ⇒ bus/timing, or the card never got a full block); `INT[14] DATTMO` = card saw no data start (we never pushed) ; `INT[10] RSPCRCERR` = response CRC |
| CMD25 + CMD12 | as CMD24, plus CMD12 R1b completes and CMD13 = TRAN | `RESP0` after CMD12 has no fatal bits | CMD12 `CMDTMO` → card still in RCV/PRG; do *not* retry CMD25 before recovering |
| Data correctness | read the sector back (CMD17/CMD18 or Android) | byte-identical | CRC ok but wrong data ⇒ wrong LBA (byte vs sector addressing!) or wrong partition |

**The single most informative early test**: after CMD1, print `RESP0` on the panel in hex. `0xC0FF8080`
means "3.0–3.6 V window + sector mode"; that one number validates clock, bus width, command encoding and
response path in one shot.

### 4.2 Failure modes that hang the bus, and how each one times out

| Failure | What hangs | Containment in this design |
|---|---|---|
| Card in RCV/PRG after an aborted CMD25 | every subsequent command → `ILLEGAL_COMMAND`; naive code loops on CMD13 forever | (a) **always** send CMD12 after a failed data phase; (b) CMD13 poll is deadline-bounded; (c) if two recoveries fail, escalate to §4.3 |
| Card never answers (dead / not powered / wrong RCA) | `CMDRDY` never sets | `cmd()` deadline (100 ms) → `MSDC_ETMO`; the hardware also raises `CMDTMO` (bit 9) |
| Data never starts (host forgot to push / FIFO wedged) | card waits in RCV forever; `XFER_COMPL` never | data-loop deadline (1 s) + hardware `DTOC`; then CMD12 |
| FIFO never drains (`TXCNT != 0` forever) | `XFER_COMPL` may already be set; card is fine, host isn't | 500 ms drain deadline, then report and stop using the channel |
| `SDC_STS.SDCBUSY` stuck | every command blocks at step 0 | 100 ms poll → `MSDC_EBUSY`; then **controller reset** (`MSDC_CFG.RST` + FIFO clear) which clears the sticky state, then re-init |
| Clock never comes back after a divider write | everything | 20 ms `CKSTB` poll → return `MSDC_ENOCLK` and **never touch the controller again**; log to panel only |
| Timer is broken (`CNTFRQ_EL0 == 0`) | all deadlines | `now_ns()` fallback + hard iteration caps in every loop (add a `spins` counter — belt and braces) |
| A write to a boot partition | silent corruption, no error at all | refuse to continue unless `EXT_CSD[179]&7 == 0` (checked at bring-up, re-checked before the first write) |
| Wrong addressing mode (CCS=0) | writes land at LBA×1 instead of ×512 → out-of-range or wrong area | latch the CCS bit from CMD1 and fail loudly if 0 and `lba > 4096` |
| Card cache not flushed | reads back stale/blank tail after a reset | CMD6 FLUSH_CACHE after the last write (§2.5) |

### 4.3 Recovery ladder (bounded, no "retry forever")

```
level 0: re-issue the same command once after clearing INT (cheap; absorbs a lost response)
level 1: CMD12 + fifo clear + INT clear, then CMD13 (≤1 s); if TRAN, resume
level 2: controller reset (MSDC_CFG.RST + FIFOCS.CLR + INT clear), CMD13 with inherited state;
         if still not TRAN -> level 3
level 3: full card re-init (CMD0 ... CMD7, §2.2) at the *slow* clock; if that fails -> level 4
level 4: give up on eMMC for this boot, mark the channel dead, continue on the panel only
```
Note there is deliberately **no** "assert RST_n" step: `EMMC_IOCON.BOOTRST` can put the part into eMMC
boot mode on the next power cycle (§1.7). The vendor's own `msdc_card_reset()` power-cycles VEMC through
the PMIC instead — not available to us, and not needed.

---

## 5. Reading the log back from rooted Android

### 5.1 On-device

`flashinfo` is exposed by name on MTK devices (the task states this; it is the standard
`/dev/block/by-name/<partition>` symlink scheme, and the same node the shim's LBA came from). With root:

```sh
# 1. sanity: does the node exist, what is its size?
adb shell su -c 'ls -l /dev/block/by-name/flashinfo'
adb shell su -c 'blockdev --getsize64 /dev/block/by-name/flashinfo'   # expect 16777216

# 2. pull only the header first (4 KiB) — cheap, works even if the ring is huge
adb exec-out su -c 'dd if=/dev/block/by-name/flashinfo bs=4096 count=1 2>/dev/null' > flashinfo.hdr

# 3. then the region that matters (1 MiB header + ring)
adb exec-out su -c 'dd if=/dev/block/by-name/flashinfo bs=4096 skip=256 count=512 2>/dev/null' > flashinfo.ring
```

Notes:
* `adb exec-out` avoids CR/LF mangling that `adb shell` can introduce; if you must use `adb shell`,
  redirect to a file on the device and `adb pull` it.
* `su -c '...'` works for Magisk/superuser variants; `su 0 dd ...` (`su 0` = uid 0) is the more portable
  form on some ROMs. If `dd` is missing (rare on Android, present via toybox), use
  `cat /dev/block/by-name/flashinfo | head -c 1048576`.
* Reading is safe and non-destructive; no need to unmount anything because `flashinfo` is not mounted.
* **Do not** use `dd` to write to that node casually — a mistyped `of=` on the wrong symlink writes a
  bootloader partition. Reads only, from this procedure.

### 5.2 Discovering the LBA in the shim (no hard-coded numbers)

Read LBA 1 (GPT header), verify `"EFI PART"`, take `partition_entry_lba` (offset 72) and
`num_partition_entries` (offset 80), read the entry array, scan for the UTF-16LE name `flashinfo`
(entry offset 56, 36 code units), take `first_lba` (entry offset 32) and `last_lba` (offset 40).
This is exactly the shape of `gptCheckHeader()`/`gptFindUserdata()` in `NativeMsdc.h` — reuse that logic.
Validate before trusting it: signature, `first_lba >= 34`, `last_lba < SEC_COUNT` (from EXT_CSD),
and `(last_lba - first_lba + 1) * 512 == 16 MiB` if you want a strict check.

### 5.3 Log layout (the contract between shim and decoder)

Fixed, little-endian, everything 512-B aligned so every update is a whole-sector write:

```
flashinfo (16 MiB)
  +0x000000 : 0..64 KiB        <- DO NOT TOUCH (possible LK/preloader metadata)  [UNVERIFIED]
  +0x100000 : struct msdc_log_hdr   (4096 B)
  +0x101000 : ring (default 1 MiB, power-of-two) -> wraps
  ...       : rest unused
```

`struct msdc_log_hdr`:

```
0    char  magic[8]      "MSDCLOG1"
8    u32   version       1
12   u32   hdr_bytes     4096
16   u64   ring_off      byte offset of ring from partition start (0x101000)
24   u64   ring_size     bytes, multiple of 512
32   u64   write_pos     next append offset within the ring (wraps)
40   u64   total_bytes   monotonic bytes ever appended (gives a global order)
48   u32   boot_count    incremented by the shim on every boot
52   u32   hdr_crc32     CRC32 over bytes [0,60) with this 4-byte field taken as zero
56   u32   flags         bit0: ring was wrapped at least once
60   u32   reserved0
64   u8    build_id[32]  free-form
...  0 padding to 4096
```

The first 64 bytes are exactly a packed C struct, so shim, decoder and any future kernel driver can
share one definition:

```c
struct msdc_log_hdr {            /* 64 bytes, little-endian: "<8sIIQQQQIIII" */
    char     magic[8];           /* "MSDCLOG1" */
    uint32_t version;            /* 1 */
    uint32_t hdr_bytes;          /* 4096 */
    uint64_t ring_off;
    uint64_t ring_size;
    uint64_t write_pos;
    uint64_t total_bytes;
    uint32_t boot_count;
    uint32_t hdr_crc32;
    uint32_t flags;
    uint32_t reserved0;
} __attribute__((packed));
```

Record (self-delimiting, 8-byte aligned, may straddle sectors only at a 512-B boundary if you keep
`len` ≤ 480):

```
0    u32  len        payload length (<= 4096-24)
4    u16  flags      bit0=continuation, bit1=final, bit2=panic
6    u8   level      0=panic 1=error 2=warn 3=info 4=debug
7    u8   src        0=shim 1=kernel 2=other
8    u64  tsc_ns     CNTPCT-derived, 0 if no timer
16   u32  crc32      CRC32 over bytes [0,16) and payload, computed with this field = 0
20   u32  seq        monotonic record counter
24   u8   payload[len]
```

Write discipline that makes it crash-safe:
1. Append the record to a **RAM staging buffer**; only flush whole 512-B sectors.
2. Before a sector goes out, write the header's `write_pos`/`total_bytes`/`hdr_crc32` **after** the data
   sector (so a lost tail never leaves the header pointing at garbage).
3. On panic: write the sector with `flags.bit2` set, then one CMD6 FLUSH_CACHE, then spin.
4. On wrap: never partially overwrite a record; step the write pointer to the ring start and write a
   4-byte "ring restart" marker record first.

### 5.4 Host-side decoder design

Behaviour:
1. `mmap`/read the image; find the header at `0x100000` (and optionally scan for the magic at any 4096
   boundary, so a mis-based dump still decodes).
2. Verify `hdr_crc32`; if it fails, still continue in "salvage" mode (the ring is usually fine).
3. Walk the whole ring start→end, not just from `write_pos`: parse every candidate record on 8-byte
   alignment, accept only those whose `len` is sane and whose `crc32` matches. This is wrap-proof and
   torn-write-proof.
4. De-duplicate by (`boot_count`, `seq`), sort by (`total_bytes`-derived order) or by `seq`, then render
   `HH:MM:SS.mmm LEVEL src text`, and finally reorder so the *last* boot's log appears last.
5. Emit three artifacts: `.txt` (human), `.jsonl` (one record per line, for diffing across boots), and a
   one-line summary (records, bytes, boots, first/last timestamp, number of rejected candidates).

A runnable reference implementation is in `D:\aios\research\msdc_log_decode.py` (self-tested; see
`python msdc_log_decode.py --selftest`, which covers no-wrap, wrapped, one-byte-corrupted and
mis-based-dump images). Usage:

```
adb exec-out su -c 'dd if=/dev/block/by-name/flashinfo bs=4096 count=512 2>/dev/null' > flashinfo.bin
python msdc_log_decode.py flashinfo.bin                 # human readable, newest boot last
python msdc_log_decode.py flashinfo.bin --json log.jsonl
python msdc_log_decode.py flashinfo.bin --base 0x100000 --scan
```

It scans every 4096 boundary for the magic, so a dump that starts at the ring (`skip=256`) or at a
wrong offset still decodes; records that fail CRC are dropped but their *length* is trusted for resync,
so a torn tail cannot hide the records written before it.

---

## 6. Milestones and honest effort

Assumes a working way to flash/run the shim, a serial/panel way to read raw hex, and the ability to boot
Android and use `su`. "Ideal hours" = focused engineering time; "calendar" assumes 2–4 test cycles per
milestone with flashing and booting in between.

| # | Milestone | Deliverable / exit criterion | Ideal | Main risk |
|---|---|---|---|---|
| **M0** | Ops sanity | Read and print `MSDC_CFG`, `MSDC_VERSION`, `MSDC_ECO_VER`, `SDC_STS`, `SDC_CFG`, `FIFOCS` on the panel. `CKSTB=1`, version non-zero. Add timer + `delay_ms`. | 2–4 h | none; pure read |
| **M1** | Card identified | CMD0/1/2/3/9/7/16 succeed; prints `RESP0` of CMD1 (`0xC0FF8080`-ish), CID manufacturer byte, CSD structure, and CMD13 = TRAN. *No writes.* | 4–8 h | clock divider layout / sampling edge at 400 kHz; CMD1 arg |
| **M1.5** | Read path (needed anyway) | CMD8 EXT_CSD read of 512 B into RAM, print `[179]`, `[192]`, `[212..215]`; GPT read + `flashinfo` LBA found. | 2–4 h | RX FIFO handshake, same machinery as TX |
| **M2** | **One sector written** | `msdc_write_sectors(lba, buf, 512)` with `MSDC_MULTI=0`; magic + one text record; read back **from the shim itself** (CMD17) and then from Android (`dd`), `cmp` equal. | 3–6 h | first real use of TX FIFO; CRC-status handling; wrong partition |
| **M3** | 1 MiB ring + flush | multi-block CMD25 (+CMD12), header, wrap logic, RAM staging, CMD6 FLUSH_CACHE, boot counter. Writes a 1 MiB synthetic log; decode script renders it. | 6–10 h | multi-block PIO is *not* exercised by the vendor driver (§7 risk R2) |
| **M4** | Readback + decode productised | `msdc_log_decode.py` + on-device pull script + docs; deterministic output across 3 boots. | 3–5 h | low |
| **M5** | Optional speed/robustness | 8-bit bus (CMD6 + `SDC_CFG.BUSWIDTH`), 25 MHz, aligned 512 KiB writes, panic path, "resume after warm reboot" handling. | 6–12 h | pad/sampling tuning at 8-bit → may need top regs |

**Totals:** to *proof of concept* (M2) ≈ **9–18 ideal hours**; to *usable kernel-log capture* (M3+M4)
≈ **18–33 ideal hours**; in calendar terms at hobby pace with 2–4 flash cycles per stage, expect
**2–4 weeks**.

**Which milestone unblocks kernel-log capture?** **M3** is the first milestone that actually captures a
real kernel log (a log needs multi-block appends, wrap-around, and a flush so the tail survives a reboot).
**M2** is the one that *proves the channel* and is the biggest single risk reduction — if M2 reads back
byte-identical from Android, everything after it is engineering rather than research. M1 alone does not
unblock anything; it only de-risks.

---

## 7. Risks, and the unverified items, stated plainly

| # | Item | Status | Mitigation |
|---|---|---|---|
| R1 | **PIO multi-block writes are not exercised by MT6765's own driver** — its policy sends everything ≥512 B to DMA (`drv_mode = MODE_SIZE_DEP`, `dma_size = 512`), so its `msdc_pio_write()` only ever runs for sub-512 B transfers and SDIO. The *mechanism* is proven on the sibling IP by NativeMsdc (MT8167) at up to 64 KiB. | **UNVERIFIED on MT6765** | M2 first with `MSDC_MULTI=0` (single-block CMD24, no CMD12, no CMD23 — the smallest possible surface). Only then enable CMD25. Fallback: 512 B per command is still ~0.5–2 ms each; a 1 MiB log = 2048 commands ≈ 2–4 s, perfectly usable |
| R2 | `MSDC_CFG.CKDIV` is 12 bits at `[19:8]` and `CKMOD` at `[21:20]` on this IP | **verified** in the MT6765 vendor `msdc_reg.h` (`MSDC_CFG_CKDIV (0xfff<<8)`, `MSDC_CFG_CKMOD (0x3<<20)`) | — |
| R3 | MSDC0 source clock is exactly 400 MHz (vs the 416 MHz MSDCPLL variant seen in the clock driver) | table says 400 MHz; `AD_MSDCPLL_416M_CK` exists in the same driver | the recommended dividers are chosen to be legal for both; measure once with `CNTPCT_EL0` if you care |
| R4 | Whether the first 64 KiB of `flashinfo` holds LK/preloader metadata | **UNVERIFIED** (the task says the partition is unused by Android; what the *bootloader* does with it is a different question) | write at `+0x100000`; back up the partition first (task states backups exist); do one write → read → restore cycle before trusting it |
| R5 | RCA = 1 at handoff | Linux and LK both use RCA 1 for eMMC; **verified for Linux**, assumed for LK | the "inherit" path starts with CMD13(1<<16); if it fails, fall back to full CMD0-based init which assigns RCA=1 itself |
| R6 | `MSDC_PS[16]` = DAT0 level (used as a busy cross-check) | vendor `msdc_stop_and_wait_busy()` polls `MSDC_PS & 0x10000`; `MSDC_PS_DAT` = `[23:16]` | use CMD13 as the authority, `MSDC_PS` only as telemetry |
| R7 | R1 bits 30/29/28/27/25/24/23/21/20/19/18/17/16/15/14/13/7/6/5/3 | **spec-derived** (JEDEC JESD84-B51 §6.13), not re-verified from a primary PDF here; bits 31, 26, 22, 12:9, 8 were confirmed in driver source | use `R1_FATAL_MASK = 0xFFFFA080` only as a *diagnostic*, never as the sole decision; completion decisions use bits 8/12:9 only |
| R8 | `MSDC_CFG.CKDRVEN`, `DVFS_*`, `PATCH_BIT*` semantics | **UNVERIFIED**, deliberately untouched | inherit and restore |
| R9 | Whether writing `MSDC_CFG` while `SDC_STS.SDCBUSY` is set is safe | **UNVERIFIED** | always wait for idle before any `MSDC_CFG`/`SDC_CFG` write |
| R10 | eMMC cache behaviour across a warm reboot (data in card cache when the SoC resets) | **UNVERIFIED** | CMD6 FLUSH_CACHE after the last write; decoder tolerates a torn tail |
| R11 | `SDC_BLK_NUM` width above 16 bits | vendor warns CMD23's argument can only carry 0xFFFF blocks, implying ≥16 bits | chunk at 128 blocks (64 KiB); no dependency on the field width |

Things I would do *before* writing any C code, in this order: (1) dump the six registers at handoff on
the panel and compare with the values LK's driver would leave; (2) confirm `CNTFRQ_EL0 != 0`;
(3) `dd` the first 64 KiB of `flashinfo` and the whole partition to a host file, so "restore" is a
one-liner if anything goes sideways.

---

## 8. Sources

Primary (all read while writing this):

1. MediaTek MT6765 MSDC driver (Xiaomi dandelion kernel release, `dandelion-q-oss`):
   - register map & bit fields — [`msdc_reg.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/msdc_reg.h)
   - FIFO sizes, `sdc_send_cmd()`, `msdc_rsp[]`, PIO access macros — [`mtk_sd.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/mtk_sd.h)
   - `msdc_init_hw()`, `msdc_set_mclk()`, `msdc_clk_stable()`, `msdc_set_buswidth()`,
     `msdc_command_start()`, `msdc_command_resp_polling()`, `msdc_pio_read()`, `msdc_pio_write()`,
     `msdc_set_timeout()`, `msdc_set_busy_timeout_ms()`, `msdc_stop_and_wait_busy()` —
     [`sd.c`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/sd.c)
   - `MSDCPLL_FREQ`, `MSDC0_SRC_0/1`, `DEFAULT_DTOC`, `MSDC_PB0/PB1_DEFAULT_VAL`, topckgen dump — [`mt6765/msdc_cust.h`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/mt6765/msdc_cust.h), [`mt6765/msdc_cust.c`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/dandelion-q-oss/drivers/mmc/host/mediatek/ComboA/mt6765/msdc_cust.c)
2. Linux upstream:
   - [`drivers/mmc/host/mtk-sd.c`](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/mtk-sd.c) — same offsets; the `rawcmd` bit-layout comment; `msdc_cmd_is_ready()`; `cmd_ints_mask`
   - [`drivers/mmc/core/mmc.c`](https://github.com/torvalds/linux/blob/master/drivers/mmc/core/mmc.c) — attach sequence; `card->rca = 1`; CMD1 with `BIT(30)`; CCS → block addressing
   - [`drivers/mmc/core/mmc_ops.c`](https://github.com/torvalds/linux/blob/master/drivers/mmc/core/mmc_ops.c) — CMD1 polling (4 ms / 1 s, eMMC v5.1 §6.4.3 comment), `mmc_poll_for_busy()`, `__mmc_switch()` argument format, `CACHE_FLUSH_TIMEOUT_MS`
3. mouseos/NativeMsdc (MT8167; polled PIO eMMC) — [`NativeMsdc.h`](https://github.com/mouseos/NativeMsdc/blob/master/NativeMsdc.h), [`NativeMsdc.cpp`](https://github.com/mouseos/NativeMsdc/blob/master/NativeMsdc.cpp);
   companion XNU port: [xnu_device_benesse_a04br3](https://github.com/mouseos/xnu_device_benesse_a04br3), [xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3)
4. Local, this workspace:
   - `D:\aios\stock_boot.dtb` — bases, `clk_src`, `host_function`, bus width (dumped with `_msdc_dtb.py`)
   - `D:\aios\research\stock_kernel.config` — `CONFIG_MMC_MTK_PRO=y`
   - `D:\aios\research\_v_clk-mt6765.c` — MSDC clock mux parents, MSDCPLL registers, `AD_MSDCPLL_416M_CK`
   - `D:\aios\research\_lk.patch` — confirms `GPIO_MSDC0_{CLK,CMD,DAT0..7,DSL,RSTB}` are configured by LK for this board
5. JEDEC JESD84-B51 (eMMC 5.1) — R1 status bit assignment, OCR/CCS, CSD/EXT_CSD field layout. Cited as the
   specification of record; no PDF was fetched for this report, hence the per-bit confidence column in §2.3.
