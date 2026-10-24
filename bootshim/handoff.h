/* Dandelion handoff interfaces
 * Follows Apple xnu boot_args ABI.
 */
#ifndef DANDELION_HANDOFF_H
#define DANDELION_HANDOFF_H
#include <stddef.h>
#include <stdint.h>
#include "fdt.h"

#define XNU_PAGE     0x4000UL
#define XNU_CMDLINE  608
#define XNU_MAX_SEGS 16

struct xnu_video { uint64_t base, display, rowbytes, width, height, depth; };

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

struct xnu_payload {
	char magic[8];          /* "DNDLXNU0" */
	uint32_t version, segment_count;
	uint64_t virt_base, phys_base, mem_size, entry_va, kernel_end_pa;
	uint64_t segments_offset, data_offset, total_size;
	char command_line[XNU_CMDLINE];
};

struct xnu_segment {
	uint64_t physical, memory_size, file_offset, file_size;
};

struct handoff_plan {
	uint64_t adt_pa, args_pa, top, entry_pa;
};

/* ADT generation */
size_t dandelion_make_adt(void *buffer, size_t capacity,
			  uint64_t phys_base, uint64_t mem_size,
			  uint64_t kernel_end_pa,
			  uint32_t timebase,
			  const unsigned char *seed, size_t seed_bytes);

/* TRNG: get 64 bytes of hardware random seed */
int mt6762_random_seed(unsigned char seed[64], uint32_t timebase);

/* Kernel handoff */
const char *handoff_boot(const struct fdt_info *fdt, uint64_t fdt_pa, uint64_t payload_addr, uint64_t payload_size);

/* Validate a payload against the device tree and compute the load plan.
 * Returns 0 on success, -1 after printing the reason on the framebuffer. */
int handoff_validate(const void *payload, size_t length, uint64_t payload_pa,
		     const struct fdt_info *fdt, uint64_t fdt_pa,
		     uint64_t shim_start, uint64_t shim_end,
		     struct handoff_plan *plan);

#endif
