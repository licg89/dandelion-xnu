/* SPDX-License-Identifier: MIT
 *
 * dandelion_gic.h -- GIC identification and bring-up shared by the boot shim
 * and by XNU's pexpert/arm/pe_fiq.c.
 *
 * Why this file exists
 * --------------------
 * MT6762G is a GICv3 part, but the interrupt controller can be running in
 * either of two very different ways, and the difference decides which
 * exception (IRQ or FIQ) the kernel's timer arrives on:
 *
 *   single security state (GICD_TYPER.SecurityExtn == 0)
 *       Group 0 is not security-protected, so a non-secure EL1 kernel may use
 *       the Group 0 / FIQ path that Apple's platform code assumes.
 *
 *   two security states (GICD_TYPER.SecurityExtn == 1)
 *       Group 0 belongs to EL3.  Non-secure EL1 can only touch Group 1, i.e.
 *       IRQ.  Programming ICC_IGRPEN0_EL1 / ICC_IAR0_EL1 there is at best a
 *       no-op, which is a silent way to run a kernel with no timer ticks.
 *
 * The old bring-up probed GICD_PIDR2 at offset 0xFE8, which is correct for a
 * 4 KiB GICv2 distributor frame but is reserved space on the 64 KiB GICv3
 * frame this SoC declares in its device tree (reg size 0x40000).  It read 0,
 * concluded "not GICv3" and skipped the whole CPU-interface setup.
 *
 * This header is freestanding: pointer-sized integers, inline asm for the
 * system registers, and no kernel or libc headers, so the same code can be
 * compiled into the shim (bare metal, MMU off, physical addresses) and into
 * XNU (mapped kernel virtual addresses).
 */
#ifndef _PEXPERT_ARM64_DANDELION_GIC_H
#define _PEXPERT_ARM64_DANDELION_GIC_H

/*
 * Step trace.
 *
 * This header is shared with the boot shim, which has no console, so the
 * default is a no-op.  pexpert/arm/pe_fiq.c redefines it to kprintf before
 * including the header, which turns the CPU-interface bring-up into a step by
 * step record: on this SoC one of the ICC_* accesses below stops the machine
 * dead with no output at all, and the only way to find out which one is to
 * announce each one before it happens.
 */
#ifndef DGIC_TRACE
#define DGIC_TRACE(step) do { } while (0)
#endif

/* ---------------------------------------------------------------------- */
/* Distributor (GICD)                                                      */
/* ---------------------------------------------------------------------- */
#define DGICD_CTLR              0x0000u
#define DGICD_TYPER             0x0004u
#define DGICD_IIDR              0x0008u
#define DGICD_IGROUPR           0x0080u
#define DGICD_ISENABLER         0x0100u
#define DGICD_ICENABLER         0x0180u
#define DGICD_ISPENDR           0x0200u
#define DGICD_ICPENDR           0x0280u
#define DGICD_IPRIORITYR        0x0400u
#define DGICD_ITARGETSR         0x0800u
#define DGICD_ICFGR             0x0c00u
#define DGICD_SGIR              0x0f00u
#define DGICD_IROUTER           0x6000u      /* GICv3, 64-bit per SPI */
#define DGICD_PIDR2_4K          0x0fe8u
#define DGICD_PIDR2_64K         0xffe8u

/* GICD_TYPER fields */
#define DGICD_TYPER_LINES_MASK  0x1fu
#define DGICD_TYPER_CPUS_SHIFT  5
#define DGICD_TYPER_CPUS_MASK   0x7u
#define DGICD_TYPER_SECEXT      (1u << 10)      /* 1 = two security states */

/* GICv3, non-secure view: bit0 = EnableGrp1NS, bit1 = EnableGrp1A,
 * bit4 = ARE_NS.  GICv2: bit0 = EnableGrp0, bit1 = EnableGrp1. */
#define DGICD_CTLR_ENABLE0      (1u << 0)
#define DGICD_CTLR_ENABLE1      (1u << 1)
#define DGICD_CTLR_ARE_NS       (1u << 4)
#define DGICD_CTLR_DS           (1u << 6)
#define DGICD_CTLR_RWP          (1u << 31)

/* ---------------------------------------------------------------------- */
/* Redistributor (GICv3, 128 KiB per PE: RD_base then SGI_base)            */
/* ---------------------------------------------------------------------- */
#define DGICR_TYPER             0x0008u
#define DGICR_WAKER             0x0014u
#define DGICR_TYPER_LAST        (1ull << 4)
#define DGICR_TYPER_VLPIS       (1ull << 1)
#define DGICR_WAKER_PROCESSORSLEEP  (1u << 1)
#define DGICR_WAKER_CHILDRENASLEEP  (1u << 2)

#define DGICR_FRAME_V3          0x20000ull      /* RD + SGI frames */
#define DGICR_FRAME_V4          0x40000ull      /* + VLPI frame     */

