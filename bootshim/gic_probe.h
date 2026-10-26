/* Dandelion boot shim - interrupt controller inventory
 *
 * The kernel used to stop silently on the phone with no timer ticks.  The
 * shim runs before any kernel code, at EL1 with the MMU off, so it can dump
 * the distributor, the redistributors and the CPU interface exactly as the
 * firmware left them -- and prove whether a timer interrupt can be delivered
 * at all, without a working kernel interrupt path.
 *
 * The register work lives in pexpert/pexpert/arm64/dandelion_gic.h, which is
 * also what XNU's pexpert/arm/pe_fiq.c uses, so what this probe reports is
 * what the kernel will do.
 */
#ifndef DANDELION_GIC_PROBE_H
#define DANDELION_GIC_PROBE_H

#include "fdt.h"
#include "dandelion_gic.h"

struct gic_probe_result {
	struct dgic gic;
	int identify;                   /* dgic_identify() result            */
	int cpu_init;                   /* dgic_cpu_init() result            */
	const char *source;             /* "device DTB" or "DANDELION.h"     */
	int timer_group1;               /* 1 = timer PPI reached CPU interface */
	int timer_group0;               /* same, Group 0 / FIQ path           */
	int timer_fired_group1;         /* 1 = the timer itself expired       */
	int timer_fired_group0;
	int timer_group0_tried;
	unsigned redist_frames;
	unsigned long redist_base[16];
	unsigned long redist_typer[16];
	unsigned int redist_waker[16];
	unsigned long self_redist;
	unsigned int timer_irq;
};

/* Run the inventory and print it on the framebuffer. */
void gic_probe_run(const struct fdt_platform *platform,
		   struct gic_probe_result *result);

#endif
