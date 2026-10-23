/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 yuma shimizu
 *
 * Xiaomi Redmi 9A (dandelion / blossom), MediaTek MT6762G / Cortex-A53.
 *
 * Derived from pexpert/pexpert/arm64/A04BR3.h in
 * mouseos/xnu_kernel_benesse_a04br3 (Benesse TAB-A04-BR3, MT8167), and
 * extended with the register addresses used by
 * iokit/Kernel/arm/DandelionPlatformExpert.cpp.
 *
 * This is the header XNU's pexpert/pexpert/arm64/board_config.h includes for
 * ARM64_BOARD_CONFIG_DANDELION; see docs/XNU_INTEGRATION.md.
 */
#ifndef _PEXPERT_ARM64_DANDELION_H
#define _PEXPERT_ARM64_DANDELION_H

/*
 * XNU platform feature selection.
 */
#define DANDELION                 1
#define NO_MONITOR                1
#define NO_ECORE                  1
#define ARM_ARCH_TIMER            1
#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7
#define __ARM_16K_PG__            1
#define __ARM64_PMAP_SUBPAGE_L1__ 1

/* This table shape exposes 39 VA bits.  The macOS 2-TiB layout cannot fit. */
#if defined(ARM_LARGE_MEMORY)
#error DANDELION requires the normal ARM64 virtual address layout
#endif
#if defined(VM_KERNEL_LINK_ADDRESS) && (VM_KERNEL_LINK_ADDRESS < 0xfffffff800000000ULL)
#error DANDELION kernel link address is outside the 39-bit TTBR1 range
#endif

/*
 * No PAN, LSE or PAC (ARMv8.0-A), and no Apple AIC, Apple SMP or Apple
 * implementation registers.  Interrupts are handled by the ARM GICv3 and
 * the CPU bring-up uses the generic PSCI/ARM64 path.
 */
#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif

/* CPU Configuration */
#define DANDELION_CPU_TYPE      CPU_TYPE_ARM64
#define DANDELION_CPU_SUBTYPE   CPU_SUBTYPE_ARM64_V8
#define DANDELION_NCPUS         8
#define DANDELION_NCLUSTERS     2
#define DANDELION_CPUS_PER_CLUSTER 4

/* Memory Configuration */
#define DANDELION_PHYS_BASE     0x40000000ULL
#define DANDELION_MEM_SIZE      0x100000000ULL  /* 4GB */
#define DANDELION_XNU_BASE      0x60000000ULL
#define DANDELION_XNU_SIZE      0x18000000ULL   /* 384MB managed window */

/* Virtual Address Space */
#define DANDELION_T1SZ          25  /* 39-bit VA space */
#define DANDELION_VA_BITS       39

/* Page Size */
#define DANDELION_PAGE_SIZE     0x4000  /* 16KB pages */

/* Cache Line Size */
#define DANDELION_CACHE_LINE    64

/* Interrupt Controller (GICv3)
 * Distributor and redistributor layout from the mainline mt6765.dtsi used by
 * mt6762g-xiaomi-dandelion.dts:
 *   reg = <0x0c000000 0x40000>,   GICD
 *         <0x0c100000 0x200000>,  GICR
 *         <0x0c400000 ...>        GICC/GICH/GICV (unused: we use ICC_*_EL1)
 */
#define DANDELION_GIC_BASE      0x0C000000ULL   /* distributor */
#define DANDELION_GICR_BASE     0x0C100000ULL   /* redistributor, CPU 0 */
#define DANDELION_GICR_STRIDE   0x00020000ULL   /* GICv3: RD + SGI frames */
#define DANDELION_GICR_STRIDE_V4 0x00040000ULL  /* GICv4: + VLPI + reserved */
#define DANDELION_GIC_TYPE      GIC_V3

/* Timer (mt6765.dtsi: clock-frequency = <13000000>, GIC_PPI 11) */
#define DANDELION_TIMER_FREQ    13000000  /* 13 MHz */
#define DANDELION_TIMER_IRQ     27  /* virtual timer INTID (GIC PPI 11) */

/* UART (Early Console) */
#define DANDELION_UART_BASE     0x11002000ULL
#define DANDELION_UART_BAUD     921600
#define DANDELION_UART_COMPAT   "mediatek,mt6577-uart"

/* Watchdog (MediaTek TOPRGU) */
#define DANDELION_WDT_BASE      0x10007000ULL

/* TRNG */
#define DANDELION_TRNG_BASE     0x1020F000ULL

/* Display */
#define DANDELION_FB_BASE       0x7EC50000ULL
#define DANDELION_FB_STRIDE     2944
#define DANDELION_FB_WIDTH      736   /* 720 visible + 16 padding */
#define DANDELION_FB_HEIGHT     1600
#define DANDELION_FB_BPP        32

/* Boot Arguments */
#define DANDELION_CMDLINE       "-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress"

/* Chip ID (development namespace, not Apple ECID) */
#define DANDELION_CHIP_ID       0x67620001
#define DANDELION_COMPATIBLE    "xiaomi,dandelion\0mediatek,mt6762g"

#endif /* _PEXPERT_ARM64_DANDELION_H */