#define DGICR_SGI_BASE          0x10000u
#define DGICR_IGROUPR0          (DGICR_SGI_BASE + 0x0080u)
#define DGICR_ISENABLER0        (DGICR_SGI_BASE + 0x0100u)
#define DGICR_ICENABLER0        (DGICR_SGI_BASE + 0x0180u)
#define DGICR_ISPENDR0          (DGICR_SGI_BASE + 0x0200u)
#define DGICR_ICPENDR0          (DGICR_SGI_BASE + 0x0280u)
#define DGICR_IPRIORITYR        (DGICR_SGI_BASE + 0x0400u)
#define DGICR_ICFGR0            (DGICR_SGI_BASE + 0x0c00u)
#define DGICR_ICFGR1            (DGICR_SGI_BASE + 0x0c04u)

/* ---------------------------------------------------------------------- */
/* CPU interface, GICv2 memory-mapped (GICC)                               */
/* ---------------------------------------------------------------------- */
#define DGICC_CTLR              0x0000u
#define DGICC_PMR               0x0004u
#define DGICC_BPR               0x0008u
#define DGICC_IAR               0x000cu
#define DGICC_EOIR              0x0010u
#define DGICC_RPR               0x0014u
#define DGICC_HPPIR             0x0018u
#define DGICC_IIDR              0x00fcu

#define DGICC_CTLR_ENABLE0      (1u << 0)
#define DGICC_CTLR_ENABLE1      (1u << 1)

/*
 * GICv2 has no system registers and no device-tree way to say "which group
 * bit", so spell it out: in the *non-secure* view of a GICv2 distributor and
 * CPU interface, bit 0 enables Group 1 (the only group non-secure software
 * can receive).  Bit 1 is reserved in that view, which is why writing 0x2
 * there looks like it worked and delivers nothing.
 */
#define DGICV2_CTLR_ENABLE_GRP1 (1u << 0)

/* Candidate GICC offsets relative to the distributor.  GIC-400 puts it at
 * +0x2000; QEMU's virt machine puts it 64 KiB above the distributor. */
#define DGICC_OFFSET_GIC400     0x2000ull
#define DGICC_OFFSET_QEMU       0x10000ull

/* INTIDs 1020..1023 are special and must not be EOIed as normal IRQs. */
#define DGIC_INTID_SPURIOUS     1020u

/* ---------------------------------------------------------------------- */
/* Modes and results                                                       */
/* ---------------------------------------------------------------------- */
#define DGIC_MODE_UNKNOWN       0u
#define DGIC_MODE_V2            2u
#define DGIC_MODE_V3            3u

#define DGIC_GROUP0             0u      /* FIQ on Apple-style platforms */
#define DGIC_GROUP1             1u      /* IRQ, the only group non-secure EL1
                                         * may use on a two-state GIC */

#define DGIC_OK                 0
#define DGIC_ERR_DEAD          (-1)     /* neither frame answers          */
#define DGIC_ERR_NO_REDIST     (-2)     /* no redistributor for this PE   */
#define DGIC_ERR_WAKE          (-3)     /* redistributor stayed asleep    */
#define DGIC_ERR_NO_CPUIF      (-4)     /* no usable CPU interface        */
#define DGIC_ERR_SRE           (-5)     /* system register interface off  */

/* 32-bit priority allocation: all equal, mid-scale. */
#define DGIC_PRIORITY_DEFAULT   0xa0u

struct dgic {
	unsigned long gicd;             /* distributor, CPU address       */
	unsigned long gicd_size;        /* declared distributor size      */
	unsigned long gicr;             /* redistributors (v3)            */
	unsigned long gicr_size;
	unsigned long gicc;             /* GICv2 CPU interface, 0 if v3   */
	unsigned int  mode;             /* DGIC_MODE_*                    */
	unsigned int  ctlr;             /* GICD_CTLR readback             */
	unsigned int  typer;            /* GICD_TYPER                     */
	unsigned int  iidr;             /* GICD_IIDR                      */
	unsigned int  pidr2_4k;         /* GICD_PIDR2 at 0xFE8            */
	unsigned int  pidr2_64k;        /* GICD_PIDR2 at 0xFFE8 (0 if <64K)*/
	unsigned int  arch_rev;         /* PIDR2[7:4] of the frame in use */
	unsigned int  secure_extn;      /* GICD_TYPER.SecurityExtn        */
	unsigned int  group;            /* DGIC_GROUP0 / DGIC_GROUP1      */
	unsigned long redist;           /* this PE's redistributor (v3)   */
	unsigned int  sre_el2_in;
	unsigned int  sre_el2_out;
	unsigned long icc_sre_el1;
	unsigned long icc_iidr;
	unsigned long icc_ctlr;
	unsigned long icc_pmr;
	unsigned long icc_igrpen0;
	unsigned long icc_igrpen1;
};

