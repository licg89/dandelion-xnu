/* Dandelion Apple Device Tree (ADT) serializer
 * Based on mouseos adt.c, adapted for Redmi 9A / MT6762G
 *
 * ADT format: Apple's proprietary flattened device tree.
 * Each node has: properties_count, children_count, then properties, then children.
 * Each property: 32-byte key (null padded), 32-bit length, value bytes (4-byte aligned).
 */
#include <stdint.h>
#include <stddef.h>

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

	/* /chosen node: name, boot-manifest-hash, dram-base, dram-size
	 * (+ random-seed when the shim has one), and one child (memory-map). */
	w_node(&w, seed_bytes ? 5 : 4, 1);
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

	/* /chosen/memory-map: kernel range */
	w_node(&w, 1, 0);
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

		w_node(&w, 8, 0);
		w_text(&w, "name", name);
		w_text(&w, "device_type", "cpu");
		w_prop(&w, "compatible", compat_cpu, sizeof(compat_cpu));
		w_word(&w, "reg", cpu_mpidr[i]);
		w_text(&w, "state", i ? "stopped" : "running");
		w_word(&w, "timebase-frequency", timebase);
		w_word(&w, "cpu-id", i & 3);
		w_word(&w, "cluster-id", cluster);
	}

	/* /arm-io node: 6 properties, 3 children */
	w_node(&w, 6, 3);
	w_text(&w, "name", "arm-io");
	w_text(&w, "device_type", "soc");
	w_text(&w, "compatible", "mediatek,mt6762g");

	/* IO range: 0x0 - 0x20000000 */
	uint64_t io_ranges[3] = { 0, 0x10000000, 0x10000000 };
	w_prop(&w, "ranges", io_ranges, sizeof(io_ranges));
	w_word(&w, "#address-cells", 2);
	w_word(&w, "#size-cells", 2);

	/* /arm-io/uart0 */
	w_node(&w, 4, 0);
	w_text(&w, "name", "uart0");
	w_text(&w, "device_type", "serial");
	w_text(&w, "compatible", "mediatek,mt6577-uart");
	uint64_t uart_reg[2] = { 0x01102000, 0x1000 };
	w_prop(&w, "reg", uart_reg, sizeof(uart_reg));

	/* /arm-io/interrupt-controller (GICv3) */
	w_node(&w, 5, 0);
	w_text(&w, "name", "interrupt-controller");
	w_text(&w, "compatible", "arm,gic-v3");
	w_text(&w, "interrupt-controller", "master");
	/* GICv3: distributor + redistributor (mt6765.dtsi layout) */
	uint64_t gic_reg[8] = {
		0x00000000, 0x0c000000, 0x00000000, 0x00040000,  /* GICD */
		0x00000000, 0x0c100000, 0x00000000, 0x00200000,  /* GICR */
	};
	w_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	w_word(&w, "virtual-timer-intid", 27);

	/* /arm-io/timer (ARM generic timer) */
	w_node(&w, 3, 0);
	w_text(&w, "name", "timer");
	w_text(&w, "compatible", "arm,armv8-timer");
	w_word(&w, "timebase-frequency", timebase);

	/* /defaults node */
	w_node(&w, 2, 0);
	w_text(&w, "name", "defaults");
	w_word(&w, "no-effaceable-storage", 1);

	return w.error ? 0 : w.used;
}
