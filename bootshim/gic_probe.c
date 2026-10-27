/* Dandelion boot shim - interrupt controller inventory (implementation) */
#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "gic_probe.h"
#include "pe_dandelion.h"

#define PROBE_REDIST_MAX 16

/* ---------------------------------------------------------------------- */
/* Small output helpers.  fb_puthex()/fb_putdec() always end the line, and
 * most of this report is "label value value" on a single line, so format
 * inline instead.                                                         */
/* ---------------------------------------------------------------------- */

static void
out(const char *text, uint32_t color)
{
	fb_puts(text, color);
}

static void
outhex(unsigned long value)
{
	static const char digits[] = "0123456789abcdef";
	char buffer[19];
	unsigned length = 0;
	int started = 0;

	buffer[length++] = '0';
	buffer[length++] = 'x';
	for (int shift = 60; shift >= 0; shift -= 4) {
		unsigned nibble = (unsigned)((value >> shift) & 0xful);
		if (nibble != 0 || started || shift == 0) {
			buffer[length++] = digits[nibble];
			started = 1;
		}
	}
	buffer[length] = '\0';
	out(buffer, CLR_WHITE);
}

static void
outdec(unsigned long value)
{
	char buffer[21];
	unsigned length = 0;

	if (value == 0) {
		buffer[length++] = '0';
	}
	while (value != 0) {
		buffer[length++] = (char)('0' + (value % 10));
		value /= 10;
	}
	for (unsigned i = 0; i < length / 2; i++) {
		char swap = buffer[i];
		buffer[i] = buffer[length - 1 - i];
		buffer[length - 1 - i] = swap;
	}
	buffer[length] = '\0';
	out(buffer, CLR_WHITE);
}

static void
field_hex(const char *label, unsigned long value)
{
	out("  ", CLR_WHITE);
	out(label, CLR_CYAN);
	out(" ", CLR_WHITE);
	outhex(value);
	fb_newline();
}

static void
field_dec(const char *label, unsigned long value)
{
	out("  ", CLR_WHITE);
	out(label, CLR_CYAN);
	out(" ", CLR_WHITE);
	outdec(value);
	fb_newline();
}

/* ---------------------------------------------------------------------- */
/* Region selection: the device's own device tree wins over DANDELION.h.   */
/* ---------------------------------------------------------------------- */

static void
gic_regions(const struct fdt_platform *platform,
	    unsigned long *gicd, unsigned long *gicd_size,
	    unsigned long *gicr, unsigned long *gicr_size,
	    const char **source, const char **compatible)
{
	if (platform && platform->gic.found && platform->gic.regions >= 1 &&
	    platform->gic.base[0] != 0) {
		*source = "device DTB";
		*compatible = platform->gic.compatible;
		*gicd = (unsigned long)platform->gic.base[0];
		*gicd_size = (unsigned long)platform->gic.size[0];
		if (platform->gic.regions >= 2) {
			*gicr = (unsigned long)platform->gic.base[1];
			*gicr_size = (unsigned long)platform->gic.size[1];
		}
		return;
	}

	*source = "DANDELION.h";
	*compatible = "build-time constants";
	*gicd = (unsigned long)DANDELION_GIC_BASE;
	*gicd_size = (unsigned long)DANDELION_GIC_SIZE;
	*gicr = (unsigned long)DANDELION_GICR_BASE;
	*gicr_size = (unsigned long)DANDELION_GICR_SIZE;
}

/* 1 for an arm,gic-v3 node, 0 for a GICv2 node, -1 with no device tree. */
static int
gic_v3_hint(const struct fdt_platform *platform)
{
	const char *compatible;

	if (!platform || !platform->gic.found) {
		return -1;
	}
	compatible = platform->gic.compatible;
	for (const char *p = compatible; *p; p++) {
		if (p[0] == 'g' && p[1] == 'i' && p[2] == 'c' && p[3] == '-' &&
		    p[4] == 'v' && p[5] == '3') {
			return 1;
		}
	}
	return 0;
}

