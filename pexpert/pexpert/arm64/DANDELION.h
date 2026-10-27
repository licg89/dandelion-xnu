/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 yuma shimizu
 *
 * Xiaomi Redmi 9A (dandelion / blossom), MediaTek MT6762G / Cortex-A53.
 *
 * This is the board header XNU includes from pexpert/pexpert/arm64/board_config.h
 * for ARM64_BOARD_CONFIG_DANDELION.  It follows the shape of Apple's
 * BCM2837.h, the other Cortex-A53 (ARMv8.0-A, 4 KiB granule) reference board.
 */
#ifndef _PEXPERT_ARM64_DANDELION_H
#define _PEXPERT_ARM64_DANDELION_H

/*
 * Platform feature selection.
 */
#define DANDELION
#define DANDELION_BRINGUP

#define NO_MONITOR                1
#define NO_ECORE                  1
#define ARM_ARCH_TIMER            1

#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_DEBUG__             7

/*
 * The Cortex-A53 in MT6762G reports ID_AA64MMFR0_EL1.TGran16 == 0: only the
 * 4 KiB and 64 KiB granules exist.  __ARM_16K_PG__ must stay undefined (that is
 * what selects ARM_PGSHIFT 12 in osfmk/arm64/proc_reg.h); see
 * docs/MMU_GRANULE.md for the device evidence.
 */
#define __ARM64_PMAP_SUBPAGE_L1__ 1

#ifndef ASSEMBLER

/*
 * Early console.  The MT6577-compatible UART is also what the boot shim uses,
 * so the first XNU output can be compared byte for byte with the shim's.
 */
#define DANDELION_UART

#define DANDELION_UART_BASE       0x11002000ULL
#define DANDELION_UART_BAUD       921600
#define DANDELION_UART_COMPAT     "mediatek,mt6577-uart"

/* GICv3, from mt6765.dtsi in the device tree:
 *   reg = <0x0c000000 0x40000>,   GICD
 *         <0x0c100000 0x200000>,  GICR
 */
#define DANDELION_GIC_BASE        0x0C000000ULL
#define DANDELION_GIC_SIZE        0x00040000ULL
#define DANDELION_GICR_BASE       0x0C100000ULL
#define DANDELION_GICR_SIZE       0x00200000ULL
#define DANDELION_GICR_STRIDE     0x00020000ULL
#define DANDELION_GICR_STRIDE_V4  0x00040000ULL

/*
 * The distributor frame is 64 KiB, so its peripheral ID registers live at
 * 0xFFE8, not at the 0xFE8 a 4 KiB GICv2 frame uses.  Reading 0xFE8 on this
 * part returns 0, which used to be mistaken for "not a GICv3" and disabled
 * the whole CPU-interface setup.  dandelion_gic.h handles both layouts.
 */
#define DANDELION_GICD_PIDR2_V3   0x0000FFE8u
#define DANDELION_GICD_PIDR2_V2   0x00000FE8u

/* Only used if the controller turns out to be a GICv2 (GIC-400 style). */
#define DANDELION_GICC_BASE       0x0C002000ULL

/*
 * GICv3 with system-register CPU interface.  This switches pexpert/arm/pe_fiq.c
 * from a stub to the real GICv3 bring-up (redistributor wake, PPI 27 for the
 * virtual timer, ICC_*_EL1 programming).
 */
#define HAS_GIC_V3                1

/*
 * GICv3 register definitions, see the GICv3 architecture specification
 * (Arm IHI 0069).  Apple keeps these in the board headers of the platforms
 * that use a GICv3 (VMAPPLE.h); pe_fiq.c expects them from ours.
 */
#define GIC_SPURIOUS_IRQ                        1023

#define GICR_PE_SIZE                            0x20000 /* redistributor region size */

#define GICD_CTLR                               0x0
#define GICD_CTLR_ENABLEGRP0                    0x1

#define GICR_TYPER                              0x08
#define GICR_WAKER                              0x14
#define GICR_IGROUPR0                           0x10080
#define GICR_ISENABLER0                         0x10100

#define GICR_TYPER_AFFINITY_VALUE_SHIFT         32
#define GICR_TYPER_LAST                         0x10

