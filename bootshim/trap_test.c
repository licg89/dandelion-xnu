/* Dandelion trap + MMU test payload
 *
 * Extends the platform test with the two pieces XNU needs before it can run
 * its own C code on this board:
 *
 *   1. real interrupt delivery -- install a vector table, enable the timer
 *      PPI in the GICv3 redistributor, unmask DAIF.I and count ten generic
 *      timer interrupts acknowledged through ICC_IAR1_EL1;
 *   2. a working exception model -- BRK and an unmapped load come back
 *      through the synchronous vector with the expected ESR_EL1;
 *   3. MMU on with a 39-bit identity map.  The 16 KiB granule is checked
 *      first for the record, but this part is a Cortex-A53 and has no 16 KiB
 *      granule (ID_AA64MMFR0_EL1.TGran16 == 0), so the 4 KiB map is the one
 *      XNU has to use.  See docs/MMU_GRANULE.md.
 *
 * Everything is reported on the framebuffer; the board has no visible UART.
 */
#include <stdint.h>
#include <stddef.h>

#include "fb.h"
#include "pe_dandelion.h"

/* Platform expert entry points (C++ implementation, extern "C") */
void dandelion_uart_init(void);
void dandelion_watchdog_disable(void);
uint64_t dandelion_timer_frequency(void);
uint64_t dandelion_timer_ticks(void);
int dandelion_gicv3_init(void);
void dandelion_gicv3_eoi(uint32_t irq);

/* trap.S */
void payload_install_vectors(void);
void payload_handle_irq(void);
uint64_t payload_handle_sync(uint64_t esr, uint64_t elr, uint64_t far,
    uint64_t spsr);
void payload_handle_unexpected(uint64_t esr, uint64_t elr, uint64_t far,
    uint64_t spsr);

/* ------------------------------------------------------------------ */
/* State shared with the handlers                                     */
/* ------------------------------------------------------------------ */

#define TIMER_IRQS 10

static volatile uint64_t g_freq;
static volatile unsigned g_irq_count;
static volatile uint32_t g_last_intid;
static volatile unsigned g_sync_count;
static volatile uint64_t g_last_esr;
static volatile uint64_t g_last_elr;
static volatile uint64_t g_last_far;

/* Page tables live above the kernel image and boot_args, inside the 384 MiB
 * region the shim claims (0x60000000..0x78000000). */
#define TABLE4K_BASE  0x61000000UL	/* L1 + 4 x L2, 4 KiB each   */
#define TABLE16K_BASE 0x61100000UL	/* L1 + L2, 16 KiB each      */

#define DESC_TABLE  0x3ULL
#define DESC_BLOCK  0x1ULL
#define DESC_AF     (1ULL << 10)
#define DESC_SH_INNER (3ULL << 8)
#define DESC_UXN    (1ULL << 54)

#define ATTR_NORMAL 0
#define ATTR_DEVICE 1

/* 4 KiB granule: an L2 block covers 2 MiB.
 * 16 KiB granule: an L2 block covers 32 MiB. */
#define BLOCK4K_SIZE  (2ULL << 20)
#define BLOCK16K_SIZE (32ULL << 20)

static uint64_t read_sctlr(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
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

/* ------------------------------------------------------------------ */
/* Interrupt handlers (called from trap.S with a saved frame on stack) */
/* ------------------------------------------------------------------ */

void payload_handle_irq(void)
{
	uint64_t iar;
	__asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));

	uint32_t intid = (uint32_t)(iar & 0xffffffu);
	if (intid < 1020u) {
		g_irq_count = g_irq_count + 1;
		g_last_intid = intid;

		/* Re-arm the virtual timer for another 10 ms. */
		uint64_t tval = g_freq / 100u;
		__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(tval) : "memory");
		dandelion_gicv3_eoi(intid);
	}
}

