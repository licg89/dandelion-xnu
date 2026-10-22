/* MT6762G TRNG (True Random Number Generator)
 * TRNG base: 0x1020f000 (from device tree trng@1020f000)
 * Clock gate: topckgen 0x10000000, bit 5 in group 2
 */
#include "handoff.h"

static volatile uint32_t *const trng = (void *)(uintptr_t)0x1020f000;

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

int mt6762_random_seed(unsigned char seed[64], uint32_t timebase)
{
	volatile uint32_t *const clocks = (void *)(uintptr_t)0x10000000;

	/* Enable TRNG clock if gated */
	uint32_t gated = clocks[0x3c / 4] & (1u << 5);
	if (gated) clocks[0x9c / 4] = 1u << 5;
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t control = trng[0];
	int result = 0;

	if (control == 0xffffffffu) { result = -1; goto restore_clock; }

	/* Enable TRNG */
	trng[0] = control | 1u;
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t previous = 0;
	for (unsigned i = 0; i < 16; i++) {
		uint64_t start = ticks();
		unsigned attempts = 1000000;

		/* Wait for data ready (bit 31) */
		while (!(trng[0] & (1u << 31))) {
			if (!--attempts || ticks() - start > timebase / 100) {
				result = -2;
				goto restore_rng;
			}
		}

		uint32_t value = trng[2];

		/* Stuck-source rejection */
		if (!value || value == 0xffffffffu || (i && value == previous)) {
			result = -3;
			goto restore_rng;
		}
		previous = value;

		for (unsigned j = 0; j < 4; j++)
			seed[i * 4 + j] = (unsigned char)(value >> (j * 8));
	}

restore_rng:
	if (!(control & 1u)) trng[0] = control & ~1u;
restore_clock:
	if (gated) clocks[0x6c / 4] = 1u << 5;
	__asm__ volatile("dsb sy" ::: "memory");

	if (result)
		for (unsigned i = 0; i < 64; i++)
			((volatile unsigned char *)seed)[i] = 0;

	return result;
}