#if defined(__aarch64__)

static inline unsigned int
dgic_read32(unsigned long base, unsigned int offset)
{
	return *(volatile unsigned int *)(base + offset);
}

static inline void
dgic_write32(unsigned long base, unsigned int offset, unsigned int value)
{
	*(volatile unsigned int *)(base + offset) = value;
}

static inline unsigned long
dgic_read64(unsigned long base, unsigned int offset)
{
	return *(volatile unsigned long *)(base + offset);
}

static inline void
dgic_write64(unsigned long base, unsigned int offset, unsigned long value)
{
	*(volatile unsigned long *)(base + offset) = value;
}

static inline unsigned long
dgic_rsr_mpidr(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	return v;
}

static inline unsigned long
dgic_cur_el(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, currentel" : "=r"(v));
	return v;
}

static inline unsigned long
dgic_cntvct(void)
{
	unsigned long v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
	return v;
}

static inline unsigned long
dgic_cntfrq(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline void
dgic_isb(void)
{
	__asm__ volatile("isb" : : : "memory");
}

/* ICC_SRE_EL1 access is safe once the CPU interface has SRE set; on a GICv2
 * these registers are UNDEFINED and must never be touched. */
static inline unsigned long
dgic_icc_sre_el1_read(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(v));
	return v;
}

static inline void
dgic_icc_sre_el1_write(unsigned long v)
{
	__asm__ volatile("msr ICC_SRE_EL1, %0" : : "r"(v) : "memory");
}

static inline unsigned long
dgic_icc_sre_el2_read(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, ICC_SRE_EL2" : "=r"(v));
	return v;
}

static inline void
dgic_icc_sre_el2_write(unsigned long v)
{
	__asm__ volatile("msr ICC_SRE_EL2, %0" : : "r"(v) : "memory");
}

static inline unsigned long
dgic_icc_iidr_read(void)
{
	unsigned long v;
	/* ICC_IIDR_EL1 has no symbolic name in clang's assembler. */
	__asm__ volatile("mrs %0, S3_0_C12_C12_2" : "=r"(v));
	return v;
}

static inline unsigned long
dgic_icc_ctlr_read(void)
{
	unsigned long v;
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(v));
	return v;
}
#endif /* __aarch64__ */

/* ---------------------------------------------------------------------- */
/* Identification                                                          */
/* ---------------------------------------------------------------------- */

/*
 * Fill in `g` from the distributor / redistributor (or GICC) regions the
 * caller mapped.  Never touches ICC_* registers: whether those are
 * implemented is exactly what we are trying to find out.
 *
 * `v3_hint` comes from the device tree compatible string: 1 for
 * "arm,gic-v3", 0 for a GICv2 compatible, -1 when there is no device tree
 * node.  It decides which peripheral-ID offset can be read safely: a GIC-400
 * distributor frame is 4 KiB, so reading 0xFFE8 there leaves the implemented
 * page and can take a data abort.  With no hint, only the 4 KiB offset is
 * read unless the declared frame is at least 128 KiB.
 */