uint64_t payload_handle_sync(uint64_t esr, uint64_t elr, uint64_t far,
    uint64_t spsr)
{
	(void)spsr;
	uint32_t ec = (uint32_t)(esr >> 26);

	g_sync_count = g_sync_count + 1;
	g_last_esr = esr;
	g_last_elr = elr;
	g_last_far = far;

	/* BRK (#0x3c) and data abort (#0x25) are the two we inject. */
	if (ec == 0x3cu || ec == 0x25u)
		return elr + 4;

	fb_puts("\n!!! UNEXPECTED SYNC EXCEPTION\n", CLR_RED);
	fb_puthex("    ESR ", esr, CLR_RED);
	fb_puthex("    ELR ", elr, CLR_RED);
	fb_puthex("    FAR ", far, CLR_RED);
	for (;;)
		__asm__ volatile("wfe");
}

void payload_handle_unexpected(uint64_t esr, uint64_t elr, uint64_t far,
    uint64_t spsr)
{
	fb_puts("\n!!! UNEXPECTED EXCEPTION\n", CLR_RED);
	fb_puthex("    ESR ", esr, CLR_RED);
	fb_puthex("    ELR ", elr, CLR_RED);
	fb_puthex("    FAR ", far, CLR_RED);
	fb_puthex("    SPSR", spsr, CLR_RED);
	for (;;)
		__asm__ volatile("wfe");
}

/* ------------------------------------------------------------------ */
/* Steps                                                              */
/* ------------------------------------------------------------------ */

static void step_vectors(void)
{
	fb_puts("\n[1] exception vectors\n", CLR_YELLOW);
	payload_install_vectors();

	uint64_t vbar;
	__asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
	fb_puthex("    VBAR_EL1     ", vbar, (vbar & 0x7ffu) == 0 ? CLR_GREEN : CLR_RED);
}

static void step_irqs(void)
{
	fb_puts("\n[2] GICv3 + timer interrupt delivery\n", CLR_YELLOW);
	dandelion_watchdog_disable();

	int rc = dandelion_gicv3_init();
	put_int("    gic init rc  ", rc, rc == 0 ? CLR_GREEN : CLR_RED);
	if (rc != 0) {
		fb_puts("    FAILED: cannot test interrupts\n", CLR_RED);
		return;
	}

	g_freq = dandelion_timer_frequency();
	g_irq_count = 0;
	g_last_intid = 0;

	/* 10 ms period on the virtual timer; PPI already enabled by init. */
	uint64_t tval = g_freq / 100u;
	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(tval) : "memory");
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)1u) : "memory");
	__asm__ volatile("isb" ::: "memory");

	fb_puthex("    CNTFRQ_EL0   ", g_freq, CLR_CYAN);
	fb_puthex("    TVAL         ", tval, CLR_CYAN);

	fb_puts("    unmasking IRQ, waiting for interrupts...\n", CLR_WHITE);
	__asm__ volatile("msr daifclr, #2" ::: "memory");

	uint64_t start = dandelion_timer_ticks();
	while (g_irq_count < TIMER_IRQS) {
		if (dandelion_timer_ticks() - start > g_freq * 3u)
			break;
	}

	/* Mask again before touching anything else. */
	__asm__ volatile("msr daifset, #2" ::: "memory");
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)0u) : "memory");
	__asm__ volatile("isb" ::: "memory");

	unsigned count = g_irq_count;
	put_int("    interrupts   ", (int)count,
	    count == TIMER_IRQS ? CLR_GREEN : CLR_RED);
	put_int("    last INTID   ", (int)g_last_intid,
	    g_last_intid == DANDELION_TIMER_IRQ ? CLR_GREEN : CLR_RED);
	if (g_last_intid != DANDELION_TIMER_IRQ)
		fb_puts("    note: expected the timer PPI\n", CLR_YELLOW);
}

static void step_brk(void)
{
	fb_puts("\n[3] BRK exception\n", CLR_YELLOW);

	unsigned before = g_sync_count;
	__asm__ volatile("brk #0x1234");

	int ok = g_sync_count == before + 1u &&
	    ((g_last_esr >> 26) & 0x3fu) == 0x3cu;

	fb_puthex("    ESR_EL1      ", g_last_esr, ok ? CLR_GREEN : CLR_RED);
	fb_puthex("    ELR_EL1      ", g_last_elr, CLR_CYAN);
	fb_puts(ok ? "    BRK handled (EC=0x3c)\n"
		   : "    FAILED: wrong exception class\n",
	    ok ? CLR_GREEN : CLR_RED);
}

