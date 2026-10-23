/*
 * Dandelion platform expert -- MediaTek MT6762G (Helio G25)
 *
 * XNU-side platform bring-up for the Redmi 9A (dandelion / blossom) port.
 *
 * The boot shim (bootshim/) enters XNU at EL1 with the MMU and caches off and
 * passes a pointer to an Apple `boot_args' structure.  Its layout is defined
 * once in bootshim/handoff.h, which mirrors `pexpert/pexpert/arm64/boot.h'
 * from xnu-8796.141.3.
 *
 * This file implements the "platform interrupt/timer/serial wiring" that the
 * reference port (mouseos/xnu_kernel_benesse_a04br3, PORTING.md) still lists
 * as outstanding for its A04BR3 target, adapted to the Dandelion hardware:
 *
 *   - early console   MediaTek MT6577 UART @ 0x11002000, 921600 8N1
 *   - interrupts      ARM GICv3, distributor @ 0x0C000000 and
 *                     redistributors @ 0x0C100000 (the mt6765.dtsi layout
 *                     used by mt6762g-xiaomi-dandelion.dts).
 *                     A04BR3 uses GICv2/AIC; Dandelion is GICv3, which is
 *                     the one place where this port deliberately diverges
 *                     from the reference.
 *   - timer           ARM generic timer, 13 MHz, virtual timer INTID 27
 *                     (GIC PPI 11, as in mt6765.dtsi)
 *   - watchdog        MediaTek WDT @ 0x10007000 (pet / disable helpers)
 *   - video           fill boot_args.Video from the LK framebuffer so the
 *                     XNU early console can draw on the panel
 *
 * The module is freestanding: it neither includes nor links against XNU yet.
 * That lets it be exercised from the shim entry path (physical addresses,
 * MMU off) and, once the XNU source tree lands, be called from
 * PE_init_platform() after mapping the same registers with ml_io_map().
 * Use dandelion_platform_set_mmio_map() for the latter.
 *
 * Interrupts stay masked (DAIF.I == 1) when dandelion_platform_expert_init()
 * returns; XNU installs its exception vectors and unmasks IRQs once the pmap
 * and the interrupt framework are usable.
 */

#include <stddef.h>
#include <stdint.h>

#include "handoff.h"
#include "pe_dandelion.h"

/*
 * boot_args ABI shared with the shim and with Apple's boot.h.  Keep the
 * offsets honest at compile time; the shim asserts the same values.
 */
static_assert(sizeof(struct xnu_boot_args) == 736,
    "xnu_boot_args must match Apple boot_args (xnu-8796.141.3)");
static_assert(offsetof(struct xnu_boot_args, device_tree) == 96,
    "boot_args.device_tree offset");
static_assert(offsetof(struct xnu_boot_args, command_line) == 108,
    "boot_args.command_line offset");

