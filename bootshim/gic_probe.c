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
	unsigned int count = 0;

	r->redist_frames = 0;
	if (r->gic.mode != DGIC_MODE_V3 || frame == 0 || r->gic.gicr_size == 0) {
		return;
	}

	while (frame + 0x20000ul <= end && count < PROBE_REDIST_MAX) {
		unsigned long typer = dgic_read64(frame, DGICR_TYPER);
		unsigned int waker = dgic_read32(frame, DGICR_WAKER);

		r->redist_base[count] = frame;
		r->redist_typer[count] = typer;
		r->redist_waker[count] = waker;
		count++;

		out("  frame ", CLR_CYAN);
		outhex(frame);
		out(" typer ", CLR_CYAN);
		outhex(typer);
		out(" waker ", CLR_CYAN);
		outhex(waker);
		out((waker & DGICR_WAKER_CHILDRENASLEEP) ? "  (asleep)\n" :
		    "  (awake)\n", CLR_WHITE);

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

	/* Full bring-up, exactly like the kernel does it. */
	r->cpu_init = dgic_cpu_init(&r->gic);
	out("  cpu init ", CLR_CYAN);
	if (r->cpu_init == DGIC_OK) {
		out("ok\n", CLR_GREEN);
	} else {
		out("failed ", CLR_RED);
		outdec((unsigned long)(-r->cpu_init));
		fb_newline();
	}

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
	 * The decisive part: arm the generic timer and ask the CPU interface
	 * whether the PPI arrived.  Group 1 is always tried.  Group 0 is only
	 * touched when the GIC is single-security-state, because on a
	 * two-state GIC those registers belong to EL3 and an access from EL1
	 * takes an undefined-instruction exception.
	 */
	r->timer_group1 = timer_test_for_group(&r->gic, DGIC_GROUP1, irq, 1);
	report_timer("timer -> IRQ (gp1) ", r->timer_group1,
	    &r->timer_group1, &r->timer_fired_group1,
	    "PENDING - delivery works\n", "never pending - NO interrupt");

	if (r->gic.mode == DGIC_MODE_V3 && !r->gic.secure_extn) {
		int result;

		r->timer_group0_tried = 1;
		result = timer_test_for_group(&r->gic, DGIC_GROUP0, irq, 0);
		report_timer("timer -> FIQ (gp0) ", result,
		    &r->timer_group0, &r->timer_fired_group0,
		    "PENDING - FIQ path works\n", "never pending - NO FIQ");
	} else if (r->gic.mode == DGIC_MODE_V2) {
		out("  timer -> FIQ (gp0) skipped: GICv2 has no non-secure Group0\n",
		    CLR_YELLOW);
	} else {
		out("  timer -> FIQ (gp0) skipped: Group0 is secure\n", CLR_YELLOW);
	}

	/* Leave the controller in the group the kernel will pick. */
	r->gic.group = r->gic.secure_extn ? DGIC_GROUP1 : DGIC_GROUP0;
	(void)dgic_cpuif_init(&r->gic);
	dgic_timer_test_clear(&r->gic, irq);

	out("  verdict  ", CLR_CYAN);
	if (r->timer_group1 == 1 || r->timer_group0 == 1) {
		out("interrupt delivery OK\n", CLR_GREEN);
	} else {
		out("no interrupt delivery - the kernel will not tick\n", CLR_RED);
	}
}
