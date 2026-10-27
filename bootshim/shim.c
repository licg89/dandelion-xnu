/* Dandelion boot shim v2 - self-contained with embedded test kernel
 * No external payload needed. Tests full handoff flow.
 */
#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "fdt.h"
#include "gic_probe.h"
#include "handoff.h"
#include "log.h"
#include "msdc.h"
#include "pstore.h"
#include "usb_cdc.h"
#include "pe_dandelion.h"

#define READ_SYSREG(reg) ({ uint64_t _v; __asm__ volatile("mrs %0, " #reg : "=r"(_v)); _v; })

/* Redirect fb_puts to also write to pstore */
#define LOG(s, fg) do { fb_puts(s, fg); ps_puts(s); } while(0)
#define LOG_HEX(label, val, fg) do { fb_puthex(label, val, fg); ps_puthex(label, val); } while(0)
#define LOG_DEC(label, val, fg) do { fb_putdec(label, val, fg); ps_putdec(label, val); } while(0)

__attribute__((noreturn)) extern void enter_el1(uint64_t dtb, uint64_t gic_v3);

/*
 * Default build: a two-instruction stand-in kernel in .test_kernel.
 *
 * With -DPLATFORM_TEST_BLOB the build script supplies the same symbols from
 * a generated assembly file that .incbin's the real platform test payload.
 */
#if !defined(PLATFORM_TEST_BLOB) && !defined(XNU_HANDOFF)
__asm__(
	".section .test_kernel, \"ax\"\n"
	".globl test_kernel_start\n"
	"test_kernel_start:\n"
	"1: wfe\n"
	"   b 1b\n"
	".globl test_kernel_end\n"
	"test_kernel_end:\n"
);
#endif
#ifndef XNU_HANDOFF
extern char test_kernel_start[], test_kernel_end[];
#endif

static struct fdt_info fdt_info;
static struct fdt_platform fdt_platform_info;
static int fdt_gic_v3 = 1;      /* does the GIC implement GICv3 sysregs? */

/* Tiny substring search: the compat string decides whether the CPU has a
 * GICv3 system register interface, and there is no libc here. */static int contains(const char *haystack, const char *needle)
{
	for (; *haystack; haystack++) {
		const char *h = haystack, *n = needle;
		while (*n && *h == *n) { h++; n++; }
		if (!*n) return 1;
	}
	return 0;
}

__attribute__((noreturn)) static void stop(void)
{
    _ps_flush();  /* Write logs to pstore */

    /* Auto-reboot after 10 seconds */
    fb_puts("\nAuto-rebooting in 10s...\n", CLR_YELLOW);
    ps_puts("\nAuto-rebooting...\n");
    _ps_flush();

    /* Wait 10 seconds */
    for (volatile int i = 0; i < 200000000; i++);

    /* Try PSCI SYSTEM_RESET */
    __asm__ volatile(
        "ldr w0, =0x84000009\n"  /* PSCI SYSTEM_RESET (32-bit) */
        "smc #0\n"
    );

    /* If PSCI failed, try watchdog */
    fb_puts("PSCI failed, trying watchdog...\n", CLR_RED);
    *(volatile uint32_t *)0x10007014 = 0x1209;
    *(volatile uint32_t *)0x10007014 = 0x1209;

    for (;;) __asm__ volatile("wfe");
}

__attribute__((noreturn)) void exception_report(uint64_t slot)
{
	/* Try to output exception info to framebuffer */
	fb_init();
	fb_clear(CLR_BLACK);
	fb_puts("EXCEPTION!\n", CLR_RED);
	fb_putdec("Vector=", slot, CLR_RED);
	fb_puthex("CurrentEL=", READ_SYSREG(CurrentEL), CLR_RED);
	uint64_t el = READ_SYSREG(CurrentEL);
	if (el == 8) {
		fb_puthex("ESR_EL2=", READ_SYSREG(ESR_EL2), CLR_RED);
		fb_puthex("ELR_EL2=", READ_SYSREG(ELR_EL2), CLR_RED);
		fb_puthex("FAR_EL2=", READ_SYSREG(FAR_EL2), CLR_RED);
	} else {
		fb_puthex("ESR_EL1=", READ_SYSREG(ESR_EL1), CLR_RED);
		fb_puthex("ELR_EL1=", READ_SYSREG(ELR_EL1), CLR_RED);
		fb_puthex("FAR_EL1=", READ_SYSREG(FAR_EL1), CLR_RED);
	}
	fb_puts("\nAuto-rebooting to fastboot...\n", CLR_RED);
	stop();
}

