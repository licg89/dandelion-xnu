# MediaTek LK (MT6765/MT6762) — what it actually does to the GIC and to USB

Ground-truth extraction for the bare-metal port on a MT6765/MT6762-class board
(MUSB controller @ `0x11200000`, U2 PHY @ `0x11CC0000`, GICv3 @ `0x0C000000`/`0x0C100000`).

**Sources (cloned shallow, read-only, treated as data):**

| What | Where | Commit |
|---|---|---|
| Full MTK LK vendor tree ("legacy MTK devices little-kernel from Mediatek (Android 10/11)") | `D:\aios\research\refs\lk-svoboda18` | `37ec8bcbc4361c0e267617f517d154656804c17a` (2024-05-01) |
| OPPO A1K CPH1923 (project 18540, MT6765/MT6762) LK port — patch *on top of* the tree above | `D:\aios\research\refs\oppo-a1k-lk` and the already-present `D:\aios\research\_lk.patch` | `main` @ clone time |
| lk2nd (Qualcomm, GICv3 reference only) | `D:\aios\research\refs\lk2nd` | `fd380ac` |
| Local kernel dumps already in `D:\aios\research\` (`_a60810.h`, `_v_clk-mt6765.c`, …) | used only as **cross-reference** for register/bit names | n/a |

Repo root for all `path:line` citations below is `D:\aios\research\refs\lk-svoboda18`
unless the path starts with `refs/`, `_lk.patch` or `refs/lk2nd/`.

**Confidence markers used throughout:** **[V]** = verified by reading the quoted code;
**[I]** = inference from the code plus the Arm GICv3/MUSB architecture (not proven on this board);
**[U]** = unverified / unknown (needs a board experiment or a source we do not have).

> Copyright note: `dev/gic/*`, `platform/mt6765/*` and `platform/common/mt_musb/*` carry
> MediaTek proprietary headers; the LK core is MIT. Quotations here are for
> interoperability analysis only.

---

## 0. Executive summary (the five answers, short form)

1. **GIC [V]:** LK programs only three things from non-secure: `GICD_CTLR` (twice),
   the `GICD_*` SPI tables, and the **CPU interface** via CP15 (`ICC_SRE`, `ICC_PMR`,
   `ICC_IGRPEN1_EL1`). It **does** enable Group 1 non-secure (`GICD_CTLR` bit 1 +
   `ICC_IGRPEN1_EL1 = 1`). It **never** writes `GICD_IGROUPR`, **never** writes
   `GICR_ISENABLER0`/`GICR_IGROUPR0`, and **never** configures any PPI
   (so **PPI 27/29/30 are untouched**). It writes `GICD_IGRPMODR` (not `IGROUPR`) —
   which is write-ignored from non-secure **[I]**.
2. **Deferral [V]:** there is **no GIC-related SMC** in LK's SIP table. The only
   secure-monitor interaction around interrupts is indirect: right before the kernel
   jump LK calls `MTK_SIP_LK_DAPC_INIT` (`0x82000101`, DEVAPC lock) and
   `MTK_SIP_LK_CRYPTO_HW_ENGINE_DISABLE` (`0x8200010C`). Kernel entry itself is
   `smc #0` with `MTK_SIP_KERNEL_BOOT` = **`0x82000115`**.
3. **USB [V]:** LK brings the PHY up *first* (`mt_usb_phy_poweron()`), then runs the
   full "recover/device-mode" sequence (`mt_usb_phy_recover()`), then touches the MUSB
   MAC (interrupt enables, QMU, `POWER` soft-connect). The MAC's `SWRST` is **not**
   written at init — it is written only when the host issues a bus reset.
   **LK programs no USB clock gate at all** — it relies on clocks the preloader left on.
4. **PHY power-down before boot [V]: YES.** `cmd_boot()` → `udc_stop()` →
   `mt_usb_disconnect_internal()` (clear `POWER.SOFT_CONN`) + `mt_usb_phy_savecurrent()`
   (SUSPENDM + pull-downs + `force_*` bits at PHY `0x68`). In a plain (non-fastboot)
   boot LK never touches USB at all.
5. **What to copy:** the two PHY sequences in §5 (they are short, self-contained,
   32-bit MMIO at `0x11CC08xx`), plus the three MAC byte writes for connect.
   Do **not** copy LK's GIC "IGRPMODR" writes (they are no-ops from NS) and do **not**
   expect LK to have woken the redistributor — in normal boot it does not.

---

## 1. GIC

### 1.1 Where the code lives / what is compiled

```
platform/mt6765/rules.mk:510          MTK_GIC_VERSION := v3
dev/gic/rules.mk:3-7                  ifeq ($(MTK_GIC_VERSION), v3)  OBJS += mt_gic_v3.o
platform/mt6765/include/platform/mt_irq.h:37-48   GICD offsets, GIC_REDIS_WAKER 0x14
platform/mt6765/include/platform/mt_reg_base.h:497-499
                                      GIC_DIST_BASE  (0x0C000000)
                                      GIC_REDIS_BASE (0x0C100000)
```

`GIC600` is **not** defined for mt6765 (only `mt6779`/`mt6785` define it — `mt6779/include/platform/mt_irq.h:55-56`),
so the `gic600_rdistif_init()` / `GICR_PWRR` code in `mt_gic_v3.c` is compiled out for this SoC. **[V]**

### 1.2 The call site and the entry point

`platform_early_init()` runs before anything else on the platform:

```c
/* platform/mt6765/platform.c:506-522 */
void platform_early_init(void)
{
	PROFILING_START("platform_early_init");
	lk_register_wdt_callback();

	/* initialize the uart */
	uart_init_early();
	...
	platform_init_interrupts();
	platform_early_init_timer();
```

```c
/* dev/gic/mt_gic_v3.c:275-290 */
void platform_init_interrupts(void)
{
	uint32_t sec;

	sec = mt_interrupt_needed_for_secure();

	if (sec)
		mt_gic_icc_msre_write();

	mt_gic_dist_init();

	if (sec)
		mt_gic_redist_init();

	mt_gic_cpu_init();
}
```

The `sec` gate is **not** an SMC and **not** a security query — it is a boot-mode test:

```c
/* platform/mt6765/interrupts.c:93-103 */
uint32_t mt_interrupt_needed_for_secure(void)
{
#if defined(MACH_FPGA) && defined(FPGA_BOOT_FROM_LK)
	return 1;
#endif

	if (g_boot_arg->boot_mode == DOWNLOAD_BOOT)
		return 1;

	return 0;
}
```

`DOWNLOAD_BOOT = 100`, `FASTBOOT = 99`, `NORMAL_BOOT = 0` (`platform/mt6765/include/platform/boot_mode.h:61-72`).
**Consequence [V]: in normal boot *and* in fastboot, `sec == 0`, therefore
`mt_gic_icc_msre_write()` and `mt_gic_redist_init()` are never executed.**
The redistributor is never woken by LK in any path a normal user boots through.

### 1.3 Distributor init — exact order, registers, values

```c
/* dev/gic/mt_gic_v3.c:202-268 */
static void mt_gic_dist_init(void)
{
	unsigned int i;
	uint64_t affinity;

	affinity = mt_irq_get_affinity();

	DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CTRL, GICD_CTLR_ARE);

	mt_git_dist_rwp();

	/*
	 * Set all global interrupts to be level triggered, active low.
	 */
	for (i = 32; i < (MT_NR_SPI + 32); i += 16) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CONFIG + i * 4 / 16, 0);
	}

	/*
	 * Set all global interrupts to this CPU only.
	 */
	for (i = 0; i < MT_NR_SPI; i++) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_ROUTE + i * 8, (affinity & 0xFFFFFFFF));
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_ROUTE + i * 8 + 4, (affinity >> 32));
	}

	/*
	 * Set all interrupts to G1S.  Leave the PPI and SGIs alone
	 * as they are set by redistributor registers.
	 */
	for (i = 0; i < NR_IRQ_LINE; i += 32)
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_IGRPMODR + i / 8, 0xFFFFFFFF);

	/*
	 * Set priority on all interrupts.
	 */
	for (i = 0; i < NR_IRQ_LINE; i += 4) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_PRI + i * 4 / 4, 0xA0A0A0A0);
	}

	/*
	 * Disable all interrupts.
	 */
	for (i = 0; i < NR_IRQ_LINE; i += 32) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_ENABLE_CLEAR + i * 4 / 32, 0xFFFFFFFF);
	}

	/*
	 * Clear all active status
	 */
	for (i = 0; i < NR_IRQ_LINE; i += 32) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_ACTIVE_CLEAR + i * 4 / 32, 0xFFFFFFFF);
	}

	/*
	 * Clear all pending status
	 */
	for (i = 0; i < NR_IRQ_LINE; i += 32) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_PENDING_CLEAR + i * 4 / 32, 0xFFFFFFFF);
	}


	dsb();
	mt_git_dist_rwp();
	DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CTRL, GICD_CTLR_ARE | GICD_CTLR_ENGRP1S | GICD_CTLR_ENGRP1NS);
	mt_git_dist_rwp();
}
```

with

```c
/* dev/gic/mt_gic_v3.c:39-44 */
#define GICD_CTLR_RWP       (1 << 31)
#define GICD_CTLR_ARE       (1 << 4)
#define GICD_CTLR_ENGRP1S   (1 << 2)
#define GICD_CTLR_ENGRP1NS  (1 << 1)
#define GICR_WAKER_ProcessorSleep   (1 << 1)
#define GICR_WAKER_ChildrenAsleep   (1 << 2)
```

and the offsets (`platform/mt6765/include/platform/mt_irq.h:37-48`):

| Symbol | Offset | Absolute |
|---|---|---|
| `GIC_DIST_CTRL` (GICD_CTLR) | `0x000` | `0x0C000000` |
| `GIC_DIST_ENABLE_SET` (ISENABLER) | `0x100` | `0x0C000100` |
| `GIC_DIST_ENABLE_CLEAR` (ICENABLER) | `0x180` | `0x0C000180` |
| `GIC_DIST_PENDING_SET/CLEAR` | `0x200 / 0x280` | `0x0C000200 / 0x0C000280` |
| `GIC_DIST_ACTIVE_SET/CLEAR` | `0x300 / 0x380` | `0x0C000300 / 0x0C000380` |
| `GIC_DIST_PRI` (IPRIORITYR) | `0x400` | `0x0C000400` |
| `GIC_DIST_CONFIG` (ICFGR) | `0xC00` | `0x0C000C00` |
| `GIC_DIST_IGRPMODR` | `0xD00` | `0x0C000D00` |
| `GIC_DIST_ROUTE` (IROUTER) | `0x6100` | `0x0C006100` |
| `GIC_REDIS_WAKER` | `0x14` | `0x0C100014` |

Sizes: `MT_NR_SPI = 241`, `MT_NR_PPI = 5`, `GIC_PPI_OFFSET = 27`,
`NR_IRQ_LINE = 27 + 5 + 241 = 273` (`mt_irq.h:60-79`).

**Written values [V]:**

1. `GICD_CTLR <- 0x00000010` (ARE), then poll `GICD_CTLR[31]` (RWP).
2. `GICD_ICFGR[2..17] <- 0` (`0x0C000C08…0x0C000C44`, i.e. INTIDs 32…272) — level-sensitive,
   as the comment says.
3. `GICD_IROUTER[0..240] <- affinity` (64-bit, two 32-bit writes; affinity built from MPIDR,
   `platform/mt6765/interrupts.c:61-91`).
4. `GICD_IGRPMODR[i] <- 0xFFFFFFFF` (note: **IGRPMODR, not IGROUPR**).
5. `GICD_IPRIORITYR[0..272] <- 0xA0A0A0A0`.
6. `GICD_ICENABLER[0..8] <- 0xFFFFFFFF` — **all SPIs disabled**.
7. `GICD_ICACTIVER <- 0xFFFFFFFF`, `GICD_ICPENDR <- 0xFFFFFFFF`.
8. `dsb`, poll RWP.
9. `GICD_CTLR <- 0x00000016` = ARE | EnGrp1S | EnGrp1NS, then poll RWP.

### 1.4 CPU interface init — this is where "group 1 NS" is actually enabled

```c
/* dev/gic/mt_gic_v3.c:168-174 */
static void mt_gic_cpu_init(void)
{
	mt_gic_icc_sre_write(0x01);
	mt_gic_icc_primask_write(0xF0);
	mt_gic_icc_igrpen1_write(0x01);
	dsb();
}
```

```c
/* dev/gic/mt_gic_v3.c:51-77, 107-125 — the CP15 encodings used */
MCR p15, 0, %0, c4,  c6,  0     /* ICC_PMR          <- 0xF0 */
MCR p15, 0, %0, c12, c12, 7     /* ICC_IGRPEN1      <- 1    */
MRC p15, 0, %0, c12, c12, 0     /* ICC_IAR1          (ack)  */
MCR p15, 0, %0, c12, c12, 1     /* ICC_EOIR1         (eoi)  */
MCR p15, 0, %0, c12, c12, 5     /* ICC_SRE          <- 1    */
```

**Answer to "does it enable group 1 non-secure?" — YES [V]:** `GICD_CTLR.EnGrp1NS = 1`
and `ICC_IGRPEN1_EL1 = 1`, both written from the non-secure side, on every boot.
`GICD_CTLR.EnGrp1S` (bit 2) and `ARE` (bit 4) are written in the same word, but from
non-secure those bits are ignored / read-as-zero-if-DS=0 **[I]** — LK never reads
`GICD_CTLR` back, so it cannot tell. Do not assume LK proves anything about bit 2/4.

### 1.5 What LK does **not** do (this is the important part for the port)

* **No `GICD_IGROUPR` write anywhere in the tree** (`grep -rn IGROUPR` → only `IGRPMODR`). **[V]**
  The comment "Set all interrupts to G1S" is attached to an `IGRPMODR` write; IGRPMODR is the
  *modifier*, and from a non-secure access it is WI **[I]**. So LK assigns **no** interrupt to
  a group; whatever group the secure firmware/preloader set survives.
* **No redistributor programming at all in normal/fastboot boot** — `mt_gic_redist_init()`
  (the only `GICR_*` writer, `mt_gic_v3.c:176-190`) is behind the `sec` gate. **[V]**

  ```c
  /* dev/gic/mt_gic_v3.c:176-190 (dead in normal boot) */
  static void mt_gic_redist_init(void)
  {
  	unsigned int value;

  #ifdef GIC600
  	gic600_rdistif_init();
  #endif

  	/* Wake up this CPU redistributor */
  	value = DRV_Reg32(GIC_REDIS_BASE + GIC_REDIS_WAKER);
  	value &= ~GICR_WAKER_ProcessorSleep;
  	DRV_WriteReg32(GIC_REDIS_BASE + GIC_REDIS_WAKER, value);

  	while (DRV_Reg32(GIC_REDIS_BASE + GIC_REDIS_WAKER) & GICR_WAKER_ChildrenAsleep);
  }
  ```

* **No `GICR_ISENABLER0` / `GICR_ICENABLER0` / `GICR_IGROUPR0` / `GICR_IPRIORITYR` access,
  ever.** Verified by searching the whole tree for `GIC_REDIS` — the only hit is `WAKER`. **[V]**
* **No PPI (INTID 27/29/30, i.e. any of `GIC_PPI_*`) is configured, masked or unmasked by LK.**
  Every LK caller passes an SPI id (202, 105, 111, 112, 128). **[V]**
  Caveat, and a trap for the port: on mt6765 the guarded helpers `mt_irq_set_mask()` /
  `mt_irq_unmask()` that begin with `if (irq < GIC_PRIVATE_SIGNALS) return;`
  (`mt_gic_v3.c:314-320, 353-362`) are **inside `#ifdef GIC600`** and are therefore
  **compiled out** here. The helpers that actually exist are the unguarded
  `mt_irq_mask()/mt_irq_unmask()` (`mt_gic_v3.c:461-479`), which always write
  `GICD_ICENABLER/ISENABLER + irq/32*4`. If you ever pass INTID < 32 to those, they write the
  **wrong** register (PPI enables live in `GICR_ISENABLER0`, and `GICD_ISENABLER[0]` is RES0
  for SGIs/PPIs) and silently do nothing. **[V]**