static void
walk_redistributors(struct gic_probe_result *r)
{
	unsigned long frame = r->gic.gicr;
	unsigned long end = frame + r->gic.gicr_size;
	unsigned long mpidr;
	unsigned int count = 0;

	r->redist_frames = 0;
	if (r->gic.mode != DGIC_MODE_V3 || frame == 0 || r->gic.gicr_size == 0) {
		return;
	}

	/*
	 * Experiment E0 of docs/R1-GIC-PLAN.md: the frame that should belong to
	 * affinity 0 reads GICR_TYPER as zero while the other seven decode to
	 * affinities 1,2,3,0x100..0x103.  Either that frame is genuinely dead,
	 * or we have been programming the wrong redistributor all along.  Print
	 * the boot CPU's MPIDR, decode every frame, and read a zero TYPER twice
	 * before believing it.
	 */
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	out("  boot mpidr ", CLR_CYAN);
	outhex(mpidr);
	out("  affinity ", CLR_CYAN);
	outhex(mpidr & 0xFF00FFFFFFul);
	fb_newline();

	while (frame + 0x20000ul <= end && count < PROBE_REDIST_MAX) {
		unsigned long typer = dgic_read64(frame, DGICR_TYPER);
		unsigned long affinity = (typer >> 32) & 0xFFFFFFFFul;
		unsigned int waker;

		if (typer == 0) {
			typer = dgic_read64(frame, DGICR_TYPER);
			affinity = (typer >> 32) & 0xFFFFFFFFul;
		}
		waker = dgic_read32(frame, DGICR_WAKER);

		r->redist_base[count] = frame;
		r->redist_typer[count] = typer;
		r->redist_waker[count] = waker;
		count++;

		out("  frame ", CLR_CYAN);
		outhex(frame);
		out(" typer ", CLR_CYAN);
		outhex(typer);
		out(" aff ", CLR_CYAN);
		outhex(affinity);
		out(" waker ", CLR_CYAN);
		outhex(waker);
		out((waker & DGICR_WAKER_CHILDRENASLEEP) ? "  (asleep)" :
		    "  (awake)", CLR_WHITE);
		if (affinity == (mpidr & 0xFF00FFFFFFul)) {
			out("  <- self", CLR_GREEN);
		}
		fb_newline();

		if (typer & DGICR_TYPER_LAST) {
			break;
		}
		frame += dgic_redist_stride(frame);
	}
	r->redist_frames = count;
}

/*
 * Send ourselves an SGI and see whether it reaches the CPU interface.  This
 * separates the two halves of the path: if an SGI arrives but the timer PPI
 * does not, the distributor and CPU interface are fine and the problem is on
 * the timer side.  Only meaningful for Group 1: a non-secure SGI is always
 * Group 1.
 */
static int
sgi_test(struct dgic *g, unsigned int sgi)
{
	unsigned long frequency = dgic_cntfrq();
	unsigned long deadline;
	int seen = 0;

	/* An SGI is only delivered when it is enabled on this PE. */
	dgic_enable_ppi(g, sgi);
	dgic_send_sgi(g, sgi, 0, 1);
	deadline = dgic_cntvct() + (frequency / 1000000ul) * 2000ul;

	while ((long)(dgic_cntvct() - deadline) < 0) {
		unsigned int intid = dgic_ack(g);
		if (intid == sgi) {
			dgic_eoi(g, sgi);
			seen = 1;
			break;
		}
	}
	return seen;
}

/*
 * Try to see the timer PPI in the CPU interface of a given group.
 * Returns 1 on success, 0 when it never appeared, -1 when that group's CPU
 * interface could not be enabled.
 */