static inline int
dgic_identify(struct dgic *g, unsigned long gicd, unsigned long gicd_size,
    unsigned long gicr, unsigned long gicr_size, int v3_hint)
{
	unsigned int rev4k = 0, rev64k = 0;
	int probe64k = (v3_hint > 0) ||
	    (v3_hint < 0 && gicd_size >= 0x20000ul);

	g->gicd = gicd;
	g->gicd_size = gicd_size;
	g->gicr = gicr;
	g->gicr_size = gicr_size;
	g->gicc = 0;
	g->redist = 0;
	g->ctlr = dgic_read32(gicd, DGICD_CTLR);
	g->typer = dgic_read32(gicd, DGICD_TYPER);
	g->iidr = dgic_read32(gicd, DGICD_IIDR);
	g->pidr2_4k = dgic_read32(gicd, DGICD_PIDR2_4K);
	g->pidr2_64k = probe64k ? dgic_read32(gicd, DGICD_PIDR2_64K) : 0;
	g->secure_extn = (g->typer & DGICD_TYPER_SECEXT) ? 1u : 0u;

	rev4k = (g->pidr2_4k >> 4) & 0xfu;
	rev64k = (g->pidr2_64k >> 4) & 0xfu;

	/*
	 * Prefer the frame size the id registers actually report.  A GICv3
	 * distributor answers at 0xFFE8, a GIC-400 at 0xFE8.
	 */
	if (rev64k >= 3) {
		g->mode = DGIC_MODE_V3;
		g->arch_rev = rev64k;
	} else if (rev4k >= 3) {
		g->mode = DGIC_MODE_V3;
		g->arch_rev = rev4k;
	} else if (rev4k == 2) {
		g->mode = DGIC_MODE_V2;
		g->arch_rev = rev4k;
	} else if (rev64k == 2) {
		g->mode = DGIC_MODE_V2;
		g->arch_rev = rev64k;
	} else if (g->typer == 0 && g->iidr == 0 && g->pidr2_4k == 0 &&
	    g->pidr2_64k == 0) {
		g->mode = DGIC_MODE_UNKNOWN;
		g->arch_rev = 0;
		return DGIC_ERR_DEAD;
	} else if (v3_hint > 0) {
		/* The node says gic-v3 and the frame is 64 KiB: trust it. */
		g->mode = DGIC_MODE_V3;
		g->arch_rev = rev64k;
	} else {
		/* The device tree says one thing and the ID registers another;
		 * keep the declared layout and let the caller print the raw
		 * values. */
		g->mode = (v3_hint == 0) ? DGIC_MODE_V2 : DGIC_MODE_UNKNOWN;
		g->arch_rev = 0;
	}

	/*
	 * Group selection.  Two security states means Group 0 belongs to EL3
	 * and only Group 1 (IRQ) is reachable from non-secure EL1.  A single
	 * security state leaves Group 0 usable, which is what Apple's FIQ
	 * based timer path expects.  GICv2 has no single-security-state mode,
	 * so Group 1 is the only option there.
	 */
	if (g->mode == DGIC_MODE_V2) {
		g->group = DGIC_GROUP1;
	} else {
		g->group = g->secure_extn ? DGIC_GROUP1 : DGIC_GROUP0;
	}

	if (g->mode == DGIC_MODE_V2) {
		/*
		 * Find the memory-mapped CPU interface.  The device tree's
		 * second region is the GICC on a GICv2 node; GIC-400 places it
		 * 8 KiB above the distributor, and QEMU's virt machine puts it
		 * 64 KiB above.  A plausible GICC_IIDR identifies it.
		 */
		unsigned long candidates[3];
		candidates[0] = gicr;
		candidates[1] = gicd + DGICC_OFFSET_GIC400;
		candidates[2] = gicd + DGICC_OFFSET_QEMU;

		for (unsigned i = 0; i < 3; i++) {
			unsigned int iidr;
			if (candidates[i] == 0) {
				continue;
			}
			iidr = dgic_read32(candidates[i], DGICC_IIDR);
			if (iidr != 0 && iidr != 0xffffffffu) {
				g->gicc = candidates[i];
				g->gicr = 0;    /* not a redistributor */
				break;
			}
		}
		return g->gicc ? DGIC_OK : DGIC_ERR_NO_CPUIF;
	}

	return DGIC_OK;
}

/* Size of one redistributor frame, from its GICR_TYPER.VLPIS bit. */
static inline unsigned long
dgic_redist_stride(unsigned long frame)
{
	unsigned long typer = dgic_read64(frame, DGICR_TYPER);
	return (typer & DGICR_TYPER_VLPIS) ? DGICR_FRAME_V4 : DGICR_FRAME_V3;
}

/*
 * Walk the redistributor region until the frame whose GICR_TYPER affinity
 * matches this PE's MPIDR is found.  Returns 0 when nothing matched.
 */
