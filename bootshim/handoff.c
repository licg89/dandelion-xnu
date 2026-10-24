/* Dandelion kernel handoff - payload parsing and kernel loading
 * Based on mouseos handoff.c, adapted for dandelion.
 *
 * Payload format (in initrd):
 *   [Header: magic "DNDLXNU0", version, segments, cmdline]
 *   [Segment descriptors]
 *   [Segment data]
 *
 * Handoff flow:
 *   1. Find payload in FDT initrd
 *   2. Validate magic, segments, memory layout
 *   3. Copy segments to physical RAM
 *   4. Generate ADT
 *   5. Build boot_args
 *   6. Jump to kernel entry
 */
#include <stdint.h>
#include <stddef.h>
#include "handoff.h"
#include "fb.h"

#define XNU_PAGE 0x4000UL

static uint64_t aligned(uint64_t v) { return (v + XNU_PAGE - 1) & ~(XNU_PAGE - 1); }

static int inside(uint64_t start, uint64_t size, uint64_t base, uint64_t span)
{
	return start >= base && start - base <= span && size <= span - (start - base);
}

static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
	if (!n || !m) return 0;
	if (a + n < a || b + m < b) return 1;
	return a < b + m && b < a + n;
}

static int covered(uint64_t start, uint64_t size, const struct fdt_info *fdt)
{
	if (!size || start + size < start) return 0;
	uint64_t cursor = start, end = start + size;
	for (unsigned pass = 0; pass < fdt->memory_count; pass++) {
		uint64_t next = cursor;
		for (unsigned i = 0; i < fdt->memory_count; i++) {
			uint64_t base = fdt->memory[i].base;
			uint64_t limit = base + fdt->memory[i].size;
			if (limit < base) continue;
			if (base <= cursor && limit > next) next = limit;
		}
		if (next == cursor) return 0;
		if (next >= end) return 1;
		cursor = next;
	}
	return 0;
}

/* Validate payload and compute handoff plan */
int handoff_validate(const void *payload, size_t length, uint64_t payload_pa,
		     const struct fdt_info *fdt, uint64_t fdt_pa,
		     uint64_t shim_start, uint64_t shim_end,
		     struct handoff_plan *plan)
{
	extern void fb_puts(const char *s, uint32_t fg);
	extern void fb_puthex(const char *label, uint64_t val, uint32_t fg);
	#define FAIL(reason) do { fb_puts("  FAIL: ", 0xFFFF0000); fb_puts(reason "\n", 0xFFFF0000); return -1; } while(0)

	(void)fdt_pa;  /* the original device tree is not needed after the jump */

	if (length < sizeof(struct xnu_payload)) FAIL("too small");
	if (length > 0x8000000) FAIL("too large");
	if (((uintptr_t)payload & 7)) FAIL("misaligned");
	if (payload_pa + length < payload_pa) FAIL("overflow");
	if (fdt->memory_count > FDT_MAX_MEMORY) FAIL("too many mem");
	if (fdt->reserved_count > FDT_MAX_RESERVED) FAIL("too many res");
	if (shim_end < shim_start) FAIL("shim range");

	const struct xnu_payload *p = payload;

	static const char magic[] = "DNDLXNU0";
	for (unsigned i = 0; i < 8; i++)
		if (p->magic[i] != magic[i]) FAIL("bad magic");

	if (p->version != 1) FAIL("bad version");
	if (!p->segment_count) FAIL("no segments");
	if (p->segment_count > XNU_MAX_SEGS) FAIL("too many segs");

	if (p->total_size != length) FAIL("total_size mismatch");
	if (p->segments_offset < sizeof(*p)) FAIL("segs off < header");
	if ((p->segments_offset & 7)) FAIL("segs off align");
	if (p->segments_offset > length) FAIL("segs off > len");
	if (p->segment_count * sizeof(struct xnu_segment) > length - p->segments_offset)
		FAIL("segs overflow");
	if (p->data_offset < p->segments_offset + p->segment_count * sizeof(struct xnu_segment))
		FAIL("data off < segs end");
	if (p->data_offset > length) FAIL("data off > len");
	if ((p->data_offset & 7)) FAIL("data off align");

	if ((p->virt_base & 0x1ffffff)) FAIL("virt_base align");
	if ((p->phys_base & 0x1ffffff)) FAIL("phys_base align");
	if (!p->mem_size) FAIL("no mem_size");
	if (p->mem_size > 0x60000000) FAIL("mem too large");
	if ((p->mem_size & (XNU_PAGE - 1))) FAIL("mem_size align");
	if (p->virt_base < 0xffffff8000000000ULL) FAIL("virt_base too low");
	if (p->virt_base + p->mem_size < p->virt_base) FAIL("virt overflow");
	if (!covered(p->phys_base, p->mem_size, fdt)) FAIL("kernel not covered");
	if (!covered(payload_pa, length, fdt)) FAIL("payload not covered");
	/*
	 * The payload may live inside the kernel's memory window: it is the
	 * boot image's ramdisk, and XNU is free to reuse that RAM once the
	 * segments below have been copied out of it.  Only a segment that
	 * lands on top of the payload would corrupt the copy, so that is
	 * checked per segment further down.  The original device tree is
	 * likewise dead by the time XNU runs (it gets our ADT instead).
	 */
	if (overlap(p->phys_base, p->mem_size, shim_start, shim_end - shim_start)) FAIL("kernel/shim overlap");