static int
timer_test_for_group(const struct dgic *base, unsigned int group,
		     unsigned int irq, int report_sgi)
{
	struct dgic trial = *base;
	unsigned long status_base;
	unsigned int pending, active, enabled, hppir;
	unsigned int forced;
	int seen;

	trial.group = group;
	dgic_set_group(&trial, group);
	if (dgic_cpuif_init(&trial) != DGIC_OK) {
		return -1;
	}

	status_base = (trial.mode == DGIC_MODE_V3 && trial.redist) ?
	    trial.redist : trial.gicd;

	seen = dgic_timer_test(&trial, irq, 2000ul);
	hppir = dgic_hppir(&trial);

	/*
	 * Control experiment: force the PPI pending in software and ask the
	 * CPU interface again.  If this shows up but the timer does not, the
	 * distributor/CPU-interface path is fine and the interrupt source is
	 * the problem; if nothing shows up, the CPU interface is not
	 * connected to this PE at all.
	 */
	if (trial.mode == DGIC_MODE_V3 && trial.redist) {
		dgic_write32(status_base, DGICR_ISPENDR0, 1u << irq);
	} else {
		dgic_write32(status_base, DGICD_ISPENDR, 1u << irq);
	}
	dgic_isb();
	forced = dgic_ack(&trial);
	if (forced < DGIC_INTID_SPURIOUS) {
		dgic_eoi(&trial, forced);
	}

	if (trial.mode == DGIC_MODE_V3 && trial.redist) {
		pending = dgic_read32(status_base, DGICR_ISPENDR0);
		active = dgic_read32(status_base, 0x10300u);   /* ISACTIVER0 */
		enabled = dgic_read32(status_base, DGICR_ISENABLER0);
	} else {
		pending = dgic_read32(status_base, DGICD_ISPENDR);
		active = dgic_read32(status_base, 0x0300u);    /* ISACTIVER  */
		enabled = dgic_read32(status_base, DGICD_ISENABLER);
	}

	out("    enabled ", CLR_CYAN);
	out((enabled & (1u << irq)) ? "yes" : "NO", CLR_WHITE);
	out("  pending ", CLR_CYAN);
	out((pending & (1u << irq)) ? "yes" : "no", CLR_WHITE);
	out("  active ", CLR_CYAN);
	out((active & (1u << irq)) ? "yes" : "no", CLR_WHITE);
	out("  hppir ", CLR_CYAN);
	outdec(hppir);
	out(" forced-ack ", CLR_CYAN);
	outdec(forced);
	out("  pmr ", CLR_CYAN);
	outhex(trial.icc_pmr);
	out(" grp0 ", CLR_CYAN);
	outdec(trial.icc_igrpen0);
	out(" grp1 ", CLR_CYAN);
	outdec(trial.icc_igrpen1);
	fb_newline();

	if (report_sgi) {
		int sgi_seen = sgi_test(&trial, 15);
		out("    sgi self-test ", CLR_CYAN);
		out(sgi_seen ? "delivered\n" : "NOT delivered\n",
		    sgi_seen ? CLR_GREEN : CLR_RED);
	}

	dgic_timer_test_clear(&trial, irq);
	return seen;
}

static void
report_timer(const char *label, int result, int *delivered, int *fired,
	     const char *ok, const char *no)
{
	*delivered = (result > 0 && (result & 1)) ? 1 : 0;
	*fired = (result > 0 && (result & 2)) ? 1 : 0;

	out("  ", CLR_WHITE);
	out(label, CLR_CYAN);
	if (result < 0) {
		out("CPU interface unavailable\n", CLR_RED);
		return;
	}
	if (*delivered) {
		out(ok, CLR_GREEN);
		return;
	}
	out(no, CLR_RED);
	out(*fired ? " (timer expired, no interrupt)" :
	    " (timer never expired)", CLR_YELLOW);
	fb_newline();
}