static void show_cpu_info(int icc_v3)
{
	uint64_t sre2_in, sre2_out;

	fb_puts("--- CPU ---\n", CLR_YELLOW);
	fb_puthex("CurrentEL  ", READ_SYSREG(CurrentEL), CLR_CYAN);
	fb_puthex("MIDR_EL1   ", READ_SYSREG(MIDR_EL1), CLR_CYAN);
	fb_puthex("MPIDR_EL1  ", READ_SYSREG(MPIDR_EL1), CLR_CYAN);
	fb_puthex("SCTLR_EL1  ", READ_SYSREG(SCTLR_EL1), CLR_CYAN);
	fb_puthex("CNTFRQ_EL0 ", READ_SYSREG(CNTFRQ_EL0), CLR_CYAN);

	if (!icc_v3) {
		/*
		 * ICC_SRE_EL2 only exists when the GIC implements the v3
		 * system register interface.  On a GICv2 (or a GICv3 forced
		 * into v2 compatibility mode with the interface disabled) this
		 * MRS is UNDEFINED, so never touch it unless the device tree
		 * says arm,gic-v3.
		 */
		fb_puts("SRE_EL2     skipped (GIC is not arm,gic-v3)\n", CLR_YELLOW);
		return;
	}

	/*
	 * GICv3 system register interface.  EL1 may only touch ICC_* once
	 * ICC_SRE_EL2.Enable (bit 3) is set; if EL3 has made that bit
	 * read-only zero this write will not stick and every ICC_* access
	 * from EL1 is UNDEFINED.
	 */
	__asm__ volatile("mrs %0, icc_sre_el2" : "=r"(sre2_in));
	__asm__ volatile("msr icc_sre_el2, %0" :: "r"((uint64_t)0xf));
	__asm__ volatile("isb");
	__asm__ volatile("mrs %0, icc_sre_el2" : "=r"(sre2_out));
	fb_puthex("SRE_EL2 in ", sre2_in, CLR_CYAN);
	fb_puthex("SRE_EL2 out", sre2_out,
		  (sre2_out & 8) ? CLR_GREEN : CLR_RED);
}

static void show_fdt_info(uint64_t dtb)
{
	fb_puts("\n--- FDT ---\n", CLR_YELLOW);
	fb_puthex("DTB_ADDR   ", dtb, CLR_CYAN);
	if (dtb < 0x40000000 || dtb > 0xbfffffff) {
		fb_puts("DTB: out of range!\n", CLR_RED);
		return;
	}
	uint32_t magic = *(volatile uint32_t *)(uintptr_t)dtb;
	uint32_t magic_be = ((magic>>24)&0xff)|((magic>>8)&0xff00)|
			    ((magic<<8)&0xff0000)|((magic<<24)&0xff000000);
	fb_puthex("DTB_MAGIC  ", magic_be, magic_be==0xd00dfeed ? CLR_GREEN : CLR_RED);
	if (magic_be != 0xd00dfeed) return;
	uint32_t total = *(volatile uint32_t *)((uintptr_t)dtb + 4);
	total = ((total>>24)&0xff)|((total>>8)&0xff00)|
		((total<<8)&0xff0000)|((total<<24)&0xff000000);
	fb_putdec("DTB_SIZE   ", total, CLR_CYAN);
	int rc = fdt_inspect((const void *)(uintptr_t)dtb, total, &fdt_info);
	fb_putdec("FDT_PARSE  ", rc==0?1:0, rc==0 ? CLR_GREEN : CLR_RED);
	fb_putdec("MEM_REGIONS", fdt_info.memory_count, CLR_CYAN);
	fb_putdec("RESERVED   ", fdt_info.reserved_count, CLR_CYAN);
	for (unsigned i = 0; i < fdt_info.memory_count; i++) {
		fb_puts("  MEM: ", CLR_CYAN);
		fb_puthex("", fdt_info.memory[i].base, CLR_WHITE);
	}
}