* **No arch-timer/GICv3 PPI programming of any kind.** All LK interrupts are SPIs:
  GPT tick = `MT_GPT_IRQ_ID 202` (`platform/mt6765/timer.c:98-108`, `mt_irq_unmask(MT_GPT_IRQ_ID)`),
  USB = `MT_USB0_IRQ_ID 105` (`platform/common/mt_musb/mt_musb.c:2045`), MSDC 111/112,
  NAND 128, MD WDT (`platform/mt6765/dummy_ap.c:233-235`). **[V]**

> **Directly useful datum for the port [V]:** LK unmaskes SPI 202 and SPI 105 through
> `GICD_ISENABLER` from non-secure, and LK's whole scheduler (and fastboot) depends on those
> interrupts arriving. So *"non-secure cannot enable group-1 interrupts"* cannot be true in
> general on this board — non-secure `GICD_ISENABLER` works for SPIs. The failure mode is
> therefore almost certainly on the **PPI/redistributor** side (INTID 30 lives in
> `GICR_*`, not `GICD_*`) and/or on group *ownership*, not on the enable path itself. **[I]**

The legacy PPI naming in `mt_irq.h:62-67` is ARMv7-era and is **not** the GICv3 arch-timer map
(`GIC_PPI_GLOBAL_TIMER 27`, `PRIVATE_TIMER 29`, `WATCHDOG_TIMER 30`). Nothing in LK uses those
constants. If your port needs INTID 30 = non-secure EL1 physical timer, you must program it
yourself; LK is not a reference for it. **[V]/[I]**

