# Getting text logs off a bare-metal aarch64 shim on an MT6762G Redmi 9A

**Scope:** engineering options, not a tutorial. The target is a shim that runs at EL1 with the MMU off, after MediaTek LK, before an Apple XNU kernel. Today the only output is the phone framebuffer and photographs. This report evaluates the requested options plus better ones, with concrete register-level notes, honest effort ranges, and a recommendation.

**Device:** Xiaomi Redmi 9A (`dandelion`, MT6762G, 8×Cortex-A53), bootloader = MediaTek LK, no UEFI, no accessible UART header, micro-USB port. Stock Android is Magisk-rooted when booted normally. `/dev/mem` is absent (`CONFIG_DEVMEM` is not set).

**Important epistemic note:** the current USB attempt's failure does **not** prove the PHY is powered down. The code in `bootshim/usb_cdc.c` has three independent defects (EP0 register model, endpoint FIFO setup, and the PHY's forced VBUS/mode register), any one of which prevents enumeration. See §2.1 and §3.1. This changes the ranking substantially.

---

## 0. Bottom line

| Rank | Option | Time to first log | Real-time? | Hardware needed | Effort (focused hours) | Main risk |
|---:|---|---|---|---|---:|---|
| **1** | **pstore/ramoops, writing the console zone (not offset 0)** | **one shim flash + one stock-boot restore** | no (one log per boot) | none | **3–8 h** | wrong zone offset / LK clears the region / ECC |
| **2** | **UART-over-USB: put the MTK USB2 PHY in UART mode and drive UART0** | one shim flash | **yes** | USB-TTL adapter + cut cable / breakout | **6–12 h** | 1.8 V levels, D+/D− polarity, UART-mode assumptions |
| 3 | Camera + template-matching screen decoder | none (works on existing photos) | no | phone holder + decent camera | 6–12 h | image resolution/glare; screen must be stable |
| 4 | Reuse LK's already-configured endpoints (vendor bulk) | one shim flash (if it works) | yes | none | 4–8 h **if** it works | LK almost certainly deinitialises USB; ~15–25 % chance |
| 5 | USB CDC-ACM with our own PHY bring-up | many flash cycles | yes | none | **30–50 h** | EP0/FIFO rewrite; PHY/clock; host driver |
| 6 | Reserved DRAM + Magisk kernel module to read it | one flash + module load | no | none (but kernel source/build) | 15–30 h | vermagic/module signing; build mismatch |
| 7 | Raw eMMC partition written by the shim | one flash; read later | no | none | 12–25 h PIO / 30–50 h full init | bricking the wrong LBA; eMMC init |
| 8 | BROM/DPL/DA, META, USB2JTAG | n/a | n/a | probe/tools | n/a | not reachable from the shim / not enabled |

**Single option to start with: pstore/ramoops, but not the current writer.** It is the only option that needs no hardware, no PHY/clock work, and no host driver, and the stock kernel already has `CONFIG_PSTORE_RAM=y`, `CONFIG_PSTORE_CONSOLE=y`, `CONFIG_PSTORE_PMSG=y`, `CONFIG_PSTORE_LAST_KMSG=y`. The current `bootshim/pstore.h` writes a valid `persistent_ram_buffer` header at **offset 0 of the pstore region**, which is the **first dump zone**, not the console zone; the console zone is after the dump region. That is almost certainly why "the ramoops driver doesn't recognise our format". The parameters needed to compute the right offset are readable from the booted phone **without flashing** (`/sys/module/ramoops/parameters/*`, `/proc/cmdline`, `/sys/fs/pstore`). Fixing this is a small, deterministic change; it gives a full boot log including the last words before a hang.

**Second step, before touching USB CDC: build the UART-over-USB cable.** The MT6765 USB2 PHY has a documented UART mode (`usb_phy_switch_to_uart()` in the stock MTK driver). It routes UART0 onto the USB D+/D− pins, so the "no accessible UART" assumption is only true for the PCB test pads, not for the USB connector. This is the cheapest **live** channel and avoids the entire MUSB/CDC stack. If the user is willing to make one cable, this is the option that actually replaces the photograph loop; pstore is the zero-hardware stepping stone.

---

## 1. What is actually true on this device (verified from the repository and artifacts)

### 1.1 Boot chain and kernel

The stock `boot` image is a normal Android boot image v2 (not an MTK header): `gzip(kernel) + ramdisk + DTB`. LK decompresses the kernel to `0x40080000`, passes the DTB pointer in `x0` (observed `0x47B80000` in earlier docs), and jumps. `fastboot boot` is unsupported on this LK: it downloads and then reboots; every experiment is `fastboot flash boot` + manual fastboot. This is from `docs/NEXT-STEPS.md` and `docs/USB_CONSOLE.md`, and it matches the observed behaviour.

The stock kernel is **Linux 4.19.127-perf-g7288046673d5**, built with clang 11.0.1. I extracted the gzip kernel from `boot_backup.img` (offset `0x800`, size `13907597`) and the embedded `IKCONFIG` (`/proc/config.gz`-style config at `0x10ac4b8` in the decompressed image). Relevant options:

```
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_PSTORE_PMSG=y
CONFIG_PSTORE_LAST_KMSG=y
CONFIG_PSTORE_COMPRESS=y            # deflate, but only for dmesg records
CONFIG_DEVMEM is not set
CONFIG_PROC_KCORE is not set
CONFIG_MTK_UART_USB_SWITCH is not set
CONFIG_USB_MTK_HDRC=y
CONFIG_MTK_MUSB_QMU_SUPPORT=y
CONFIG_MTK_USB_TYPEC is not set
CONFIG_MMC_MTK_PRO=y
CONFIG_CMDLINE="ramoops_memreserve=4M"
```

Consequences:

* `/dev/mem` cannot be made to work; `/proc/kcore` also does not exist. There is **no userspace path to arbitrary physical DRAM**.
* pstore/ramoops **is** compiled in, including a MTK `/proc/last_kmsg` compatibility option.
* The kernel uses MTK's private MUSB driver (`CONFIG_USB_MTK_HDRC`), not mainline `CONFIG_USB_MUSB_HDRC`; the mainline PHY driver `CONFIG_PHY_MTK_TPHY` is **not** set. The PHY is handled by the monolithic `mediatek,mt6765-usb20` driver.
* UART over USB is **not enabled in the kernel**, but the PHY code for it exists (see §5). The feature is disabled by config, not absent from silicon.

### 1.2 The USB node and clocks (device DTB)

From `stock_boot.dtb` (the DTB in the boot image):

```
usb0@11200000 {
    compatible = "mediatek,mt6765-usb20";
    reg = <0x0 0x11200000 0x0 0x10000
           0x0 0x11cc0000 0x0 0x10000>;
    interrupts = <0 0x49 8>;              /* GIC SPI 73 */
    clocks = <&infracfg_ao 0x8
              &topckgen 0x76
              &topckgen 0x20>;
    clock-names = "usb0", "usb0_clk_top_sel", "usb0_clk_univpll3_d4";
    charger = <&charger>;                 /* MT6357 charger detector */
    nvmem-cells = <&efuse_idx107>;        /* PHY calibration efuse */
    mode = <2>; multipoint; num_eps = <16>;
};
```

The second register range is the PHY (`0x11CC0000`, 64 KiB). The first clock is **not** a dedicated USB gate in `infracfg_ao`: clock ID 8 is `CLK_IFR_ICUSB` ("interface USB bus clock"), programmed through `ifr2_cg_regs`:

* **Enable** `CLK_IFR_ICUSB` by writing `1 << 8` to **`0x10001084`** (CG_CLR); **disable** by writing `1 << 8` to `0x10001080` (CG_SET). The polarity is counter-intuitive: `mtk_clk_gate_ops_setclr` uses the "set" register to **gate off**, and the status `0x10001090` bit 8 reads **0 when the clock is running, 1 when gated**. Do not blindly write `0x10001080`; that turns the USB bus clock off.
* `usb0_clk_top_sel` is the topckgen mux `usb_top_sel` in `CLK_CFG_5` (`0x10000090`), bit 16, parent 1 = `univpll3_d4`; update bit 22 in `CLK_CFG_UPDATE` (`0x10000004`). The mux has a **gate bit 23** (`0 = on`): clear it by writing `1 << 23` to `0x10000098` (CLR), and read-modify-write `0x10000090` because bits 0–7/8–15/24–31 belong to `sspm_sel`/`dxcc_sel`/`spm_sel`.
* Optional/check gates that the vendor driver leaves to the preloader/LK: `0x10000104` bits 8 (`usb20_48m_en`) and 9 (`univpll_48m_en`) are active-high 48 MHz gates; `0x1000c014` bit 4 is the `SSUSB26M` gate (active-high). The vendor clock driver comments these out, but the PHY's HS slew calibration assumes a 48 MHz reference, so if the PHY stays dead after the ICUSB/mux fix, read these three bits before writing anything. `0x10003000` bit 29 is the USB IP software reset (deasserted = 0); a reset there also resets the MUSB controller, so use it only before configuring MUSB.
* `usb0_clk_univpll3_d4` is the parent PLL divider, not a gate.

This is from the MT6765 clock driver source (`clk-mt6765.c`): `usb_top_parents = { "clk26m", "univpll3_d4" }`, and `GATE_IFR2(CLK_IFR_ICUSB, "ifr_icusb", "axi_ck", 8)`. The current shim never enables either the gate or the mux; it assumes LK left them on. That assumption is unverified and likely false after LK's USB deinit.