namespace {

/* ------------------------------------------------------------------ */
/* MMIO access                                                        */
/*                                                                    */
/* Defaults are the physical addresses used while the MMU is off.     */
/* dandelion_platform_set_mmio_map() replaces them with kernel VAs.   */
/* ------------------------------------------------------------------ */

uint64_t g_uart_base = DANDELION_UART_BASE;
uint64_t g_gicd_base = DANDELION_GIC_BASE;
uint64_t g_gicr_base = DANDELION_GICR_BASE;
uint64_t g_wdt_base  = DANDELION_WDT_BASE;

inline uint32_t mmio_read32(uint64_t base, uint32_t offset)
{
	return *reinterpret_cast<volatile uint32_t *>(
	    static_cast<uintptr_t>(base + offset));
}

inline void mmio_write32(uint64_t base, uint32_t offset, uint32_t value)
{
	*reinterpret_cast<volatile uint32_t *>(
	    static_cast<uintptr_t>(base + offset)) = value;
}

inline uint64_t mmio_read64(uint64_t base, uint32_t offset)
{
	return *reinterpret_cast<volatile uint64_t *>(
	    static_cast<uintptr_t>(base + offset));
}

inline void mmio_write64(uint64_t base, uint32_t offset, uint64_t value)
{
	*reinterpret_cast<volatile uint64_t *>(
	    static_cast<uintptr_t>(base + offset)) = value;
}

/* ------------------------------------------------------------------ */
/* System registers                                                   */
/* ------------------------------------------------------------------ */

inline uint64_t read_mpidr(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
	return value;
}

inline uint64_t read_cntfrq(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

inline uint64_t read_cntvct(void)
{
	uint64_t value;
	__asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
	return value;
}

inline void instruction_sync(void)
{
	__asm__ volatile("isb" : : : "memory");
}

/* ------------------------------------------------------------------ */
/* Early console: MediaTek MT6577 UART (8250 compatible)              */
/* ------------------------------------------------------------------ */

constexpr uint32_t kUartThr = 0x00;   /* transmit holding register     */
constexpr uint32_t kUartIer = 0x04;   /* interrupt enable              */
constexpr uint32_t kUartFcr = 0x08;   /* FIFO control                  */
constexpr uint32_t kUartLcr = 0x0c;   /* line control                  */
constexpr uint32_t kUartMcr = 0x10;   /* modem control                 */
constexpr uint32_t kUartLsr = 0x14;   /* line status                   */

constexpr uint32_t kUartLsrThre = 1u << 5;   /* THR empty               */
constexpr uint32_t kUartLcr8N1 = 0x03;
constexpr uint32_t kUartFcrEnable = 1u << 0;
constexpr uint32_t kUartFcrClearRx = 1u << 1;
constexpr uint32_t kUartFcrClearTx = 1u << 2;
constexpr uint32_t kUartMcrDtrRts = 0x03;

inline void uart_putc_raw(char c)
{
	/*
	 * LK normally leaves UART0 running at 921600 8N1.  Never spin
	 * forever: if the UART is unclocked or fenced off, drop the byte
	 * and keep booting (the framebuffer console is the fallback).
	 */
	for (unsigned spin = 0; spin < 1000000u; spin++) {
		if (mmio_read32(g_uart_base, kUartLsr) & kUartLsrThre)
			break;
	}
	mmio_write32(g_uart_base, kUartThr, static_cast<uint32_t>(c) & 0xffu);
}

inline void uart_puthex_raw(uint64_t value)
{
	static const char kDigits[] = "0123456789abcdef";
	bool started = false;

	uart_putc_raw('0');
	uart_putc_raw('x');
	for (int shift = 60; shift >= 0; shift -= 4) {
		unsigned nibble = static_cast<unsigned>((value >> shift) & 0xfu);
		if (nibble != 0 || started || shift == 0) {
			uart_putc_raw(kDigits[nibble]);
			started = true;
		}
	}
}

inline void uart_putdec_raw(uint64_t value)
{
	char buffer[20];
	unsigned length = 0;

	if (value == 0) {
		uart_putc_raw('0');
		return;
	}
	while (value != 0 && length < sizeof(buffer)) {
		buffer[length++] = static_cast<char>('0' + (value % 10));
		value /= 10;
	}
	while (length > 0)
		uart_putc_raw(buffer[--length]);
}

/* ------------------------------------------------------------------ */
/* GICv3                                                              */
/*                                                                    */
/* Offsets follow the ARM GICv3 architecture and the Linux            */
/* irqchip/arm-gic-v3.h definitions.                                  */
/* ------------------------------------------------------------------ */

/* Distributor (GICD) */
constexpr uint32_t kGicdCtlr        = 0x0000;
constexpr uint32_t kGicdTyper       = 0x0004;
constexpr uint32_t kGicdIgroup      = 0x0080;   /* + 4 * n */
constexpr uint32_t kGicdIsenabler   = 0x0100;   /* + 4 * n */
constexpr uint32_t kGicdIcenabler   = 0x0180;   /* + 4 * n */
constexpr uint32_t kGicdIpriorityr  = 0x0400;   /* + 4 * n */
constexpr uint32_t kGicdIcfgr       = 0x0c00;   /* + 4 * n */
constexpr uint32_t kGicdIgrmodr     = 0x0d00;   /* + 4 * n */
constexpr uint32_t kGicdIrouter     = 0x6000;   /* + 8 * n */

constexpr uint32_t kGicdCtlrEnableGrp0  = 1u << 0;
constexpr uint32_t kGicdCtlrEnableGrp1Ns = 1u << 1;
constexpr uint32_t kGicdCtlrAreNs       = 1u << 4;
constexpr uint32_t kGicdCtlrRwp         = 1u << 31;

/* Redistributor (GICR): RD frame at 0x00000, SGI frame at 0x10000. */
constexpr uint32_t kGicrTyper       = 0x0008;
constexpr uint64_t kGicrTyperVlpis  = 1ull << 1;
constexpr uint32_t kGicrWaker       = 0x0014;

constexpr uint32_t kGicrSgiBase     = 0x10000;
constexpr uint32_t kGicrIgroup0     = kGicrSgiBase + 0x0080;
constexpr uint32_t kGicrIsenabler0  = kGicrSgiBase + 0x0100;
constexpr uint32_t kGicrIcenabler0  = kGicrSgiBase + 0x0180;
constexpr uint32_t kGicrIpriorityr  = kGicrSgiBase + 0x0400;   /* + 4 * n */
constexpr uint32_t kGicrIcfgr0      = kGicrSgiBase + 0x0c00;
constexpr uint32_t kGicrIcfgr1      = kGicrSgiBase + 0x0c04;
constexpr uint32_t kGicrIgrmodr0    = kGicrSgiBase + 0x0d00;

constexpr uint32_t kGicrWakerProcessorSleep = 1u << 1;
constexpr uint32_t kGicrWakerChildrenAsleep = 1u << 2;

/* CPU interface (system registers) */
constexpr uint64_t kIccSreSre = 1u << 0;
constexpr uint32_t kIccSpurious = 0x3ff;
constexpr uint32_t kGicPriorityDefault = 0xa0;
constexpr uint32_t kGicPriorityWord =
    kGicPriorityDefault | (kGicPriorityDefault << 8) |
    (kGicPriorityDefault << 16) | (kGicPriorityDefault << 24);

/* Maximum INTID for this GIC, rounded up to a multiple of 32. */
inline uint32_t gicd_max_irqs(uint32_t typer)
{
	return ((typer & 0x1fu) + 1u) * 32u;
}

/*
 * Locate the redistributor that belongs to the current CPU.
 *
 * Redistributors form one contiguous region starting at DANDELION_GICR_BASE
 * and advertise their MPIDR affinity in GICR_TYPER[63:32].  Each frame is
 * 128 KiB on GICv3 (RD_base + SGI_base) and 256 KiB when the redistributor
 * reports VLPI support (GICv4), the same walk Linux performs in
 * irq-gic-v3.c.  A frame reports VLPI support in GICR_TYPER bit 1.
 */
uint64_t gicr_find_self(void)
{
	uint64_t mpidr = read_mpidr();
	uint64_t affinity =
	    ((mpidr & 0xffu) << 32) |
	    (((mpidr >> 8) & 0xffu) << 40) |
	    (((mpidr >> 16) & 0xffu) << 48) |
	    (((mpidr >> 32) & 0xffu) << 56);
	uint64_t base = g_gicr_base;

	for (unsigned i = 0; i < DANDELION_NCPUS; i++) {
		uint64_t typer = mmio_read64(base, kGicrTyper);
		if ((typer & 0xffffffff00000000ULL) == affinity)
			return base;

		base += (typer & kGicrTyperVlpis) ?
		    DANDELION_GICR_STRIDE_V4 : DANDELION_GICR_STRIDE;
	}

	/* Single-CPU fallback: assume the first frame belongs to us. */
	return g_gicr_base;
}

void gicd_wait_rwp(void)
{
	for (unsigned spin = 0; spin < 1000000u; spin++) {
		if ((mmio_read32(g_gicd_base, kGicdCtlr) & kGicdCtlrRwp) == 0)
			return;
	}
}

void gicd_init(void)
{
	/* Disable both groups while (re)programming the distributor. */
	uint32_t ctlr = mmio_read32(g_gicd_base, kGicdCtlr);
	ctlr &= ~(kGicdCtlrEnableGrp0 | kGicdCtlrEnableGrp1Ns);
	mmio_write32(g_gicd_base, kGicdCtlr, ctlr);
	gicd_wait_rwp();

	uint32_t max_irqs = gicd_max_irqs(mmio_read32(g_gicd_base, kGicdTyper));
	unsigned groups = max_irqs / 32u;

	/* Every SPI belongs to non-secure group 1, no group-1 secure split. */
	for (unsigned i = 0; i < groups; i++) {
		mmio_write32(g_gicd_base, kGicdIgroup + i * 4u, 0xffffffffu);
		mmio_write32(g_gicd_base, kGicdIgrmodr + i * 4u, 0u);
	}

	/* Mid-range default priority for every interrupt. */
	for (unsigned i = 0; i < max_irqs / 4u; i++) {
		mmio_write32(g_gicd_base, kGicdIpriorityr + i * 4u, kGicPriorityWord);
	}

	/* Level-sensitive SPIs by default; ICFGR[0..1] are SGIs/PPIs. */
	for (unsigned i = 2; i < max_irqs / 16u; i++) {
		mmio_write32(g_gicd_base, kGicdIcfgr + i * 4u, 0u);
	}

	/*
	 * Route every SPI to CPU 0 (affinity 0, IRM = 0).  Secondary CPUs are
	 * not started yet; XNU can re-target individual interrupts later with
	 * dandelion_gicv3_irq_set_target().
	 */
	for (unsigned irq = 32; irq < max_irqs; irq++) {
		mmio_write64(g_gicd_base, kGicdIrouter + irq * 8u, 0u);
	}

	/* Affinity routing on, non-secure group 1 enabled. */
	ctlr = mmio_read32(g_gicd_base, kGicdCtlr);
	ctlr |= kGicdCtlrAreNs | kGicdCtlrEnableGrp1Ns;
	mmio_write32(g_gicd_base, kGicdCtlr, ctlr);
	gicd_wait_rwp();
}

bool gicr_wake(uint64_t redist)
{
	uint32_t waker = mmio_read32(redist, kGicrWaker);
	waker &= ~kGicrWakerProcessorSleep;
	mmio_write32(redist, kGicrWaker, waker);

	for (unsigned spin = 0; spin < 1000000u; spin++) {
		if ((mmio_read32(redist, kGicrWaker) & kGicrWakerChildrenAsleep) == 0)
			return true;
	}
	return false;
}

void gicr_configure(uint64_t redist)
{
	/* Reconfigure the SGI/PPI frame from a clean slate. */
	mmio_write32(redist, kGicrIcenabler0, 0xffffffffu);
	mmio_write32(redist, kGicrIgroup0, 0xffffffffu);
	mmio_write32(redist, kGicrIgrmodr0, 0u);

	for (unsigned i = 0; i < 8; i++) {
		mmio_write32(redist, kGicrIpriorityr + i * 4u, kGicPriorityWord);
	}

	/* SGIs are edge triggered; PPIs (timer) are level sensitive. */
	mmio_write32(redist, kGicrIcfgr0, 0u);
	mmio_write32(redist, kGicrIcfgr1, 0u);
}

void gicr_enable_ppi(uint64_t redist, uint32_t irq)
{
	if (irq < 32)
		mmio_write32(redist, kGicrIsenabler0, 1u << irq);
}

bool gic_cpu_interface_init(void)
{
	uint64_t sre;
	__asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));