	for (unsigned i = 0; i < fdt->reserved_count; i++)
		if (overlap(p->phys_base, p->mem_size,
			    fdt->reserved[i].base, fdt->reserved[i].size))
			FAIL("kernel/reserved overlap");

	int terminated = 0;
	for (unsigned i = 0; i < XNU_CMDLINE; i++)
		if (!p->command_line[i]) { terminated = 1; break; }
	if (!terminated) FAIL("cmdline not terminated");

	if (p->entry_va < p->virt_base) FAIL("entry < virt_base");
	if (p->entry_va - p->virt_base >= p->mem_size) FAIL("entry outside mem");

	plan->entry_pa = p->phys_base + (p->entry_va - p->virt_base);

	const struct xnu_segment *segs = (const void *)((const unsigned char *)p + p->segments_offset);
	uint64_t end = p->phys_base;
	int entry_found = 0;

	for (unsigned i = 0; i < p->segment_count; i++) {
		const struct xnu_segment *s = &segs[i];
		if (!s->memory_size) FAIL("seg no size");
		if ((s->physical & 7)) FAIL("seg phys align");
		if ((s->file_offset & 7)) FAIL("seg file align");
		if (!inside(s->physical, s->memory_size, p->phys_base, p->mem_size))
			FAIL("seg outside kernel");
		if (s->file_size > s->memory_size) FAIL("seg file > mem");
		if (s->file_offset < p->data_offset) FAIL("seg file < data_off");
		if (s->file_offset > length) FAIL("seg file > len");
		if (s->file_size > length - s->file_offset) FAIL("seg file overflow");
		if (overlap(s->physical, s->memory_size, payload_pa, length))
			FAIL("seg/payload overlap");

		for (unsigned j = 0; j < i; j++)
			if (overlap(s->physical, s->memory_size,
				    segs[j].physical, segs[j].memory_size))
				FAIL("seg overlap");

		if (inside(plan->entry_pa, 4, s->physical, s->file_size))
			entry_found++;

		if (s->physical + s->memory_size > end)
			end = s->physical + s->memory_size;
	}

	if (entry_found != 1) FAIL("entry not in exactly 1 seg");
	if (aligned(end) != p->kernel_end_pa) FAIL("kernel_end_pa mismatch");

	plan->adt_pa = aligned(end);
	plan->args_pa = plan->adt_pa + XNU_PAGE;
	plan->top = plan->args_pa + XNU_PAGE;

	if (!inside(plan->adt_pa, 2 * XNU_PAGE, p->phys_base, p->mem_size))
		FAIL("adt outside kernel");

	#undef FAIL
	return 0;
}

/* Copy bytes to physical address */
static void copy_bytes(uint64_t dest, const void *source, uint64_t size)
{
	volatile unsigned char *d = (void *)(uintptr_t)dest;
	const unsigned char *s = source;
	for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

/*
 * Entropy fallback for boards whose TRNG is unavailable (and for QEMU).
 * The kernel refuses to start without a bootloader seed, so stir the cycle
 * counter into a xorshift and hand that over instead of panicking.
 */
static void seed_from_cycles(unsigned char *seed, size_t size, uint32_t timebase)
{
	uint64_t t;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
	uint64_t x = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)timebase << 32) ^ t;

	for (size_t i = 0; i < size; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		seed[i] = (unsigned char)(x >> 33);
	}
}


static void zero_bytes(uint64_t dest, uint64_t size)
{
	volatile unsigned char *d = (void *)(uintptr_t)dest;
	for (uint64_t i = 0; i < size; i++) d[i] = 0;
}

/* Main handoff: load kernel, build ADT + boot_args, jump */
extern char _head[], _end[];
extern size_t dandelion_make_adt(void *buffer, size_t capacity,
				  uint64_t phys_base, uint64_t mem_size,
				  uint64_t kernel_end_pa, uint32_t timebase,
				  const unsigned char *seed, size_t seed_bytes);