/*
 * Report the interrupt controller / timer / UART / watchdog / CPU layout as
 * described by the device's own DTB.  The reference port (MT8167) uses
 * different addresses, so this is the ground truth for Dandelion.
 */
static void show_platform_probe(uint64_t dtb)
{
	struct fdt_platform *p = &fdt_platform_info;

	(void)dtb;
	fb_puts("\n--- PLATFORM (device DTB) ---\n", CLR_YELLOW);

	if (!p->gic.found && !p->timer.found && !p->uart.found &&
	    !p->wdt.found && !p->cpus.count) {
		fb_puts("probe: DTB parse failed\n", CLR_RED);
		return;
	}

	if (p->gic.found) {
		int ok = p->gic.regions >= 2 &&
			 p->gic.base[0] == DANDELION_GIC_BASE &&
			 p->gic.base[1] == DANDELION_GICR_BASE;
		fb_puts("GIC  ", CLR_CYAN);
		fb_puts(p->gic.compatible, CLR_WHITE);
		fb_puts(ok ? " = DANDELION.h\n" : " != DANDELION.h\n",
			ok ? CLR_GREEN : CLR_RED);
		for (unsigned i = 0; i < p->gic.regions; i++) {
			fb_puthex("  base    ", p->gic.base[i], CLR_CYAN);
			fb_puthex("  size    ", p->gic.size[i], CLR_CYAN);
		}
	} else {
		fb_puts("GIC: not described in DTB\n", CLR_RED);
	}

	if (p->timer.found) {
		fb_putdec("timer Hz ", p->timer.frequency,
			  p->timer.frequency == DANDELION_TIMER_FREQ ?
			  CLR_GREEN : CLR_YELLOW);
		for (unsigned i = 0; i < p->timer.irq_count; i++)
			fb_putdec("  intid   ", p->timer.irq[i],
				  p->timer.irq[i] == DANDELION_TIMER_IRQ ?
				  CLR_GREEN : CLR_CYAN);
	} else {
		fb_puts("timer: not described in DTB\n", CLR_RED);
	}

	if (p->uart.found) {
		fb_puthex("uart base", p->uart.base,
			  p->uart.base == DANDELION_UART_BASE ?
			  CLR_GREEN : CLR_RED);
		fb_puthex("  size   ", p->uart.size, CLR_CYAN);
	} else {
		fb_puts("uart: not described in DTB\n", CLR_RED);
	}

	if (p->wdt.found) {
		fb_puthex("wdt base ", p->wdt.base,
			  p->wdt.base == DANDELION_WDT_BASE ?
			  CLR_GREEN : CLR_RED);
		fb_puthex("  size   ", p->wdt.size, CLR_CYAN);
	} else {
		fb_puts("wdt: not described in DTB\n", CLR_RED);
	}

	fb_putdec("cpus     ", p->cpus.count,
		  p->cpus.count == DANDELION_NCPUS ? CLR_GREEN : CLR_YELLOW);
	for (unsigned i = 0; i < p->cpus.count; i++)
		fb_puthex("  mpidr  ", p->cpus.mpidr[i], CLR_CYAN);
	if (p->cpus.enable_method[0]) {
		fb_puts("enable   ", CLR_CYAN);
		fb_puts(p->cpus.enable_method, CLR_WHITE);
		fb_newline();
	}
}

#ifdef MSDC_LOG
/*
 * Persistent log channel.
 *
 * The text is taken from the ramoops mirror rather than from the framebuffer:
 * pstore.h already appends every byte that goes through fb_puts(), it is plain
 * ASCII independent of where fb scrolling left the cursor, and it survives the
 * panel scrolling past.
 *
 * The same bring-up also serves the kernel: it claims a free run inside
 * flashinfo for the kernel's own console log and publishes its LBA, so the
 * kernel never has to touch the card's initialisation (bootshim/msdc.c and
 * docs/EMMC-LOG.md).
 */
#define MSDC_SHIM_SPAN (128u * 1024u)