	if ((sre & kIccSreSre) == 0) {
		sre |= kIccSreSre;
		__asm__ volatile("msr icc_sre_el1, %0" : : "r"(sre) : "memory");
		instruction_sync();
		__asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
	}

	/*
	 * If EL3 fenced the system-register interface off, the MMIO CPU
	 * interface would be required instead.  MT6762G firmware leaves SRE
	 * writable, but report it rather than silently running without IRQs.
	 */
	if ((sre & kIccSreSre) == 0)
		return false;

	/* Accept all priorities, no priority grouping. */
	__asm__ volatile("msr icc_pmr_el1, %0" : : "r"((uint64_t)0xffu) : "memory");
	__asm__ volatile("msr icc_bpr1_el1, %0" : : "r"((uint64_t)0u) : "memory");
	/* EOImode 0: EOIR1 both drops priority and deactivates. */
	__asm__ volatile("msr icc_ctlr_el1, %0" : : "r"((uint64_t)0u) : "memory");
	__asm__ volatile("msr icc_igrpen1_el1, %0" : : "r"((uint64_t)1u) : "memory");
	instruction_sync();
	return true;
}

/* ------------------------------------------------------------------ */
/* Watchdog: MediaTek TOPRGU (drivers/watchdog/mtk_wdt.c)             */
/* ------------------------------------------------------------------ */