### 1.6 What is left behind at handoff

```c
/* app/mt_boot/mt_boot.c:1434-1447 */
	enter_critical_section();
	/* do any platform specific cleanup before kernel entry */
	platform_uninit();
#ifdef HAVE_CACHE_PL310
	l2_disable();
#endif

	arch_disable_cache(UCACHE);
	arch_disable_mmu();

#ifndef MACH_FPGA
	extern void platform_sec_post_init(void)__attribute__((weak));
	if (platform_sec_post_init)
		platform_sec_post_init();
#endif
```

```c
/* platform/mt6765/platform.c:919-930 */
void platform_uninit(void)
{
	...
	platform_deinit_interrupts();
	return;
}
```

```c
/* dev/gic/mt_gic_v3.c:292-305 */
void platform_deinit_interrupts(void)
{
	unsigned int irq;

	for (irq = 0; irq < NR_IRQ_LINE; irq += 32) {
		DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_ENABLE_CLEAR + irq * 4 / 32, 0xFFFFFFFF);
	}

	dsb();

	while ((irq = mt_gic_icc_iar1_read()) != 1023 ) {
		mt_gic_icc_eoir1_write(irq);
	}
}
```

**State handed to the next image [V]:**

* `GICD_ICENABLER` all-ones → **every SPI disabled** (including USB 105, GPT 202).
* `GICD_CTLR` left at `0x16` (never restored/cleared), `GICD_IPRIORITYR = 0xA0`, `IROUTER` = boot CPU.
* `ICC_SRE_EL1 = 1`, `ICC_PMR = 0xF0`, `ICC_IGRPEN1_EL1 = 1` — **still enabled**.
* `GICR_*` untouched since the preloader (whatever state that was).
* Then caches/MMU off, `platform_sec_post_init()` runs, then the SMC jump.

### 1.7 Exception level

* **[V]** LK is 32-bit AArch32 (`arch/arm/crt0.S` reads `SCTLR` via `mrc p15,0,...`), builds with
  LPAE (`MTK_LM_MODE := yes` in `target/k65v1_64_bsp/rules.mk`, `platform_init_mmu()` calls
  `arm_mmu_lpae_init()` at `platform/mt6765/platform.c:467`), and it **cannot** enter the 64-bit
  kernel by itself — it asks EL3 to do it:

  ```c
  /* app/mt_boot/mt_boot.c:553-559 */
  void lk_jump64(u32 addr, u32 arg1, u32 arg2, u32 arg3)
  {
  	pal_log_err("\n[LK]jump to K64 0x%x\n", addr);
  	pal_log_info("smc jump\n");
  	jumparch64_smc(addr, arg1, arg2, arg3);
  	panic("%s Fail to enter EL1\n", __func__);
  }
  ```
  ```asm
  @ arch/arm/exceptions.S:140-149
  FUNCTION(jumparch64_smc)
  	push {r4}
  	mov r4, r3
  	mov r3, r2
  	mov r2, r1
  	mov r1, r0
  	ldr r0, =0x82000115  /*r0: ATF Function ID, please check MTK_SIP_KERNEL_BOOT_AARCH32 ...*/
  	smc #0
  	pop {r4}
  	b   .
  ```
* **[V]** The tree contains **no** HYP/EL2 programming: no `mcr/mrc p15, 4, ...` (HCR_EL2,
  CNTHCTL, VBAR_EL2 would all need that encoding), no `ICH_*`, no virtual-GIC setup.
  Search: `grep -rn "p15, 4"` → no hits.
* **[I]** Therefore the EL1-vs-EL2 entry of BL33 is decided entirely by ATF/BL31, not by LK.
  Your measurement (LK at EL2 non-secure) is consistent with the code; nothing in LK depends on it.
* **[V]/[U]** The one piece of code that *would* depend on EL3 is dead in normal boot:

  ```c
  /* dev/gic/mt_gic_v3.c:88-105 — only reachable in DOWNLOAD_BOOT */
  static void mt_gic_icc_msre_write(void)
  {
  	uint32_t reg;

  #define MON_MODE    "#22"
  #define SVC_MODE    "#19"

  	/*
  	 * switch to monitor mode and mark ICC_MSRE.
  	 */
  	__asm__ volatile("CPS " MON_MODE "\n"
  	                 "MRC p15, 6, %0, c12, c12, 5\n"
  	                 "ORR %0, %0, #9\n"
  	                 "MCR p15, 6, %0, c12, c12, 5\n"
  	                 "CPS " SVC_MODE "\n" : "=r" (reg));

  	dsb();
  }
  ```
  `p15, 6, c12, c12, 5` is `ICC_MSRE`, which is only accessible from Monitor mode; `CPS #22`
  from non-secure state is not a legal way to get there. So this routine can only ever be a
  no-op or a trap — **do not model your port on it**, and do not conclude from its existence
  that LK runs secure. **[I]**

---

## 2. If LK defers to the secure side: the exact SMC/DAPC mechanism

### 2.1 The mechanism

* Transport: `smc #0` (SMC32/SIP), function IDs in the `0x820001xx` range, dispatched by
  MediaTek ATF (BL31) at EL3. `app/mt_boot/mt_secure_call.c:45-61`:

  ```c
  size_t mt_secure_call_all(size_t function_id,
  	size_t arg0, size_t arg1, size_t arg2,
  	size_t arg3, size_t *r1, size_t *r2, size_t *r3)
  {
  	LOCAL_REG_SET_DECLARE;

  	__asm__ volatile ("smc #0x0\n" : "+r"(reg0),
  		"+r"(reg1), "+r"(reg2), "+r"(reg3), "+r"(reg4));
  	...
  ```
  with `arg0..arg3` in `r1..r4`/`r0`-style marshalling (`LOCAL_REG_SET_DECLARE`, lines 37-43).

