/*
 * Dandelion XNU boot shim for QEMU's aarch64 "virt" machine -- the C half.
 *
 * What the phone's DarwinBoot does for XNU, this does: put the kernel image at
 * a 256 MiB-aligned physical base, describe the machine in an Apple device
 * tree, fill in struct boot_args, and jump to the kernel's entry point with
 * x0 pointing at the boot args.
 *
 * The numbers are the ones the phone's firmware log reports at hand-off, so
 * the kernel sees the same shape of boot it sees on the device:
 *
 *   phys base   0x4D004000   (kernel pa, from the log; 256 MiB window)
 *   virt base   0xFFFFFFF007004000
 *                            VM_KERNEL_LINK_ADDRESS.  start.s builds its
 *                            bootstrap block mappings from both bases, and a
 *                            2 MiB block descriptor drops the low 21 bits of
 *                            the address, so the two bases must agree modulo
 *                            2 MiB.  0x4D004000 & 0x1fffff == 0x4000 and
 *                            0xFFFFFFF007004000 & 0x1fffff == 0x4000: they do.
 *   entry pa    0x4D3CB488   (0x4D004000 + 0x3C7488) -- exactly the
 *                            "entry pa" the phone's log shows.
 *   ADT         at the first 16 KiB boundary above the kernel image
 *   boot_args   one 16 KiB page above the ADT
 *   topOfKernelData  one page above that
 *
 * The only thing this shim cannot copy from the phone is the ADT itself: the
 * phone's blob describes MediaTek hardware.  See adt_qemu.c.
 */
#include <stdint.h>
#include <stddef.h>

/* ---- the hand-off layout ------------------------------------------------ */

#define PHYS_BASE       0x4D004000UL
#define VIRT_BASE       0xFFFFFFF007004000UL	/* VM_KERNEL_LINK_ADDRESS */
#define MEM_SIZE        0x10000000UL		/* 256 MiB, as on the phone */
#define MACHO_PA        0x48000000UL		/* -device loader puts it here */
#define RAMDISK_PA      0x4E000000UL		/* ... and the ramdisk here */
#define XNU_PAGE        0x4000UL

/*
 * Size of the ramdisk, filled in by build.sh from the image it was pointed at
 * (Resources/UniXNU/kernel.macho.rd by default).  0 means "no ramdisk": the
 * ADT then says so and the kernel boots without a root device.
 */
#ifndef RAMDISK_SIZE
#define RAMDISK_SIZE    0
#endif

/* ---- QEMU virt devices -------------------------------------------------- */

#define PL011_PA        0x09000000UL
#define PL011_DR        (PL011_PA + 0x00)
#define PL011_FR        (PL011_PA + 0x18)
#define PL011_CR        (PL011_PA + 0x30)
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)
#define PL011_FR_TXFF   (1u << 5)

/* ---- the same boot_args the kernel expects (pexpert/arm64/boot.h) ------- */

#define XNU_CMDLINE 608

struct xnu_video {
	uint64_t base, display, rowbytes, width, height, depth;
};

struct xnu_boot_args {
	uint16_t revision, version;
	uint64_t virt_base, phys_base, mem_size, top_of_kernel_data;
	struct xnu_video video;
	uint32_t machine_type;
	uint64_t device_tree;
	uint32_t device_tree_length;
	char command_line[XNU_CMDLINE];
	uint64_t boot_flags, mem_size_actual;
};

/* Framebuffer: the phone's panel geometry.  QEMU has no display here, so the
 * video console paints into RAM; it is dead weight, but the kernel's console
 * path expects one and it keeps behavior identical to the phone. */
#define FB_PA      0x7EC50000UL
#define FB_STRIDE  2944
#define FB_W       736
#define FB_H       1600

extern size_t qemu_make_adt(void *buffer, size_t capacity,
    uint64_t phys_base, uint64_t mem_size, uint64_t kernel_end_pa,
    uint32_t timebase, uint32_t cpu_count,
    uint64_t ramdisk_pa, uint32_t ramdisk_size,
    const unsigned char *seed, size_t seed_bytes);

/* ---- tiny PL011 writer -------------------------------------------------- */