constexpr uint32_t kWdtMode        = 0x00;
constexpr uint32_t kWdtRestart     = 0x08;
constexpr uint32_t kWdtSwrst       = 0x14;

constexpr uint32_t kWdtModeEnable  = 1u << 0;
constexpr uint32_t kWdtModeKey     = 0x22000000u;
constexpr uint32_t kWdtRestartKey  = 0x1971u;
constexpr uint32_t kWdtSwrstKey    = 0x1209u;

} /* namespace */

/* ------------------------------------------------------------------ */
/* Exported interface                                                 */
/* ------------------------------------------------------------------ */

extern "C" {

/*
 * Point the platform expert at kernel virtual addresses obtained from
 * ml_io_map() once the XNU pmap is up.  A zero argument keeps the physical
 * address for that block.  Intended to be called before
 * dandelion_platform_expert_init().
 */
void dandelion_platform_set_mmio_map(uint64_t uart, uint64_t gicd,
    uint64_t gicr, uint64_t wdt)
{
	g_uart_base = uart ? uart : DANDELION_UART_BASE;
	g_gicd_base = gicd ? gicd : DANDELION_GIC_BASE;
	g_gicr_base = gicr ? gicr : DANDELION_GICR_BASE;
	g_wdt_base  = wdt  ? wdt  : DANDELION_WDT_BASE;
}

void dandelion_uart_init(void)
{
	/* Keep LK's divisor (921600 baud); only restore the 8N1 framing. */
	mmio_write32(g_uart_base, kUartFcr,
	    kUartFcrEnable | kUartFcrClearRx | kUartFcrClearTx);
	mmio_write32(g_uart_base, kUartLcr, kUartLcr8N1);
	mmio_write32(g_uart_base, kUartIer, 0u);
	mmio_write32(g_uart_base, kUartMcr, kUartMcrDtrRts);
}

void dandelion_console_init(void)
{
	dandelion_uart_init();
}

void dandelion_console_putc(char c)
{
	if (c == '\n')
		uart_putc_raw('\r');
	uart_putc_raw(c);
}

void dandelion_console_puts(const char *text)
{
	while (*text != '\0')
		dandelion_console_putc(*text++);
}

void dandelion_console_puthex(uint64_t value)
{
	uart_puthex_raw(value);
}

void dandelion_console_putdec(uint64_t value)
{
	uart_putdec_raw(value);
}

/*
 * Initialise the distributor, this CPU's redistributor and the CPU
 * interface.  Returns 0 on success, -1 if the system-register interface is
 * unavailable, -2 if the redistributor does not wake up.
 */
int dandelion_gicv3_init(void)
{
	gicd_init();

	uint64_t redist = gicr_find_self();
	if (!gicr_wake(redist))
		return -2;

	gicr_configure(redist);
	gicr_enable_ppi(redist, DANDELION_TIMER_IRQ);

	if (!gic_cpu_interface_init())
		return -1;

	return 0;
}

/*
 * Per-CPU bring-up for secondary cores: wake the local redistributor,
 * program its SGI/PPI frame and enable this CPU's CPU interface.
 */
int dandelion_gicv3_cpu_up(void)
{
	uint64_t redist = gicr_find_self();
	if (!gicr_wake(redist))
		return -2;

	gicr_configure(redist);
	gicr_enable_ppi(redist, DANDELION_TIMER_IRQ);

	if (!gic_cpu_interface_init())
		return -1;

	return 0;
}

int dandelion_gicv3_irq_enable(uint32_t irq)
{
	if (irq >= 1020)
		return -1;

	if (irq < 32) {
		mmio_write32(gicr_find_self(), kGicrIsenabler0, 1u << irq);
	} else {
		mmio_write32(g_gicd_base,
		    kGicdIsenabler + (irq / 32u) * 4u, 1u << (irq % 32u));
		gicd_wait_rwp();
	}
	return 0;
}

int dandelion_gicv3_irq_disable(uint32_t irq)
{
	if (irq >= 1020)
		return -1;

	if (irq < 32) {
		mmio_write32(gicr_find_self(), kGicrIcenabler0, 1u << irq);
	} else {
		mmio_write32(g_gicd_base,
		    kGicdIcenabler + (irq / 32u) * 4u, 1u << (irq % 32u));
		gicd_wait_rwp();
	}
	return 0;
}

void dandelion_gicv3_irq_set_priority(uint32_t irq, uint8_t priority)
{
	if (irq >= 1020)
		return;

	uint32_t shift = (irq % 4u) * 8u;
	uint32_t mask = 0xffu << shift;
	uint32_t offset;
	uint64_t base;

	if (irq < 32) {
		base = gicr_find_self();
		offset = kGicrIpriorityr + (irq / 4u) * 4u;
	} else {
		base = g_gicd_base;
		offset = kGicdIpriorityr + (irq / 4u) * 4u;
	}

	uint32_t value = mmio_read32(base, offset);
	value = (value & ~mask) | (static_cast<uint32_t>(priority) << shift);
	mmio_write32(base, offset, value);
}

/*
 * Route a shared peripheral interrupt to the CPU described by `mpidr'.
 * PPIs and SGIs are per-CPU and have no router register.
 */
void dandelion_gicv3_irq_set_target(uint32_t irq, uint64_t mpidr)
{
	if (irq < 32 || irq >= 1020)
		return;

	uint64_t route =
	    ((mpidr & 0xffu) << 32) |
	    (((mpidr >> 8) & 0xffu) << 40) |
	    (((mpidr >> 16) & 0xffu) << 48) |
	    (((mpidr >> 32) & 0xffu) << 56);

	mmio_write64(g_gicd_base, kGicdIrouter + irq * 8u, route);
}

/* Acknowledge the highest priority group-1 interrupt (INTID or spurious). */
uint32_t dandelion_gicv3_ack(void)
{
	uint64_t iar;
	__asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));

	uint32_t irq = static_cast<uint32_t>(iar & 0xffffffu);
	/* 1020..1023 are special INTIDs; report them as spurious. */
	return irq >= kIccSpurious ? kIccSpurious : irq;
}