* The full LK ID table is `platform/mt6765/include/platform/plat_smc_id_table.h:39-73`.
  Excerpt (`0x82000101` and `0x82000115` are the two that matter here):

  ```c
  #define MTK_SIP_BL_INIT								0x82000100
  #define MTK_SIP_LK_DAPC_INIT						0x82000101
  #define MTK_SIP_LK_MD_REG_WRITE						0x82000102
  #define MTK_SIP_LK_RPMB_INIT						0x82000103
  #define MTK_SIP_LK_RPMB_UNINIT						0x82000104
  #define MTK_SIP_LK_RPMB_HMAC						0x82000105
  #define MTK_SIP_LK_WDT								0x82000106
  #define MTK_SIP_LK_LASTBUS							0x82000107
  #define MTK_SIP_LK_PLAT_DEBUG						0x82000108
  #define MTK_SIP_LK_LASTPC							0x82000109
  #define MTK_SIP_LK_DUMP_ATF_LOG_INFO				0x8200010A
  #define MTK_SIP_LK_CRYPTO_HW_ENGINE_INIT			0x8200010B
  #define MTK_SIP_LK_CRYPTO_HW_ENGINE_DISABLE			0x8200010C
  ...
  #define MTK_SIP_KERNEL_BOOT							0x82000115
  ```

  There is **no** GIC interrupt-controller SMC (no "enable group", no "set affinity",
  no "wake redistributor"). The AArch32 helper for DAPC also exists in assembly and is
  **never called** — `dapc_smc` at `arch/arm/exceptions.S:152-158` (same ID `0x82000101`);
  the C path via `mt_secure_call()` is the one used. **[V]**

### 2.2 Where it is called (order matters)

```c
/* platform/mt6765/platform.c:1032-1047 */
void platform_sec_post_init(void)
{
	unsigned int ret = 0;
	/* Lock Device APC in LK */
	dprintf(CRITICAL, "[DEVAPC] sec_post_init\n");

	ret = crypto_hw_engine_disable();
	if (ret) {
		dprintf(CRITICAL, "[SEC] crypto engine HW disable fail\n");
	}

#if DEVAPC_TURN_ON
	dprintf(CRITICAL, "[DEVAPC] platform_sec_post_init - SMC call to ATF from LK\n");
	mt_secure_call(MTK_SIP_LK_DAPC_INIT, 0, 0, 0, 0);
#endif
}
```

`#define DEVAPC_TURN_ON 1` (`platform/mt6765/platform.c:87`) → **the call is compiled in and runs
on every boot. [V]**

Sequence at the end of `boot_linux_fdt()`:

1. `platform_uninit()` → `platform_deinit_interrupts()` (all SPIs off, IAR drained) — `mt_boot.c:1436`
2. `arch_disable_cache(UCACHE); arch_disable_mmu();` — `mt_boot.c:1441-1442`
3. `platform_sec_post_init()` → `crypto_hw_engine_disable()` (`0x8200010C`) then
   **`MTK_SIP_LK_DAPC_INIT` (`0x82000101`)** — `mt_boot.c:1445-1447`
4. `lk_jump64()` → `smc #0` with **`0x82000115`** (`MTK_SIP_KERNEL_BOOT`) → ATF erets to the kernel
   — `mt_boot.c:1498-1506`

### 2.3 Does this explain "Android gets the timer, we do not"?

* **What DEVAPC is [I]:** MediaTek's Device Access Permission Control — an ATF-configured
  fabric firewall that decides, per bus master/domain, which physical address ranges may be
  accessed. `MTK_SIP_LK_DAPC_INIT` is LK telling ATF "lock it down now, before the kernel runs".
  The register set lives in ATF, not in this tree — we have **no** register/offset evidence
  here. **[U]**
* **Can it block your GIC writes? [U]/[I]** If the GIC (or its redistributor) were in a
  DEVAPC-protected slave set, non-secure writes would be dropped/aborted. But (a) the same
  non-secure world (Linux) does program the same GIC after the call, and (b) LK itself writes
  GICD before the call — so a plain "GIC is firewalled" theory does not fit. What *does* fit is a
  **per-address-range** rule that is narrower than "the whole GIC".
* **How to tell experimentally (do this before blaming DEVAPC):**
  1. Read `GICD_CTLR` (`0x0C000000`) right after your write, and again after `dsb`/`isb`.
     If bit 1 does not stick, you are either not seeing the write land, or the read is masked.
     Compare with the same read performed by a payload that runs *after* the Android kernel has
     booted (e.g. from a kernel module or `/dev/mem`).
  2. Read `GICD_IIDR` (`0x0C000008`): ProdID field (bits 31:24) tells GIC500 (0x0) vs GIC600 (0x2),
     same test LK uses at `mt_gic_v3.c:433-438`. If this read aborts/hangs, you have an access
     problem (firewall/clock/power), not a GIC programming problem.
  3. Read `GICR_TYPER` (`0x0C100008`, 64-bit) and `GICR_WAKER` (`0x0C100014`).
     `WAKER[2] ChildrenAsleep` must be 0; `WAKER[1] ProcessorSleep` must be 0.
  4. Read `GICR_IGROUPR0` (`0x0C100080`) bit 30 and `GICR_ISENABLER0` (`0x0C100100`) bit 30.
  5. Do the same five reads **inside the Android kernel** (a tiny module or a `devmem` read).
     Whatever differs between the two runs is your actual blocker.
* **The one thing LK proves [V]:** non-secure `GICD_CTLR.EnGrp1NS` and `ICC_IGRPEN1_EL1` writes
  are the normal, supported path on this board (LK does it every boot, and the Android kernel
  relies on the GIC not being in a weird state). If your shim cannot reproduce even that, the
  difference is in *how* you execute the write (EL, `ICC_SRE`, MMU/cache attributes, GIC clock/
  power domain), not in some hidden secure-side blessing.

---

## 3. USB device-controller bring-up for fastboot

### 3.1 Entry point and ordering

```c
/* app/mt_boot/mt_boot.c:1907-1915 */
fastboot:
	target_fastboot_init();
	if (!usb_init)
		udc_init(&surf_udc_device);

	mt_part_dump();
	sz = target_get_max_flash_size();
	fastboot_init(target_get_scratch_address(), sz);
	udc_start();
```

`udc_init()` — **PHY first, MAC second** (`platform/common/mt_musb/mt_musb.c:1664-1727`):

```c
int udc_init(struct udc_device *dev)
{
	...
	thread_sleep(20);
	...
	/* usb phy init */
	board_usb_init();
	mt_usb_phy_recover();

	thread_sleep(20);
	...
```

```c
/* platform/common/mt_musb/mt_musb.c:135-138 */
void board_usb_init(void)
{
	mt_usb_phy_poweron();
}
```

`udc_start()` — MAC programming, **no MAC reset** (`mt_musb.c:1964-2061`):

```c
int udc_start(void)
{
	...
	/* register interrupt handler */
	mt_usb_irq_init();

	/* go to RUN mode */
	mt_usb_phy_recover();

	/* clear INTRTX, INTRRX and INTRUSB */
	writew(0xffff, INTRTX); /* writew */
	writew(0xffff, INTRRX); /* writew */
	writeb(0xff, INTRUSB); /* writeb */

#ifdef SUPPORT_QMU
	usb_qmu_init_and_start();

	/* set DMA channel 0 burst mode to boost QMU speed */
	writew((readw(USB_DMA_CNTL(1)) | 0x600), USB_DMA_CNTL(1));
#endif

	/* unmask usb irq */
#ifdef USB_GINTR
	mt_irq_unmask(MT_USB0_IRQ_ID);
#endif
	writeb((INTRUSB_SUSPEND | INTRUSB_RESUME | INTRUSB_RESET |INTRUSB_DISCON), INTRUSBE);

	/* enable the pullup resistor */
	mt_usb_connect_internal();

	while (1) { ... }
}
```