static inline uint32_t rd32(uint64_t pa)
{
	return *(volatile uint32_t *)(uintptr_t)pa;
}
static inline void wr32(uint64_t pa, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)pa = v;
}

static void uart_init(void)
{
	uint32_t cr = rd32(PL011_CR);

	if ((cr & (PL011_CR_UARTEN | PL011_CR_TXE)) !=
	    (PL011_CR_UARTEN | PL011_CR_TXE)) {
		wr32(PL011_CR, cr | PL011_CR_UARTEN | PL011_CR_TXE);
	}

	for (unsigned spin = 0; spin < 100000; spin++) {
		if (!(rd32(PL011_FR) & PL011_FR_TXFF)) {
			break;
		}
	}
}

static void putc_(char c)
{
	for (unsigned spin = 0; spin < 1000000; spin++) {
		if (!(rd32(PL011_FR) & PL011_FR_TXFF)) {
			break;
		}
	}
	wr32(PL011_DR, (uint32_t)(unsigned char)c);
}

static void puts_(const char *s)
{
	while (*s) {
		putc_(*s++);
	}
}

static void puthex_(uint64_t v)
{
	char buf[17];
	buf[16] = 0;
	for (int i = 15; i >= 0; i--) {
		buf[i] = "0123456789abcdef"[v & 0xf];
		v >>= 4;
	}
	puts_(buf);
}

static void putdec_(uint64_t v)
{
	char buf[21];
	int i = 20;

	buf[20] = 0;
	if (!v) {
		buf[--i] = '0';
	}
	while (v && i > 0) {
		buf[--i] = (char)('0' + (v % 10));
		v /= 10;
	}
	puts_(&buf[i]);
}

/* ---- memory helpers ----------------------------------------------------- */

static void zero(uint64_t pa, uint64_t bytes)
{
	volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pa;

	while (bytes >= 8) {
		*p++ = 0;
		bytes -= 8;
	}
	volatile unsigned char *b = (volatile unsigned char *)p;
	while (bytes--) {
		*b++ = 0;
	}
}

static void copy(uint64_t to, const void *from, uint64_t bytes)
{
	volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)to;
	const uint64_t *s = (const uint64_t *)from;

	while (bytes >= 8) {
		*d++ = *s++;
		bytes -= 8;
	}
	volatile unsigned char *db = (volatile unsigned char *)d;
	const unsigned char *sb = (const unsigned char *)s;
	while (bytes--) {
		*db++ = *sb++;
	}
}

/* ---- Mach-O ------------------------------------------------------------- */

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype;
	uint32_t ncmds, sizeofcmds, flags, reserved;
};
struct load_command {
	uint32_t cmd, cmdsize;
};
struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize;
	uint64_t fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

#define MH_MAGIC_64    0xfeedfacfU
#define LC_SEGMENT_64  0x19U
#define LC_UNIXTHREAD  0x05U
#define ARM_THREAD_STATE64 6U