void
gic_probe_run(const struct fdt_platform *platform, struct gic_probe_result *r)
{
	unsigned long gicd = 0, gicd_size = 0, gicr = 0, gicr_size = 0;
	const char *source = "?";
	const char *compatible = "?";
	unsigned int irq = DANDELION_TIMER_IRQ;

	out("\n--- GIC INVENTORY ---\n", CLR_YELLOW);

	/*
	 * MediaTek wraps the GIC in a clock-domain synchroniser, and until it
	 * is enabled the distributor accepts some writes (ISENABLER, priority)
	 * while silently dropping others (IGROUPR, GICD_CTLR group enables).
	 * Android's kernel turns it on through its Power/dcm framework right
	 * before initialising the GIC:
	 *
	 *   [Power/dcm] [GIC_SYNC_DCM 0x00000200] current state:1
	 *   INFRA_BUS_DCM_CTRL (0x...8070): 0x40ff0603
	 *   PERI_BUS_DCM_CTRL  (0x...8074): 0xb47f83e3
	 *
	 * GIC_SYNC_DCM is bit 9 of the INFRACFG_AO bus DCM control registers,
	 * i.e. 0x10001070 / 0x10001074.
	 */
	{
		volatile uint32_t *infra_bus = (volatile uint32_t *)0x10001070UL;
		volatile uint32_t *peri_bus = (volatile uint32_t *)0x10001074UL;
		uint32_t before_infra = *infra_bus;
		uint32_t before_peri = *peri_bus;

		*infra_bus = before_infra | (1u << 9);
		*peri_bus = before_peri | (1u << 9);
		fb_puts("  dcm infra ", CLR_CYAN);
		outhex(before_infra);
		out(" -> ", CLR_WHITE);
		outhex(*infra_bus);
		fb_newline();
		fb_puts("  dcm peri  ", CLR_CYAN);
		outhex(before_peri);
		out(" -> ", CLR_WHITE);
		outhex(*peri_bus);
		fb_newline();
	}

	for (unsigned i = 0; i < sizeof(*r); i++) {
		((unsigned char *)r)[i] = 0;
	}

	gic_regions(platform, &gicd, &gicd_size, &gicr, &gicr_size,
	    &source, &compatible);

	/*
	 * Test the interrupt the kernel actually uses: the EL0 virtual timer,
	 * PPI 11 (INTID 27).  The device tree lists all four arch timer
	 * interrupts and its first entry is the *secure* physical timer, which
	 * non-secure EL1 can neither arm nor receive.
	 */
	if (platform && platform->timer.found) {
		int listed = 0;
		for (unsigned i = 0; i < platform->timer.irq_count; i++) {
			if (platform->timer.irq[i] == DANDELION_TIMER_IRQ) {
				listed = 1;
			}
		}
		if (!listed) {
			out("  note: DTB does not list INTID ", CLR_YELLOW);
			outdec(DANDELION_TIMER_IRQ);
			out(" for this timer\n", CLR_YELLOW);
		}
	}
	r->timer_irq = irq;

	out("  source  ", CLR_CYAN);
	out(source, CLR_WHITE);
	fb_newline();

	if (platform && platform->gic.found) {
		out("  dt gic  ", CLR_CYAN);
		out(compatible, CLR_WHITE);
		fb_newline();
	}

	if (gicd == 0 || gicd_size == 0) {
		out("  no GIC region to probe\n", CLR_RED);
		r->identify = DGIC_ERR_DEAD;
		return;
	}

	field_hex("gicd     ", gicd);
	field_hex("gicd size", gicd_size);
	if (gicr) {
		field_hex("region1  ", gicr);
		field_hex("region1sz", gicr_size);
	}

	r->identify = dgic_identify(&r->gic, gicd, gicd_size, gicr, gicr_size,
	    gic_v3_hint(platform));

	field_hex("GICD_CTLR ", r->gic.ctlr);
	field_hex("GICD_TYPER", r->gic.typer);
	field_dec("  irq lines", ((r->gic.typer & DGICD_TYPER_LINES_MASK) + 1u) * 32u);
	field_hex("GICD_IIDR ", r->gic.iidr);
	field_hex("PIDR2 FE8 ", r->gic.pidr2_4k);
	field_hex("PIDR2 FFE8", r->gic.pidr2_64k);
	field_dec("arch rev  ", r->gic.arch_rev);

	out("  SecurityExtn ", CLR_CYAN);
	outdec(r->gic.secure_extn);
	out(r->gic.secure_extn ?
	    "  2 states -> Group0 is EL3, Group1/IRQ only\n" :
	    "  single state -> Group0/FIQ usable\n",
	    r->gic.secure_extn ? CLR_YELLOW : CLR_GREEN);

	out("  mode     ", CLR_CYAN);
	out(r->gic.mode == DGIC_MODE_V3 ? "GICv3" :
	    r->gic.mode == DGIC_MODE_V2 ? "GICv2" : "unknown", CLR_WHITE);
	out(r->gic.group == DGIC_GROUP1 ? "  group1 (IRQ)\n" :
	    "  group0 (FIQ)\n", CLR_WHITE);

	if (r->gic.mode == DGIC_MODE_V2) {
		field_hex("gicc     ", r->gic.gicc);
		field_hex("gicc iidr", r->gic.gicc ?
		    dgic_read32(r->gic.gicc, DGICC_IIDR) : 0);
	}

	if (r->identify != DGIC_OK) {
		out("  identify failed: ", CLR_RED);
		outdec((unsigned long)(-r->identify));
		fb_newline();
	}

	/* Redistributors, as the hardware reports them. */
	walk_redistributors(r);
	if (r->gic.mode == DGIC_MODE_V3) {
		r->self_redist = dgic_find_redist(&r->gic);
		field_hex("self redist", r->self_redist);
	}

	/*
	 * Read the CPU-interface ID register *before* touching the distributor.
	 * If this hangs, ICC_* is unreachable from EL1 on this part no matter
	 * what we program; if it works here and hangs later, the distributor or
	 * redistributor programming is what breaks it.
	 */
	if (r->gic.mode == DGIC_MODE_V3) {
		out("  icc probe (pristine)", CLR_CYAN);
		outhex(dgic_icc_sre_el1_read());
		out(" sre ok", CLR_GREEN);
		fb_newline();
	}

	/* Full bring-up, exactly like the kernel does it, with checkpoints so a
	 * hang points at one step instead of the whole sequence. */
	out("  distributor", CLR_CYAN);
	dgic_distributor_init(&r->gic);
	fb_puts(" ok", CLR_GREEN);
	fb_newline();

	if (r->gic.mode == DGIC_MODE_V3) {
		out("  redistributor", CLR_CYAN);
		r->cpu_init = dgic_redist_init(&r->gic, 1);
		fb_puts(r->cpu_init == DGIC_OK ? " ok" : " failed", CLR_WHITE);
		fb_newline();
	}

	/*
	 * CPU interface, one register at a time.  On this SoC an ICC_* access
	 * can stall the CPU instead of taking an UNDEFINED exception, so each
	 * step prints before the access and the last line on screen names the
	 * register that hung.
	 */
	{
		unsigned long sre, iidr, ctlr;

		out("  sre_rd", CLR_CYAN);
		sre = dgic_icc_sre_el1_read();
		outhex(sre);
		fb_newline();

		out("  sre_wr", CLR_CYAN);
		dgic_icc_sre_el1_write(sre | 1ul);
		dgic_isb();
		fb_puts(" ok\n", CLR_GREEN);

		out("  iidr", CLR_CYAN);
		iidr = dgic_icc_iidr_read();
		outhex(iidr);
		fb_newline();

		out("  bpr1", CLR_CYAN);
		__asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(0ul) : "memory");
		fb_puts(" ok\n", CLR_GREEN);

		out("  pmr", CLR_CYAN);
		__asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xfful) : "memory");
		fb_puts(" ok\n", CLR_GREEN);

		out("  ctlr", CLR_CYAN);
		__asm__ volatile("msr ICC_CTLR_EL1, %0" : : "r"(0ul) : "memory");
		fb_puts(" ok\n", CLR_GREEN);

		out("  grpen1", CLR_CYAN);
		__asm__ volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(1ul) : "memory");
		dgic_isb();
		fb_puts(" ok\n", CLR_GREEN);

		out("  cpuif_rd", CLR_CYAN);
		ctlr = dgic_icc_ctlr_read();
		outhex(ctlr);
		__asm__ volatile("mrs %0, ICC_PMR_EL1" : "=r"(r->gic.icc_pmr));
		out(" pmr ", CLR_CYAN);
		outhex(r->gic.icc_pmr);
		fb_newline();
		r->cpu_init = DGIC_OK;
	}

	out("  tick ", CLR_CYAN);
	outhex(dgic_cntfrq());
	out(" / ", CLR_WHITE);
	outhex(dgic_cntvct());
	fb_newline();

	if (r->gic.mode == DGIC_MODE_V3) {
		field_hex("ICC_SRE_EL1", r->gic.icc_sre_el1);
		field_hex("ICC_IIDR ", r->gic.icc_iidr);
		field_hex("ICC_CTLR ", r->gic.icc_ctlr);
	}
	field_hex("GICD_CTLR post", dgic_read32(r->gic.gicd, DGICD_CTLR));
	if (r->gic.mode == DGIC_MODE_V2) {
		field_hex("IGROUPR0 ", dgic_read32(r->gic.gicd, DGICD_IGROUPR));
	}
	if (r->gic.mode == DGIC_MODE_V3 && r->gic.redist) {
		field_hex("GICR_WAKER", dgic_read32(r->gic.redist, DGICR_WAKER));
		field_hex("IGROUPR0 ", dgic_read32(r->gic.redist, DGICR_IGROUPR0));
		field_hex("ISENABLER0", dgic_read32(r->gic.redist, DGICR_ISENABLER0));
		field_hex("IPRIORITY", dgic_read32(r->gic.redist,
		    DGICR_IPRIORITYR + (irq / 4u) * 4u));
	}

	if (r->cpu_init != DGIC_OK) {
		out("  timer test skipped (no usable CPU interface)\n", CLR_RED);
		return;
	}

	/*
	 * Which writes does the non-secure view actually accept?  Each probe
	 * prints its label before the access and the readback after it, so a bus
	 * stall names the exact register on screen.
	 */
	if (r->gic.mode == DGIC_MODE_V3 && r->gic.redist) {
		unsigned long redist = r->gic.redist;

		out("  w isen (ppi)", CLR_CYAN);
		dgic_write32(redist, DGICR_ISENABLER0, 1u << irq);
		outhex(dgic_read32(redist, DGICR_ISENABLER0));
		fb_newline();

		out("  w icfg1      ", CLR_CYAN);
		dgic_write32(redist, DGICR_ICFGR1, dgic_read32(redist, DGICR_ICFGR1));
		outhex(dgic_read32(redist, DGICR_ICFGR1));
		fb_newline();

		out("  w igupr0     ", CLR_CYAN);
		dgic_write32(redist, DGICR_IGROUPR0, 0xffffffffu);
		outhex(dgic_read32(redist, DGICR_IGROUPR0));
		fb_newline();

		out("  w ctlr 0x13  ", CLR_CYAN);
		dgic_write32(r->gic.gicd, DGICD_CTLR, 0x13u);
		dgic_wait_rwp(r->gic.gicd);
		outhex(dgic_read32(r->gic.gicd, DGICD_CTLR));
		fb_newline();

		out("  w ctlr 0x11  ", CLR_CYAN);
		dgic_write32(r->gic.gicd, DGICD_CTLR, 0x11u);
		dgic_wait_rwp(r->gic.gicd);
		outhex(dgic_read32(r->gic.gicd, DGICD_CTLR));
		fb_newline();

		out("  w icfgr1 zero", CLR_CYAN);
		dgic_write32(redist, DGICR_ICFGR1, 0u);
		outhex(dgic_read32(redist, DGICR_ICFGR1));
		fb_newline();
	}

	/*
	 * Can the non-secure world drive a *shared peripheral* interrupt at all?
	 * SPI group bits live in GICD_IGROUPR (writable from NS) rather than the
	 * redistributor's secure-only view, so this is a different path from the
	 * arch timer's PPI.  Force one SPI pending and see whether the CPU
	 * interface hands it to us: if this works, an MTK system timer (or any
	 * other SPI source) can provide the kernel's tick even though PPI 27
	 * cannot.
	 */
	{
		unsigned int spi = 200;
		unsigned int word_off = DGICD_IGROUPR + (spi / 32u) * 4u;
		unsigned int en_off = DGICD_ISENABLER + (spi / 32u) * 4u;
		unsigned int pend_off = DGICD_ISPENDR + (spi / 32u) * 4u;
		unsigned int bit = 1u << (spi % 32u);
		unsigned int id;

		out("  spi group    ", CLR_CYAN);
		dgic_write32(r->gic.gicd, word_off, 0xffffffffu);
		outhex(dgic_read32(r->gic.gicd, word_off));
		fb_newline();

		out("  spi enable   ", CLR_CYAN);
		dgic_write32(r->gic.gicd, en_off, bit);
		outhex(dgic_read32(r->gic.gicd, en_off));
		fb_newline();

		out("  spi priority ", CLR_CYAN);
		dgic_set_priority32(r->gic.gicd, DGICD_IPRIORITYR, spi,
		    DGIC_PRIORITY_DEFAULT);
		outhex(dgic_read32(r->gic.gicd,
		    DGICD_IPRIORITYR + (spi / 4u) * 4u));
		fb_newline();

		out("  spi router   ", CLR_CYAN);
		dgic_write64(r->gic.gicd, DGICD_IROUTER + spi * 8u, 0ul);
		outhex(dgic_read64(r->gic.gicd, DGICD_IROUTER + spi * 8u));
		fb_newline();

		out("  spi force    ", CLR_CYAN);
		dgic_write32(r->gic.gicd, pend_off, bit);
		outhex(dgic_read32(r->gic.gicd, pend_off));
		fb_newline();

		for (volatile unsigned long spin = 0; spin < 200000ul; spin++) {
		}

		out("  spi ack      ", CLR_CYAN);
		id = dgic_ack(&r->gic);
		outdec(id);
		out(id == spi ? "  <- SPI visible to NS" :
		    "  <- SPI NOT delivered", id == spi ? CLR_GREEN : CLR_RED);
		fb_newline();
		if (id < DGIC_INTID_SPURIOUS) {
			dgic_eoi(&r->gic, id);
		}
		r->spi_probe = id;
	}

	/* Arm the timer, then poll the CPU interface, with a marker on each side
	 * so a stall lands on one line instead of the whole test. */
	out("  arm tval     ", CLR_CYAN);
	__asm__ volatile("msr CNTV_TVAL_EL0, %0" : :
	    "r"(dgic_cntfrq() / 1000u) : "memory");
	fb_puts("ok\n", CLR_GREEN);

	out("  arm ctl      ", CLR_CYAN);
	__asm__ volatile("msr CNTV_CTL_EL0, %0" : : "r"(1ul) : "memory");
	dgic_isb();
	fb_puts("ok\n", CLR_GREEN);

	out("  poll         ", CLR_CYAN);
	{
		unsigned long deadline = dgic_cntvct() + (dgic_cntfrq() / 1000ul) * 20ul;
		unsigned int hppir = 0, polls = 0;

		while ((long)(dgic_cntvct() - deadline) < 0) {
			hppir = dgic_hppir(&r->gic);
			polls++;
			if (hppir == irq) {
				break;
			}
		}
		out("hppir ", CLR_WHITE);
		outdec(hppir);
		out(" polls ", CLR_CYAN);
		outdec(polls);
		fb_newline();
		r->timer_group1 = (hppir == irq);
	}

	out("  sgi self     ", CLR_CYAN);
	dgic_send_sgi(&r->gic, 15u, 0u, 1u);
	fb_puts("ok\n", CLR_GREEN);

	/*
	 * GICD_IGROUPR is RAZ/WI in the non-secure view, so the zero we read
	 * says nothing about the real group assignment: the secure firmware
	 * decides what non-secure code may receive.  Android/Linux uses the EL1
	 * *physical* timer (CNTP, INTID 30) while Apple's XNU uses the *virtual*
	 * timer (CNTV, INTID 27), so try both and see which one the firmware
	 * actually handed us.
	 */
	{
		static const unsigned int candidates[2] = { 30u, 27u };

		for (unsigned c = 0; c < 2; c++) {
			unsigned int intid = candidates[c];
			unsigned int hppir = 0;
			unsigned long deadline;
			int fired = 0;

			dgic_enable_ppi(&r->gic, intid);
			dgic_timer_test_clear(&r->gic, intid);

			if (intid == 30u) {
				__asm__ volatile("msr CNTP_TVAL_EL0, %0" : :
				    "r"(dgic_cntfrq() / 1000u) : "memory");
				__asm__ volatile("msr CNTP_CTL_EL0, %0" : :
				    "r"(1ul) : "memory");
			} else {
				__asm__ volatile("msr CNTV_TVAL_EL0, %0" : :
				    "r"(dgic_cntfrq() / 1000u) : "memory");
				__asm__ volatile("msr CNTV_CTL_EL0, %0" : :
				    "r"(1ul) : "memory");
			}
			dgic_isb();

			deadline = dgic_cntvct() + (dgic_cntfrq() / 1000ul) * 20ul;
			while ((long)(dgic_cntvct() - deadline) < 0) {
				unsigned long sample;

				if (intid == 30u) {
					__asm__ volatile("mrs %0, CNTP_CTL_EL0"
					    : "=r"(sample));
				} else {
					__asm__ volatile("mrs %0, CNTV_CTL_EL0"
					    : "=r"(sample));
				}
				if (sample & 4ul) {
					fired = 1;
				}
				hppir = dgic_hppir(&r->gic);
				if (hppir == intid) {
					break;
				}
			}

			__asm__ volatile("msr CNTP_CTL_EL0, %0" : : "r"(0ul) : "memory");
			__asm__ volatile("msr CNTV_CTL_EL0, %0" : : "r"(0ul) : "memory");
			dgic_isb();

			out(intid == 30u ? "  timer CNTP30 " : "  timer CNTV27 ",
			    CLR_CYAN);
			outhex(hppir);
			out(hppir == intid ? "  <- DELIVERED\n" :
			    "  <- not delivered\n",
			    hppir == intid ? CLR_GREEN : CLR_RED);

			/*
			 * Did the timer itself fire?  ISTATUS (bit 2) was sampled
			 * inside the poll loop, before the control register was
			 * cleared, so this answers "did the compare value pass"
			 * independently of any interrupt routing.
			 */
			out("               fired ", CLR_CYAN);
			out(fired ? "yes" : "NO",
			    fired ? CLR_GREEN : CLR_RED);
			fb_newline();

			if (intid == 30u) {
				r->timer_group0 = (hppir == intid);
			} else {
				r->timer_group1 = (hppir == intid);
			}
		}
	}

	out("  verdict      ", CLR_CYAN);
	if (r->timer_group0 == 1 || r->timer_group1 == 1) {
		out("at least one timer PPI is deliverable\n", CLR_GREEN);
	} else {
		out("no timer PPI is deliverable to NS\n", CLR_RED);
	}

	/*
	 * Alias hunt.  Everything above assumes the distributor lives at the
	 * address the arm,gic-v3 node gives (0x0c000000), where reads work but
	 * the group/enable writes are silently dropped.  The device tree also
	 * describes mediatek,gic_cpu at 0x0c400000 (256 KiB).  If that is the
	 * non-secure view of the same distributor, its registers are the ones
	 * that matter.  Read TYPER, then try to write IGROUPR and GICD_CTLR and
	 * see which base accepts them.
	 */
	{
		static const unsigned long bases[3] = {
			0x0C000000UL, 0x0C400000UL, 0x0C200000UL
		};

		for (unsigned b = 0; b < 3; b++) {
			unsigned long base = bases[b];
			unsigned int typer, igupr, ctlr;

			out("  base ", CLR_CYAN);
			outhex(base);
			typer = dgic_read32(base, DGICD_TYPER);
			out(" typer ", CLR_CYAN);
			outhex(typer);
			fb_newline();

			out("    igupr w/r ", CLR_CYAN);
			dgic_write32(base, DGICD_IGROUPR, 0xffffffffu);
			igupr = dgic_read32(base, DGICD_IGROUPR);
			outhex(igupr);
			out(igupr == 0xffffffffu ? "  <- writable" : "  <- read-only",
			    igupr == 0xffffffffu ? CLR_GREEN : CLR_RED);
			fb_newline();

			out("    ctlr  w/r ", CLR_CYAN);
			dgic_write32(base, DGICD_CTLR, 0x13u);
			ctlr = dgic_read32(base, DGICD_CTLR);
			outhex(ctlr);
			out((ctlr & 0x1u) ? "  <- EnableGrp1NS set" :
			    "  <- bit0 dropped",
			    (ctlr & 0x1u) ? CLR_GREEN : CLR_RED);
			fb_newline();
		}
	}

	/*
	 * MediaTek's own interrupt controller (mediatek,mt6577-sysirq at
	 * 0x10200b00) sits under the GIC and carries almost every peripheral
	 * interrupt, including the system timer (timer@10017000, sysirq hwirq
	 * 194).  Android's /proc/interrupts shows that NS can use it, so this is
	 * the fallback path for a kernel tick if the GIC stays closed.  Dump
	 * both register blocks so the layout can be matched against the Linux
	 * drivers (timer-mediatek.c, irqchip/mtk-sysirq.c).
	 */
	{
		out("\n--- MTK sysirq / sys_timer ---\n", CLR_YELLOW);

		for (unsigned i = 0; i < 8; i++) {
			out("  sysirq ", CLR_CYAN);
			outhex(0x10200B00UL + i * 4u);
			out("=", CLR_WHITE);
			outhex(dgic_read32(0x10200B00UL, i * 4u));
			fb_newline();
		}
		for (unsigned i = 0; i < 8; i++) {
			out("  gpt    ", CLR_CYAN);
			outhex(0x10017000UL + i * 4u);
			out("=", CLR_WHITE);
			outhex(dgic_read32(0x10017000UL, i * 4u));
			fb_newline();
		}
	}

	dgic_timer_test_clear(&r->gic, 27u);
	dgic_timer_test_clear(&r->gic, 30u);
	return;
}