`mt_usb_irq_init()` (`mt_musb.c:1618-1633`) first disables all EP interrupts
(`INTRTXE/INTRRXE/INTRUSBE <- 0`) and sets SPI 105 level/low via
`mt_irq_set_sens()` / `mt_irq_set_polarity()`.

Connect (`mt_musb.c:1636-1651`):

```c
void mt_usb_connect_internal(void)
{
	u8 tmpReg8;

	/* connect */
	tmpReg8 = readb(POWER);
	tmpReg8 |= PWR_SOFT_CONN;
	tmpReg8 |= PWR_ENABLE_SUSPENDM;

#ifdef USB_FORCE_FULL_SPEED
	tmpReg8 &= ~PWR_HS_ENAB;
#else
	tmpReg8 |= PWR_HS_ENAB;
#endif
	writeb(tmpReg8, POWER);
}
```

### 3.2 Register bases and widths **[V]**

```
platform/mt6765/include/platform/mt_reg_base.h:249   #define USB_BASE     (0x11200000)
platform/mt6765/include/platform/mt_reg_base.h:252   #define USBSIF_BASE  (0x11CC0000)
platform/common/mt_musb/mt_musb.h:41                 #define USB20_PHY_BASE (USBSIF_BASE + 0x0800)   /* = 0x11CC0800 */
```

| Reg | Symbol | Address | Width |
|---|---|---|---|
| MUSB `POWER` | `mt_musb.h:56` | `0x11200061` (`USB_BASE+0x01`) | 8-bit |
| `INTRTX` / `INTRRX` | `:57-58` | `0x11200002` / `0x11200004` | 16-bit |
| `INTRTXE` / `INTRRXE` | `:59-60` | `0x11200006` / `0x11200008` | 8-bit |
| `INTRUSB` / `INTRUSBE` | `:61-62` | `0x1120000A` / `0x1120000B` | 8-bit |
| `INDEX` | `:64` | `0x1120000E` | 8-bit |
| `SWRST` | `:199` | `0x11200074` | 8-bit |
| `USB_L1INTM` | `:265` | `0x112000A4` | 32-bit |
| PHY ACR6 | `0x18` | `0x11CC0818` | 32-bit |
| PHY ACR3 | `0x1C` | `0x11CC081C` | 32-bit |
| PHY ACR4 | `0x20` | `0x11CC0820` | 32-bit |
| PHY DTM0 | `0x68` | `0x11CC0868` | 32-bit |
| PHY DTM1 | `0x6C` | `0x11CC086C` | 32-bit |

`POWER` bits (`mt_musb.h:73-81`): `SOFT_CONN (1<<6)`, `HS_ENAB (1<<5)`, `HS_MODE (1<<4)`,
`ENABLE_SUSPENDM (1<<0)`.

PHY macros used by the driver (`platform/mt6765/mt_musb/mt_musbphy.c:112-127`) are plain
32-bit read-modify-write on `USB20_PHY_BASE + offset`.

### 3.3 PHY power-on — literal sequence (`mt_musbphy.c:234-279`)

```c
void mt_usb_phy_poweron(void)
{
	/*
	 * force_uart_en	1'b0		0x68 26
	 * RG_UART_EN		1'b0		0x6c 16
	 * rg_usb20_gpio_ctl	1'b0		0x20 09
	 * usb20_gpio_mode	1'b0		0x20 08
	 * RG_USB20_BC11_SW_EN	1'b0		0x18 23
	 * rg_usb20_dp_100k_mode 1'b1		0x20 18
	 * USB20_DP_100K_EN	1'b0		0x20 16
	 * RG_USB20_DM_100K_EN	1'b0		0x20 17
	 * RG_USB20_OTG_VBUSCMP_EN	1'b1	0x18 20
	 * force_suspendm		1'b0	0x68 18
	*/

	/* force_uart_en, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 26));
	/* RG_UART_EN, 1'b0 */
	USBPHY_CLR32(0x6c, (0x1 << 16));
	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
	USBPHY_CLR32(0x20, ((0x1 << 9) | (0x1 << 8)));

	/* RG_USB20_BC11_SW_EN, 1'b0 */
	USBPHY_CLR32(0x18, (0x1 << 23));

	/* rg_usb20_dp_100k_mode, 1'b1 */
	USBPHY_SET32(0x20, (0x1 << 18));
	/* USB20_DP_100K_EN 1'b0, RG_USB20_DM_100K_EN, 1'b0 */
	USBPHY_CLR32(0x20, ((0x1 << 16) | (0x1 << 17)));

	/* RG_USB20_OTG_VBUSCMP_EN, 1'b1 */
	USBPHY_SET32(0x18, (0x1 << 20));

	/* force_suspendm, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 18));

	/* RG_USB20_PHY_REV[7:0] = 8'b01000000 */
	USBPHY_CLR32(0x18, (0xFF << 24));
	USBPHY_SET32(0x18, (0x40 << 24));

	/* wait for 800 usec. */
	udelay(800);
```

Cross-checked against the kernel PHY header already in this workspace
(`D:\aios\research\_a60810.h`, "A60810" U2 PHY = the MT6765-class IP). The bit positions and
names match field-for-field **[V]**:

* `0x18` = `U3D_USBPHYACR6`: `RG_USB20_PHY_REV (0xff<<24)`, `RG_USB20_BC11_SW_EN (1<<23)`,
  `RG_USB20_OTG_VBUSCMP_EN (1<<20)` — `_a60810.h:129-137`.
* `0x68` = `U3D_U2PHYDTM0`: `FORCE_UART_EN (1<<26)`, `FORCE_DATAIN (1<<23)`,
  `FORCE_DM/DP_PULLDOWN (1<<21/1<<20)`, `FORCE_XCVRSEL (19)`, **`FORCE_SUSPENDM (1<<18)`**,
  `FORCE_TERMSEL (17)`, `RG_DATAIN (0xf<<10)`, `RG_DMPULLDOWN (7)`, `RG_DPPULLDOWN (6)`,
  `RG_XCVRSEL (0x3<<4)`, **`RG_SUSPENDM (1<<3)`**, `RG_TERMSEL (2)` — `_a60810.h:179-205`.
* `0x6C` = `U3D_U2PHYDTM1`: `RG_UART_EN (1<<16)`, `FORCE_VBUSVALID (13)`, `FORCE_SESSEND (12)`,
  `FORCE_BVALID (11)`, `FORCE_AVALID (10)`, `FORCE_IDDIG (9)`, `FORCE_IDPULLUP (8)`,
  `RG_VBUSVALID (5)`, `RG_SESSEND (4)`, `RG_BVALID (3)`, `RG_AVALID (2)`, `RG_IDDIG (1)`,
  `RG_IDPULLUP (0)` — `_a60810.h:207-229`.

### 3.4 PHY "recover" = the real device-mode enable (`mt_musbphy.c:351-424`)

Called **twice** (end of `udc_init()`, and again in `udc_start()`):

