/* Dandelion Apple Device Tree (ADT) serializer
 * Based on mouseos adt.c, adapted for Redmi 9A / MT6762G
 *
 * ADT format: Apple's proprietary flattened device tree.
 * Each node has: properties_count, children_count, then properties, then children.
 * Each property: 32-byte key (null padded), 32-bit length, value bytes (4-byte aligned).
 */
#include <stdint.h>
#include <stddef.h>
#include "pe_dandelion.h"

/*
 * Where the ADT says the platform's devices live.  On the phone these are the
 * MediaTek addresses from DANDELION.h; under QEMU's virt machine the same
 * nodes have to describe QEMU's GICv3 and PL011 instead, otherwise the kernel
 * maps the wrong registers and hangs before it can say anything.
 */
#ifdef QEMU_VIRT
#define ADT_SOC_COMPAT   "qemu,virt"
#define ADT_UART_COMPAT  "arm,pl011"
#define ADT_UART_BASE    0x09000000ULL
#define ADT_GIC_BASE     0x08000000ULL
#define ADT_GIC_SIZE     0x00010000ULL
#define ADT_GICR_BASE    0x080A0000ULL
#define ADT_GICR_SIZE    0x000F6000ULL
#else
#define ADT_SOC_COMPAT   "mediatek,mt6762g"
#define ADT_UART_COMPAT  "mediatek,mt6577-uart"
#define ADT_UART_BASE    DANDELION_UART_BASE
#define ADT_GIC_BASE     DANDELION_GIC_BASE
#define ADT_GIC_SIZE     DANDELION_GIC_SIZE
#define ADT_GICR_BASE    DANDELION_GICR_BASE
#define ADT_GICR_SIZE    DANDELION_GICR_SIZE
#endif

struct writer { unsigned char *data; size_t used, capacity; int error; };

static void w_byte(struct writer *w, unsigned char v)
{
	if (w->used >= w->capacity) w->error = 1;
	else w->data[w->used] = v;
	w->used++;
}
static void w_u32(struct writer *w, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		w_byte(w, (unsigned char)(v >> (8 * i)));
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
	while (i < 32 && key[i]) { w_byte(w, (unsigned char)key[i]); i++; }
	if (i == 32) { w->error = 1; return; }
	while (i++ < 32) w_byte(w, 0);
	w_u32(w, len);
	const unsigned char *p = val;
	for (uint32_t j = 0; j < len; j++) w_byte(w, p[j]);
	while (w->used & 3) w_byte(w, 0);
}
static void w_text(struct writer *w, const char *key, const char *val)
{
	uint32_t len = 0;
	while (val[len]) len++;
	w_prop(w, key, val, len + 1);
}
static void w_word(struct writer *w, const char *key, uint32_t val)
{
	w_prop(w, key, &val, sizeof(val));
}

/*
 * Build ADT for Dandelion / MT6762G
 *
 * Layout:
 * / (root)
 *   /chosen
 *     /memory-map
 *   /cpus
 *     /cpu0 ... /cpu7
 *   /arm-io
 *     /uart0
 *     /interrupt-controller
 *     /timer
 */
/*
 * Where XNU's own console writes.
 *
 * patches/xnu/native-emmc-serial.patch looks for an ADT node named "emmc-log"
 * and reads log-lba / log-sectors / clock-div from it.  The LBA is absolute on
 * purpose: the kernel is told where to write instead of being made to parse the
 * GPT, which halves the code and the risk (docs/UNIXNU-EMMC-CONSOLE.md 3.3).
 *
 * flashinfo starts at LBA 122109919 on this device (measured, docs/EMMC-LOG.md).
 * Layout inside it, agreed with tools/mk_unixnu_esp.sh and PayloadBootDxe:
 *
 *     0      .. 30000   FAT volume: \UniXNU.efi, \EFI\UniXNU\*, kernel.macho
 *     30000  .. 30512   XNU log channel, 512 sectors = 256 KiB   <- here
 *     30512  .. 30640   the firmware's own log block, 128 sectors
 *
 * clock-div is 26, which is what DandelionEmmcDxe calibrates to on this board,
 * not the 260 bootshim uses: the kernel inherits the controller as firmware
 * left it.
 */
#define ADT_EMMC_LOG_BASE     0x11230000ULL   /* MSDC0, from DANDELION.h */
#define ADT_EMMC_LOG_SIZE     0x00010000ULL
#define ADT_EMMC_LOG_LBA      (122109919u + 30000u)
#define ADT_EMMC_LOG_SECTORS  512u
#define ADT_EMMC_LOG_CLKDIV   26u

