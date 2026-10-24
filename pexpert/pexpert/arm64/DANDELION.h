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
#define DANDELION_GICR_BASE       0x0C100000ULL
#define DANDELION_GICR_STRIDE     0x00020000ULL
#define DANDELION_GICR_STRIDE_V4  0x00040000ULL

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

#define DANDELION_CHIP_ID         0x67620001
#define DANDELION_COMPATIBLE      "xiaomi,dandelion\0mediatek,mt6762g"

#endif /* ! ASSEMBLER */

#endif /* ! _PEXPERT_ARM64_DANDELION_H */