/* Signal end of interrupt (EOImode 0: drops priority and deactivates). */
void dandelion_gicv3_eoi(uint32_t irq)
{
	/* Writing a special INTID to EOIR1 is UNPREDICTABLE; drop it. */
	if (irq >= kIccSpurious)
		return;

	__asm__ volatile("msr icc_eoir1_el1, %0" : : "r"(static_cast<uint64_t>(irq))
	    : "memory");
}

/* Explicit deactivate, for kernels that run EOImode 1. */
void dandelion_gicv3_deactivate(uint32_t irq)
{
	__asm__ volatile("msr icc_dir_el1, %0" : : "r"(static_cast<uint64_t>(irq))
	    : "memory");
}

/*
 * Send a software generated interrupt to the CPUs in `target_mask' on the
 * cluster selected by `affinity1' (ICC_SGI1R_EL1.Aff1).  MT6762G puts CPU0-3
 * in cluster 0 and CPU4-7 in cluster 1 (Aff1 = 1).
 */
int dandelion_gicv3_send_sgi_to(uint32_t irq, uint32_t target_mask,
    uint32_t affinity1)
{
	if (irq > 15 || target_mask == 0 || affinity1 > 0xffu)
		return -1;

	uint64_t value =
	    (static_cast<uint64_t>(irq & 0xfu) << 24) |
	    (static_cast<uint64_t>(affinity1 & 0xffu) << 16) |
	    (static_cast<uint64_t>(target_mask) & 0xffffu);

	__asm__ volatile("msr icc_sgi1r_el1, %0" : : "r"(value) : "memory");
	instruction_sync();
	return 0;
}