```c
void mt_usb_phy_recover(void)
{
	...
	/* clean PUPD_BIST_EN */
	/* PUPD_BIST_EN = 1'b0 */
	/* PMIC will use it to detect charger type */
	/* NEED?? USBPHY_CLR8(0x1d, 0x10);*/
	USBPHY_CLR32(0x1c, (0x1 << 12));

	/* force_uart_en, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 26));
	/* RG_UART_EN, 1'b0 */
	USBPHY_CLR32(0x6C, (0x1 << 16));
	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
	USBPHY_CLR32(0x20, (0x1 << 9));
	USBPHY_CLR32(0x20, (0x1 << 8));

	/* force_suspendm, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 18));

	/* RG_DPPULLDOWN, 1'b0, RG_DMPULLDOWN, 1'b0 */
	USBPHY_CLR32(0x68, ((0x1 << 6) | (0x1 << 7)));

	/* RG_XCVRSEL[1:0], 2'b00. */
	USBPHY_CLR32(0x68, (0x3 << 4));

	/* RG_TERMSEL, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 2));
	/* RG_DATAIN[3:0], 4'b0000 */
	USBPHY_CLR32(0x68, (0xF << 10));

	/* force_dp_pulldown... force_datain, 1'b0 */
	USBPHY_CLR32(0x68, ((0x1 << 20) | (0x1 << 21) | (0x1 << 19) | (0x1 << 17) | (0x1 << 23)));

	/* RG_USB20_BC11_SW_EN, 1'b0 */
	USBPHY_CLR32(0x18, (0x1 << 23));
	/* RG_USB20_OTG_VBUSCMP_EN, 1'b1 */
	USBPHY_SET32(0x18, (0x1 << 20));

	/* RG_USB20_PHY_REV[7:0] = 8'b01000000 */
	USBPHY_CLR32(0x18, (0xFF << 24));
	USBPHY_SET32(0x18, (0x40 << 24));

	/* wait 800 usec. */
	udelay(800);

	/* force enter device mode */
	USBPHY_CLR32(0x6C, (0x10<<0));
	USBPHY_SET32(0x6C, (0x2F<<0));
	USBPHY_SET32(0x6C, (0x3F<<8));
}
```

Decoded with the `_a60810.h` field names, the last three writes are the "pretend the cable is
plugged into a host" trick **[V]**:

* `DTM1 &= ~0x10` → `RG_SESSEND = 0`
* `DTM1 |= 0x2F`    → `RG_IDPULLUP=1, RG_IDDIG=1, RG_AVALID=1, RG_BVALID=1, RG_VBUSVALID=1`
* `DTM1 |= 0x3F00`  → force those six UTMI signals (`FORCE_IDPULLUP..FORCE_VBUSVALID`)

Without this, the MUSB core sees "no VBUS / session end" and will never accept the
`SOFT_CONN` pull-up. **This is the step a bare-metal shim most often forgets.** **[I]**

Charger-detect helpers, if the shim also wants BC1.1 (`mt_musbphy.c:438-448`):

```c
void Charger_Detect_Init(void)   { USBPHY_SET8(0x1a, 0x80); }   /* RG_USB20_BC11_SW_EN = 1 */
void Charger_Detect_Release(void){ USBPHY_CLR8(0x1a, 0x80); }   /* RG_USB20_BC11_SW_EN = 0 */
```

### 3.5 When the MAC *is* reset

There is no MAC reset at init. `SWRST` is written only from the bus-reset interrupt path —
`mt_udc_irq()` dispatches it on `INTRUSB_RESET` (`mt_musb.c:1520-1523`), and
`mt_udc_reset()` is the only writer of `SWRST` in the whole file (`mt_musb.c:563-633`):

```c
static void mt_udc_reset(void)
{
	/* MUSBHDRC automatically does the following when reset signal is detected */
	...
	/* disable all endpoint interrupts */
	writeb(0, INTRTXE);
	writeb(0, INTRRXE);
	writeb(0, INTRUSBE);

	writew(SWRST_SWRST | SWRST_DISUSBRESET, SWRST);      /* 0x03 -> 0x11200074 */
	...
	/* restore RXMAP and TXMAP if the endpoint has been configured */
	mt_udc_rxtxmap_recover();
	...
	/* enable suspend */
	writeb((INTRUSB_SUSPEND | INTRUSB_RESUME | INTRUSB_RESET |INTRUSB_DISCON), INTRUSBE);

	txn_status = 0;
}
```

with `SWRST_SWRST = (1<<1)`, `SWRST_DISUSBRESET = (1<<0)`, `SWRST_PHY_RST = (1<<7)`
(`mt_musb.h:235-243`). Note LK writes **`0x03`**, never `SWRST_PHY_RST`. So:

* order is **PHY power-on/recover → (enumeration) → PHY recover again → MAC interrupt programming
  → `POWER.SOFT_CONN` → host sees connect → host sends reset → `SWRST=0x03`**.
* If your shim starts from an unknown MAC state, doing `SWRST=0x03` *once before* `SOFT_CONN`
  is safe and matches what LK does on the first bus reset anyway. **[I]**

### 3.6 Clocks — the honest answer

**LK contains no USB clock-gate or PLL programming at all. [V]**
Evidence: `grep -n "clk\|clock\|CLK\|INFRA_" platform/common/mt_musb/mt_musb.c` returns only
comments and the e-fuse "clkref" calibration variable; the mt6765 platform has no USB clock
writes either (the only `CLK_CFG_*` accesses in `platform/mt6765/*.c` are display-related,
`ddp_dump.c:616-673`). LK inherits the USB clocks from the preloader.

The *only* LK code that pokes USB-adjacent wrapper/power registers is the optional USB2JTAG
feature, and only when env var `usb2jtag=1` (`platform/mt6765/mt_usb2jtag.c:43-61`):

```c
void usb2jtag_hw_init(void)
{
	dprintf(CRITICAL,"[USB2JTAG] usb2jtag_hw_init\n");

	/* enabling USB SW_EN */
	*(volatile unsigned int *) 0x11200820 |= 0xF300;
	*(volatile unsigned int *) 0x11200818 &= 0xFF7FFFFF;
	*(volatile unsigned int *) 0x11200800 |= 0x1;
	*(volatile unsigned int *) 0x11200808 &= 0xFFFDFFFF;

	udelay(100);

	*(volatile unsigned int *) 0x10001F00 |= 0x4030;

	*(volatile unsigned int *) 0x11F40800 |= 0x1;
	*(volatile unsigned int *) 0x11F40818 &= ~(1 << 23);
	*(volatile unsigned int *) 0x11F40820 |= (1 << 9);
}
```

Register *names* for those addresses are not in this tree — **treat the bit meanings as [U]**.
(They are *not* the U2 PHY; the U2 PHY is at `0x11CC0800`.)

For a shim that needs to re-enable clocks the kernel-side names are the right place to look,
not LK **[U]** (`D:\aios\research\_v_clk-mt6765.c`): `CLK_TOP_USB20_192M` /
`usb20_192m_ck` (FACTOR off `univpll`, line 489), the divider chain
`usb20_192m_d4/d8/d16/d32` (lines 490-495), `CLK_TOP_USB_TOP_SEL` mux on `CLK_CFG_5`
(lines 827-829), and the `CLK_IFR_ICUSB` gate (`GATE_IFR2(..., "axi_ck", 8)`, line 1025).

---

## 4. Does LK power the PHY down before jumping to the boot image?

**Yes — on the fastboot-controlled boot path. [V]**

```
fastboot "boot" command
  app/mt_boot/sys_commands.c:755-770
      fastboot_okay("");
      udc_stop();
      timer_cancel(&wdt_timer);
      mtk_wdt_restart();
      ...
      boot_linux((void *) boot_hdr->kernel_addr, ...);
```

```c
/* platform/common/mt_musb/mt_musb.c:2063-2071 */
int udc_stop(void)
{
	thread_sleep(10);

	mt_usb_disconnect_internal();
	mt_usb_phy_savecurrent();

	return 0;
}
```