/*
 * Invalidate the whole data cache by set/way before turning the caches on.
 * LK ran with caches enabled and may have left lines for the physical memory
 * the shim and this payload now use; those would shadow our freshly written
 * page tables once the MMU starts using them.
 */
static void dcache_invalidate_all(void)
{
	uint64_t clidr;
	__asm__ volatile("mrs %0, clidr_el1" : "=r"(clidr));
	unsigned loc = (unsigned)((clidr >> 24) & 7u);
	if (loc == 0)
		return;

	__asm__ volatile("dsb sy" ::: "memory");

	for (unsigned level = 0; level < loc; level++) {
		uint32_t ctype = (uint32_t)((clidr >> (3 * level)) & 7u);
		/* 0 = no cache, 1 = instruction only; 2/3/4 all have data. */
		if (ctype == 0 || ctype == 1)
			continue;

		__asm__ volatile("msr csselr_el1, %0"
		    : : "r"((uint64_t)level << 1) : "memory");
		__asm__ volatile("isb" ::: "memory");

		uint64_t ccsidr;
		__asm__ volatile("mrs %0, ccsidr_el1" : "=r"(ccsidr));

		unsigned line_shift = (unsigned)(ccsidr & 7u) + 4;
		unsigned ways = (unsigned)(((ccsidr >> 3) & 0x3ffu) + 1u);
		unsigned sets = (unsigned)(((ccsidr >> 13) & 0x7fffu) + 1u);
		unsigned way_shift = 32u - (unsigned)__builtin_ctz(ways);

		for (unsigned w = 0; w < ways; w++) {
			for (unsigned s = 0; s < sets; s++) {
				uint64_t setway = ((uint64_t)w << way_shift) |
				    ((uint64_t)s << line_shift);
				__asm__ volatile("dc isw, %0"
				    : : "r"(setway) : "memory");
			}
		}
	}

	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}

/*
 * Identity map the first 4 GiB, 39-bit VA (T0SZ=25), 4 KiB granule:
 *   L1[0..3] -> four L2 tables, each L2 entry is a 2 MiB block.
 *   0x00000000-0x3fffffff  device (GIC, UART, TOPRGU)
 *   0x40000000-0x7bffffff  normal (DRAM, kernel, page tables)
 *   0x7c000000-0x7fffffff  device (LK framebuffer carveout)
 */
static void build_tables_4k(void)
{
	volatile uint64_t *l1 = (volatile uint64_t *)(uintptr_t)TABLE4K_BASE;

	for (unsigned i = 0; i < 512; i++)
		l1[i] = 0;

	for (unsigned gb = 0; gb < 4; gb++) {
		uint64_t l2_base = TABLE4K_BASE + 0x1000UL +
		    (uint64_t)gb * 0x1000UL;
		volatile uint64_t *l2 = (volatile uint64_t *)(uintptr_t)l2_base;

		l1[gb] = l2_base | DESC_TABLE;

		for (unsigned i = 0; i < 512; i++) {
			uint64_t address = ((uint64_t)gb << 30) |
			    ((uint64_t)i * BLOCK4K_SIZE);
			int device = (gb == 0) ||
			    (gb == 1 && address >= 0x7c000000ULL);

			l2[i] = address | DESC_BLOCK | DESC_AF |
			    DESC_SH_INNER |
			    ((device ? ATTR_DEVICE : ATTR_NORMAL) << 2) |
			    DESC_UXN;
		}
	}
}

/*
 * Same map with a 16 KiB granule, 39-bit VA:
 *   L1[0] -> L2, L2[i] = 32 MiB blocks.
 */
static void build_tables_16k(void)
{
	volatile uint64_t *l1 = (volatile uint64_t *)(uintptr_t)TABLE16K_BASE;
	volatile uint64_t *l2 =
	    (volatile uint64_t *)(uintptr_t)(TABLE16K_BASE + 0x4000UL);

	for (unsigned i = 0; i < 2048; i++) {
		l1[i] = 0;
		l2[i] = 0;
	}

	l1[0] = (TABLE16K_BASE + 0x4000UL) | DESC_TABLE;

	for (unsigned i = 0; i < 128; i++) {
		uint64_t address = (uint64_t)i * BLOCK16K_SIZE;
		int device = (i < 32) || (i >= 62);

		l2[i] = address | DESC_BLOCK | DESC_AF | DESC_SH_INNER |
		    ((device ? ATTR_DEVICE : ATTR_NORMAL) << 2) | DESC_UXN;
	}
}

