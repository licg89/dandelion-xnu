/* Dump what fdt_probe_platform() extracts from a DTB.
 *
 * Usage: fdt_probe_dump <file.dtb>
 * Useful to check the device DTB on a host before flashing the shim.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "fdt.h"

static unsigned char *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long length = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buffer = malloc((size_t)length);
	if (!buffer || fread(buffer, 1, (size_t)length, f) != (size_t)length) {
		fclose(f);
		free(buffer);
		return NULL;
	}
	fclose(f);
	*size = (size_t)length;
	return buffer;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <file.dtb>\n", argv[0]);
		return 2;
	}

	size_t size = 0;
	unsigned char *data = read_file(argv[1], &size);
	if (!data) return 2;

	printf("dtb: %zu bytes\n", size);

	struct fdt_info info;
	int irc = fdt_inspect(data, size, &info);
	if (irc == 0) {
		printf("memory regions: %u\n", info.memory_count);
		for (unsigned i = 0; i < info.memory_count; i++)
			printf("  mem 0x%llx +0x%llx\n",
			       (unsigned long long)info.memory[i].base,
			       (unsigned long long)info.memory[i].size);
		printf("reserved: %u\n", info.reserved_count);
	} else {
		printf("fdt_inspect: FAILED (%d)\n", irc);
	}

	struct fdt_platform p;
	if (fdt_probe_platform(data, size, &p)) {
		printf("fdt_probe_platform: FAILED\n");
		free(data);
		return 1;
	}

	if (p.gic.found) {
		printf("gic: %s, %u regions\n", p.gic.compatible, p.gic.regions);
		for (unsigned i = 0; i < p.gic.regions; i++)
			printf("  gic 0x%llx +0x%llx\n",
			       (unsigned long long)p.gic.base[i],
			       (unsigned long long)p.gic.size[i]);
	} else {
		printf("gic: not found\n");
	}

	if (p.timer.found) {
		printf("timer: %s, %u Hz, %u irqs\n", p.timer.compatible,
		       p.timer.frequency, p.timer.irq_count);
		for (unsigned i = 0; i < p.timer.irq_count; i++)
			printf("  intid %u\n", p.timer.irq[i]);
	} else {
		printf("timer: not found\n");
	}

	printf("uart: %s 0x%llx +0x%llx\n",
	       p.uart.found ? p.uart.compatible : "not found",
	       (unsigned long long)p.uart.base,
	       (unsigned long long)p.uart.size);
	printf("wdt:  %s 0x%llx +0x%llx\n",
	       p.wdt.found ? p.wdt.compatible : "not found",
	       (unsigned long long)p.wdt.base,
	       (unsigned long long)p.wdt.size);

	printf("cpus: %u, enable-method=%s\n", p.cpus.count,
	       p.cpus.enable_method[0] ? p.cpus.enable_method : "?");
	for (unsigned i = 0; i < p.cpus.count; i++)
		printf("  mpidr 0x%llx\n", (unsigned long long)p.cpus.mpidr[i]);

	free(data);
	return 0;
}
