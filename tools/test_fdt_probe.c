/* Host-side test for fdt_probe_platform().
 *
 * Usage: test_fdt_probe <test.dtb>
 * Generate the DTB with: python tools/make_test_dtb.py out/test.dtb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fdt.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

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
		fprintf(stderr, "usage: %s <test.dtb>\n", argv[0]);
		return 2;
	}

	size_t size = 0;
	unsigned char *data = read_file(argv[1], &size);
	if (!data) return 2;

	struct fdt_platform p;
	int rc = fdt_probe_platform(data, size, &p);
	printf("probe rc=%d\n", rc);
	CHECK(rc == 0);

	CHECK(p.gic.found);
	CHECK(p.gic.regions == 3);
	CHECK(p.gic.base[0] == 0x0C000000ULL && p.gic.size[0] == 0x40000ULL);
	CHECK(p.gic.base[1] == 0x0C100000ULL && p.gic.size[1] == 0x200000ULL);
	CHECK(p.gic.base[2] == 0x0C400000ULL && p.gic.size[2] == 0x2000ULL);
	CHECK(strcmp(p.gic.compatible, "arm,gic-v3") == 0);

	CHECK(p.timer.found);
	CHECK(p.timer.frequency == 13000000);
	CHECK(p.timer.irq_count == 4);
	CHECK(p.timer.irq[0] == 29);   /* PPI 13 */
	CHECK(p.timer.irq[1] == 30);   /* PPI 14 */
	CHECK(p.timer.irq[2] == 27);   /* PPI 11, virtual timer */
	CHECK(p.timer.irq[3] == 26);   /* PPI 10, hypervisor timer */

	/* Two UARTs are described; the console one must win. */
	CHECK(p.uart.found);
	CHECK(p.uart.base == 0x11002000ULL);
	CHECK(p.uart.size == 0x1000ULL);

	CHECK(p.wdt.found);
	CHECK(p.wdt.base == 0x10007000ULL);
	CHECK(p.wdt.size == 0x1000ULL);

	CHECK(p.cpus.count == 8);
	CHECK(p.cpus.mpidr[0] == 0x0);
	CHECK(p.cpus.mpidr[4] == 0x100);
	CHECK(strcmp(p.cpus.enable_method, "psci") == 0);

	/* A truncated blob must be rejected without crashing. */
	struct fdt_platform ignored;
	CHECK(fdt_probe_platform(data, 40, &ignored) != 0);
	CHECK(fdt_probe_platform(data, size - 8, &ignored) != 0);

	free(data);

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all fdt_probe_platform checks passed\n");
	return 0;
}
