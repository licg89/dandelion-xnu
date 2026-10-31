/*
 * Apple Device Tree (ADT) serializer for QEMU's aarch64 "virt" machine.
 *
 * This is bootshim/adt.c with the device addresses of the Redmi 9A replaced by
 * QEMU's, and with the "emmc-log" node dropped: XNU (
 * pexpert/arm/pe_serial.c, XNU_EMMC_SERIAL) maps that node's "reg" and drives
 * the eMMC controller as its console, and there is no eMMC behind QEMU.
 *
 * Why a QEMU-specific tree at all, rather than the 6044-byte blob the phone
 * ships (Resources/UniXNU/kernel.macho.adt): the kernel does not get its
 * interrupt controller base from a board header, it gets it from this tree.
 * pexpert/arm/pe_identify_machine.c looks up /arm-io/gic and hands those
 * addresses to pe_fiq.c, which programs them.  The phone tree says
 * 0x0c000000/0x0c100000; QEMU's GICv3 is at 0x08000000/0x080a0000, and a
 * kernel told the wrong addresses faults on the first distributor read.
 * The layout, property names and sizes below are byte-identical to the
 * phone's builder, so this is the same ABI -- only the numbers differ.
 *
 * ADT format: each node is (property_count, child_count) followed by its
 * properties and then its children; each property is a 32-byte NUL-padded key,
 * a 32-bit length and 4-byte-aligned value bytes.
 */
#include <stdint.h>
#include <stddef.h>

/* QEMU virt (hw/arm/virt.c): GICv3 distributor, redistributors, PL011. */
#define ADT_GIC_BASE     0x08000000ULL
#define ADT_GIC_SIZE     0x00010000ULL
#define ADT_GICR_BASE    0x080A0000ULL
#define ADT_GICR_SIZE    0x000F6000ULL
#define ADT_UART_BASE    0x09000000ULL

struct writer {
	unsigned char *data;
	size_t used, capacity;
	int error;
};

static void w_byte(struct writer *w, unsigned char v)
{
	if (w->used >= w->capacity) {
		w->error = 1;
	} else {
		w->data[w->used] = v;
	}
	w->used++;
}
static void w_u32(struct writer *w, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++) {
		w_byte(w, (unsigned char)(v >> (8 * i)));
	}
}
static void w_u64(struct writer *w, uint64_t v)
{
	w_u32(w, (uint32_t)v);
	w_u32(w, (uint32_t)(v >> 32));
}
static void w_node(struct writer *w, uint32_t props, uint32_t children)
{
	w_u32(w, props);
	w_u32(w, children);
}
static void w_prop(struct writer *w, const char *key, const void *val, uint32_t len)
{
	unsigned i = 0;

	while (i < 32 && key[i]) {
		w_byte(w, (unsigned char)key[i]);
		i++;
	}
	if (i == 32) {
		w->error = 1;
		return;
	}
	while (i++ < 32) {
		w_byte(w, 0);
	}
	w_u32(w, len);
	const unsigned char *p = val;
	for (uint32_t j = 0; j < len; j++) {
		w_byte(w, p[j]);
	}
	while (w->used & 3) {
		w_byte(w, 0);
	}
}
static void w_text(struct writer *w, const char *key, const char *val)
{
	uint32_t len = 0;

	while (val[len]) {
		len++;
	}
	w_prop(w, key, val, len + 1);
}
static void w_word(struct writer *w, const char *key, uint32_t val)
{
	w_prop(w, key, &val, sizeof(val));
}