static inline unsigned long
dgic_find_redist(struct dgic *g)
{
	unsigned long mpidr = dgic_rsr_mpidr();
	unsigned long want = ((mpidr & 0xfful) << 32) |
	    (((mpidr >> 8) & 0xfful) << 40) |
	    (((mpidr >> 16) & 0xfful) << 48) |
	    (((mpidr >> 32) & 0xfful) << 56);
	unsigned long frame = g->gicr;
	unsigned long end = g->gicr + g->gicr_size;
	unsigned int guard = 0;

	if (g->gicr == 0 || g->gicr_size == 0) {
		return 0;
	}

	while (frame + DGICR_FRAME_V3 <= end && guard++ < 256) {
		unsigned long typer = dgic_read64(frame, DGICR_TYPER);
		if ((typer & 0xffffffff00000000ull) == want) {
			return frame;
		}
		if (typer & DGICR_TYPER_LAST) {
			break;
		}
		frame += dgic_redist_stride(frame);
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Bring-up                                                                */
/* ---------------------------------------------------------------------- */

static inline void
dgic_wait_rwp(unsigned long gicd)
{
	for (unsigned int spin = 0; spin < 1000000u; spin++) {
		if ((dgic_read32(gicd, DGICD_CTLR) & DGICD_CTLR_RWP) == 0) {
			return;
		}
	}
}

/* Set one byte inside a 32-bit priority register. */
static inline void
dgic_set_priority32(unsigned long base, unsigned int offset, unsigned int irq,
    unsigned int priority)
{
	unsigned int index = offset + (irq / 4u) * 4u;
	unsigned int shift = (irq % 4u) * 8u;
	unsigned int value = dgic_read32(base, index);

	value &= ~(0xffu << shift);
	value |= (priority & 0xffu) << shift;
	dgic_write32(base, index, value);
}

/* Also give every interrupt in a 32-bit group a sane priority. */
static inline void
dgic_fill_priorities(unsigned long base, unsigned int offset, unsigned int words,
    unsigned int priority)
{
	unsigned int word = (priority & 0xffu) * 0x01010101u;
	for (unsigned int i = 0; i < words; i++) {
		dgic_write32(base, offset + i * 4u, word);
	}
}

/*
 * Put every interrupt into the given group.  On a two-state GIC only Group 1
 * is reachable from non-secure EL1, so this normally selects Group 1; the
 * probe also uses it to check what Group 0 does on a single-state GIC.
 */
static inline void
dgic_set_group(struct dgic *g, unsigned int group)
{
	unsigned int words = (g->typer & DGICD_TYPER_LINES_MASK) + 1u;
	unsigned int word = (group == DGIC_GROUP1) ? 0xffffffffu : 0u;

	if (words > 32u) {
		words = 32u;
	}

	g->group = group;

	for (unsigned int i = 0; i < words; i++) {
		dgic_write32(g->gicd, DGICD_IGROUPR + i * 4u, word);
	}
	if (g->mode == DGIC_MODE_V3 && g->redist) {
		dgic_write32(g->redist, DGICR_IGROUPR0, word);
	}
}

/*
 * Configure the distributor for the group selected in dgic_identify().
 * Safe to call once per boot from the boot processor.
 */
static inline void
dgic_distributor_init(struct dgic *g)
{
	unsigned long gicd = g->gicd;
	unsigned int max_irqs = ((g->typer & DGICD_TYPER_LINES_MASK) + 1u) * 32u;
	unsigned int words = max_irqs / 32u;

	if (words == 0 || words > 32) {
		words = 32;
		max_irqs = 1024;
	}
	(void)words;

	/* Enables must not be changed while the distributor is updating. */
	dgic_wait_rwp(gicd);

	if (g->mode == DGIC_MODE_V3) {
		unsigned int ctlr = dgic_read32(gicd, DGICD_CTLR);

		ctlr |= DGICD_CTLR_ARE_NS;              /* affinity routing */
		ctlr |= DGICD_CTLR_ENABLE0;             /* Grp1NS in the NS view */
		ctlr |= DGICD_CTLR_ENABLE1;             /* Grp1A */
		dgic_write32(gicd, DGICD_CTLR, ctlr);
		dgic_wait_rwp(gicd);

		/*
		 * Every SPI goes to the selected group; on a two-state GIC only
		 * Group 1 is reachable from non-secure EL1.  Route them all at
		 * the boot CPU; secondary CPUs come up later and are free to
		 * re-target individual lines.
		 */
		dgic_set_group(g, g->group);
		dgic_fill_priorities(gicd, DGICD_IPRIORITYR, max_irqs / 4u,
		    DGIC_PRIORITY_DEFAULT);
		/* Level-sensitive SPIs; ICFGR[0..1] hold SGIs/PPIs. */
		for (unsigned int i = 2; i < max_irqs / 16u; i++) {
			dgic_write32(gicd, DGICD_ICFGR + i * 4u, 0u);
		}
		/* Route every SPI at the boot CPU (affinity 0, IRM 0). */
		for (unsigned int irq = 32; irq < max_irqs; irq++) {
			dgic_write64(gicd, DGICD_IROUTER + irq * 8u, 0ul);
		}
		dgic_wait_rwp(gicd);
		return;
	}

	/* GICv2: no affinity routing.  Group 1 = IRQ. */
	dgic_set_group(g, g->group);
	dgic_fill_priorities(gicd, DGICD_IPRIORITYR, max_irqs / 4u,
	    DGIC_PRIORITY_DEFAULT);
	/* Send every SPI to CPU0: ITARGETSR covers IRQ 32.. instead. */
	for (unsigned int irq = 32; irq < max_irqs; irq += 4) {
		dgic_write32(gicd, DGICD_ITARGETSR + irq, 0x01010101u);
	}
	/* Level-sensitive SPIs. */
	for (unsigned int i = 2; i < max_irqs / 16u; i++) {
		dgic_write32(gicd, DGICD_ICFGR + i * 4u, 0u);
	}
	dgic_write32(gicd, DGICD_CTLR, DGICV2_CTLR_ENABLE_GRP1);
}

/*
 * Wake this PE's redistributor.
 *
 * `program_group` controls whether the SGI/PPI group register is written.
 * MediaTek's GIC-500 runs with both security states implemented, so
 * GICR_IGROUPR0 belongs to EL3: writing it from the boot shim (MMU off) is
 * silently dropped, and writing it from kernel context (MMU on) stalls the
 * bus and hangs the machine.  Callers that run after the MMU is on must pass
 * 0 and let the secure firmware keep ownership of the group registers.
 */
static inline int
dgic_redist_init(struct dgic *g, int program_group)
{
	unsigned long redist = dgic_find_redist(g);
	unsigned int waker;

	if (redist == 0) {
		return DGIC_ERR_NO_REDIST;
	}
	g->redist = redist;

	waker = dgic_read32(redist, DGICR_WAKER);
	if (waker & DGICR_WAKER_CHILDRENASLEEP) {
		waker &= ~DGICR_WAKER_PROCESSORSLEEP;
		dgic_write32(redist, DGICR_WAKER, waker);
		for (unsigned int spin = 0; spin < 1000000u; spin++) {
			if ((dgic_read32(redist, DGICR_WAKER) &
			    DGICR_WAKER_CHILDRENASLEEP) == 0) {
				break;
			}
		}
		if (dgic_read32(redist, DGICR_WAKER) & DGICR_WAKER_CHILDRENASLEEP) {
			return DGIC_ERR_WAKE;
		}
	}

	/* SGIs and PPIs: selected group, equal priorities, no group modifier. */
	if (program_group) {
		dgic_set_group(g, g->group);
	}
	dgic_fill_priorities(redist, DGICR_IPRIORITYR, 8, DGIC_PRIORITY_DEFAULT);
	/* SGIs edge triggered; PPIs (including the timer) level sensitive. */
	dgic_write32(redist, DGICR_ICFGR0, 0u);
	dgic_write32(redist, DGICR_ICFGR1, 0u);
	return DGIC_OK;
}

/*
 * Program this PE's CPU interface for the selected group.  On GICv3 this is
 * the ICC_* system register block; on GICv2 it is the memory-mapped GICC.
 */
static inline int
dgic_cpuif_init(struct dgic *g)
{
	if (g->mode == DGIC_MODE_V2) {
		unsigned long gicc = g->gicc;

		if (gicc == 0) {
			return DGIC_ERR_NO_CPUIF;
		}
		/*
		 * EOImode 0, no priority grouping.  Bit 0 of the non-secure
		 * view enables Group 1, which is the only group a non-secure
		 * kernel can receive on a GICv2.
		 */
		dgic_write32(gicc, DGICC_CTLR, DGICV2_CTLR_ENABLE_GRP1);
		dgic_write32(gicc, DGICC_PMR, 0xf0u);
		dgic_write32(gicc, DGICC_BPR, 0u);
		dgic_isb();
		g->icc_ctlr = dgic_read32(gicc, DGICC_CTLR);
		g->icc_pmr = dgic_read32(gicc, DGICC_PMR);
		return DGIC_OK;
	}

	/* GICv3: the system register interface has to be enabled first. */
	g->icc_sre_el1 = dgic_icc_sre_el1_read();
	if ((g->icc_sre_el1 & 1u) == 0) {
		dgic_icc_sre_el1_write(g->icc_sre_el1 | 1u);
		dgic_isb();
		g->icc_sre_el1 = dgic_icc_sre_el1_read();
	}
	if ((g->icc_sre_el1 & 1u) == 0) {
		return DGIC_ERR_SRE;
	}

	DGIC_TRACE("gic: after sre");
	g->icc_iidr = dgic_icc_iidr_read();

	if (g->group == DGIC_GROUP0) {
		DGIC_TRACE("gic: bpr0");
		__asm__ volatile("msr ICC_BPR0_EL1, %0" : : "r"(0ul) : "memory");
		DGIC_TRACE("gic: pmr");
		__asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xfful) : "memory");
		DGIC_TRACE("gic: ctlr");
		__asm__ volatile("msr ICC_CTLR_EL1, %0" : : "r"(0ul) : "memory");
		dgic_isb();
		DGIC_TRACE("gic: igrpen0");
		__asm__ volatile("msr ICC_IGRPEN0_EL1, %0" : : "r"(1ul) : "memory");
	} else {
		DGIC_TRACE("gic: bpr1");
		__asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(0ul) : "memory");
		DGIC_TRACE("gic: pmr");
		__asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xfful) : "memory");
		DGIC_TRACE("gic: ctlr");
		__asm__ volatile("msr ICC_CTLR_EL1, %0" : : "r"(0ul) : "memory");
		dgic_isb();
		DGIC_TRACE("gic: igrpen1");
		__asm__ volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(1ul) : "memory");
	}
	dgic_isb();
	DGIC_TRACE("gic: read ctlr");
	g->icc_ctlr = dgic_icc_ctlr_read();
	DGIC_TRACE("gic: read pmr");
	__asm__ volatile("mrs %0, ICC_PMR_EL1" : "=r"(g->icc_pmr));
	/*
	 * Do NOT read ICC_IGRPEN0_EL1 here.  When the GIC has two security
	 * states (GICD_TYPER.SecurityExtn == 1) and the kernel runs non-secure,
	 * every Group 0 system register is UNDEFINED - and on this SoC that
	 * takes the machine down with no output at all, which is exactly how
	 * the boot used to end right here.  Group 0 belongs to EL3; report it
	 * as disabled rather than asking.
	 */
	DGIC_TRACE("gic: igrpen0 assumed 0 (Group 0 is EL3)");
	g->icc_igrpen0 = 0;
	DGIC_TRACE("gic: read igrpen1");
	__asm__ volatile("mrs %0, ICC_IGRPEN1_EL1" : "=r"(g->icc_igrpen1));
	DGIC_TRACE("gic: cpuif init ok");
	return DGIC_OK;
}