### 1.3 The MT6765 USB2 PHY is a "a60810"-style PHY at `0x11CC0000 + 0x800`

The stock driver is `drivers/misc/mediatek/usb20/mt6765/usb20_phy.c` (Xiaomi `dandelion-q-oss` tree) with register definitions in `mtk-phy-a60810.h`. It maps `of_iomap(node, 1)` = `0x11CC0000`, and the `USBPHY_*` macros address the **U2 COM bank at `0x11CC0800 + offset`**. The raw `usb2jtag` code confirms this: it writes `usb_phy_base + 0x820`, `+0x800`, `+0x818`, which are ACR4/ACR0/ACR6 at offsets `0x20/0x00/0x18` plus `0x800`.

Key registers (PHY base + 0x800 + offset):

| Offset | Name | Important bits |
|---:|---|---|
| `0x00` | USBPHYACR0 | PLL force on (15), INTR_EN (5), BGR enable (0) |
| `0x04` | USBPHYACR1 | INTR_CAL[4:0] (19), VRT/TERM VREF select |
| `0x14` | USBPHYACR5 | HSTX_SRCAL_EN (15), HSTX_SRCTRL[2:0] (12) |
| `0x18` | USBPHYACR6 | PHY_REV[7:0] (24), BC1.2 SW_EN (23), OTG_VBUSCMP_EN (20), DISCTH[3:0] (4) |
| `0x20` | U2PHYACR4 | DP_100K_MODE (18), DM/DP 100K_EN (17/16), GPIO_CTL (9), GPIO_MODE (8) |
| `0x68` | U2PHYDTM0 | RG_UART_MODE[1:0] (30), FORCE_UART_* (29–26), FORCE_SUSPENDM (18), pulldowns (6/7), SUSPENDM (3) |
| `0x6C` | U2PHYDTM1 | FORCE_* (13–8), RG_VBUSVALID (5), SESSEND (4), BVALID (3), AVALID (2), IDDIG (1), IDPULLUP (0) |
| `0x80` | U2PHYBC12C | BC1.2 charge-detect enable (0) |

The stock driver's `usb_phy_poweron()` is short: clear UART/GPIO/BC1.2 bits, set DP 100 K mode, set `OTG_VBUSCMP_EN`, clear `FORCE_SUSPENDM`, set `PHY_REV = 0x40`, wait 800 µs. The full `usb_phy_recover()` does the device-mode restore and, crucially, calls `set_usb_phy_mode(PHY_DEV_ACTIVE)`, which writes:

```
0x6C: clear 0x10; set 0x2F; set (0x3F << 8)
    = 0x3F2F
```

**Why `+0x800` and not `+0x000`:** the vendor `mtk_musb.h` defines every `USBPHY_*` access as `mtk_musb->xceiv->io_priv + 0x800 + offset`, and LK's `mt_musbphy.c` uses `USB20_PHY_BASE = USBSIF_BASE + 0x800` with `USBSIF_BASE = 0x11CC0000` in the MT6765 `mt_reg_base.h`. Both resolve the U2 COM bank to **`0x11CC0800`**. Concretely, `USBPHY_SET32(0xF10 - 0x800, ...)` (the frequency meter) is `0x11CC0800 + 0x710 = 0x11CC0F10`, not `0x11CC0710`; this relative-vs-absolute offset is where the `0x11CC0000` interpretation differs. A parallel note in this workspace (`research/mtk-usb-phy.md`) argues for the `0x11CC0000` aperture; the macro/header evidence is stronger, but the PHY register file may be decoded at both, so probe `0x11CC00F0` and `0x11CC08F0`; the driver's writes — including `0x6C` — target `+0x800`. If the `ICUSB` clock gate is still closed, **all** PHY reads return 0 regardless of aperture; that is why the clock gate must be opened first. The earlier `usb phy cc=0` observation is therefore consistent with "PHY not clocked" or "PHY in save-current", not necessarily "PHY unpowered".

That sets `VBUSVALID | AVALID | BVALID | SESSEND=0 | IDDIG=1 | IDPULLUP=1` and the force bits. This is what makes the MUSB core see VBUS and drive the D+ pull-up. **The current shim never writes `0x6C`.** It only writes `POWER.SOFTCONN|HSEN`, `DEVCTL`, and `FADDR`. If LK left the PHY in its `usb_phy_savecurrent()` state (which sets `SESSEND=1`, clears `VBUSVALID/AVALID`, and pulls D+/D− down), then `DEVCTL` will read 0 and the host will see nothing — exactly the observed symptom. It is **not** necessary that the PHY be powered off.

The `usb_phy_savecurrent()` sequence in the same driver sets `SUSPENDM`, `FORCE_SUSPENDM`, both pulldowns, `XCVRSEL=01`, `TERMSEL=1`, and finally `PHY_IDLE_MODE` (`0x6C = 0x11...` with `SESSEND=1`). This is the state the kernel's `mt_usb_disable()` leaves; LK's fastboot shutdown very likely does the equivalent.

### 1.4 LK and the preloader do reserve and pass pstore

This is important because the boot DTB alone has **no `ramoops` node**, which is why the earlier notes doubted pstore. But LK builds the ramoops parameters at runtime:

* `lk.img` contains the literal format string at `0x88e30`:

  ```
  ramoops.mem_address=0x%x ramoops.mem_size=0x%x ramoops.pmsg_size=0x%x ramoops.console_size=0x%x
  ```

  This is appended to the kernel command line; the kernel's `ramoops` module parameters pick it up (`ramoops.mem_address`, `ramoops.mem_size`, `ramoops.console_size`, `ramoops.pmsg_size`). The kernel binary contains exactly those parameter names and `ramoops: using module parameters`, so the module-parameter path is used, not a DT node.
* `lk.img` also contains the mblock code that creates `/reserved-memory/mblock-%d-%s` nodes with `compatible = "mediatek,%s"` and `no-map` (`0x8cc18` region of strings). That is how a `mblock-11-pstore` node can exist at runtime even though it is absent from the boot-image DTB.
* The preloader (`mmcblk0boot0.img`) contains `RAM_CONSOLE pstore_addr:0x%x, pstore_size:0x%x, pstore_console_size:0x%x, pstore_pmsg_size:0x%x` and `pstore.reserve pstore memory failed` (`0x331a0`, `0x32fb0`), and the literal `0x47C90000` at `0x23e00`. `bootshim/log.h` records "0x47c90000, size 896KB (from device tree mblock-11-pstore)".

So the premise "no ramoops node in the device DTB" is true but not decisive: **ramoops is configured by LK's command line and the preloader's reserved mblock**, not by a DT node. The exact values should be read from the running kernel, not guessed (§3.3).

### 1.5 The current shim's USB code: three independent defects

This matters for the effort estimate of option 1, and for the claim "the bootloader powers the PHY down".

**(a) EP0 uses the wrong register and the wrong bits.** In MUSB, EP0's control status register is `CSR0` at indexed offset `0x12`; `COUNT0` is at `0x18`. `CSR0` bits (peripheral mode):

```
RXPKTRDY  0x0001
TXPKTRDY  0x0002     <-- send an IN packet
SENTSTALL 0x0004
DATAEND   0x0008     <-- last packet / status stage
SETUPEND  0x0010
SENDSTALL 0x0020
SVDRXPKTRDY 0x0040   <-- acknowledge/clear RXPKTRDY after reading SETUP/data
```

The shim's `ep0_send()` calls `ep_write()` which writes `TXCSR_TXPKTRDY` = `0x0001` to `CSR0`. That is **RXPKTRDY**, not TXPKTRDY; it does not start an IN transfer. `ep0_ack_status()` also writes `0x0001`. `drain_ep0()` reads `MUSB_RXCSR` at offset `0x16`, which is **not** the EP0 CSR (it is the RXCSR of the selected endpoint; for EP0, that offset is reserved). It never reads `CSR0.RXPKTRDY` or `COUNT0`. It therefore never sees a SETUP packet, never clears `SVDRXPKTRDY`, and never sends a descriptor. `request_count` stays 0, exactly as the panel shows.

**(b) EP1's FIFO is never allocated.** MUSB with `CONFIGDATA.DYNFIFO` uses `TXFIFOSZ` (`0x62`) and `TXFIFOADD` (`0x64`) for the selected endpoint; `TXFIFOADD` is the byte offset shifted right by 3. The shim sets `TXMAXP=64` and `TXCSR.MODE` but never writes `TXFIFOSZ`/`TXFIFOADD`. EP1 therefore uses the default FIFO address (0, shared with EP0) and the default size. Even if EP0 worked, the log bytes would go into EP0's FIFO and corrupt control transfers.

**(c) `TXCSR.FIFONOTEMPTY` is checked as `0x0200`.** The correct bit is `0x0002`. `0x0200` is a host-mode data-toggle bit; in peripheral mode the check never fires, so `usb_cdc_putc()` writes into a possibly full FIFO.

In addition, the CDC class requests are blanket-ACKed without reading the OUT data stage (`SET_LINE_CODING` is a 7-byte control-OUT), so a CDC host may fail to open the port even with EP0 fixed. A vendor-specific bulk interface (no class requests) is simpler for a first bring-up; see §2.2.

