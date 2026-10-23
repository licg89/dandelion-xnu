/* Dandelion boot shim - FDT parser
 * Parses the Flattened Device Tree passed by LK.
 */
#ifndef DANDELION_FDT_H
#define DANDELION_FDT_H
#include <stdint.h>
#include <stddef.h>

#define FDT_MAX_MEMORY    8
#define FDT_MAX_RESERVED  32

#define FDT_MAX_GIC_REGIONS 3
#define FDT_MAX_TIMER_IRQS  8
#define FDT_MAX_CPUS        8
#define FDT_COMPAT_LEN      48
#define FDT_ENABLE_LEN      16

struct fdt_range { uint64_t base, size; };

struct fdt_gic_info {
	int found;
	unsigned regions;
	uint64_t base[FDT_MAX_GIC_REGIONS];
	uint64_t size[FDT_MAX_GIC_REGIONS];
	char compatible[FDT_COMPAT_LEN];
};

struct fdt_timer_info {
	int found;
	uint32_t frequency;
	unsigned irq_count;
	uint32_t irq[FDT_MAX_TIMER_IRQS];   /* absolute INTIDs */
	char compatible[FDT_COMPAT_LEN];
};

struct fdt_mmio_info {
	int found;
	uint64_t base, size;
	char compatible[FDT_COMPAT_LEN];
};

struct fdt_cpu_info {
	unsigned count;
	uint64_t mpidr[FDT_MAX_CPUS];
	char enable_method[FDT_ENABLE_LEN];
};

/*
 * Hardware addresses as described by the *device's own* DTB.  The shim uses
 * this to report ground truth instead of trusting hardcoded values.
 */
struct fdt_platform {
	struct fdt_gic_info gic;
	struct fdt_timer_info timer;
	struct fdt_mmio_info uart;
	struct fdt_mmio_info wdt;
	struct fdt_cpu_info cpus;
};

struct fdt_info {
	uint32_t bytes;
	unsigned memory_count;
	unsigned reserved_count;
	struct fdt_range memory[FDT_MAX_MEMORY];
	struct fdt_range reserved[FDT_MAX_RESERVED];
	uint64_t initrd_start;
	uint64_t initrd_end;
};

int fdt_inspect(const void *data, size_t available, struct fdt_info *info);
int fdt_probe_platform(const void *data, size_t available,
		       struct fdt_platform *out);

#endif