size_t dandelion_make_adt(void *buffer, size_t capacity,
			  uint64_t phys_base, uint64_t mem_size,
			  uint64_t kernel_end_pa,
			  uint32_t timebase,
			  const unsigned char *seed, size_t seed_bytes)
{
	if ((seed_bytes && (!seed || seed_bytes != 64)) ||
	    (!seed_bytes && seed))
		return 0;

	struct writer w = { buffer, 0, capacity, 0 };

	uint64_t mem_range[2] = { phys_base, kernel_end_pa - phys_base };

	/* Root node: 6 properties, 4 children */
	w_node(&w, 6, 4);
	w_text(&w, "name", "device-tree");
	w_text(&w, "device_type", "device-tree");

	/* Compatible: xiaomi,dandelion + mediatek,mt6762g */
	static const char compat[] = "xiaomi,dandelion\0mediatek,mt6762g";
	w_prop(&w, "compatible", compat, sizeof(compat));

	w_text(&w, "model", "Xiaomi Redmi 9A");
	w_word(&w, "#address-cells", 2);
	w_word(&w, "#size-cells", 2);

	/* /chosen node: name, boot-manifest-hash, dram-base, dram-size,
	 * darwinboot-memory-size (+ random-seed when the shim has one), and one
	 * child (memory-map).
	 *
	 * darwinboot-memory-size is what DarwinBoot reads for the size of the
	 * memory window it allocates for the kernel: Boot() passes it straight to
	 * AllocateWindow(&Kernel, WindowSize, &Address), and a missing property
	 * leaves WindowSize at 0, which comes back as "boot refused: Out of
	 * Resources; firmware retained".  Its parser (DarwinBootPkg/Driver/
	 * WindowSize.h) accepts 256 MiB .. 8 GiB aligned to 2 MiB, and
	 * Test/window-size.c lists 0x10000000 as the smallest legal value.
	 *
	 * 256 MiB is the starting choice on purpose: the parser only bounds the
	 * request, the allocation still needs one real conventional-memory run,
	 * so the smallest legal window is the one most likely to succeed on a
	 * device whose memory map we have not characterised yet.  Raise it when
	 * the kernel or a ramdisk actually needs more. */
	static const uint64_t darwinboot_memory_size = 0x10000000ULL;	/* 256 MiB */

	w_node(&w, seed_bytes ? 6 : 5, 1);
	w_text(&w, "name", "chosen");

	/* Boot manifest hash (development namespace, not Apple secure boot) */
	static const unsigned char manifest[48] = {
		0x06,0xc2,0x72,0x0d,0x73,0x22,0x16,0x6d,
		0x2d,0x70,0xfd,0x97,0xed,0x5a,0x8c,0x56,
		0x9a,0xf4,0x11,0x58,0x93,0xe2,0x03,0x2d,
		0xd8,0x61,0x2e,0xb7,0x6d,0x6e,0x79,0xfe,
		0x4a,0x63,0x83,0xc1,0x11,0xdb,0x2a,0x61,
		0xe1,0xb2,0x35,0x8c,0x2e,0x94,0x21,0x93,
	};
	w_prop(&w, "boot-manifest-hash", manifest, sizeof(manifest));

	if (seed_bytes)
		w_prop(&w, "random-seed", seed, (uint32_t)seed_bytes);

	w_prop(&w, "dram-base", &phys_base, 8);
	w_prop(&w, "dram-size", &mem_size, 8);
	w_prop(&w, "darwinboot-memory-size", &darwinboot_memory_size, 8);

	/* /chosen/memory-map: kernel range */
	w_node(&w, 2, 0);
	w_text(&w, "name", "memory-map");
	w_prop(&w, "Kernel-Dandelion", mem_range, sizeof(mem_range));

	/* /cpus node: 3 properties, 8 children */
	w_node(&w, 3, 8);
	w_text(&w, "name", "cpus");
	w_word(&w, "#address-cells", 1);
	w_word(&w, "#size-cells", 0);

	static const char compat_cpu[] = "arm,cortex-a53\0ARM,v8";

	/* 8 CPUs in 2 clusters: Aff0 0-3 (cluster 0), Aff0 0x100-0x103 (cluster 1) */
	static const uint32_t cpu_mpidr[8] = {
		0x000, 0x001, 0x002, 0x003,
		0x100, 0x101, 0x102, 0x103,
	};
	for (unsigned i = 0; i < 8; i++) {
		char name[] = "cpu0";
		name[3] += (i & 7);
		uint32_t cluster = i >> 2;

		w_node(&w, 11, 0);
		w_text(&w, "name", name);
		w_text(&w, "device_type", "cpu");
		w_prop(&w, "compatible", compat_cpu, sizeof(compat_cpu));
		w_word(&w, "reg", cpu_mpidr[i]);
		w_text(&w, "state", i ? "stopped" : "running");
		w_word(&w, "timebase-frequency", timebase);
		/* pe_identify_machine() derives gPEClockFrequencyInfo from these; a
		 * missing bus frequency makes its rate denominators zero. */
		w_word(&w, "clock-frequency", 2000000000u);
		w_word(&w, "bus-frequency", 400000000u);
		w_word(&w, "peripheral-frequency", 100000000u);
		w_word(&w, "cpu-id", i & 3);
		w_word(&w, "cluster-id", cluster);
	}

	/* /arm-io node: 6 properties, 4 children */
	w_node(&w, 6, 4);
	w_text(&w, "name", "arm-io");
	w_text(&w, "device_type", "soc");
	w_text(&w, "compatible", ADT_SOC_COMPAT);

	/*
	 * pe_arm_get_soc_base_phys() takes the second cell of "ranges" as the SoC
	 * base, and every "reg" below is relative to it.  Use the GIC distributor
	 * base so the interrupt controller sits at offset 0.
	 */
	uint64_t io_ranges[3] = { 0, ADT_GIC_BASE, 0x14000000 };
	w_prop(&w, "ranges", io_ranges, sizeof(io_ranges));
	w_word(&w, "#address-cells", 2);
	w_word(&w, "#size-cells", 2);

	/* /arm-io/uart0 */
	w_node(&w, 4, 0);
	w_text(&w, "name", "uart0");
	w_text(&w, "device_type", "serial");
	w_text(&w, "compatible", ADT_UART_COMPAT);
	uint64_t uart_reg[2] = { ADT_UART_BASE - ADT_GIC_BASE, 0x1000 };
	w_prop(&w, "reg", uart_reg, sizeof(uart_reg));

	/*
	 * /arm-io/emmc-log -- the console channel on a board with no UART.
	 *
	 * Five properties.  reg is not optional: pe_serial.c registers the channel
	 * only when
	 *
	 *     emmc_base_vaddr = ml_io_map (soc_base + *reg_prop, *(reg_prop + 1));
	 *     if (emmc_base_vaddr != 0 && emmc_log_lba != 0 && emmc_log_sectors >= 2)
	 *             register_serial_functions (&emmc_serial_functions);
	 *
	 * so without reg the mapping is built from whatever follows it and the
	 * channel silently never registers.  Like every other arm-io child, reg is
	 * relative to the soc base that "ranges" names.
	 */
	w_node(&w, 5, 0);
	w_text(&w, "name", "emmc-log");
	uint64_t emmc_log_reg[2] = { ADT_EMMC_LOG_BASE - ADT_GIC_BASE, ADT_EMMC_LOG_SIZE };
	w_prop(&w, "reg", emmc_log_reg, sizeof(emmc_log_reg));
	uint32_t emmc_log_lba = ADT_EMMC_LOG_LBA;
	uint32_t emmc_log_sectors = ADT_EMMC_LOG_SECTORS;
	uint32_t emmc_log_div = ADT_EMMC_LOG_CLKDIV;
	w_word(&w, "log-lba", emmc_log_lba);
	w_word(&w, "log-sectors", emmc_log_sectors);
	w_word(&w, "clock-div", emmc_log_div);

	/*
	 * /arm-io/gic (GICv3).  pe_init_fiq() reads four 64-bit cells: GICD base
	 * and size, then GICR base and size.  pe_arm_map_interrupt_controller()
	 * reads the same property as two cells, so the layout works for both.
	 */
	w_node(&w, 6, 0);
	w_text(&w, "name", "gic");
	w_text(&w, "device_type", "interrupt-controller");
	w_text(&w, "compatible", "arm,gic-v3");
	w_text(&w, "interrupt-controller", "master");
	uint64_t gic_reg[4] = {
		0,                            /* GICD @ soc base */
		ADT_GIC_SIZE,
		ADT_GICR_BASE - ADT_GIC_BASE,
		ADT_GICR_SIZE,
	};
	w_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	w_word(&w, "virtual-timer-intid", 27);

	/* /arm-io/timer (ARM generic timer, system registers only) */
	w_node(&w, 5, 0);
	w_text(&w, "name", "timer");
	w_text(&w, "device_type", "timer");
	w_text(&w, "compatible", "arm,armv8-timer");
	/* pe_arm_map_interrupt_controller() insists on a "reg" property even
	 * though the arm64 timer is driven through system registers. */
	uint64_t timer_reg[2] = { 0, 0x1000 };
	w_prop(&w, "reg", timer_reg, sizeof(timer_reg));
	w_word(&w, "timebase-frequency", timebase);

	/* /defaults node */
	w_node(&w, 2, 0);
	w_text(&w, "name", "defaults");
	w_word(&w, "no-effaceable-storage", 1);

	return w.error ? 0 : w.used;
}