/* Send an SGI to the boot cluster (affinity 1 == 0). */
int dandelion_gicv3_send_sgi(uint32_t irq, uint32_t target_mask)
{
	return dandelion_gicv3_send_sgi_to(irq, target_mask, 0);
}

/* CNTFRQ_EL0, in Hz. */
uint64_t dandelion_timer_frequency(void)
{
	return read_cntfrq();
}

/* Free-running virtual counter value (CNTVCT_EL0). */
uint64_t dandelion_timer_ticks(void)
{
	return read_cntvct();
}

/*
 * Program the virtual timer for a one second tick.  The PPI is enabled in
 * the redistributor, but delivery stays masked until XNU unmasks DAIF.I.
 */
void dandelion_timer_init(void)
{
	uint64_t frequency = read_cntfrq();

	if (frequency == 0) {
		frequency = DANDELION_TIMER_FREQ;
		__asm__ volatile("msr cntfrq_el0, %0" : : "r"(frequency)
		    : "memory");
	}

	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(frequency) : "memory");
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)1u) : "memory");
	instruction_sync();
}

void dandelion_watchdog_pet(void)
{
	mmio_write32(g_wdt_base, kWdtRestart, kWdtRestartKey);
}

/*
 * The bootloader may leave the TOPRGU watchdog armed.  A development kernel
 * without a timer tick would be reset mid-boot, so disarm it here; the
 * production port should instead pet it from the platform timer.
 */