static uint32_t rd32le(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const unsigned char *p)
{
	return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

/* Walk the load commands.  Returns the kernel's entry VA, fills the segment
 * span and, for LC_SEGMENT_64, the physical copy of a segment. */
static int load_kernel(uint64_t *entry_va_out, uint64_t *end_va_out,
    unsigned *segment_count_out)
{
	const struct mach_header_64 *mh = (const void *)(uintptr_t)MACHO_PA;
	const unsigned char *file = (const unsigned char *)(uintptr_t)MACHO_PA;
	uint64_t entry_va = 0, end_va = 0;
	unsigned segments = 0;
	uint32_t offset = sizeof(*mh);

	if (mh->magic != MH_MAGIC_64 || mh->cputype != 0x0100000CU) {
		puts_("SHIM: not an arm64 Mach-O at ");
		puthex_(MACHO_PA);
		puts_(" (magic ");
		puthex_(mh->magic);
		puts_(")\n");
		return -1;
	}

	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *lc =
		    (const struct load_command *)(const void *)(file + offset);

		if (lc->cmdsize < sizeof(*lc)) {
			puts_("SHIM: bad load command size\n");
			return -1;
		}
#if SHIM_VERBOSE
		puts_("SHIM: lc ");
		putdec_(i);
		puts_(" cmd ");
		puthex_(lc->cmd);
		puts_(" size ");
		putdec_(lc->cmdsize);
		puts_(" file off ");
		putdec_(offset);
		puts_("\n");
#endif
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *seg = (const void *)lc;

			if (seg->vmsize == 0) {
				offset += lc->cmdsize;
				continue;
			}
			if (seg->vmaddr < VIRT_BASE) {
				puts_("SHIM: segment below virt base: ");
				puthex_(seg->vmaddr);
				puts_("\n");
				return -1;
			}
			{
				uint64_t pa = PHYS_BASE + (seg->vmaddr - VIRT_BASE);

				if (seg->filesize) {
					copy(pa, file + seg->fileoff, seg->filesize);
				}
				if (seg->vmsize > seg->filesize) {
					zero(pa + seg->filesize,
					    seg->vmsize - seg->filesize);
				}
			}
			segments++;
			if (seg->vmaddr + seg->vmsize > end_va) {
				end_va = seg->vmaddr + seg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/*
			 * struct thread_command { cmd, cmdsize, flavor, count }
			 * followed by count 32-bit words of thread state; for
			 * ARM_THREAD_STATE64 that is x0-x28, fp, lr, sp, pc, cpsr,
			 * so the pc is word 32 of the state.  Everything is read
			 * bytewise: the load commands are only 4-byte aligned.
			 */
			const unsigned char *state = file + offset + 8;
			uint32_t flavor = rd32le(state);
			uint32_t count = rd32le(state + 4);

			if (flavor == ARM_THREAD_STATE64 && count >= 33) {
				entry_va = rd64le(state + 8 + 32 * 8);
			}
		}
		offset += lc->cmdsize;
	}

	if (!segments || !entry_va || !end_va) {
		puts_("SHIM: incomplete Mach-O (segments ");
		putdec_(segments);
		puts_(", entry ");
		puthex_(entry_va);
		puts_(")\n");
		return -1;
	}

	*entry_va_out = entry_va;
	*end_va_out = end_va;
	*segment_count_out = segments;
	return 0;
}

/* ---- entry -------------------------------------------------------------- */

/* Entropy for /chosen/random-seed: the kernel panics without one.  There is
 * no TRNG here, so stir the virtual counter. */
static void seed_from_counter(unsigned char *seed, size_t size, uint32_t timebase)
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

static uint64_t align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