**Conclusion:** the observed "POWER reads 0x60, DEVCTL reads 0, no host device" is consistent with (i) the PHY in save-current and (ii) EP0/FIFO bugs. It is not evidence that the PHY is dead. The cheapest USB experiment is to add the `0x6C=0x3F2F` write and read back `DEVCTL.BDEVICE`, then fix EP0/FIFO — not to port a whole T-PHY driver.

### 1.6 What the current pstore writer does

`bootshim/pstore.h` writes:

```
struct persistent_ram_buffer {
    u32 sig;      /* 0x43474244 = "DBGC" */
    u32 start;    /* 0 */
    u32 size;     /* n */
    u8  data[];   /* text at +12 */
};
```

The header layout and signature are **correct** for Linux `persistent_ram` (`PERSISTENT_RAM_SIG = 0x43474244`). The problem is the **address**: `PSTORE_BASE = 0x47C90000` is the **start of the whole pstore region**, and in the upstream 4.19 `ramoops` layout the region starts with the **dump zones**, not the console zone. The console zone begins after the dump region. Writing the header at offset 0 makes `ramoops_pstore_read()` treat it as a **dmesg/dump record**: it calls `ramoops_read_kmsg_hdr()` expecting `"====<sec>.<usec>-D\n"`, fails, frees the old log and **zaps** the zone. No `console-ramoops-*` file is created, and the text is lost. This is the most likely explanation of "the ramoops driver doesn't recognise our format".

There is a second, smaller issue: `fb_puthex()` calls `ps_puthex()` and then `fb_puts(label)` (which calls `ps_puts(label)` again), so labels/values are duplicated in the pstore stream. It is readable but wasteful; a single console write path would be cleaner.

---

## 2. The requested options, one by one

### 2.1 Option 1 — power up the MTK USB2 PHY and implement CDC-ACM

**What the host sees.** If it works: a new USB device on the phone's port. With the current CDC-ACM descriptors, Windows 10/11 should bind `usbser.sys` by interface class (02/02/01) and show a COM port; Linux shows `/dev/ttyACM0`. With a vendor-specific bulk interface instead, Windows shows an unknown device until WinUSB/libusb is bound (Zadig), and a libusb program reads EP1 IN. No COM port in the vendor case.

**What must be implemented.**

1. **Clocks.** Enable `CLK_IFR_ICUSB` by writing `1 << 8` to `0x10001084` (not `0x10001080`), select `usb_top_sel` parent 1 (`0x10000090` bit 16, update `0x10000004` bit 22), ungate the mux (`0x10000098` bit 23), and check the optional 48 MHz/26 MHz gates in §1.2. Do this before touching the PHY/controller.
2. **PHY power-on and recovery.** Port `usb_phy_poweron()` + `usb_phy_recover()` from `usb20_phy.c` (about 100 lines). The essential parts are:
   * clear UART/GPIO/BC1.2 bits, set `DP_100K_MODE`, set `OTG_VBUSCMP_EN`, clear `FORCE_SUSPENDM`, set `PHY_REV=0x40`, wait 800 µs;
   * `set_usb_phy_mode(PHY_DEV_ACTIVE)`: `0x11CC086C = 0x3F2F`;
   * optional `hs_slew_rate_cal()` (uses the frequency meter at `0xF00/0xF0C/0xF10`; can be skipped if the default `HSTX_SRCTRL` works, but the stock driver does it on every recover);
   * optional efuse `RG_USB20_INTR_CAL` from `get_devinfo_with_index(107)`; the shim cannot easily read the PMIC efuse, so use the default/`0x1E` path or skip it. This is a tuning, not a connectivity requirement.
   * **Expected readbacks:** `0x11CC0818` should be about `0x40100000` after power-on (PHY_REV=0x40 + `OTG_VBUSCMP_EN`), and about `0x401000F0` after the `DISCTH=0xF` tail; `0x11CC086C` should be `0x3F2F` after `PHY_DEV_ACTIVE`. If `0x11CC086C` reads back 0, the PHY is not clocked (ICUSB/mux/48 M/26 M) or the `VUSB33` rail is off.
3. **MUSB EP0 rewrite.** Implement the real MUSB control endpoint: read `CSR0` at `0x12`, `COUNT0` at `0x18`; on `RXPKTRDY`, read the 8-byte SETUP from the FIFO at `0x20`, write `SVDRXPKTRDY` (`0x0040`) to clear, then:
   * control-IN: write descriptor bytes to the FIFO, set `TXPKTRDY` (`0x0002`), and on the last packet also `DATAEND` (`0x0008`); wait for `TXPKTRDY` to clear before the next packet;
   * control-OUT with data: read `COUNT0` bytes, then `SVDRXPKTRDY | DATAEND` (`0x0048`);
   * no-data request: `SVDRXPKTRDY | DATAEND` (`0x0048`).
   This is the part that is currently absent; it is a few hundred lines if written carefully, or can be adapted from the MUSB gadget driver.
4. **Dynamic FIFO.** For EP1 IN: `TXFIFOSZ = 3` (64 bytes; `MUSB_FIFOSZ_SIZE` encoding 0=8, 1=16, 2=32, 3=64), `TXFIFOADD = 8` (64 >> 3). EP0 keeps its 64-byte FIFO at offset 0.
5. **Descriptors and host protocol.** Either CDC-ACM (needs `SET_LINE_CODING`/`GET_LINE_CODING`/`SET_CONTROL_LINE_STATE` handling and a notification endpoint) or, preferably for the first bring-up, a **vendor-specific interface with one bulk IN endpoint** and no class requests. The host program is ~50 lines of libusb; on Windows bind WinUSB with Zadig once. A vendor interface avoids the `usbser`/INF and class-request failure modes.
6. **Log transport.** A ring buffer in DRAM, drained by EP1 IN packets. Do not block the console on a full FIFO; drop bytes and count drops. The current `usb_cdc_putc()` writes one byte per packet, which is extremely inefficient (one USB packet per character). Batch 64-byte packets; at 12 Mbit/s that is ~1 MB/s theoretical, far more than the console needs.

