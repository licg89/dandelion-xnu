/*
 * Dandelion (Redmi 9A / MT6762G) platform expert definitions
 * Based on mouseos A04BR3, adapted for MT6762G
 */
#ifndef __PE_DANDELION_H__
#define __PE_DANDELION_H__

/* CPU Configuration */
#define DANDELION_CPU_TYPE      ARM_CPU_TYPE_CORTEX_A53
#define DANDELION_CPU_SUBTYPE   ARM_CPU_SUBTYPE_V8
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

/* Interrupt Controller */
#define DANDELION_GIC_BASE      0x0C000000ULL
#define DANDELION_GIC_TYPE      GIC_V3

/* Timer */
#define DANDELION_TIMER_FREQ    13000000  /* 13 MHz */
#define DANDELION_TIMER_IRQ     27  /* Virtual timer PPI */

/* UART (Early Console) */
#define DANDELION_UART_BASE     0x11002000ULL
#define DANDELION_UART_BAUD     921600
#define DANDELION_UART_COMPAT   "mediatek,mt6577-uart"

/* Watchdog */
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

#endif /* __PE_DANDELION_H__ */