/* Enable one PPI (IRQ < 32) on this PE, in the selected group. */
static inline void
dgic_enable_ppi(struct dgic *g, unsigned int irq)
{
	if (irq >= 32) {
		return;
	}
	if (g->mode == DGIC_MODE_V3) {
		unsigned int icfgr;

		if (g->redist == 0) {
			return;
		}
		dgic_set_priority32(g->redist, DGICR_IPRIORITYR, irq,
		    DGIC_PRIORITY_DEFAULT);
		/*
		 * PPIs 16..31 are configured in ICFGR1, two bits each, where 0
		 * means level sensitive (what the generic timer needs).
		 */
		icfgr = DGICR_ICFGR0 + ((irq / 16u) * 4u);
		if (irq >= 16) {
			unsigned int shift = ((irq - 16u) % 16u) * 2u + 1u;
			dgic_write32(g->redist, icfgr,
			    dgic_read32(g->redist, icfgr) & ~(0x3u << (shift - 1u)));
		}
		dgic_write32(g->redist, DGICR_ISENABLER0, 1u << irq);
		return;
	}
	dgic_set_priority32(g->gicd, DGICD_IPRIORITYR, irq, DGIC_PRIORITY_DEFAULT);
	dgic_write32(g->gicd, DGICD_ISENABLER, 1u << irq);
}