void dandelion_watchdog_disable(void)
{
	uint32_t mode = mmio_read32(g_wdt_base, kWdtMode);
	mmio_write32(g_wdt_base, kWdtMode, (mode & ~kWdtModeEnable) | kWdtModeKey);
}

/* Immediate SoC reset through the watchdog (never returns). */
__attribute__((noreturn)) void dandelion_watchdog_reset(void)
{
	for (;;) {
		mmio_write32(g_wdt_base, kWdtSwrst, kWdtSwrstKey);
		for (volatile unsigned spin = 0; spin < 100000u; spin++)
			;
	}
}

/*
 * Publish the LK framebuffer through the Apple Boot_Video structure.  The
 * shim currently leaves it zeroed, so the XNU early console has no output
 * until this runs.
 */
void dandelion_platform_setup_video(struct xnu_boot_args *args)
{
	if (args == NULL || args->video.base != 0)
		return;

	args->video.base = DANDELION_FB_BASE;
	args->video.display = 0;
	args->video.rowbytes = DANDELION_FB_STRIDE;
	args->video.width = DANDELION_FB_WIDTH;
	args->video.height = DANDELION_FB_HEIGHT;
	args->video.depth = DANDELION_FB_BPP;
}

/*
 * Platform entry point.  Safe to call with the MMU off (physical addresses)
 * or after dandelion_platform_set_mmio_map() installed kernel mappings.
 * Returns 0 when the platform is usable, or a negative errno-style code.
 */
int dandelion_platform_expert_init(struct xnu_boot_args *args)
{
	dandelion_console_init();
	dandelion_console_puts("\nXNU: dandelion platform expert (MT6762G, Redmi 9A)\n");

	if (args == NULL || args->revision != 2 || args->version != 2 ||
	    args->phys_base == 0 || args->mem_size == 0) {
		dandelion_console_puts("XNU: fatal: boot_args missing or invalid\n");
		return -1;
	}

	dandelion_console_puts("XNU: boot_args rev=");
	dandelion_console_putdec(args->revision);
	dandelion_console_puts(" ver=");
	dandelion_console_putdec(args->version);
	dandelion_console_puts(" phys=");
	dandelion_console_puthex(args->phys_base);
	dandelion_console_puts(" size=");
	dandelion_console_puthex(args->mem_size);
	dandelion_console_puts(" virt=");
	dandelion_console_puthex(args->virt_base);
	dandelion_console_puts("\n");

	dandelion_console_puts("XNU: adt=");
	dandelion_console_puthex(args->device_tree);
	dandelion_console_puts(" len=");
	dandelion_console_putdec(args->device_tree_length);
	dandelion_console_puts(" cmdline=\"");
	if (args->command_line[0] != '\0') {
		for (unsigned i = 0; i < XNU_CMDLINE && args->command_line[i] != '\0'; i++)
			dandelion_console_putc(args->command_line[i]);
	}
	dandelion_console_puts("\"\n");

	dandelion_platform_setup_video(args);
	dandelion_console_puts("XNU: video ");
	dandelion_console_putdec(args->video.width);
	dandelion_console_puts("x");
	dandelion_console_putdec(args->video.height);
	dandelion_console_puts(" stride=");
	dandelion_console_putdec(args->video.rowbytes);
	dandelion_console_puts(" base=");
	dandelion_console_puthex(args->video.base);
	dandelion_console_puts("\n");

	dandelion_watchdog_disable();
	dandelion_console_puts("XNU: TOPRGU watchdog disabled\n");

	dandelion_timer_init();
	dandelion_console_puts("XNU: generic timer ");
	dandelion_console_putdec(dandelion_timer_frequency());
	dandelion_console_puts(" Hz, PPI ");
	dandelion_console_putdec(DANDELION_TIMER_IRQ);
	dandelion_console_puts("\n");

	int result = dandelion_gicv3_init();
	if (result != 0) {
		dandelion_console_puts("XNU: fatal: GICv3 init failed (");
		dandelion_console_putdec(static_cast<uint64_t>(-result));
		dandelion_console_puts(")\n");
		return result;
	}

	dandelion_console_puts("XNU: GICv3 dist=");
	dandelion_console_puthex(g_gicd_base);
	dandelion_console_puts(" redist=");
	dandelion_console_puthex(g_gicr_base);
	dandelion_console_puts("\n");
	dandelion_console_puts("XNU: platform expert ready, IRQs masked\n");

	return 0;
}

} /* extern "C" */