/* T0SZ=25 (39-bit VA), requested TG0, inner-shareable WB/WB, EPD1,
 * IPS=40-bit.  TG0 is 0 for 4 KiB and 2 for 16 KiB. */
static uint64_t tcr_value(uint64_t tg0)
{
	return 25ULL | (tg0 << 14) | (3ULL << 12) | (1ULL << 10) |
	    (1ULL << 8) | (25ULL << 16) | (1ULL << 23) | (2ULL << 32);
}

/*
 * Program TCR/TTBR0 and switch the MMU on with the caches still disabled.
 * Everything up to the first fetch after the ISB is reached only if the
 * tables are interpreted the way we built them, so a hang here points at
 * the table layout (or the granule) rather than at cache attributes.
 */
static void mmu_try(uint64_t tcr, uint64_t ttbr)
{
	uint64_t mair = 0xffULL;	/* attr0 = normal WB, attr1 = device */

	__asm__ volatile("msr mair_el1, %0" : : "r"(mair) : "memory");
	__asm__ volatile("msr tcr_el1, %0" : : "r"(tcr) : "memory");
	__asm__ volatile("msr ttbr0_el1, %0" : : "r"(ttbr) : "memory");
	__asm__ volatile("msr ttbr1_el1, %0" : : "r"((uint64_t)0) : "memory");
	__asm__ volatile("isb" ::: "memory");

	uint64_t tcr_rb, ttbr_rb;
	__asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr_rb));
	__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr_rb));

	fb_puthex("      TCR_EL1    ", tcr_rb, CLR_CYAN);
	fb_putdec("      TG0        ", (tcr_rb >> 14) & 3ULL,
	    ((tcr_rb >> 14) & 3ULL) == ((tcr >> 14) & 3ULL) ?
	    CLR_GREEN : CLR_RED);
	fb_puthex("      TTBR0_EL1  ", ttbr_rb, CLR_CYAN);

	/* Tables were written with the caches off; drop anything stale. */
	dcache_invalidate_all();
	__asm__ volatile("tlbi vmalle1" ::: "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("ic iallu" ::: "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("isb" ::: "memory");

	uint64_t sctlr = read_sctlr() | (1ULL << 0);	/* M = 1 */
	fb_puts("      enabling MMU...\n", CLR_CYAN);
	__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
	__asm__ volatile("isb" ::: "memory");
	fb_puts("      MMU on\n", CLR_GREEN);
}

static void mmu_off(void)
{
	__asm__ volatile("msr sctlr_el1, %0"
	    : : "r"(read_sctlr() & ~(1ULL << 0)) : "memory");
	__asm__ volatile("isb" ::: "memory");
}