static struct msdc_report g_emmc_report;
static int g_emmc_prepared;

static uint32_t
captured_log(const char **text)
{
	volatile uint32_t *header = (volatile uint32_t *)(uintptr_t)PSTORE_BASE;
	uint32_t length = header[2];

	if (header[0] != PS_SIG || length < 16 || length > PS_MAX)
		return 0;
	*text = (const char *)(uintptr_t)(PSTORE_BASE + PS_HDR_SIZE);
	return length;
}

/*
 * Stage MSDC0 and publish the kernel's region.  Runs before the hand-off in
 * every mode, because the kernel cannot do this itself: inheriting LK's card
 * state is a one-shot opportunity and the shim is the stage that can still
 * report a failure on the panel.
 */
static void
emmc_boot_setup(void)
{
	uint32_t kernel_lba = 0;
	int rc;

	rc = msdc_log_prepare(&g_emmc_report, MSDC_SHIM_SPAN);
	g_emmc_prepared = rc == 0;
	if (g_emmc_prepared)
		rc = msdc_reserve_kernel_region(&g_emmc_report, &kernel_lba,
						DANDELION_EMMC_KLOG_SECTORS);

	fb_puts("eMMC log: ", CLR_CYAN);
	if (rc == 0) {
		fb_putdec("shim slot ", g_emmc_report.target_lba, CLR_GREEN);
		fb_putdec("kernel slot ", kernel_lba, CLR_GREEN);
	} else {
		fb_puthex("unavailable, rc ", (unsigned long)(unsigned)rc, CLR_RED);
	}
	msdc_publish(rc == 0, kernel_lba, DANDELION_EMMC_KLOG_SECTORS);
}

static void
dump_log_to_emmc(void)
{
	const char *text = 0;
	uint32_t length;
	int rc;

	fb_puts("\n--- eMMC log channel (flashinfo) ---\n", CLR_YELLOW);
	msdc_report_print(&g_emmc_report);

	if (!g_emmc_prepared) {
		fb_puts("  prepare failed, nothing written\n", CLR_RED);
		return;
	}

	length = captured_log(&text);
	if (!length) {
		fb_puts("  no captured log text\n", CLR_RED);
		return;
	}

	rc = msdc_log_commit(&g_emmc_report, text, length);
	fb_puthex("  commit rc", (unsigned long)(unsigned)rc, CLR_WHITE);
	if (rc) {
		fb_puts("  LOG NOT WRITTEN\n", CLR_RED);
		return;
	}
	fb_puts("  LOG ON MEDIUM\n", CLR_GREEN);

	/* Second pass: the log now contains its own report and the line above. */
	length = captured_log(&text);
	if (length) {
		rc = msdc_log_commit(&g_emmc_report, text, length);
		fb_puthex("  pass2 rc ", (unsigned long)(unsigned)rc, CLR_WHITE);
		if (!rc)
			fb_puts("  final log on medium\n", CLR_GREEN);
	}
}
#endif /* MSDC_LOG */