/* Acknowledge the highest priority interrupt of the selected group. */
static inline unsigned int
dgic_ack(struct dgic *g)
{
	unsigned long iar;

	if (g->mode == DGIC_MODE_V2) {
		return dgic_read32(g->gicc, DGICC_IAR) & 0x3ffu;
	}
	if (g->group == DGIC_GROUP0) {
		__asm__ volatile("mrs %0, ICC_IAR0_EL1" : "=r"(iar));
	} else {
		__asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
	}
	iar &= 0xfffffful;
	if (iar >= DGIC_INTID_SPURIOUS) {
		return DGIC_INTID_SPURIOUS;
	}
	return (unsigned int)iar;
}

static inline void
dgic_eoi(struct dgic *g, unsigned int irq)
{
	if (irq >= DGIC_INTID_SPURIOUS) {
		return;
	}
	if (g->mode == DGIC_MODE_V2) {
		dgic_write32(g->gicc, DGICC_EOIR, irq & 0x3ffu);
		return;
	}
	if (g->group == DGIC_GROUP0) {
		__asm__ volatile("msr ICC_EOIR0_EL1, %0" : : "r"((unsigned long)irq)
		    : "memory");
	} else {
		__asm__ volatile("msr ICC_EOIR1_EL1, %0" : : "r"((unsigned long)irq)
		    : "memory");
	}
	dgic_isb();
}

/*
 * Highest priority pending interrupt, as the CPU interface sees it.  This is
 * the right register for a timer self test: a level-sensitive PPI does not
 * necessarily latch a bit in GICR_ISPENDR0 (the distributor tracks the line
 * level instead), but it always shows up in HPPIR once the interrupt is
 * enabled and the priority mask allows it.
 */
static inline unsigned int
dgic_hppir(struct dgic *g)
{
	unsigned long value;

	if (g->mode == DGIC_MODE_V2) {
		if (g->gicc == 0) {
			return DGIC_INTID_SPURIOUS;
		}
		return dgic_read32(g->gicc, DGICC_HPPIR) & 0x3ffu;
	}
	if (g->group == DGIC_GROUP0) {
		__asm__ volatile("mrs %0, ICC_HPPIR0_EL1" : "=r"(value));
	} else {
		__asm__ volatile("mrs %0, ICC_HPPIR1_EL1" : "=r"(value));
	}
	value &= 0xfffffful;
	return (value >= DGIC_INTID_SPURIOUS) ? DGIC_INTID_SPURIOUS :
	       (unsigned int)value;
}