void shim_main(void)
{
	uint64_t entry_va = 0, end_va = 0;
	unsigned segments = 0;
	uint32_t timebase, cpus = 1;

	uart_init();
	puts_("\n=== dandelion QEMU shim ===\n");

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timebase));
	{
		uint64_t mpidr;
		__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
		(void)mpidr;
	}
	/* The ADT advertises the CPUs the kernel may look for.  QEMU is started
	 * with -smp 1 below (the port has one CPU's worth of bring-up code), so
	 * describe exactly one; extra "stopped" CPUs make XNU's topology code
	 * try to start CPUs that are not there. */
	{
		uint64_t mpidr;
		__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
		(void)mpidr;
	}

	puts_("SHIM: timebase ");
	putdec_(timebase);
	puts_("\nSHIM: mach_kernel at ");
	puthex_(MACHO_PA);
	puts_(", kernel window ");
	puthex_(PHYS_BASE);
	puts_("\n");

	if (load_kernel(&entry_va, &end_va, &segments)) {
		return;
	}

	uint64_t entry_pa = PHYS_BASE + (entry_va - VIRT_BASE);
	uint64_t kernel_end_pa = align_up(PHYS_BASE + (end_va - VIRT_BASE),
	    XNU_PAGE);
	uint64_t adt_pa = kernel_end_pa;
	uint64_t args_pa = adt_pa + XNU_PAGE;
	uint64_t top_pa = args_pa + XNU_PAGE;

	puts_("SHIM: segments ");
	putdec_(segments);
	puts_(", entry ");
	puthex_(entry_va);
	puts_(" -> pa ");
	puthex_(entry_pa);
	puts_("\nSHIM: kernel end ");
	puthex_(kernel_end_pa);
	puts_(", adt ");
	puthex_(adt_pa);
	puts_(", boot_args ");
	puthex_(args_pa);
	puts_("\n");

	if (top_pa > PHYS_BASE + MEM_SIZE) {
		puts_("SHIM: kernel does not fit in the 256 MiB window\n");
		return;
	}

	if (RAMDISK_SIZE) {
		puts_("SHIM: ramdisk ");
		putdec_(RAMDISK_SIZE);
		puts_(" bytes at ");
		puthex_(RAMDISK_PA);
		puts_(" (rd=md0)\n");
	} else {
		puts_("SHIM: no ramdisk: the kernel will have no root device\n");
	}

	/* Nothing above the kernel image may be stale. */
	zero(kernel_end_pa, top_pa - kernel_end_pa);

	unsigned char seed[64];
	seed_from_counter(seed, sizeof(seed), timebase);

	size_t adt_size = qemu_make_adt((void *)(uintptr_t)adt_pa, XNU_PAGE,
	    PHYS_BASE, MEM_SIZE, kernel_end_pa, timebase, cpus,
	    RAMDISK_PA, (uint32_t)RAMDISK_SIZE, seed, sizeof(seed));
	if (!adt_size) {
		puts_("SHIM: ADT did not fit in one page\n");
		return;
	}

	struct xnu_boot_args ba;
	unsigned char *raw = (unsigned char *)&ba;

	for (unsigned i = 0; i < sizeof(ba); i++) {
		raw[i] = 0;
	}
	ba.revision = 2;
	ba.version = 2;
	ba.virt_base = VIRT_BASE;
	ba.phys_base = PHYS_BASE;
	ba.mem_size = MEM_SIZE;
	ba.mem_size_actual = MEM_SIZE;
	ba.top_of_kernel_data = top_pa;
	/*
	 * deviceTreeP is a *virtual* address in the kernel's window: the kernel
	 * adds (virtBase - physBase) to reach its own mappings, and by the time
	 * anything reads the tree the bootstrap V=P map makes both work.
	 */
	ba.device_tree = VIRT_BASE + (adt_pa - PHYS_BASE);
	ba.device_tree_length = (uint32_t)adt_size;
	ba.video.base = FB_PA | 1;	/* bit 0: physical, needs mapping */
	ba.video.display = 1;
	ba.video.rowbytes = FB_STRIDE;
	ba.video.width = FB_W;
	ba.video.height = FB_H;
	ba.video.depth = 32;

	/*
	 * wdt=-1 switches off XNU's interrupt-masked / preemption-disabled
	 * panics (osfmk/arm/arm_init.c, KF_INTERRUPT_MASKED_DEBUG_OVRD's
	 * boot-arg twin).  They compare wall time against a 13.6 ms budget
	 * and QEMU's TCG runs this kernel about three times slower than the
	 * phone, so `waitForSystemMapper()` in bsd_init trips the check even
	 * though nothing is wrong.  Everything else here matches the phone:
	 * UniXNU.toml passes "-v keepsyms=1 debug=0x144 rd=md0".
	 */
	static const char cmdline[] =
	    "-v serial=3 debug=0x144 keepsyms=1 -noprogress wdt=-1 rd=md0 "
	    "-dl_uart -dl_qemu";
	if (sizeof(cmdline) > XNU_CMDLINE) {
		puts_("SHIM: command line too long\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(cmdline); i++) {
		ba.command_line[i] = cmdline[i];
	}

	copy(args_pa, &ba, sizeof(ba));

	/* Everything above was written with the caches off; make it visible. */
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

	puts_("SHIM: adt ");
	putdec_(adt_size);
	puts_(" bytes, jumping to the kernel\n");

	/*
	 * x0 = boot args.  start.s copies it with the MMU off, so this is the
	 * physical address, exactly as the phone's hand-off passes it.
	 */
	((void (*)(uint64_t))entry_pa)(args_pa);

	puts_("SHIM: the kernel returned\n");
	for (;;) {
		__asm__ volatile("wfi");
	}
}