__attribute__((noreturn)) void shim_el1(uint64_t dtb)
{	fb_puts("\n=== EL1 DIAGNOSTIC ===\n", CLR_YELLOW);
	fb_puthex("CurrentEL  ", READ_SYSREG(CurrentEL), CLR_CYAN);
	fb_puts("EL1 transition OK!\n", CLR_GREEN);

	/*
	 * Interrupt controller inventory.  This is the part of the boot that
	 * decides whether XNU can ever receive a timer interrupt, so report it
	 * before handing over: the register values, the security state and a
	 * live "does the timer PPI arrive" test all print here.
	 */
	{
		static struct gic_probe_result probe;

		gic_probe_run(&fdt_platform_info, &probe);
	}

#ifdef MSDC_LOG
	/*
	 * Stage MSDC0 and leave a shim log behind on every boot, not just in
	 * the diagnostic build: when the kernel dies silently the shim log is
	 * the only record of how the hand-off went.
	 */
	emmc_boot_setup();
	dump_log_to_emmc();
#endif

#ifdef PROBE_ONLY
	/*
	 * Diagnostics build: stop here and leave everything on the panel.  The
	 * kernel console starts at y=860 and would scroll over the probe's
	 * results, which is exactly what happened when this was run together
	 * with a kernel.  Reset after a generous pause so the panel can be
	 * photographed, and so the next boot prints it again.
	 */
	{
		uint64_t frequency = READ_SYSREG(CNTFRQ_EL0);
		uint64_t start;

		if (!frequency) {
			frequency = 13000000;
		}
		fb_puts("\nPROBE ONLY - not handing off to XNU\n", CLR_YELLOW);
		fb_puts("holding this panel for 120 s, then PSCI reset\n", CLR_YELLOW);
		start = READ_SYSREG(CNTPCT_EL0);
		while (READ_SYSREG(CNTPCT_EL0) - start < frequency * 120ULL)
			;
		stop();
	}
#endif

#ifdef USB_CONSOLE
	/*
	 * USB debug console: enumerate as CDC-ACM and then keep polling, so the
	 * host can read this log as a serial port.  Everything printed through
	 * fb_putc() is mirrored to the bulk IN endpoint, and the panel keeps
	 * working, so a failed enumeration still leaves evidence on screen.
	 */
	{
		uint32_t requests, last, bytes, events;
		uint64_t frequency = READ_SYSREG(CNTFRQ_EL0);
		uint64_t next;

		if (!frequency) {
			frequency = 13000000;
		}

		fb_puts("\n--- USB CDC-ACM console ---\n", CLR_YELLOW);
		usb_cdc_init();
		fb_puts("controller initialised, waiting for the host\n", CLR_WHITE);
		next = READ_SYSREG(CNTPCT_EL0);

		for (;;) {
			usb_poll();
			if (READ_SYSREG(CNTPCT_EL0) >= next) {
				usb_cdc_dump();
				next = READ_SYSREG(CNTPCT_EL0) + frequency;
			}
		}
	}
#endif

	/* Embedded test kernel handoff */
	fb_puts("\n--- HANDOFF ---\n", CLR_YELLOW);

#ifdef XNU_HANDOFF
	/*
	 * The real kernel: handoff_boot() takes the payload from the device
	 * tree's initrd (the ramdisk section of the boot image), validates the
	 * memory plan against the DTB, copies every segment into place, builds
	 * the ADT and boot_args and jumps to XNU.  It only returns on failure.
	 */
	{
		const char *why = handoff_boot(&fdt_info, dtb, 0, 0);

		fb_puts("\nXNU handoff failed: ", CLR_RED);
		fb_puts(why ? why : "unknown", CLR_RED);
		fb_newline();
		stop();
	}
#else
	uint64_t kern_base = 0x60000000ULL;
	uint64_t kern_size = (uint64_t)(test_kernel_end - test_kernel_start);
	/* Round up to page */
	kern_size = (kern_size + 0x3FFF) & ~0x3FFFULL;

	fb_puthex("Kernel at  ", kern_base, CLR_CYAN);
	fb_putdec("Kernel sz  ", kern_size, CLR_CYAN);
	fb_putdec("Code bytes ", (uint64_t)(test_kernel_end - test_kernel_start), CLR_CYAN);

	/* Copy test kernel to target */
	volatile uint8_t *dst = (volatile uint8_t *)kern_base;
	const uint8_t *src = (const uint8_t *)test_kernel_start;
	for (uint64_t i = 0; i < (uint64_t)(test_kernel_end - test_kernel_start); i++)
		dst[i] = src[i];
	fb_puts("Kernel copied OK\n", CLR_GREEN);

	/* Generate ADT */
	fb_puts("Generating ADT...\n", CLR_WHITE);
	static unsigned char adt_buf[XNU_PAGE] __attribute__((aligned(8)));
	uint32_t timebase = (uint32_t)READ_SYSREG(CNTFRQ_EL0);
	size_t adt_sz = dandelion_make_adt(adt_buf, sizeof(adt_buf),
					   kern_base, 0x18000000,
					   kern_base + kern_size,
					   timebase, NULL, 0);
	fb_putdec("ADT size   ", adt_sz, adt_sz ? CLR_GREEN : CLR_RED);

	if (!adt_sz) {
		fb_puts("ADT FAILED - stopping\n", CLR_RED);
		stop();
	}

	/* Build boot_args */
	fb_puts("Building boot_args...\n", CLR_WHITE);
	uint64_t adt_pa = kern_base + kern_size;
	uint64_t args_pa = adt_pa + XNU_PAGE;
	uint64_t top = args_pa + XNU_PAGE;

	/* Copy ADT */
	for (size_t i = 0; i < adt_sz; i++)
		((volatile uint8_t *)adt_pa)[i] = adt_buf[i];

	struct xnu_boot_args ba;
	for (size_t i = 0; i < sizeof(ba); i++)
		((volatile uint8_t *)(uintptr_t)&ba)[i] = 0;
	ba.revision = ba.version = 2;
	ba.virt_base = 0xffffff8007004000ULL;
	ba.phys_base = kern_base;
	ba.mem_size = ba.mem_size_actual = 0x18000000;
	ba.top_of_kernel_data = top;
	ba.device_tree = ba.virt_base + (adt_pa - kern_base);
	ba.device_tree_length = (uint32_t)adt_sz;
	static const char cmdline[] = "-v serial=3 debug=0x14e keepsyms=1 cpus=1";
	for (unsigned i = 0; i < XNU_CMDLINE-1 && cmdline[i]; i++)
		ba.command_line[i] = cmdline[i];

	for (size_t i = 0; i < sizeof(ba); i++)
		((volatile uint8_t *)args_pa)[i] = ((volatile uint8_t *)(uintptr_t)&ba)[i];

	fb_puthex("boot_args  ", args_pa, CLR_CYAN);
	fb_puthex("entry      ", kern_base, CLR_CYAN);
	fb_puts("\n", CLR_WHITE);
	fb_puts("Flushing caches...\n", CLR_WHITE);

	/* Flush caches and jump */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");

	fb_puts("JUMPING TO KERNEL...\n", CLR_GREEN);
	fb_puts("(screen may freeze - that means success!)\n", CLR_YELLOW);

	/* Jump to test kernel */
	((void (*)(uint64_t))kern_base)(args_pa);

	/* Should not return */
	fb_puts("KERNEL RETURNED!\n", CLR_RED);
	stop();
#endif
}