/*
 * Bring up this PE: distributor (optional), redistributor and CPU interface.
 * Returns DGIC_OK or a negative DGIC_ERR_*.
 *
 * `program_distributor` must be 0 for callers that run with the MMU on: the
 * distributor's group-control registers (GICD_CTLR, GICD_IGROUPR) belong to
 * EL3 on this SoC and writing them from non-secure kernel context stalls the
 * bus.  The shim passes 1 (with the MMU off the same writes are harmlessly
 * dropped), the kernel passes 0.
 */
static inline int
dgic_cpu_init(struct dgic *g, int program_distributor)
{
	int error;

	if (program_distributor) {
		dgic_distributor_init(g);
	}
	if (g->mode == DGIC_MODE_V3) {
		error = dgic_redist_init(g, program_distributor);
		if (error != DGIC_OK) {
			return error;
		}
	}
	return dgic_cpuif_init(g);
}

/* ---------------------------------------------------------------------- */
/* Self test: does a PPI actually become pending?                          */
/* ---------------------------------------------------------------------- */

/* Software generated interrupt to the CPUs in `target_mask` (bit n = CPU n). */
static inline void
dgic_send_sgi(struct dgic *g, unsigned int sgi, unsigned int aff1,
    unsigned int target_mask)
{
	if (sgi > 15) {
		return;
	}
	if (g->mode == DGIC_MODE_V2) {
		dgic_write32(g->gicd, DGICD_SGIR,
		    (sgi << 24) | (target_mask & 0xffu));
		return;
	}
	__asm__ volatile("msr ICC_SGI1R_EL1, %0" : :
	    "r"(((unsigned long)(sgi & 0xful) << 24) |
	    (unsigned long)(aff1 & 0xffu) << 16 |
	    (unsigned long)(target_mask & 0xffffu)) : "memory");
	dgic_isb();
}

/*
 * Arm the EL0 virtual timer for one tick and watch the CPU interface for the
 * resulting PPI.  This answers "can this CPU see an interrupt at all?"
 * without needing a working kernel interrupt path.
 *
 * Returns 1 when the interrupt reached the CPU interface, 0 when it never
 * appeared.  The caller should clear the pending state afterwards.
 */
static inline int
dgic_timer_test(struct dgic *g, unsigned int irq, unsigned long timeout_us)
{
	unsigned long frequency = dgic_cntfrq();
	unsigned long start, deadline;
	unsigned int pending_bit = 1u << irq;
	int delivered = 0, fired = 0;

	if (frequency == 0 || irq >= 32) {
		return 0;
	}

	dgic_enable_ppi(g, irq);
	/* Clear a stale pending bit before arming. */
	if (g->mode == DGIC_MODE_V3 && g->redist) {
		dgic_write32(g->redist, DGICR_ICPENDR0, pending_bit);
	} else {
		dgic_write32(g->gicd, DGICD_ICPENDR, pending_bit);
	}

	/* One tick of 1 kHz, then unmask; the counter runs regardless of DAIF. */
	__asm__ volatile("msr CNTV_TVAL_EL0, %0" : :
	    "r"(frequency / 1000u) : "memory");
	__asm__ volatile("msr CNTV_CTL_EL0, %0" : : "r"(1ul) : "memory");
	dgic_isb();

	start = dgic_cntvct();
	deadline = start + (frequency / 1000000ul) * timeout_us;

	while ((long)(dgic_cntvct() - deadline) < 0) {
		unsigned long ctl;
		unsigned int intid;

		__asm__ volatile("mrs %0, CNTV_CTL_EL0" : "=r"(ctl));
		if (ctl & 4ul) {                /* ISTATUS: the timer expired */
			fired = 1;
		}
		/*
		 * Ask the CPU interface what it would hand to the CPU: reading
		 * ICC_IAR1/IAR0 acknowledges the interrupt, so a match proves
		 * end-to-end delivery (and having to EOI it makes the test
		 * leave no active interrupt behind).
		 */
		intid = dgic_ack(g);
		if (intid == irq) {
			dgic_eoi(g, irq);
			delivered = 1;
			break;
		}
	}

	/* Stop the timer again so the caller starts from a known state. */
	__asm__ volatile("msr CNTV_CTL_EL0, %0" : : "r"(0ul) : "memory");
	dgic_isb();

	/* bit0: reached the CPU interface, bit1: the timer itself expired. */
	return (delivered ? 1 : 0) | (fired ? 2 : 0);
}

static inline void
dgic_timer_test_clear(struct dgic *g, unsigned int irq)
{
	unsigned int pending_bit = 1u << irq;

	if (irq >= 32) {
		return;
	}
	if (g->mode == DGIC_MODE_V3 && g->redist) {
		dgic_write32(g->redist, DGICR_ICPENDR0, pending_bit);
	} else {
		dgic_write32(g->gicd, DGICD_ICPENDR, pending_bit);
	}
}

#endif /* _PEXPERT_ARM64_DANDELION_GIC_H */