**Main risk.** The PHY/clock sequence is now known and short, but three things can still bite: (a) the `vusb`/`va12` regulators may be off (the kernel's `regulator_get("vusb"/"va12")` path suggests board supplies; if LK turned them off, the shim cannot easily re-enable them without PMIC/pwrap code); (b) the PHY's `0x6C` forced VBUS works only if the PHY is out of save-current and its PLL/bandgap are up; (c) the EP0 rewrite has many timing corner cases (SETUPEND, DATAEND, status-stage races) that are easy to get subtly wrong. A fourth risk is host-side: Windows may not bind `usbser` for an unknown VID/PID, and libusb/WinUSB requires a one-time driver binding.

**Effort.** **30–50 focused hours**, realistically:
* clocks + PHY port + panel readback: 6–10 h;
* EP0/FIFO rewrite: 10–16 h;
* descriptors + host program + batching: 4–8 h;
* debug cycles (each flash ~5–10 min, plus a host enumeration test): 10–20 h.
If the goal is only "a vendor bulk endpoint that streams bytes", the low end is ~25 h; a polished CDC-ACM port is at the high end.

**Verifiable without flashing?** Only partially. The host-side libusb/WinUSB/CDC driver setup can be tested with any device. The phone-side register sequence can be partially validated on the panel (read back `0x11CC086C`, `0x68`, `DEVCTL`) in one flash, but enumeration itself needs the flash. There is no `fastboot boot`, so every iteration is a flash.

**Critical assessment.** This option is **not** the place to start. The current failure is over-determined: even a perfect PHY bring-up would still fail because EP0 is wrong. The 30–50 h estimate is real. It becomes attractive only if the user needs a **USB-standard** channel (e.g., a host tool that expects a COM port) and cannot use the UART-over-USB cable.

### 2.2 Option 2 — reuse whatever LK leaves configured

There are two distinct sub-options; they have very different odds.

**(2a) Keep LK's PHY/controller/endpoints and talk a minimal vendor protocol on LK's bulk IN.** If LK leaves the PHY in device-active mode, the controller configured, and the host port powered, the shim can skip EP0 entirely: write bytes to LK's bulk IN endpoint FIFO and set `TXPKTRDY`; a libusb program on the host opens the **MediaTek fastboot device** (typically VID `0x0E8D`, PID `0x2000`/`0x201C`) and reads the bulk IN endpoint. No re-enumeration, no descriptors. This is the cheapest possible live channel **if** the precondition holds.

What the host sees: the same fastboot device that was present before the jump; `lsusb` shows `0e8d:2000` (or similar); a libusb program claims the interface and reads EP1 IN. No COM port. The host must not run `fastboot` (it would send commands and may reset the device).

**What must be implemented.** Find LK's endpoint configuration: which endpoint is bulk IN, its `TXMAXP`, `TXFIFOSZ`/`TXFIFOADD`, and the current `TXCSR` state. The shim can dump the indexed registers to the panel in one flash. Then a writer that waits for `TXCSR.TXPKTRDY` to clear and pushes 64-byte packets. No PHY writes, no EP0. Host: libusb open by VID/PID, claim interface 0, read endpoint.

**Main risk.** LK almost certainly calls a USB deinit / PHY save-current before jumping; the kernel's own `mt_usb_disable()` does `usb_phy_savecurrent()` + `usb_enable_clock(false)`, and the observed `DEVCTL=0` / no host device is consistent with that. If so, LK's endpoints are gone and this option is dead. A second risk: the host's fastboot process may have closed and the host controller may have reset the port when LK jumped; the device may no longer be enumerated even if the PHY is on. A third: the shim's current `usb_cdc_init()` soft-resets the MUSB core, which destroys LK's configuration; a reuse path must **not** reset the core and must **not** toggle `SOFTCONN`.

**Effort.** 4–8 h if it works (mostly register dumping and a libusb program). Probability: **15–25 %** on the evidence. The experiment is cheap: one flash that dumps the MUSB indexed registers and `DEVCTL`, plus a host `lsusb` after the shim boots. If the device is still present in the host's device list, it is worth 2 more hours; if not, abandon.

**Verifiable without flashing?** The host-side libusb read can be tested **while the phone is in fastboot** (no shim flash): open the MediaTek fastboot device and read its bulk IN endpoint. If libusb cannot even open it in fastboot, the host side is not ready. The shim side needs one flash. So it is **partially** verifiable.

**(2b) Keep the PHY on and re-enumerate with our own descriptors.** This is option 1 minus the PHY power-on, but plus the full EP0/descriptor work. It saves maybe 4–8 h of PHY work and loses the ability to recover if LK actually powered the PHY down. Effort ~20–40 h; not attractive relative to option 1.

**Critical assessment.** Option 2a is a good **five-minute experiment**, not a plan. The current shim already destroys LK's state with a soft reset, so the existing binary cannot answer the question; a dedicated "don't touch anything, dump the registers" build is needed. If the host still sees the fastboot device after that build, pursue it; otherwise move on.

### 2.3 Option 3 — log to memory and retrieve it later

#### 3a. Reserved DRAM read by a booted Android

**What the host sees.** Nothing during the shim. After Android boots, an ADB shell command reads a file or `/proc` node; the host pulls it. No live channel.

**What must be implemented.** A mechanism for Android to read the reserved region. The obvious mechanisms are absent:
* `/dev/mem`: `CONFIG_DEVMEM` is not set; the node does not exist and cannot be made to work.
* `/proc/kcore`: `CONFIG_PROC_KCORE` is not set.
* `/dev/kmem`: not present on modern kernels.
* `pstore`: this is the one existing "reserved DRAM → file" driver; see 3c. It is strictly better than a custom region.
* A **Magisk-loaded kernel module** could `ioremap()` a reserved region and expose `/proc/dandelion_log`. This is viable but requires the exact 4.19.127 Xiaomi source, matching `vermagic`, and module signing/`CONFIG_MODULE_SIG_FORCE` checks. Effort 15–30 h; risk moderate. It is a fallback if pstore is broken, not a first step.
* The **framebuffer tail** is not a reliable alternative: `/dev/graphics/fb0` exposes the framebuffer's virtual size (736×4800×4 ≈ 14.1 MB), not the full 16 MB reserved region, so a log at 15 MB is beyond the device; Android's `mtkfb` also reinitialises/clears the framebuffer at boot. The `read_log.py` idea of reading 15 MB into the framebuffer via `/dev/mem` cannot work, and the `fb0` variant can only read within the 14.1 MB that Android is actively drawing.

**Main risk.** There is no safe, unused, userspace-readable reserved region other than pstore. Writing to an unknown reserved region can hit TEE/modem/display memory; a kernel module is the only generic escape hatch and brings build/signing risk.

**Effort.** 15–30 h for the module path; 1–2 h to prove the framebuffer idea does not work. **Verifiable without flashing?** The module build/vermagic can be checked locally; the readback needs one flash + module load.

**Critical assessment.** This option collapses into 3c. Do not build a custom reserved region + module before checking whether pstore already provides the same thing.

#### 3b. Raw partition written by the shim (eMMC)

**What the host sees.** After Android boots, a block-device dump (`dd if=/dev/block/by-name/expdb ...`) pulled with `adb pull`; the host decodes it. No live channel.

**What must be implemented.** An eMMC driver in the shim: MSDC0 at `0x11230000` (MT6765 MSDC), card init (CMD0/CMD1/CMD2/CMD3/CMD9/CMD7/CMD16), 512-byte block read/write (CMD17/CMD24), MTK MSDC host registers (CFG/IOCON/PS/INT/CMD/ARG/DAT/DMA), and the pad/clock setup. LK's `msdc.c` is the reference. The safest target is the `expdb` partition (10 MB, exception database) or the `flashinfo` partition (16 MB, addressed by LBA), not `boot`/`lk`/`userdata`. The shim would write a magic + text at a high offset, then Android reads the partition with `dd` (root). The eMMC may already be initialised by LK (it read the boot image), but the shim cannot rely on LK's driver state; it must re-init or reuse the controller carefully.

A parallel design in this workspace (`research/mtk-msdc-pio.md`) makes a useful correction to the naive estimate: **do not re-implement the clock tree, pad muxing or DMA.** LK has already used MSDC0 to load the boot image, so the gate is open, the pads are muxed, the card is powered and in the transfer state; a polled-PIO path only needs to slow the bus clock, re-identify the card from CMD0, and issue CMD24/CMD17 with `MSDC_CFG.PIO=1`, `MSDC_TXDATA/RXDATA`, and `MSDC_INT` polling. That brings a **minimum viable single-block CMD24 write + Android `dd` readback** down to roughly **12–25 h**, not 30–50 h, provided LK really leaves MSDC0 up. The clock/pad re-init fallback is the 30–50 h case.

**Main risk.** **Bricking.** A wrong LBA, a wrong partition offset, or a failed write can corrupt `boot`, `lk`, `preloader`, or `userdata`. The `expdb`/`flashinfo` partitions are relatively safe, but the eMMC's write cache/power-loss behaviour and the bootloader's possible use of `expdb` (AEE/mrdump) can overwrite the log. The PIO driver is 300–700 lines and has many timing/CRC corner cases.

**Effort.** **12–25 h** for the PIO minimum-viable path (per `research/mtk-msdc-pio.md`); 30–50 h if the clock/pad/card state must be re-initialised from scratch. **Verifiable without flashing?** The readback is by `dd` from Android (no re-flash), but a bug can require a flash to recover; the driver cannot be fully tested without flashing. **Still not the first choice**: pstore/UART give a log with far less brick risk.

**Critical assessment.** Not recommended for a debug log. It is a storage project, not a logging project.

#### 3c. pstore/ramoops written by the shim and read by Android — **the recommended first option**

**Is ramoops configured?** Yes, but not via a DT node. The kernel config has `CONFIG_PSTORE_RAM=y`, `CONFIG_PSTORE_CONSOLE=y`, `CONFIG_PSTORE_PMSG=y`, `CONFIG_PSTORE_LAST_KMSG=y`. LK appends `ramoops.mem_address=... ramoops.mem_size=... ramoops.pmsg_size=... ramoops.console_size=...` to the kernel command line (literal string in `lk.img` at `0x88e30`); the preloader reserves a `pstore` region (literal `0x47C90000` at `0x23e00`, and `RAM_CONSOLE pstore_addr/...`); `bootshim/log.h` records 0x47C90000 + 0xE0000 (896 KiB) from the runtime `mblock-11-pstore`. The boot-image DTB has no `ramoops` node because LK adds the mblock node at runtime, not because pstore is missing.

**What the host sees.** Nothing during the shim. After the next Android boot, `adb shell` can read a regular file under `/sys/fs/pstore` (or `/proc/last_kmsg`); the host pulls it. The file is the **previous** boot's console snapshot. This is a one-shot log per boot, not a live stream.

**What must be implemented (small).**

1. **Read the actual parameters from the running phone, once.** No flashing needed:
   ```
   adb shell su -c 'for f in /sys/module/ramoops/parameters/*; do echo "$f=$(cat $f)"; done'
   adb shell su -c 'cat /proc/cmdline'
   adb shell su -c 'dmesg | grep -i -E "ramoops|pstore"'
   adb shell su -c 'ls -l /sys/fs/pstore; cat /sys/fs/pstore/console-ramoops-0'
   adb shell su -c 'cat /proc/last_kmsg'
   adb shell su -c 'ls -l /dev/pmsg0'
   ```
   `/sys/module/ramoops/parameters/` exposes `mem_address`, `mem_size`, `console_size`, `pmsg_size`, `record_size`, `ftrace_size`, `mem_type`, `dump_oops`, `ecc`. The `ramoops_probe()` code copies the pdata into these module parameters, so this is the exact layout the kernel uses. This is the single most valuable zero-flash check in this report.

2. **Compute the console zone.** In the upstream 4.19 layout (the device kernel has no `bconsole` string, so it is the single-console variant), `ramoops_probe()` does:
   ```
   dump_mem_sz = mem_size - console_size - ftrace_size - pmsg_size;
   max_dump_cnt = dump_mem_sz / record_size;
   paddr = mem_address;
   /* dump zones: record_size each, max_dump_cnt of them */
   paddr += record_size * max_dump_cnt;
   console = paddr;              /* <-- write here */
   paddr += console_size;
   ftrace = paddr; paddr += ftrace_size;
   pmsg = paddr;
   ```
   `record_size` and `ftrace_size` are **not** passed by LK's format string, so they keep the 4.19 defaults **4096** each (unless `/sys/module/ramoops/parameters` says otherwise). `console_size` and `pmsg_size` are set by LK. The zone sizes are rounded down to powers of two by `ramoops_probe()` before the layout, so use the rounded values.

   A worked example, assuming `mem_address=0x47C90000`, `mem_size=0xE0000`, `console_size=0x40000`, `pmsg_size=0x10000`, `record_size=0x1000`, `ftrace_size=0x1000`:
   ```
   dump = 0xE0000 - 0x40000 - 0x1000 - 0x10000 = 0x8F000
   dump_used = 0x1000 * (0x8F000 / 0x1000) = 0x8F000
   console = 0x47C90000 + 0x8F000 = 0x47D1F000
   ftrace  = 0x47D5F000
   pmsg    = 0x47D60000 (0x10000, ends at 0x47D70000)
   ```
   If `pmsg_size=0x1000`, the console moves to `0x47D2E000`, ftrace to `0x47D6E000`, pmsg to `0x47D6F000`; the total still ends at `0x47D70000`. Do **not** hard-code either guess; use the module parameters.

3. **Write the header at `console`, not at `mem_address`.** Keep the `persistent_ram_buffer` format: `sig=0x43474244`, `start=0`, `size=n`, text at `+12`. The header and signature are already correct in `pstore.h`; only the base address is wrong. If the console zone is 0x40000, cap `n` at `console_size - 12 - 0x1000` (as the current `PS_MAX` does), and keep the text within the zone.

4. **Consider writing the same text to the pmsg zone as a second copy.** The pmsg zone starts at `mem_address + dump_used + console_size + ftrace_size`; it is exposed as `/dev/pmsg0` and as `pmsg-ramoops-0` (id 0). It is less likely to be overwritten by the kernel's own console writes, and it gives a fallback if the console file is subtly wrong. Cost: ~20 lines. The pmsg zone may be smaller than the console; copy `min(pmsg_size - 12, n)`.

5. **Fix the duplicate writes.** `fb_puts()` calls `ps_puts()`, and `fb_puthex()` calls `ps_puthex()` then `fb_puts(label)` (which calls `ps_puts(label)` again). The pstore text is not a byte-for-byte mirror of the screen; labels/values are duplicated. For a clean log, route all output through one `console_putc()` that writes both the framebuffer and the pstore buffer, or make `fb_puthex()`/`fb_putdec()` not call the `ps_*` variants. This is cosmetic but matters when the log is the only debug channel.

6. **Add a `dsb sy` before the final jump/reset** so the DRAM writes are visible to the next stage. With the MMU off, the writes are strongly ordered, but a barrier is cheap insurance.

7. **Retrieval.** The pstore filesystem must be mounted at `/sys/fs/pstore`; Android's init usually does, but if the directory is empty:
   ```
   adb shell su -c 'mount -t pstore pstore /sys/fs/pstore'
   adb shell su -c 'ls -l /sys/fs/pstore'
   adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0'
   ```
   The console file name in this kernel is `console-ramoops-0`: the kernel binary contains `console-%s-%llu`, so the upstream always-`-<id>` naming applies. The `pmsg` file is `pmsg-ramoops-0`. If `/proc/last_kmsg` exists (`CONFIG_PSTORE_LAST_KMSG=y`), `cat /proc/last_kmsg` is the simplest read. The files are mode 0444, but SELinux may restrict `shell`; use `su -c`.

**Does the kernel preserve the shim's buffer?** Yes, by design:
* `persistent_ram_new()` reads the buffer; `persistent_ram_post_init()` checks `sig ^ PERSISTENT_RAM_SIG` (for the console zone `sig=0` → expects `0x43474244`), validates `size <= buffer_size` and `start <= size`, and calls `persistent_ram_save_old()` — this copies the ring (our text) into `old_log`.
* `ramoops_init_prz()` then calls `persistent_ram_zap()`, which resets `start/size` to 0 for new kernel writes, but **does not free `old_log`**.
* `pstore` creates `console-ramoops-0` from `old_log` at mount/probe. Our text is the "previous boot" console snapshot. The kernel's own console writes go into the reset ring, not into `old_log`, so the file stays as our log for that boot.
* The catch: if `ecc` is non-zero (`/sys/module/ramoops/parameters/ecc`), the zone has ECC bytes and the simple header/data layout is wrong. The default is 0; check it. If ECC is on, either compute the ECC (hard) or use the pmsg zone (same ECC issue) or fall back to UART.

**Main risk.** The layout/zone offset (mitigated by reading `/sys/module/ramoops/parameters`), LK clearing the region before the shim (unlikely: LK's kedump/clear happens before the shim, and the shim writes after), and ECC. The `ramoops_memreserve=4M` built-in cmdline is a separate reservation; it does not change the LK-supplied `mem_address/mem_size` but is worth noting if the params look different.

**Effort.** **3–8 focused hours**:
* zero-flash parameter read + pstore file check: 0.5–1 h;
* compute the offset and patch `pstore.h`/`shim.c`: 1–3 h;
* clean up duplicate writes: 0.5–1 h;
* one shim flash + one stock-boot restore to verify: 1–2 h;
* contingency for layout/ECC: 1–3 h.
If the module parameters match the worked example, it can be done in an afternoon.

**Verifiable without flashing?** **Yes, substantially**: the module parameters, cmdline, `dmesg`, `/sys/fs/pstore`, `/proc/last_kmsg` and `/dev/pmsg0` all tell you whether the layout is as expected and whether the kernel exposes the previous console. The only thing that needs a flash is the shim's write itself.

**Critical assessment.** This is the best expected-value first step. It is not live, and it costs two flashes per log (shim image + stock boot to read), but it gives the **complete log of the boot that hung**, including the last lines, with no hardware and no risk to the USB/PHY. The existing writer is close; the missing insight is that offset 0 is the dump region. This should be fixed before any more USB work.

### 2.4 Option 4 — MTK-specific debug transports already on the board

**4a. BROM / DPL / DA (preloader download modes).** The BROM is in ROM and runs at power-on; the preloader then runs and exposes the DPL/DA protocol for SP Flash Tool/mtkclient. Neither is reachable from the shim: the shim runs after LK, and there is no path back to BROM without a reset. `mtkclient` enters BROM by powering off and holding Vol+/− + power while connecting. **What the host sees:** a MediaTek BROM/preloader USB device (typically VID `0x0E8D`, PIDs `0x0003`/`0x2000`) in SP Flash Tool/mtkclient, but only when the phone is power-cycled into BROM/preloader, never after LK. **Verifiable without flashing:** no. Could a **warm reset** preserve DRAM and let a host read it via BROM/DA? In principle BROM has memory-read commands, but the preloader's DRAM/EMI initialisation runs again on reset and will retrain/write patterns, so the shim's log is not reliably preserved; and entering BROM requires the right key/test-point sequence. Not a usable log channel.

**4b. META mode.** `usb20.c` has `if (get_boot_mode() == META_BOOT || ADVMETA_BOOT) musb_force_on = 1`, and the META daemon/SP META tool exposes a USB COM-like channel. But META is a **boot mode/Android-side** feature; it does not run the shim. **What the host sees:** a MediaTek META COM port / META tool connection, but only after Android boots in META mode. **Verifiable without flashing:** no (Android-side). It is useful only for post-boot debugging, not for the pre-XNU shim. No.

**4c. USB2JTAG.** `CONFIG_MTK_USB2JTAG_SUPPORT` is not set in the kernel config; the PHY code has an `usb2jtag_usb_init()` that puts the PHY into a USB2 JTAG mode, but a JTAG probe and the JTAG toolchain would be required. **What the host sees:** a JTAG probe, not a log stream. **Verifiable without flashing:** no. It is a debugger transport, not a log console; not practical.

**4d. UART-over-USB — the best MTK-specific option, and a better option than the requested list.** The MT6765 PHY has a UART mode used by MTK's factory/preloader "UART DYNAMIC SWITCH". The stock driver's `usb_phy_switch_to_uart()` (compiled out because `CONFIG_MTK_UART_USB_SWITCH` is not set) shows exactly what to do:

```
/* 0x18: BC1.2 off */
clear 0x18 bit 23
/* 0x68 (U2PHYDTM0): enter UART mode */
set 0x68 bit 3        (SUSPENDM)
set 0x68 bit 18       (FORCE_SUSPENDM)
clear 0x68 bits 31:30; set bit 30   (RG_UART_MODE = 01)
clear 0x68 bit 29     (FORCE_UART_I = 0)
set 0x68 bit 28       (FORCE_UART_BIAS_EN)
set 0x68 bit 27       (FORCE_UART_TX_OE)
set 0x68 bit 26       (FORCE_UART_EN)
set 0x6C bit 18       (RG_UART_BIAS_EN)
set 0x6C bit 17       (RG_UART_TX_OE)
set 0x6C bit 16       (RG_UART_EN)
set 0x20 bit 17       (DM_100K_EN)
clear 0x68 bits 6/7   (DP/DM pulldown off)
/* AP GPIO: route UART0 to the USB pins */
0x10005600 bits [6:4] = 1   (GPIO_SEL_UART0 = 1 << 4)
```

The AP GPIO controller is `gpio@10005000` (`compatible = "mediatek,gpio"`); `RG_GPIO_SELECT` is at `+0x600`. UART0 is `serial@11020000` at `0x11002000`, 8250-compatible, 26 MHz input; `CLK_IFR_UART0` is `infracfg_ao` ifr2 bit 22, enabled by writing `1 << 22` to `0x10001084` (same set/clr convention as ICUSB; `0x10001080` gates it off). The bootargs already use `console=ttyS0,921600n1`, so LK/kernel expect 921600; the shim can use **115200** for margin (DLL=14, DLM=0 at 26 MHz, 0.75 % error) and the host terminal can match. The UART mode is a **physical UART on D+/D−**, not a USB device: the host uses a **USB-TTL adapter** (3.3 V or 1.8 V) with GND common, adapter RX to the phone's TX line, and **no VBUS connection** to the adapter. Try D+ then D− (the PHY code does not document which is TX; the UART mode's `RG_UART_I`/`TXD` mapping is board-specific). If the phone's PHY UART mode drives 1.8 V, use a 1.8 V adapter or a level shifter; a 3.3 V adapter's input threshold may not reliably see 1.8 V highs.

**What the host sees.** A COM port belonging to the **USB-TTL adapter** (FT232/CP2102/CH340), not to the phone. The phone is not enumerated as a USB device. The shim writes UART0 THR; the host receives text at 115200/921600 8N1. No USB stack, no descriptors, no EP0, no PHY USB bring-up beyond the UART mode.

**What must be implemented.** ~50–100 lines: the PHY UART sequence above, the AP GPIO select, UART0 8250 init (LCR/DLL/DLM/FCR/MCR), and a `uart_putc()` called from `fb_putc()`/a single console path. Enable the `ICUSB` gate and `usb_top_sel` first (the PHY register writes need the PHY clock). If the UART's baud is already programmed by LK, only THR writes are needed. Add a `dsb`/small delay after the PHY sequence.

**Main risk.** (a) **Electrical**: the D+/D− pins are 1.8 V USB PHY pins; a 3.3 V adapter's TX driving the phone's RX may overstress the PHY; use a level shifter or a 1.8 V adapter, and do not connect VBUS. (b) **Polarity/mapping**: D+ vs D− for TX/RX is not documented in the driver; try both. (c) **UART mode may require the USB PHY PLL to be in a particular state**; the stock sequence assumes the PHY is powered and clocked. (d) The USB connector's ESD/protection components may load the UART signal; factory cables are short. (e) If the phone is also connected to a PC for ADB, the PC's USB host will see invalid USB signalling on D+/D− and may not enumerate; use a **charge-only** cable or a standalone adapter, not the PC's USB port, for the UART.

**Effort.** **6–12 focused hours**: PHY/UART code 4–8 h, cable/breakout 1–2 h, debug/polarity/level 2–4 h. It is one flash per code change and gives **live** logs; no restore flash. **Verifiable without flashing?** The adapter loopback and the host terminal can be tested without the phone; the phone side needs one flash. It is partially verifiable.

**Critical assessment.** This is the **best live channel** and the most important "better option" in this report. It is cheaper than fixing USB CDC by a factor of 3–5, and it does not depend on MUSB/EP0 at all. The main reason not to start with it is the cable/level work and the uncertainty of the UART-mode mapping; pstore is the zero-hardware step that unblocks the current debugging immediately. If the user is willing to spend one day on a cable, UART-over-USB is the option that truly replaces the photograph loop.

### 2.5 Option 5 — host-side alternatives that need no device changes

**5a. Camera + template-matching screen decoder.** The shim's font is a fixed 8×16 bitmap (`bootshim/font.h`), the screen geometry is known (720 visible pixels, stride 2944, `fb_putc()` at `px = 8 + cursor_x`, `cursor_y += 18`), and the console is monospaced. A host program can:
1. capture a still (or a video frame) with a fixed phone holder and controlled lighting;
2. locate the screen rectangle and the character grid (known aspect/stride);
3. for each 8×16 cell, compare against the known glyph bitmaps (normalised cross-correlation or Hamming distance after thresholding), with sub-pixel alignment and a model for the known horizontal "ghost" (the docs note each glyph is drawn twice);
4. emit the text with a confidence per cell, and flag low-confidence cells for a second photo.

Accuracy/effort honestly:
* The existing photos are **1000×1333**; the phone screen occupies roughly 600–700 px of that width, so each 8×16 font pixel maps to roughly **0.8–1.0 camera pixels**. The text is legible (the existing `artifacts/shot.jpg` shows the full GIC inventory and `usb:` lines), but glyph-level matching at that scale is marginal; hex digits will have errors.
* A **12 MP camera** (4000×3000) framed on the same screen gives ~4–5 camera pixels per font pixel, which is plenty for template matching. With a fixed mount, a dark room and a diffuse light, **>99 % character accuracy** is realistic for the known font; the limiting factor is glare/reflection and the screen's subpixel/ghost rendering, not OCR.
* **Tesseract** on the existing 1.3 MP photos is more like **85–95 %** character accuracy after grayscale/threshold/upscale; it confuses `0/O`, `1/l/I`, `5/S`, `8/B`, which is exactly the wrong failure mode for addresses/registers. Template matching is better because the font is known.
* Throughput: template matching ~**1–3 s per screen** on a laptop (OpenCV, 90×50 cells), plus capture; Tesseract ~3–8 s. A 50-line screen is one capture; a 1000-line boot log needs ~20 captures and someone/something to scroll (the shim's console scrolls automatically; a capture every few seconds can reconstruct the sequence, but ordering/dedup is work).
* Effort: **6–12 h** for the template-matching pipeline (including the glyph extraction from `font.h`, which the repo already has in `tools/qemu/font8x16.py`), plus a phone holder and light. Tesseract-only is **3–6 h** but less accurate.
* **Verifiable without flashing?** Yes — the existing photos and any new photo can be decoded offline; no device changes.
* **Host sees:** image files and decoded text; no live device.

**Critical assessment.** This is a good **fallback** and a way to salvage the existing photo workflow, but it does not solve the "too slow" problem for interactive debugging: it is still one screen at a time, and the shim's screen only holds ~50 lines. Use it if pstore and UART both fail, or as a cross-check for a pstore log.

**5b. Camera + QR/visual code.** The shim could render each log line as a QR/barcode or a custom high-contrast cell code; the host decodes frames. This gives much higher bandwidth (a QR v10 is ~200 bytes; at 10 fps, ~2 KB/s) and avoids OCR errors, but requires a QR encoder in the shim (or a simpler custom code) and a camera decoder. Effort 10–20 h; overkill for a 50-line screen. Mention only as a variant.

**5c. Screen capture via ADB / HDMI / MIPI DSI capture.** ADB only exists after Android boots; the shim is before Android. There is no HDMI. A MIPI DSI capture device/FPGA is expensive and complex. No.

**5d. Manual photography.** The current method; zero effort, but the bottleneck. It is the baseline against which the other options should be judged.

---

## 3. Ranking and recommendation

### 3.1 Ranking by expected value (time-to-first-useful-log, risk, hardware)

| Rank | Option | Why it is here |
|---:|---|---|
| **1** | **pstore console-zone fix** | 3–8 h, no hardware, kernel support already present, full log, zero-flash parameter check. The only real work is computing the console offset and testing one boot. |
| **2** | **UART-over-USB** | 6–12 h + a cable, **live** log, one flash per code change. The best long-term channel; the cable/level work is the only reason it is not #1. |
| 3 | Camera + template matching | 6–12 h, no device changes, works on existing photos, but not live and needs a good camera/mount. |
| 4 | Option 2a (reuse LK endpoints) | 4–8 h if it works, but ~15–25 % probability; a cheap experiment after pstore, not a plan. |
| 5 | USB CDC (option 1) | 30–50 h and several independent hard parts; the current code's EP0/FIFO defects make the existing failure uninformative. |
| 6 | Reserved DRAM + Magisk module | 15–30 h, build/signing risk; only if pstore is broken. |
| 7 | Raw eMMC | 12–25 h PIO / 30–50 h full init, brick risk; a storage project, not a logging project. |
| 8 | BROM/DPL/META/USB2JTAG | Not reachable from the shim / not enabled / needs a probe. |

### 3.2 The single option to start with

**Start with pstore/ramoops, but first do the zero-flash parameter check.** Concretely:

1. Boot the stock Android (the phone is already restored).
2. Read the exact ramoops layout:
   ```
   adb shell su -c 'for f in /sys/module/ramoops/parameters/*; do echo "$f=$(cat $f)"; done'
   adb shell su -c 'cat /proc/cmdline'
   adb shell su -c 'dmesg | grep -i -E "ramoops|pstore"'
   adb shell su -c 'ls -l /sys/fs/pstore; cat /sys/fs/pstore/console-ramoops-0'
   adb shell su -c 'cat /proc/last_kmsg'
   adb shell su -c 'ls -l /dev/pmsg0'
   ```
3. If the parameters are present and `/sys/fs/pstore` shows a previous console (even the Android boot log), pstore is proven. Compute `console = mem_address + record_size * ((mem_size - console_size - ftrace_size - pmsg_size) / record_size)` and set `PSTORE_BASE` to that value (and `PSTORE_ZONE_SIZE` to `console_size`).
4. Move `ps_reset()` after the DTB pointer is available (or hard-code the values read in step 2), write the header at `console`, keep the `DBGC/start=0/size=n` format, cap the text at `console_size - 12 - 0x1000`, add `dsb sy` before the jump/reset, and de-duplicate the `fb_put*`/`ps_put*` calls.
5. Test: flash the shim boot image, let it run, force fastboot, flash the stock boot, boot Android, `adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0'` (or `/proc/last_kmsg`). The log should contain the shim's lines, including the last line before the hang.

**Why this one:** it is the only option that is both **cheap** and **certain enough** to try today. It uses an existing kernel driver, needs no USB/PHY/cable, and the parameters are readable without flashing. It directly addresses the debugging need: the full log of a boot that hangs, instead of a photograph of the last 50 lines. It also validates the pstore region and the Android-side read path, which remains useful even after a live channel exists.

**What it is not:** pstore is **not live**, and it costs **two flashes per log** (shim image + stock boot). If the user needs interactive, live logs, the next step is UART-over-USB. Do not interpret the pstore recommendation as "don't build the UART cable"; build it in parallel.

### 3.3 Why not start with USB CDC (option 1)

Because the expected value is poor:
* The current failure is not diagnostic: EP0 uses `0x0001` instead of `0x0002` for `CSR0.TXPKTRDY`, reads the wrong CSR for EP0, never acknowledges `SVDRXPKTRDY`, and never allocates EP1's FIFO. The PHY may also be in save-current. Any of these alone prevents enumeration.
* The fix is 30–50 h with multiple flash cycles, and the host-side CDC/WinUSB driver adds another variable.
* The PHY part is now much better understood (the `usb_phy_recover()` sequence is ~100 lines), but that does not reduce the EP0/FIFO work.

If the user insists on a USB-standard channel, do the **cheap PHY probe first**: add a build that only writes `0x11CC086C=0x3F2F` (after enabling the clock) and prints `DEVCTL`/`BDEVICE`; if `BDEVICE` becomes 1 and the host sees a device, the PHY is alive and the remaining work is EP0/FIFO. If `BDEVICE` stays 0, check the `vusb`/`va12` regulators and the clock gate before writing more code.

### 3.4 Why option 2 is only a cheap experiment

The shim's current `usb_cdc_init()` soft-resets the MUSB core. That destroys any endpoint configuration LK left. A reuse build must:
* not reset the core,
* not toggle `SOFTCONN`,
* dump `TXMAXP`/`TXCSR`/`TXFIFOSZ`/`TXFIFOADD` for the endpoints LK used,
* check whether the host still sees the MediaTek fastboot device after the jump.

If the host device disappears (likely, given `DEVCTL=0` and LK's USB deinit), the option is dead. If it remains, a libusb program can read the bulk IN endpoint, and the shim can skip EP0 entirely. The experiment is one flash and one `lsusb`; do it only after pstore, and do not spend more than a day on it.

---

## 4. Concrete implementation sketches

### 4.1 pstore: the minimal patch

```c
/* Values read from /sys/module/ramoops/parameters on the phone, not guessed. */
#define PS_MEM_ADDR     0x47C90000UL
#define PS_MEM_SIZE     0x000E0000UL
#define PS_CONSOLE_SIZE 0x00040000UL
#define PS_PMSG_SIZE    0x00010000UL   /* verify */
#define PS_FTRACE_SIZE  0x00001000UL   /* default 4096 */
#define PS_RECORD_SIZE  0x00001000UL   /* default 4096 */

static inline uint32_t ps_console_base(void)
{
    uint32_t dump = PS_MEM_SIZE - PS_CONSOLE_SIZE - PS_FTRACE_SIZE - PS_PMSG_SIZE;
    uint32_t used = PS_RECORD_SIZE * (dump / PS_RECORD_SIZE);
    return PS_MEM_ADDR + used;
}
```

Then change `ps_hdr()`/`ps_char()` to use `ps_console_base()` instead of `PSTORE_BASE`, and set `PS_MAX = PS_CONSOLE_SIZE - PS_HDR_SIZE - 0x1000`. Keep the `DBGC` signature and `start=0`. If the parameters differ, change the six constants (or parse `/chosen/bootargs` for `ramoops.*` and use the values). The pmsg copy, if desired, uses `PS_MEM_ADDR + used + PS_CONSOLE_SIZE + PS_FTRACE_SIZE`.

**Important ordering:** `shim_main()` currently calls `ps_reset()` before `fb_init()` and before the DTB is parsed. Either move the pstore init after the DTB pointer is available, or hard-code the constants for this device. If parsing the DTB, look for `/chosen/bootargs` (LK may put the ramoops parameters there) and for a `/reserved-memory/mblock-*-pstore` node (`reg` gives the base/size). The preloader/LK strings show the mechanism; the runtime values are authoritative.

### 4.2 pstore: what to check if the log does not appear

| Symptom | Likely cause | Check |
|---|---|---|
| `console-ramoops-0` missing | wrote at the wrong zone (dump vs console), or kernel zapped it | `/sys/fs/pstore` listing; `dmesg \| grep ramoops`; verify `console` formula |
| file exists but empty/garbage | header `size/start` invalid, or ECC | `/sys/module/ramoops/parameters/ecc`; header bytes at console |
| file contains Android log, not shim | shim wrote before/after the kernel and the ring was overwritten | check the timing; write the pmsg zone too |
| `/sys/fs/pstore` empty | pstore fs not mounted | `mount -t pstore pstore /sys/fs/pstore` |
| `/proc/last_kmsg` missing | `CONFIG_PSTORE_LAST_KMSG` path not created on this build | use the pstore file |
| shim log truncated | `PS_MAX`/zone too small or ring wrapped | use `console_size` and the pmsg copy |

### 4.3 UART-over-USB: the minimal patch

```c
#define PHY_U2  0x11CC0800UL      /* U2 COM bank */
#define GPIO_SEL 0x10005600UL     /* gpio@10005000 + 0x600 */
#define UART0   0x11002000UL

static void phy_uart_mode(void)
{
    /* enable ICUSB gate and usb_top_sel first (see §1.2) */
    clr32(PHY_U2 + 0x18, 1u << 23);          /* BC1.2 off */
    set32(PHY_U2 + 0x68, 1u << 3);           /* SUSPENDM */
    set32(PHY_U2 + 0x68, 1u << 18);          /* FORCE_SUSPENDM */
    clr32(PHY_U2 + 0x68, 3u << 30);          /* RG_UART_MODE = 01 */
    set32(PHY_U2 + 0x68, 1u << 30);
    clr32(PHY_U2 + 0x68, 1u << 29);          /* FORCE_UART_I = 0 */
    set32(PHY_U2 + 0x68, 1u << 28);          /* FORCE_UART_BIAS_EN */
    set32(PHY_U2 + 0x68, 1u << 27);          /* FORCE_UART_TX_OE */
    set32(PHY_U2 + 0x68, 1u << 26);          /* FORCE_UART_EN */
    set32(PHY_U2 + 0x6C, 1u << 18);          /* RG_UART_BIAS_EN */
    set32(PHY_U2 + 0x6C, 1u << 17);          /* RG_UART_TX_OE */
    set32(PHY_U2 + 0x6C, 1u << 16);          /* RG_UART_EN */
    set32(PHY_U2 + 0x20, 1u << 17);          /* DM_100K_EN */
    clr32(PHY_U2 + 0x68, (1u << 6) | (1u << 7));
    /* route UART0 to the USB pins */
    clr32(GPIO_SEL, 7u << 4);
    set32(GPIO_SEL, 1u << 4);
}

static void uart0_init_115200(void)
{
    /* enable UART0 bus gate: write 1<<22 to 0x10001084 */
    /* 26 MHz / (16 * 14) = 116071, 0.75% error; LCR DLAB sequence */
    out8(UART0 + 3, 0x83);  out8(UART0 + 0, 14); out8(UART0 + 1, 0);
    out8(UART0 + 3, 0x03);  out8(UART0 + 2, 0x07); out8(UART0 + 4, 0x03);
}
```

Then route `fb_putc()` (or the single console path) to `uart_putc()` in addition to the framebuffer. Test with a **USB-TTL adapter on D+/D−, GND common, no VBUS**; try D+ and D− as the phone's TX. If the phone's UART mode is 1.8 V, use a 1.8 V adapter or a level shifter. The stock driver's `usb_phy_switch_to_usb()` is the reverse sequence if the phone needs to go back to USB.

### 4.4 MUSB EP0/FIFO: the corrections (for option 1)

```
EP0: CSR0 = 0x12, COUNT0 = 0x18, FIFO = 0x20
     TXPKTRDY = 0x0002, RXPKTRDY = 0x0001,
     DATAEND = 0x0008, SVDRXPKTRDY = 0x0040
EP1 IN: TXMAXP = 64, TXFIFOSZ = 3, TXFIFOADD = 64 >> 3,
        TXCSR.TXPKTRDY = 0x0001, FIFONOTEMPTY = 0x0002
```

A correct control-IN sequence for a descriptor:

```
select_ep(0);
read 8 bytes from FIFO (SETUP);
write CSR0 = SVDRXPKTRDY;                 /* 0x0040 */
write FIFO, up to 64 bytes;
write CSR0 = TXPKTRDY;                    /* 0x0002 */
wait for TXPKTRDY to clear;
if more data: repeat FIFO/TXPKTRDY;
last packet: write CSR0 = TXPKTRDY | DATAEND;  /* 0x000A */
```

For a control-OUT with data:

```
read COUNT0 bytes from FIFO;
write CSR0 = SVDRXPKTRDY | DATAEND;       /* 0x0048 */
```

The current code's `0x0001` writes are the single most important bug. The second is the missing dynamic-FIFO setup; the third is the wrong `FIFONOTEMPTY` bit.

---

## 5. Sources

External material I relied on (all fetched 2026-10-27):

* Linux 4.19 `fs/pstore/ram.c` — ramoops layout, module parameters, dump/console/ftrace/pmsg order:
  https://raw.githubusercontent.com/torvalds/linux/v4.19/fs/pstore/ram.c
* Linux 4.19 `fs/pstore/ram_core.c` — `persistent_ram_buffer`, `PERSISTENT_RAM_SIG`, `persistent_ram_post_init`/`save_old`/`zap`:
  https://raw.githubusercontent.com/torvalds/linux/v4.19/fs/pstore/ram_core.c
* Linux 4.19 `drivers/usb/musb/musb_regs.h` — `CSR0`/`TXCSR`/`RXCSR` bits, `DEVCTL` bits, `TXFIFOSZ`/`TXFIFOADD`, `FIFONOTEMPTY`:
  https://raw.githubusercontent.com/torvalds/linux/v4.19/drivers/usb/musb/musb_regs.h
* Linux 4.19 `drivers/usb/musb/musb_gadget_ep0.c` — the real EP0 state machine (`SVDRXPKTRDY`, `DATAEND`, `SETUPEND`, status stages):
  https://raw.githubusercontent.com/torvalds/linux/v4.19/drivers/usb/musb/musb_gadget_ep0.c
* Xiaomi `dandelion-q-oss` kernel, `fs/pstore/ram.c` and `fs/pstore/inode.c` — MTK variant with `pstore_set_addr_size` and the pstore file naming:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/fs/pstore/ram.c
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/fs/pstore/inode.c
* Xiaomi `dandelion-q-oss`, `drivers/misc/mediatek/usb20/mt6765/usb20_phy.c` — `usb_phy_poweron`, `usb_phy_recover`, `usb_phy_savecurrent`, `set_usb_phy_mode`, `usb_phy_switch_to_uart`, `hs_slew_rate_cal`:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/drivers/misc/mediatek/usb20/mt6765/usb20_phy.c
* Xiaomi `dandelion-q-oss`, `drivers/misc/mediatek/usb20/mt6765/usb20.c` — clock/regulator setup, MUSB glue, `mt_usb_enable/disable`, META boot force-on, `usb_dump_debug_register`:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/drivers/misc/mediatek/usb20/mt6765/usb20.c
* Xiaomi `dandelion-q-oss`, `drivers/misc/mediatek/usb20/mt6765/usb20.h` — GPIO select macros (`RG_GPIO_SELECT`, `GPIO_SEL_UART0`) and the UART-mode API:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/drivers/misc/mediatek/usb20/mt6765/usb20.h
* Xiaomi `dandelion-q-oss`, `drivers/misc/mediatek/usb20/mtk_musb.h` — the `USBPHY_*` macros (`io_priv + 0x800 + offset`), which is the evidence that the U2 COM bank is at `0x11CC0800`, not `0x11CC0000`:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/drivers/misc/mediatek/usb20/mtk_musb.h
* The local LK reference in this workspace: `research/refs/lk-svoboda18/platform/mt6765/mt_musb/mt_musbphy.c` and `research/refs/lk-svoboda18/platform/mt6765/include/platform/mt_reg_base.h` (`USBSIF_BASE = 0x11CC0000`, `USB20_PHY_BASE = USBSIF_BASE + 0x800`).
* Xiaomi `dandelion-q-oss`, `drivers/misc/mediatek/usb20/mt6765/mtk-phy-a60810.h` — PHY register bit definitions and offsets:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/drivers/misc/mediatek/usb20/mt6765/mtk-phy-a60810.h
* Xiaomi `dandelion-q-oss`, `drivers/clk/mediatek/clk-mt6765.c` — `usb_top_sel` mux, `CLK_IFR_ICUSB` gate, `usb_top_parents`:
  https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/dandelion-q-oss/drivers/clk/mediatek/clk-mt6765.c
* Linux 4.19 `drivers/phy/mediatek/phy-mtk-tphy.c` — mainline T-PHY reference (not the MT6765 `a60810` layout; use only for concepts):
  https://raw.githubusercontent.com/torvalds/linux/v4.19/drivers/phy/mediatek/phy-mtk-tphy.c
* mtkclient README — BROM mode entry (power off, Vol+/− + power), preloader/BROM distinction:
  https://raw.githubusercontent.com/bkerler/mtkclient/main/README.md

Local artifacts used as primary evidence (read-only):

* `D:\aios\boot_backup.img` — stock boot image; kernel at `0x800`, size `13907597`; cmdline `bootopt=64S3,32N2,64N2 ...` at `0x40`; DTB at `0x0f3b000`.
* The extracted stock kernel (`Linux 4.19.127-perf-g7288046673d5`) and its embedded config (`CONFIG_PSTORE_*`, `CONFIG_DEVMEM=n`, `CONFIG_PROC_KCORE=n`, `CONFIG_MTK_UART_USB_SWITCH=n`, `CONFIG_CMDLINE="ramoops_memreserve=4M"`).
* `D:\aios\stock_boot.dtb` — `usb0@11200000` regs/clocks, `gpio@10005000`, `serial@11020000`, reserved-memory list.
* `D:\aios\artifacts\backup\lk.img` — the `ramoops.mem_address=...` format string at `0x88e30`, mblock string block at `0x8cc18`, `kedump`/`ram_console` strings.
* `D:\aios\artifacts\backup\mmcblk0boot0.img` — preloader `pstore` reservation strings at `0x331a0`/`0x32fb0`, literal `0x47C90000` at `0x23e00`.
* `D:\aios\dandelion-xnu\bootshim\usb_cdc.c`, `usb_cdc.h`, `pstore.h`, `fb.c`, `log.h`, `shim.c` — the current implementation and the defects described in §1.5/§1.6.
* `D:\aios\dandelion-xnu\docs\USB_CONSOLE.md`, `NEXT-STEPS.md`, `HARDWARE_ANALYSIS.md`, `BOOT_STATUS.md` — earlier observations (DEVCTL=0, POWER=0x60, no host device, `fastboot boot` unsupported, framebuffer photos).
* `D:\aios\artifacts\shot.jpg` — a 1000×1333 photo of the shim screen; the text is legible at roughly 0.8–1.0 camera pixels per font pixel, which is the basis for the camera accuracy estimate.

Local notes from parallel work in the same workspace, used as cross-checks:

* `research/mtk-usb-phy.md` — register-level PHY note. It is useful for the clock-gate details (`0x10001084` enable, `0x10000098` mux gate, `0x10000104` 48 MHz, `0x1000c014` 26 MHz, `0x10003000` reset) and for the `0x18` readbacks. It argues for the `0x11CC0000` PHY aperture; the vendor `mtk_musb.h` and LK `mt_reg_base.h`/`mt_musbphy.c` evidence above is stronger for `0x11CC0800`, but probe both (`0x11CC00F0` and `0x11CC08F0`).
* `research/mtk-msdc-pio.md` — polled-PIO eMMC design; it is the basis for the lower 12–25 h estimate on the eMMC option (single-block CMD24/CMD17, no clock-tree/DMA re-implementation).

**Caveat on the Xiaomi source:** the `dandelion-q-oss` tree is a reference, not necessarily the exact device build. The device kernel binary has `console-%s-%llu` (always `-id` naming) and no `bconsole` string, so its pstore layout is the upstream single-console variant; the Xiaomi tree has an additional `bprz` boot-console zone. The **module parameters on the phone are authoritative**; do not hard-code the Xiaomi-tree layout without checking them.

---

## 6. One-page decision

* **Today:** fix pstore. Read `/sys/module/ramoops/parameters/*`; compute the console offset; write the `DBGC` header there; read `console-ramoops-0` (or `/proc/last_kmsg`) after the next Android boot. 3–8 h, no hardware, full log. Two flashes per log.
* **This week:** build the UART-over-USB cable and the PHY UART-mode patch. 6–12 h, live logs, one flash per code change. This is the real replacement for the photograph loop.
* **Only if a USB-standard channel is mandatory:** fix the MUSB EP0/FIFO and the PHY recover sequence. 30–50 h. Start with a register-only probe (`0x6C=0x3F2F`, `DEVCTL.BDEVICE`) before writing the CDC stack.
* **Do not** start with raw eMMC, a custom reserved-DRAM module, BROM/DPL, or META; none of them solves the pre-XNU live-log problem as cheaply as pstore + UART.
* **Keep** the camera/template-matching decoder as a cross-check/fallback; it works on the existing photos and needs no device changes, but it is not live.

The key correction to the prior conclusion: the USB PHY is probably **not unpowered**; it is likely in the MTK `usb_phy_savecurrent()` state and/or its `ICUSB` clock is gated, and the shim never writes the `0x6C = 0x3F2F` device-active value. The USB code also has EP0/FIFO bugs that would prevent enumeration even with a perfect PHY. That is why pstore and UART-over-USB are the rational next steps, not a T-PHY rewrite.