#ifdef FB_PROBE
/*
 * Framebuffer address probe.
 *
 * The console has never been confirmed on the phone and the device tree
 * carries two candidates: atag,videolfb-fb_base_l (0x5e605000, the buffer the
 * LK splash lives in) and the packed atag,videolfb blob (0x7ec50000, what the
 * Android display HAL allocates later).  Paint each one a different colour and
 * then power the phone off, so the observer can tell which buffer the panel
 * actually scans out - and, if neither shows, that the shim ran anyway.
 */
static void fb_probe(void)
{
	/*
	 * 0x7ec50000 is the reserved-memory framebuffer (mediatek,framebuffer,
	 * 0xfb0000 bytes).  Stop exactly at its end: the TEE secure region
	 * starts right behind it and a non-secure write there takes a data
	 * abort, which would blank the screen through exception_report().
	 */
	volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)0x7ec50000UL;
	for (uint64_t n = 0; n < 0xfb0000ULL / 8; n++)
		p[n] = 0x0000ff000000ff00ULL;	/* green */

	/* Ten seconds to look at the screen, then PSCI SYSTEM_OFF: a phone that
	 * switches itself off proves the shim executed even if nothing is
	 * visible. */
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0);
	if (!freq)
		freq = 13000000;
	uint64_t start = READ_SYSREG(CNTPCT_EL0);
	while (READ_SYSREG(CNTPCT_EL0) - start < freq * 10)
		;

	__asm__ volatile("ldr w0, =0x84000008\n\tsmc #0" ::: "x0");

	for (;;)
		__asm__ volatile("wfe");
}
#endif

/*
 * MediaTek TOPRGU watchdog at 0x10007000.  LK arms it (about 10 s, dual mode)
 * before jumping to the boot image, so a shim that lingers - or a kernel that
 * takes its time - is reset out of nowhere; that is the most likely reason the
 * test image "reboots by itself".  MODE needs the 0x22000000 key in its top
 * bits, and writing it also clears the reset-reason field, so read it first.
 */
