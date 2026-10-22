/* Dandelion boot shim - FDT parser
 * Parses the Flattened Device Tree passed by LK.
 */
#ifndef DANDELION_FDT_H
#define DANDELION_FDT_H
#include <stdint.h>
#include <stddef.h>

#define FDT_MAX_MEMORY    8
#define FDT_MAX_RESERVED  16

struct fdt_range { uint64_t base, size; };

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

#endif