size_t qemu_make_adt(void *buffer, size_t capacity,
    uint64_t phys_base, uint64_t mem_size, uint64_t kernel_end_pa,
    uint32_t timebase, uint32_t cpu_count,
    const unsigned char *seed, size_t seed_bytes)
{
	if ((seed_bytes && (!seed || seed_bytes != 64)) || (!seed_bytes && seed)) {
		return 0;
	}
	if (cpu_count == 0 || cpu_count > 8) {
		cpu_count = 8;
	}

	struct writer w = { buffer, 0, capacity, 0 };
	uint64_t mem_range[2] = { phys_base, kernel_end_pa - phys_base };

	/* Root: 6 properties, 4 children. */
	w_node(&w, 6, 4);
	w_text(&w, "name", "device-tree");
	w_text(&w, "device_type", "device-tree");
	static const char compat[] = "qemu,virt\0arm,armv8";
	w_prop(&w, "compatible", compat, sizeof(compat));
	w_text(&w, "model", "QEMU virt aarch64");
	w_word(&w, "#address-cells", 2);
	w_word(&w, "#size-cells", 2);

	/*
	 * /chosen: name, boot-manifest-hash, random-seed, dram-base, dram-size,
	 * darwinboot-memory-size, and one child (memory-map).
	 *
	 * The kernel refuses to start without a bootloader entropy seed
	 * (osfmk/prng/prng_random.c), so the shim always hands one over.
	 */
	static const uint64_t darwinboot_memory_size = 0x10000000ULL;	/* 256 MiB */
	w_node(&w, seed_bytes ? 6 : 5, 1);
	w_text(&w, "name", "chosen");
	static const unsigned char manifest[48] = {
		0x06, 0xc2, 0x72, 0x0d, 0x73, 0x22, 0x16, 0x6d,
		0x2d, 0x70, 0xfd, 0x97, 0xed, 0x5a, 0x8c, 0x56,
		0x9a, 0xf4, 0x11, 0x58, 0x93, 0xe2, 0x03, 0x2d,
		0xd8, 0x61, 0x2e, 0xb7, 0x6d, 0x6e, 0x79, 0xfe,
		0x4a, 0x63, 0x83, 0xc1, 0x11, 0xdb, 0x2a, 0x61,
		0xe1, 0xb2, 0x35, 0x8c, 0x2e, 0x94, 0x21, 0x93,
	};
	w_prop(&w, "boot-manifest-hash", manifest, sizeof(manifest));
	if (seed_bytes) {
		w_prop(&w, "random-seed", seed, (uint32_t)seed_bytes);
	}
	w_prop(&w, "dram-base", &phys_base, 8);
	w_prop(&w, "dram-size", &mem_size, 8);
	w_prop(&w, "darwinboot-memory-size", &darwinboot_memory_size, 8);

	/*
	 * /chosen/memory-map.  IOFindBSDRoot() looks for a "RAMDisk" entry by
	 * name; two zero cells mean "no ramdisk", which is what this boot is:
	 * the point of the QEMU run is to reach the heap corruption, not to
	 * mount a root filesystem.
	 */
	w_node(&w, 3, 0);
	w_text(&w, "name", "memory-map");
	w_prop(&w, "Kernel-Dandelion", mem_range, sizeof(mem_range));
	{
		static const uint64_t ramdisk_slot[2] = { 0, 0 };
		w_prop(&w, "RAMDisk", ramdisk_slot, sizeof(ramdisk_slot));
	}

	/* /cpus: 3 properties, N children. */
	w_node(&w, 3, cpu_count);
	w_text(&w, "name", "cpus");
	w_word(&w, "#address-cells", 1);
	w_word(&w, "#size-cells", 0);

	static const char compat_cpu[] = "arm,cortex-a53\0ARM,v8";
	static const uint32_t cpu_mpidr[8] = {
		0x000, 0x001, 0x002, 0x003,
		0x100, 0x101, 0x102, 0x103,
	};
	for (unsigned i = 0; i < cpu_count; i++) {
		char name[] = "cpu0";
		name[3] += (char)(i & 7);
		uint32_t cluster = i >> 2;

		w_node(&w, 11, 0);
		w_text(&w, "name", name);
		w_text(&w, "device_type", "cpu");
		w_prop(&w, "compatible", compat_cpu, sizeof(compat_cpu));
		w_word(&w, "reg", cpu_mpidr[i]);
		w_text(&w, "state", i ? "stopped" : "running");
		w_word(&w, "timebase-frequency", timebase);
		/*
		 * pe_identify_machine() derives gPEClockFrequencyInfo from these;
		 * a missing bus frequency makes its rate denominators zero.
		 */
		w_word(&w, "clock-frequency", 1000000000u);
		w_word(&w, "bus-frequency", 250000000u);
		w_word(&w, "peripheral-frequency", 250000000u);
		w_word(&w, "cpu-id", i & 3);
		w_word(&w, "cluster-id", cluster);
	}

	/* /arm-io: 6 properties, 3 children (uart0, gic, timer). */
	w_node(&w, 6, 3);
	w_text(&w, "name", "arm-io");
	w_text(&w, "device_type", "soc");
	w_text(&w, "compatible", "qemu,virt");
	/*
	 * pe_arm_get_soc_base_phys() takes the second cell of "ranges" as the
	 * SoC base and every child's "reg" is relative to it, so put the
	 * interrupt controller at offset 0.
	 */
	uint64_t io_ranges[3] = { 0, ADT_GIC_BASE, 0x14000000 };
	w_prop(&w, "ranges", io_ranges, sizeof(io_ranges));
	w_word(&w, "#address-cells", 2);
	w_word(&w, "#size-cells", 2);

	/* /arm-io/uart0: the PL011 the shim and the kernel's rprintf sink use. */
	w_node(&w, 4, 0);
	w_text(&w, "name", "uart0");
	w_text(&w, "device_type", "serial");
	w_text(&w, "compatible", "arm,pl011");
	uint64_t uart_reg[2] = { ADT_UART_BASE - ADT_GIC_BASE, 0x1000 };
	w_prop(&w, "reg", uart_reg, sizeof(uart_reg));

	/* /arm-io/gic: GICv3 as QEMU models it (gic-version=3). */
	w_node(&w, 6, 0);
	w_text(&w, "name", "gic");
	w_text(&w, "device_type", "interrupt-controller");
	w_text(&w, "compatible", "arm,gic-v3");
	w_text(&w, "interrupt-controller", "master");
	uint64_t gic_reg[4] = {
		0,				/* GICD @ soc base */
		ADT_GIC_SIZE,
		ADT_GICR_BASE - ADT_GIC_BASE,
		ADT_GICR_SIZE,
	};
	w_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	w_word(&w, "virtual-timer-intid", 27);

	/* /arm-io/timer: ARM generic timer, system registers only. */
	w_node(&w, 5, 0);
	w_text(&w, "name", "timer");
	w_text(&w, "device_type", "timer");
	w_text(&w, "compatible", "arm,armv8-timer");
	uint64_t timer_reg[2] = { 0, 0x1000 };
	w_prop(&w, "reg", timer_reg, sizeof(timer_reg));
	w_word(&w, "timebase-frequency", timebase);

	/* /defaults. */
	w_node(&w, 2, 0);
	w_text(&w, "name", "defaults");
	w_word(&w, "no-effaceable-storage", 1);

	return w.error ? 0 : w.used;
}
