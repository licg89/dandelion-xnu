/* Dandelion platform test payload
 *
 * Runs at EL1 with the MMU off, immediately after the shim's handoff, and
 * exercises the platform expert piece by piece.  Everything is reported on
 * the framebuffer because the board has no accessible serial port.
 *
 * The payload is deliberately split into small steps: if one of them faults,
 * the shim's exception vector prints ESR/FAR and the last line on screen
 * tells us which register access went wrong.
 */
#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "handoff.h"
#include "pe_dandelion.h"

/* Platform expert entry points (C++ implementation, extern "C") */
void dandelion_uart_init(void);
void dandelion_console_init(void);
void dandelion_platform_setup_video(struct xnu_boot_args *args);
void dandelion_watchdog_disable(void);
void dandelion_timer_init(void);
uint64_t dandelion_timer_frequency(void);
uint64_t dandelion_timer_ticks(void);
int dandelion_gicv3_init(void);
int dandelion_gicv3_irq_enable(uint32_t irq);
int dandelion_gicv3_irq_disable(uint32_t irq);
uint32_t dandelion_gicv3_ack(void);
void dandelion_gicv3_eoi(uint32_t irq);

static uint64_t read_sysreg_current_el(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return v;
}

static uint64_t read_sysreg_mpidr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	return v;
}

static void put_int(const char *label, int value, uint32_t ok_color)
{
	if (value < 0) {
		fb_puts(label, CLR_RED);
		fb_puts(" -", CLR_RED);
		fb_putdec("", (uint64_t)(-value), CLR_RED);
	} else {
		fb_putdec(label, (uint64_t)value, ok_color);
	}
}

static void step_uart(void)
{
	fb_puts("\n[1] UART\n", CLR_YELLOW);
	dandelion_uart_init();
	dandelion_console_init();
	fb_puts("    init ok (writes to 0x", CLR_GREEN);
	fb_puthex("", DANDELION_UART_BASE, CLR_GREEN);
	fb_puts("    not wired to anything visible)\n", CLR_GREEN);
}

static void step_video(struct xnu_boot_args *args)
{
	fb_puts("\n[2] video handoff\n", CLR_YELLOW);
	dandelion_platform_setup_video(args);
	fb_puthex("    base    ", args->video.base, CLR_CYAN);
	fb_putdec("    width   ", args->video.width, CLR_CYAN);
	fb_putdec("    height  ", args->video.height, CLR_CYAN);
	fb_putdec("    rowbytes", args->video.rowbytes, CLR_CYAN);
	fb_putdec("    depth   ", args->video.depth, CLR_CYAN);
}

static void step_watchdog(void)
{
	fb_puts("\n[3] watchdog (TOPRGU)\n", CLR_YELLOW);
	dandelion_watchdog_disable();
	fb_puts("    disabled (0x", CLR_GREEN);
	fb_puthex("", DANDELION_WDT_BASE, CLR_GREEN);
	fb_puts("    no longer armed)\n", CLR_GREEN);
}

static void step_timer(void)
{
	fb_puts("\n[4] generic timer\n", CLR_YELLOW);

	uint64_t freq = dandelion_timer_frequency();
	if (freq == 0) {
		fb_putdec("    CNTFRQ  ", 0, CLR_RED);
		fb_puts("    FAILED: counter frequency is zero\n", CLR_RED);
		return;
	}
	fb_putdec("    CNTFRQ  ", freq, CLR_GREEN);

	dandelion_timer_init();

	uint64_t t0 = dandelion_timer_ticks();
	for (volatile uint32_t i = 0; i < 20000000u; i++)
		;
	uint64_t t1 = dandelion_timer_ticks();

	fb_puthex("    t0      ", t0, CLR_CYAN);
	fb_puthex("    t1      ", t1, CLR_CYAN);
	fb_puthex("    delta   ", t1 - t0, (t1 != t0) ? CLR_GREEN : CLR_RED);
	if (t1 == t0)
		fb_puts("    FAILED: counter does not advance\n", CLR_RED);

	/* Stop the timer again so it cannot make a PPI pending. */
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)0) : "memory");
	__asm__ volatile("isb" ::: "memory");
}

static void step_gic(void)
{
	fb_puts("\n[5] GICv3\n", CLR_YELLOW);
	fb_puthex("    GICD    ", DANDELION_GIC_BASE, CLR_CYAN);
	fb_puthex("    GICR    ", DANDELION_GICR_BASE, CLR_CYAN);

	/* Any bad access here faults and the shim prints ESR/FAR for us. */
	int rc = dandelion_gicv3_init();
	put_int("    init rc ", rc, (rc == 0) ? CLR_GREEN : CLR_RED);
	if (rc != 0) {
		fb_puts("    FAILED: distributor/redistributor init\n", CLR_RED);
		return;
	}

	/* No interrupt should be pending with DAIF masked: 1023 = spurious.
	 * If the timer already fired, seeing INTID 27 is also a good sign. */
	uint32_t ack = dandelion_gicv3_ack();
	int ack_ok = (ack == 1023) || (ack == DANDELION_TIMER_IRQ);
	fb_puthex("    ack     ", ack, ack_ok ? CLR_GREEN : CLR_YELLOW);
	if (ack == DANDELION_TIMER_IRQ) {
		dandelion_gicv3_eoi(ack);
		fb_puts("    (timer PPI fired)\n", CLR_GREEN);
	} else if (ack != 1023) {
		fb_puts("    note: expected 1023 (spurious)\n", CLR_YELLOW);
	}

	int en = dandelion_gicv3_irq_enable(DANDELION_TIMER_IRQ);
	put_int("    ppi en  ", en, (en == 0) ? CLR_GREEN : CLR_RED);

	int dis = dandelion_gicv3_irq_disable(DANDELION_TIMER_IRQ);
	put_int("    ppi dis ", dis, (dis == 0) ? CLR_GREEN : CLR_RED);
}

void platform_test_main(uint64_t boot_args_pa)
{
	fb_init();
	fb_clear(CLR_BLACK);

	fb_puts("=== DANDELION PLATFORM TEST ===\n", CLR_YELLOW);

	struct xnu_boot_args *args =
		(struct xnu_boot_args *)(uintptr_t)boot_args_pa;

	fb_puthex("boot_args", boot_args_pa, CLR_CYAN);
	fb_puthex("EL       ", read_sysreg_current_el() >> 2, CLR_CYAN);
	fb_puthex("MPIDR    ", read_sysreg_mpidr(), CLR_CYAN);

	if (args->revision != 2 || args->version != 2) {
		fb_putdec("revision ", args->revision, CLR_RED);
		fb_putdec("version  ", args->version, CLR_RED);
		fb_puts("FAILED: bad boot_args magic\n", CLR_RED);
		return;
	}

	fb_puthex("phys_base", args->phys_base, CLR_CYAN);
	fb_puthex("mem_size ", args->mem_size, CLR_CYAN);

	step_uart();
	step_video(args);
	step_watchdog();
	step_timer();
	step_gic();

	fb_puts("\n=== TEST FINISHED (no exception) ===\n", CLR_GREEN);
	fb_puts("Result stays on screen; reset to reboot.\n", CLR_WHITE);

	for (;;)
		__asm__ volatile("wfe");
}