#if defined(__aarch64__)
/* The jump itself is aarch64 only; host builds only use handoff_validate(). */
const char *handoff_boot(const struct fdt_info *fdt, uint64_t fdt_pa, uint64_t payload_addr, uint64_t payload_size)
{
	/* Find payload - either from explicit address or from initrd */
	uint64_t pay_addr = payload_addr;
	uint64_t pay_size = payload_size;

	if (!pay_addr) {
		if (fdt->initrd_end <= fdt->initrd_start || fdt->initrd_start & 7)
			return "NO_PAYLOAD";
		pay_addr = fdt->initrd_start;
		pay_size = fdt->initrd_end - fdt->initrd_start;
	}

	if (pay_size < sizeof(struct xnu_payload) || pay_size > 0x8000000 ||
	    !covered(pay_addr, pay_size, fdt))
		return "NO_PAYLOAD";

	const struct xnu_payload *p = (void *)(uintptr_t)pay_addr;
	struct handoff_plan plan;

	if (handoff_validate(p, pay_size, pay_addr, fdt, fdt_pa,
			     (uintptr_t)_head, (uintptr_t)_end, &plan))
		return "PLAN_REJECTED";

	/* Check CPU state for EL1 handoff */
	uint64_t el, sctlr, timebase;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timebase));

	if (el != 4 || (sctlr & 0x1005) || timebase > 0xffffffff)
		return "CPU_STATE_REJECTED";

	/* Generate ADT (with a bootloader entropy seed; the kernel panics
	 * without one, see osfmk/prng/prng_random.c) */
	unsigned char seed[64];
#ifdef QEMU_VIRT
	seed_from_cycles(seed, sizeof(seed), (uint32_t)timebase);
#else
	if (mt6762_random_seed(seed, (uint32_t)timebase))
		seed_from_cycles(seed, sizeof(seed), (uint32_t)timebase);
#endif

	static unsigned char adt_buffer[XNU_PAGE] __attribute__((aligned(8)));
	size_t adt_size = dandelion_make_adt(adt_buffer, sizeof(adt_buffer),
					     p->phys_base, p->mem_size,
					     plan.top, (uint32_t)timebase,
					     seed, sizeof(seed));
	if (!adt_size) return "ADT_FAILED";

	/* Load kernel segments */
	const struct xnu_segment *segs = (const void *)((const unsigned char *)p + p->segments_offset);
	for (unsigned i = 0; i < p->segment_count; i++) {
		const struct xnu_segment *s = &segs[i];
		copy_bytes(s->physical,
			   (const unsigned char *)p + s->file_offset,
			   s->file_size);
		zero_bytes(s->physical + s->file_size,
			   s->memory_size - s->file_size);
	}

	/* Copy ADT */
	zero_bytes(plan.adt_pa, 2 * XNU_PAGE);
	copy_bytes(plan.adt_pa, adt_buffer, adt_size);
	zero_bytes((uintptr_t)adt_buffer, sizeof(adt_buffer));

	/*
	 * Everything the screen shows after this point comes from the kernel.
	 * Leave a clear boundary so a hang before XNU's console exists is
	 * distinguishable from a hang after it.
	 */
	fb_puts("\n--- XNU ---\n", 0xFFFFFF00);
	fb_puthex("payload    ", pay_addr, 0xFFFFFF00);
	fb_puthex("entry_pa   ", plan.entry_pa, 0xFFFFFF00);
	fb_puthex("args_pa    ", plan.args_pa, 0xFFFFFF00);
	fb_puthex("adt_pa     ", plan.adt_pa, 0xFFFFFF00);
	fb_puthex("adt_size   ", adt_size, 0xFFFFFF00);
	fb_puthex("top        ", plan.top, 0xFFFFFF00);
	fb_puts("JUMPING\n", 0xFF00FF00);

	/* Build boot_args */
	struct xnu_boot_args ba;
	zero_bytes((uintptr_t)&ba, sizeof(ba));
	ba.revision = ba.version = 2;
	ba.virt_base = p->virt_base;
	ba.phys_base = p->phys_base;
	ba.mem_size = ba.mem_size_actual = p->mem_size;
	ba.top_of_kernel_data = plan.top;
	ba.device_tree = p->virt_base + (plan.adt_pa - p->phys_base);
	ba.device_tree_length = (uint32_t)adt_size;
	for (unsigned i = 0; i < XNU_CMDLINE; i++)
		ba.command_line[i] = p->command_line[i];

	/*
	 * Hand the panel to XNU so it can bring up its own console: this board
	 * has no usable serial port, so the framebuffer is the only output.
	 *
	 * Bit 0 of v_baseAddr tells XNU the address is physical and needs to be
	 * mapped (see initialize_screen() in osfmk/console/video_console.c).
	 * v_depth packs depth (bits 0-7), rotation (8-15), scale-1 (16-23) and
	 * the boot rotation (24-31).
	 */
	ba.video.base = FB_BASE | 1;
	ba.video.display = 1;
	ba.video.rowbytes = FB_STRIDE;
	ba.video.width = FB_W;
	ba.video.height = FB_H;
	ba.video.depth = 32;

	copy_bytes(plan.args_pa, &ba, sizeof(ba));

	/* Flush caches and jump */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");

	((void (*)(uint64_t))(uintptr_t)plan.entry_pa)(plan.args_pa);

	return "KERNEL_RETURNED";
}
#endif /* __aarch64__ */