```c
/* platform/common/mt_musb/mt_musb.c:1653-1662 */
/* Turn off the USB connection by disabling the pullup resistor */
void mt_usb_disconnect_internal(void)
{
	u8 tmpReg8;

	/* connect */
	tmpReg8 = readb(POWER);
	tmpReg8 &= ~PWR_SOFT_CONN;
	writeb(tmpReg8, POWER);
}
```

`mt_usb_phy_savecurrent()` is the PHY "power down / suspend" (`mt_musbphy.c:281-349`):

```c
void mt_usb_phy_savecurrent(void)
{
	...
	/* force_uart_en, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 26));
	/* RG_UART_EN, 1'b0 */
	USBPHY_CLR32(0x6c, (0x1 << 16));
	/* rg_usb20_gpio_ctl, 1'b0, usb20_gpio_mode, 1'b0 */
	USBPHY_CLR32(0x20, (0x1 << 9));
	USBPHY_CLR32(0x20, (0x1 << 8));

	/* RG_USB20_BC11_SW_EN, 1'b0 */
	USBPHY_CLR32(0x18, (0x1 << 23));
	/* RG_USB20_OTG_VBUSCMP_EN, 1'b0 */
	USBPHY_CLR32(0x18, (0x1 << 20));

	/* RG_SUSPENDM, 1'b1 */
	USBPHY_SET32(0x68, (0x1 << 3));
	/* force_suspendm, 1'b1 */
	USBPHY_SET32(0x68, (0x1 << 18));

	/* RG_DPPULLDOWN, 1'b1, RG_DMPULLDOWN, 1'b1 */
	USBPHY_SET32(0x68, ((0x1 << 6) | (0x1 << 7)));

	/* RG_XCVRSEL[1:0], 2'b01. */
	USBPHY_CLR32(0x68, (0x3 << 4));
	USBPHY_SET32(0x68, (0x1 << 4));
	/* RG_TERMSEL, 1'b1 */
	USBPHY_SET32(0x68, (0x1 << 2));
	/* RG_DATAIN[3:0], 4'b0000 */
	USBPHY_CLR32(0x68, (0xF << 10));

	/* force_dp_pulldown ... force_datain, 1'b1 */
	USBPHY_SET32(0x68, ((0x1 << 20) | (0x1 << 21) | (0x1 << 19) | (0x1 << 17) | (0x1 << 23)));

	udelay(800);

	/* RG_SUSPENDM, 1'b0 */
	USBPHY_CLR32(0x68, (0x1 << 3));

	udelay(1);

	return;
}
```

Semantics **[V]** via `_a60810.h`: clear `RG_SUSPENDM/force`, i.e. hold the PHY in suspend with
DP/DM pulled down, XCVRSEL=01, TERMSEL=1 (SE0), then pulse `RG_SUSPENDM=0`.

Same call sites for the other exit paths:

* `cmd_reboot` — `app/mt_boot/sys_commands.c:577-590` (`udc_stop(); ... mtk_arch_reset(...)`)
* `cmd_reboot_bootloader` — `sys_commands.c:594-601`
* `cmd_continue` — `sys_commands.c:329-342`

**What this means for reusing LK's state [V]/[I]:**

* After a *fastboot* session, the link is deliberately dead: `POWER.SOFT_CONN=0` and the PHY is
  in suspend. You cannot "inherit a live link" from that path; you must re-run
  `mt_usb_phy_poweron()` + `mt_usb_phy_recover()` (or the subset in §5) and re-set `POWER`.
* On a *normal* boot (no fastboot), **LK never touches USB at all** — no PHY writes, no MAC
  writes, no clock writes. Whatever the preloader left is what the kernel gets.
  If your shim runs *instead of* LK and the port expects "LK-like" USB state, note that in the
  normal path there is no such state to copy.

---

## 5. What to copy — minimal bare-metal shim recipe

All addresses below are 32-bit MMIO, `0x11CC0800`-based for the PHY and `0x11200000`-based for
the MAC. Verified against LK; bit names cross-checked against `_a60810.h`.

### 5.1 PHY power-on (do this first, ~1 ms)

```c
#define PHY 0x11CC0800u        /* USB20_PHY_BASE = USBSIF_BASE + 0x800 */
static inline void phy_clr(u32 off, u32 m){ writel(readl(PHY+off) & ~m, PHY+off); }
static inline void phy_set(u32 off, u32 m){ writel(readl(PHY+off) |  m, PHY+off); }

phy_clr(0x68, 1u<<26);            /* FORCE_UART_EN   = 0  */
phy_clr(0x6C, 1u<<16);            /* RG_UART_EN      = 0  */
phy_clr(0x20, (1u<<9)|(1u<<8));   /* gpio_ctl=0, gpio_mode=0 */
phy_clr(0x18, 1u<<23);            /* RG_USB20_BC11_SW_EN = 0 */
phy_set(0x20, 1u<<18);            /* rg_usb20_dp_100k_mode = 1 */
phy_clr(0x20, (1u<<16)|(1u<<17)); /* DP_100K_EN=0, DM_100K_EN=0 */
phy_set(0x18, 1u<<20);            /* RG_USB20_OTG_VBUSCMP_EN = 1 */
phy_clr(0x68, 1u<<18);            /* FORCE_SUSPENDM = 0 */
phy_clr(0x18, 0xFFu<<24);         /* RG_USB20_PHY_REV = 0x40 */
phy_set(0x18, 0x40u<<24);
udelay(800);
```

### 5.2 PHY recover / force device mode (this is what actually lights the link)

```c
phy_clr(0x1C, 1u<<12);                              /* RG_USB20_PUPD_BIST_EN = 0 */
phy_clr(0x68, 1u<<26);
phy_clr(0x6C, 1u<<16);
phy_clr(0x20, (1u<<9)|(1u<<8));
phy_clr(0x68, 1u<<18);                              /* FORCE_SUSPENDM = 0 */
phy_clr(0x68, (1u<<6)|(1u<<7));                     /* RG_DP/DM_PULLDOWN = 0 */
phy_clr(0x68, 3u<<4);                               /* RG_XCVRSEL = 00 */
phy_clr(0x68, 1u<<2);                               /* RG_TERMSEL = 0 */
phy_clr(0x68, 0xFu<<10);                            /* RG_DATAIN = 0 */
phy_clr(0x68, (1u<<20)|(1u<<21)|(1u<<19)|(1u<<17)|(1u<<23));
phy_clr(0x18, 1u<<23);                              /* BC11_SW_EN = 0 */
phy_set(0x18, 1u<<20);                              /* OTG_VBUSCMP_EN = 1 */
phy_clr(0x18, 0xFFu<<24); phy_set(0x18, 0x40u<<24); /* PHY_REV = 0x40 */
udelay(800);
phy_clr(0x6C, 0x10);                                /* RG_SESSEND = 0 */
phy_set(0x6C, 0x2F);                                /* IDPULLUP/IDDIG/AVALID/BVALID/VBUSVALID = 1 */
phy_set(0x6C, 0x3F00);                              /* force those 6 UTMI signals */
```

### 5.3 MAC connect (only after 5.1 + 5.2)

```c
#define MUSB 0x11200000u
/* optional, only if you do not know the MAC state: */
/* writeb(0x03, MUSB + 0x74);  */   /* SWRST = SWRST|DISUSBRESET (LK's bus-reset value) */
writeb(0x00, MUSB + 0x06);          /* INTRTXE  = 0 */
writeb(0x00, MUSB + 0x08);          /* INTRRXE  = 0 */
writeb(0x00, MUSB + 0x0B);          /* INTRUSBE = 0 */
writew(0xFFFF, MUSB + 0x02);        /* ack INTRTX  */
writew(0xFFFF, MUSB + 0x04);        /* ack INTRRX  */
writeb(0xFF,   MUSB + 0x0A);        /* ack INTRUSB */
/* POWER = SOFT_CONN | HS_ENAB | ENABLE_SUSPENDM = 0x61 */
writeb(readb(MUSB + 0x01) | 0x61, MUSB + 0x01);
```