static void step_mmu(void)
{
	fb_puts("\n[4] MMU (39-bit VA, identity map)\n", CLR_YELLOW);

	/* 4 KiB first: the most basic granule, so if this fails the problem is
	 * in the tables or attributes rather than in the 16 KiB walk. */
	volatile uint64_t *l4 = (volatile uint64_t *)(uintptr_t)TABLE4K_BASE;
	volatile uint64_t *l4_0 =
	    (volatile uint64_t *)(uintptr_t)(TABLE4K_BASE + 0x1000UL);
	volatile uint64_t *l4_1 =
	    (volatile uint64_t *)(uintptr_t)(TABLE4K_BASE + 0x2000UL);

	fb_puts("    4K granule\n", CLR_WHITE);
	build_tables_4k();
	fb_puthex("      L1[0]      ", l4[0], CLR_CYAN);
	fb_puthex("      L2gb0[0]   ", l4_0[0], CLR_CYAN);
	fb_puthex("      L2gb1[256] ", l4_1[256], CLR_CYAN);
	fb_puthex("      L2gb1[480] ", l4_1[480], CLR_CYAN);

	mmu_try(tcr_value(0), TABLE4K_BASE);
	fb_puts("    4K MMU works\n", CLR_GREEN);
	mmu_off();
	fb_puts("      MMU off again\n", CLR_CYAN);

	fb_puts("    16K granule\n", CLR_WHITE);
	build_tables_16k();
	volatile uint64_t *l16 = (volatile uint64_t *)(uintptr_t)TABLE16K_BASE;
	volatile uint64_t *l16_l2 =
	    (volatile uint64_t *)(uintptr_t)(TABLE16K_BASE + 0x4000UL);
	fb_puthex("      L1[0]      ", l16[0], CLR_CYAN);
	fb_puthex("      L2[32]     ", l16_l2[32], CLR_CYAN);
	fb_puthex("      L2[48]     ", l16_l2[48], CLR_CYAN);
	fb_puthex("      L2[63]     ", l16_l2[63], CLR_CYAN);

	mmu_try(tcr_value(2), TABLE16K_BASE);
	fb_puts("    16K MMU works\n", CLR_GREEN);

	/* Translation is known good now, so turn the caches on too. */
	uint64_t sctlr = read_sctlr() | (1ULL << 1) | (1ULL << 2) |
	    (1ULL << 12);
	fb_puts("      enabling caches...\n", CLR_CYAN);
	__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
	__asm__ volatile("isb" ::: "memory");
	fb_puts("      MMU + caches on\n", CLR_GREEN);

	sctlr = read_sctlr();
	fb_puthex("      SCTLR_EL1  ", sctlr, (sctlr & 1u) ? CLR_GREEN : CLR_RED);
}

static void step_abort_after_mmu(void)
{
	fb_puts("\n[5] data abort while the MMU is on\n", CLR_YELLOW);

	unsigned before = g_sync_count;

	/* Bit 38 set: inside the 39-bit VA space, but L1[4] is not mapped. */
	volatile uint64_t *bad =
	    (volatile uint64_t *)(uintptr_t)0x4000000000ULL;
	uint64_t value = *bad;
	(void)value;

	uint32_t ec = (uint32_t)((g_last_esr >> 26) & 0x3fu);
	uint32_t fsc = (uint32_t)(g_last_esr & 0x3fu);
	int ok = g_sync_count == before + 1u && ec == 0x25u && fsc == 0x05u;

	fb_puthex("    ESR_EL1      ", g_last_esr, ok ? CLR_GREEN : CLR_RED);
	fb_puthex("    ELR_EL1      ", g_last_elr, CLR_CYAN);
	fb_puthex("    FAR_EL1      ", g_last_far, CLR_CYAN);
	fb_puts(ok ? "    translation fault, level 1 (expected)\n"
		   : "    FAILED: unexpected fault status\n",
	    ok ? CLR_GREEN : CLR_RED);
}

static void step_brk_after_mmu(void)
{
	fb_puts("\n[6] BRK with the MMU on\n", CLR_YELLOW);

	unsigned before = g_sync_count;
	__asm__ volatile("brk #0x5678");

	int ok = g_sync_count == before + 1u &&
	    ((g_last_esr >> 26) & 0x3fu) == 0x3cu &&
	    (g_last_esr & 0xffffu) == 0x5678u;

	fb_puthex("    ESR_EL1      ", g_last_esr, ok ? CLR_GREEN : CLR_RED);
	fb_puts(ok ? "    BRK handled (EC=0x3c, ISS=0x5678)\n"
		   : "    FAILED: wrong ESR\n",
	    ok ? CLR_GREEN : CLR_RED);
}

void trap_test_main(uint64_t boot_args_pa)
{
	(void)boot_args_pa;

	fb_init();
	fb_clear(CLR_BLACK);

	fb_puts("=== DANDELION TRAP / MMU TEST ===\n", CLR_YELLOW);

	uint64_t current_el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
	fb_putdec("EL", (current_el >> 2) & 3u, CLR_CYAN);

	dandelion_uart_init();

	step_vectors();
	step_irqs();
	step_brk();
	step_mmu();
	if ((read_sctlr() & 1u) != 0) {
		step_abort_after_mmu();
		step_brk_after_mmu();
	}

	fb_puts("\n=== TRAP TEST FINISHED ===\n", CLR_GREEN);
	fb_puts("Result stays on screen; reset to reboot.\n", CLR_WHITE);

	for (;;)
		__asm__ volatile("wfe");
}