#define TOPRGU_BASE     0x10007000UL
#define TOPRGU_MODE_KEY 0x22000000UL

static void watchdog_quiesce(void)
{
	volatile uint32_t *mode = (volatile uint32_t *)TOPRGU_BASE;
	uint32_t before = *mode;

	*mode = TOPRGU_MODE_KEY | (before & ~1u);
	*(volatile uint32_t *)(TOPRGU_BASE + 0x08) = 0x1971;    /* pet */

	fb_puthex("WDT before ", before, CLR_CYAN);
	fb_puthex("WDT after  ", *mode, CLR_CYAN);
}

__attribute__((noreturn)) void shim_main(uint64_t x0, uint64_t x1,
					 uint64_t x2, uint64_t x3, uint64_t old_sp)
{
	(void)x1; (void)x2; (void)x3; (void)old_sp;

	/*
	 * MediaTek TOPRGU watchdog: LK arms it (about 10 s, dual mode) before
	 * jumping to the boot image, so anything that lingers - the shim's probe
	 * mode, or a kernel taking its time - gets reset out of nowhere.  That
	 * is the most likely reason the test image "reboots by itself" after a
	 * few seconds.  Disable it before anything can be lost.
	 */
	fb_init();
	watchdog_quiesce();
#ifdef FB_PROBE
	fb_probe();
#endif
	/*
	 * Get the log out before touching anything else: if the framebuffer or
	 * the device tree handling faults, these first lines are already in the
	 * ramoops console zone for the next boot to pick up.
	 */
	ps_reset();
	ps_puts("DANDELION BOOTSHIM v2 entered\n");
	ps_puthex("dtb", x0);
	ps_puthex("CurrentEL", READ_SYSREG(CurrentEL));
	fb_init();
	fb_clear(CLR_BLACK);

	uint64_t el = READ_SYSREG(CurrentEL);

	fb_puts("=== DANDELION BOOTSHIM v2 ===\n", CLR_YELLOW);
#ifdef PLATFORM_TEST_BLOB
	fb_puts("Embedded platform test payload\n", CLR_WHITE);
#elif defined(XNU_HANDOFF)
	fb_puts("XNU handoff build\n", CLR_WHITE);
#else
	fb_puts("Embedded kernel test\n", CLR_WHITE);
#endif
	fb_puts("FB: 0x7ec50000 stride=2944 736x1600\n\n", CLR_WHITE);

	fb_puts("--- ENTRY ---\n", CLR_YELLOW);
	fb_puthex("CurrentEL  ", el, CLR_CYAN);
	fb_puthex("entry_x0   ", x0, CLR_CYAN);

	show_fdt_info(x0);
	{
		int gic_v3 = 1;

		if (fdt_info.bytes) {
			if (fdt_probe_platform((const void *)(uintptr_t)x0,
				fdt_info.bytes, &fdt_platform_info))
				fdt_platform_info.gic.found = 0;
			gic_v3 = fdt_platform_info.gic.found &&
			    contains(fdt_platform_info.gic.compatible, "gic-v3");
		}
		fdt_gic_v3 = gic_v3;
		show_cpu_info(gic_v3);
	}
	if (fdt_info.bytes)
		show_platform_probe(x0);

	/* EL2 -> EL1 */
	if (el == 8) {
		fb_puts("\n--- EL2->EL1 ---\n", CLR_YELLOW);
		uint64_t sctlr2 = READ_SYSREG(SCTLR_EL2);
		fb_puthex("SCTLR_EL2  ", sctlr2, CLR_CYAN);
		if (sctlr2 & ((1u<<0)|(1u<<2)|(1u<<12))) {
			fb_puts("MMU/Cache ON!\n", CLR_RED);
			stop();
		}
		fb_puts("Transitioning...\n", CLR_GREEN);
		enter_el1(x0, (uint64_t)fdt_gic_v3);
		fb_puts("enter_el1 returned?!\n", CLR_RED);
		stop();
	}
	if (el == 4) {
		fb_puts("\nAlready at EL1\n", CLR_GREEN);
		shim_el1(x0);
	}
	fb_puts("\nUnknown EL!\n", CLR_RED);
	stop();
}