To **keep** an existing link alive instead of re-enumerating: do *not* write `SWRST`, do *not*
clear `SOFT_CONN`; verify `readb(0x11200061) & 0x40` is set and that
`readl(0x11CC086C) & 0x3F00` is set. If either is 0, run 5.1 → 5.2 → 5.3.
Also confirm `readl(0x11CC0868) & (1u<<18)` (FORCE_SUSPENDM) is **0** — that bit alone kills the link.

### 5.4 Clock gates

LK has nothing to copy here **[V]**. If your shim resets/re-gates USB you must restore the
INFRA gates yourself; LK's absence of any gate write is itself evidence that on this SoC the
preloader leaves them on and nobody turns them off until the kernel's clk framework does.
If the link is dead after your shim but the PHY writes read back correctly, suspect a gate — the
kernel names in `_v_clk-mt6765.c` (`usb20_192m_ck` chain, `CLK_TOP_USB_TOP_SEL` on `CLK_CFG_5`,
`CLK_IFR_ICUSB`) are the places to look, but the preloader's exact values are **not in any
source we have [U]**.

### 5.5 GIC in the shim (short version)

What LK proves is safe from NS, and which you can copy verbatim:

```c
writel(0x00000002, 0x0C000000);          /* GICD_CTLR.EnGrp1NS (LK writes 0x16; bits 2/4 are
                                            secure-owned and ignored from NS) */
/* CP15: */
mcr p15, 0, 1,   c12, c12, 5             /* ICC_SRE = 1 */
mcr p15, 0, 0xF0,c4,  c6,  0             /* ICC_PMR = 0xF0 */
mcr p15, 0, 1,   c12, c12, 7             /* ICC_IGRPEN1 = 1 */
isb
```

What LK does **not** give you, and you must do yourself for INTID 30:

```c
/* redistributor for the booting CPU (GICR base + 0x20000 per core) */
u32 w = readl(GICR + 0x14);              /* GICR_WAKER */
w &= ~(1u<<1);  writel(w, GICR + 0x14);  /* ProcessorSleep = 0 */
while (readl(GICR + 0x14) & (1u<<2)) ;   /* wait ChildrenAsleep == 0 */
u32 g = readl(GICR + 0x80);              /* GICR_IGROUPR0 */
g |= (1u<<30);  writel(g, GICR + 0x80);  /* INTID 30 -> Group 1 NS */
writel(1u<<30, GICR + 0x100);            /* GICR_ISENABLER0: enable INTID 30 */
```
*(This block is the Arm GICv3 sequence, not LK code — marked **[I]**. LK's own redistributor
wake is `mt_gic_v3.c:184-189`, identical in substance but unconditional-free of the group step.)*

---

## 6. Explicitly unverified / guessed

| # | Claim | Status |
|---|---|---|
| 1 | From non-secure, `GICD_CTLR` bits 2 (`EnGrp1S`) and 4 (`ARE`/`ARE_S`) written by LK are ignored; only bit 1 sticks. | **[I]** Arm GICv3 architecture, not measured here. Read `GICD_CTLR` on the board to settle it. |
| 2 | `GICD_IGRPMODR` writes at `mt_gic_v3.c:232-233` are WI from non-secure (so LK assigns no group). | **[I]** Depends on `GICD_CTLR.DS`. If DS=1, IGRPMODR is RAZ/WI anyway. |
| 3 | `CPS #22` / `ICC_MSRE` path is unreachable from non-secure. | **[I]** We never ran it; it is only compiled-in for `DOWNLOAD_BOOT`/FPGA (where it may run in a different context we cannot see). |
| 4 | LK runs at EL1 vs EL2. | **[U]** Set by ATF; no evidence in-tree either way. Your EL2 measurement is compatible with everything here. |
| 5 | DEVAPC (`0x82000101`) covers or does not cover the GIC / USB ranges. | **[U]** The ATF-side register set is not in this tree. Needs a board experiment (§2.3). |
| 6 | USB clock gates: which INFRA gates the preloader leaves on for `0x11200000`/`0x11CC0000`. | **[U]** Not in LK source at all (verified absence, not verified values). |
| 7 | The exact meaning of `0x11200800/0x818/0x820`, `0x10001F00`, `0x11F40800` in `mt_usb2jtag.c`. | **[U]** Only used when `usb2jtag=1`. Do not copy blindly. |
| 8 | `_a60810.h` is the correct PHY for this exact board. | **[I]** It is a MT6765-era MTK U2 PHY header from the local kernel dump; every bit LK touches matches it exactly, which is strong but indirect confirmation. |
| 9 | `MTK_SIP_LK_DAPC_INIT` is what makes Android's GIC work but not yours. | **[U]** Plausible only if your shim runs **after** LK's `platform_sec_post_init()`; if you replace LK entirely, the call never happens and cannot be the cause. Check your boot order first. |

---

## 7. Reproduction

```powershell
cd D:\aios\research\refs
git init lk-svoboda18; cd lk-svoboda18
git remote add origin https://github.com/svoboda18/lk.git
git fetch --depth 1 origin 37ec8bcbc4361c0e267617f517d154656804c17a
git checkout FETCH_HEAD
cd ..; git clone --depth 1 https://github.com/calan152/oppo-a1k-lk.git oppo-a1k-lk
git clone --depth 1 https://github.com/msm8916-mainline/lk2nd.git lk2nd
```

Useful greps (all run at the `lk-svoboda18` root):

```powershell
# every GIC register the tree touches
Select-String -Recurse -Include *.c,*.h -Pattern 'GIC_DIST_|GIC_REDIS_|GICR_|ICC_'
# proof that IGROUPR is never written and PPIs are never enabled
Select-String -Recurse -Include *.c,*.h -Pattern 'IGROUPR|GIC_PRIVATE_SIGNALS|GICR_ISENABLER'
# every SMC and its ID
Select-String -Recurse -Include *.c,*.h,*.S -Pattern 'mt_secure_call|0x8200|smc #'
# USB: PHY and MAC accesses
Select-String -Recurse -Include *.c,*.h -Pattern 'USB20_PHY_BASE|USBPHY_(SET|CLR)32|SWRST|POWER'
```

### Note on the OPPO A1K port (`_lk.patch`, project 18540, MT6762/MT6765)

The port is a patch against the tree above and **does not modify any GIC code** (no `dev/gic/*`,
no `platform/mt6765/interrupts.c` in its file list), i.e. it boots Android with exactly the GIC
behaviour documented in §1. Its only USB change is a fastboot ZLP fix **[V]**:

```diff
--- a/platform/common/mt_musb/mt_musb.c
+++ b/platform/common/mt_musb/mt_musb.c
@@ -1893,7 +1893,9 @@ int udc_request_queue(struct udc_endpoint *ept, struct udc_request *req)
 #ifdef SUPPORT_QMU /* For QMU, we don't call mt_ep_write() */
-		mu3d_hal_insert_transfer_gpd(ept->num, USB_DIR_IN, ept->tx_urb->buffer, req->length, true, true, false, (ept->type == USB_EP_XFER_ISO ? 0 : 1), ept->maxpkt);
+		/* PC-side fastboot reads an exact DATA length; a trailing ZLP is
+		 * interpreted as an empty status packet before the following OKAY. */
+		mu3d_hal_insert_transfer_gpd(ept->num, USB_DIR_IN, ept->tx_urb->buffer, req->length, true, true, false, 0, ept->maxpkt);
```

That is the same QMU parameter your shim must get right if it ever speaks fastboot over this
controller: **`zlp = 0` for IN data** (LK's default passes `1` for non-ISO transfers).