#define GICR_WAKER_PROCESSORSLEEP               0x2
#define GICR_WAKER_CHILDRENASLEEP               0x4

#define ICC_CTLR_EOIMODE                        0x1
#define ICC_SRE_SRE                             0x1

/* ARM generic timer: clock-frequency = <13000000>, GIC PPI 11. */
#define DANDELION_TIMER_FREQ      13000000
#define DANDELION_TIMER_IRQ       27

/* MediaTek TOPRGU watchdog. */
#define DANDELION_WDT_BASE        0x10007000ULL

/* TRNG. */
#define DANDELION_TRNG_BASE       0x1020F000ULL

/* Simple framebuffer handed over by the boot shim. */
#define DANDELION_FB_BASE         0x7EC50000ULL
#define DANDELION_FB_STRIDE       2944
#define DANDELION_FB_WIDTH        736
#define DANDELION_FB_HEIGHT       1600
#define DANDELION_FB_BPP          32

/* CPU configuration documented for board support code. */
#define DANDELION_NCPUS           8
#define DANDELION_NCLUSTERS       2
#define DANDELION_CPUS_PER_CLUSTER 4

/*
 * eMMC log channel.
 *
 * The panel and the ramoops ring both die with the machine, and this kernel
 * has a habit of resetting without saying anything.  The boot shim stages
 * MSDC0 (it is the only stage that can still inherit LK's card state) and
 * publishes the LBA of a free run inside the unused `flashinfo` partition
 * here; the kernel then streams its console output there with a polled PIO
 * writer.  A kernel that dies silently leaves its log on the medium.
 *
 * Hand-off record layout is fixed and lives in plain DRAM:
 */
struct dandelion_emmc_handoff {
	uint32_t magic;         /* DANDELION_EMMC_HANDOFF_MAGIC when valid */
	uint32_t ok;            /* 1: controller staged, region verified    */
	uint32_t lba;           /* first sector of the log block            */
	uint32_t sectors;       /* sectors reserved                         */
	uint32_t clock_div;     /* CKDIV the shim left programmed           */
	uint32_t reserved;
};

#define DANDELION_EMMC_HANDOFF_PA    0x5FD00000ULL
#define DANDELION_EMMC_HANDOFF_MAGIC 0x444D4844U  /* "DHMD" */

/* MSDC0, the eMMC controller (stock DTB /msdc@11230000). */
#define DANDELION_MSDC_BASE       0x11230000ULL
#define DANDELION_MSDC_SIZE       0x00010000ULL

/*
 * Where the kernel sees MSDC0.  osfmk/arm64/start.s installs this block
 * mapping in the V=P bootstrap tables as Device-nGnRnE, which is what lets the
 * log channel come up at the first printf instead of waiting for ml_io_map()
 * (which needs the VM, and therefore loses everything printed before it).
 * The two constants must stay in step.
 */
#define DANDELION_MSDC_VA         0x41000000ULL

/*
 * Where the kernel's block sits inside flashinfo, and how much of it is
 * reserved.  The shim writes its own log at +1 MiB in a slot it finds by
 * scanning; +2 MiB is far enough that the two can never overlap.
 */
#define DANDELION_EMMC_KLOG_OFFSET  4096U   /* +2 MiB, in 512 B sectors */
#define DANDELION_EMMC_KLOG_SECTORS 1024U   /* 512 KiB                  */

#define DANDELION_CHIP_ID         0x67620001
#define DANDELION_COMPATIBLE      "xiaomi,dandelion\0mediatek,mt6762g"

/*
 * Interrupt controller state owned by pexpert/arm/pe_fiq.c and queried by the
 * exception handlers in osfmk/arm64/sleh.c.  The GIC is driven through the
 * shared code in dandelion_gic.h, which picks Group 0 (FIQ) or Group 1 (IRQ)
 * from GICD_TYPER.SecurityExtn; these are the only entry points the handlers
 * need.
 */
uint32_t dandelion_gic_ack(void);
void     dandelion_gic_eoi(uint32_t intid);
int      dandelion_gic_uses_group0(void);

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM64_DANDELION_H */
