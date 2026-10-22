/* TRNG probe - dump registers and try different configurations */
#include <stdint.h>
#include "fb.h"
#include "font.h"

#define READ_SYSREG(reg) ({ uint64_t _v; __asm__ volatile("mrs %0, " #reg : "=r"(_v)); _v; })

static void delay(uint64_t count) { while (count--) __asm__ volatile("nop"); }
static uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }

static int row_y;
static void cls(void) {
	for (int y = 0; y < 1600; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(0x7ec50000UL + (uint64_t)y * 2944);
		for (int x = 0; x < 736; x++) row[x] = 0xFF000000;
	}
	row_y = 0;
}
static void putstr(const char *s, uint32_t fg) {
	if (row_y + 16 > 1600) return;
	int col = 8;
	while (*s && col + 8 < 720) {
		char ch = *s;
		if (ch < 0x20 || ch > 0x7e) ch = '?';
		const unsigned char *g = font8x16[ch - 0x20];
		for (int gy = 0; gy < 16; gy++) {
			volatile uint32_t *row = (volatile uint32_t *)(0x7ec50000UL + (uint64_t)(row_y + gy) * 2944);
			for (int gx = 0; gx < 8; gx++)
				if (g[gy] & (0x80 >> gx)) row[col + gx] = fg;
		}
		col += 8; s++;
	}
	row_y += 18;
}
static void puthex(const char *label, uint64_t val, uint32_t fg) {
	static const char hex[] = "0123456789ABCDEF";
	putstr(label, fg); putstr(" ", fg);
	putstr("0x", fg);
	for (int s = 60; s >= 0; s -= 4) {
		char c[2] = { hex[(val >> s) & 0xf], 0 };
		putstr(c, fg);
	}
	putstr("\n", fg);
}
static void putdec(const char *label, uint64_t val, uint32_t fg) {
	char buf[20]; int i = 0;
	if (val == 0) buf[i++] = '0';
	else { uint64_t v = val; char tmp[20]; int j = 0;
		while (v) { tmp[j++] = '0' + (v % 10); v /= 10; }
		while (j > 0) buf[i++] = tmp[--j]; }
	buf[i] = 0;
	putstr(label, fg); putstr(" ", fg); putstr(buf, fg); putstr("\n", fg);
}

/* Dump a range of registers */
static void dump_regs(const char *label, uintptr_t base, unsigned count, uint32_t stride)
{
	putstr(label, 0xFFFFFF00);
	for (unsigned i = 0; i < count; i++) {
		volatile uint32_t *r = (volatile uint32_t *)(base + i * stride);
		char off[16];
		off[0] = ' '; off[1] = ' ';
		static const char hex[] = "0123456789ABCDEF";
		off[2] = hex[(i * stride) >> 8 & 0xf];
		off[3] = hex[(i * stride) >> 4 & 0xf];
		off[4] = hex[(i * stride) & 0xf];
		off[5] = ':'; off[6] = ' '; off[7] = 0;
		putstr(off, 0xFF00FFFF);
		puthex("", *r, 0xFF00FFFF);
	}
}

/* Stubs for start.S references */
__attribute__((noreturn)) void shim_el1(uint64_t dtb) { (void)dtb; for(;;) __asm__ volatile("wfe"); }
__attribute__((noreturn)) void exception_report(uint64_t slot) { (void)slot; for(;;) __asm__ volatile("wfe"); }

__attribute__((noreturn)) void shim_main(uint64_t dtb)
{
	(void)dtb;
	cls();

	putstr("=== TRNG PROBE ===\n", 0xFFFFFF00);
	putstr("MT6762G TRNG register dump\n\n", 0xFFFFFFFF);

	/* Topckgen registers */
	putstr("--- TOPCKGEN (0x10000000) ---\n", 0xFFFFFF00);
	dump_regs("CG  ", 0x10000000, 1, 4);  /* 0x00 */
	dump_regs("    ", 0x10000030, 1, 4);  /* 0x30 */
	dump_regs("    ", 0x10000038, 1, 4);  /* 0x38 - CG2 */
	dump_regs("    ", 0x10000068, 1, 4);  /* 0x68 - CLRCG2 */
	dump_regs("    ", 0x10000098, 1, 4);  /* 0x98 - SETCG2 */
	putstr("\n", 0xFF000000);

	/* TRNG registers - dump 16 registers at 4-byte stride */
	putstr("--- TRNG (0x1020f000) ---\n", 0xFFFFFF00);
	dump_regs("REG ", 0x1020f000, 16, 4);
	putstr("\n", 0xFF000000);

	/* Also dump at 8-byte stride in case registers are 64-bit */
	putstr("--- TRNG 8-byte stride ---\n", 0xFFFFFF00);
	dump_regs("REG ", 0x1020f000, 8, 8);
	putstr("\n", 0xFF000000);

	/* Try enabling TRNG with different clock configs */
	putstr("--- TRNG INIT TESTS ---\n", 0xFFFFFF00);

	volatile uint32_t *trng = (volatile uint32_t *)0x1020f000;
	volatile uint32_t *clk = (volatile uint32_t *)0x10000000;

	/* Test 1: Enable clock bit 5, then try TRNG */
	putstr("Test 1: CLK bit5 + TRNG enable\n", 0xFF00FFFF);
	clk[0x98/4] = 1u << 5;  /* SETCG2 bit 5 */
	delay(10000);
	uint32_t ctrl = trng[0];
	puthex("TRNG[0]=", ctrl, 0xFF00FFFF);
	trng[0] = ctrl | 1u;
	delay(100000);
	puthex("TRNG[0]=", trng[0], ctrl & (1u<<31) ? 0xFF00FF00 : 0xFFFF0000);
	puthex("TRNG[2]=", trng[2], 0xFF00FFFF);

	/* Test 2: Try without clock enable */
	putstr("\nTest 2: TRNG enable only (no clock)\n", 0xFF00FFFF);
	trng[0] = ctrl | 1u;
	delay(100000);
	puthex("TRNG[0]=", trng[0], 0xFF00FFFF);
	puthex("TRNG[2]=", trng[2], 0xFF00FFFF);

	/* Test 3: Try with different clock bits */
	putstr("\nTest 3: CLK bit4 + TRNG\n", 0xFF00FFFF);
	clk[0x68/4] = 1u << 5;  /* CLRCG2 bit 5 - disable first */
	clk[0x98/4] = 1u << 4;  /* SETCG2 bit 4 */
	delay(10000);
	ctrl = trng[0];
	puthex("TRNG[0]=", ctrl, 0xFF00FFFF);
	trng[0] = ctrl | 1u;
	delay(100000);
	puthex("TRNG[0]=", trng[0], ctrl & (1u<<31) ? 0xFF00FF00 : 0xFFFF0000);

	/* Test 4: Long wait with bit 5 */
	putstr("\nTest 4: Long wait (10M cycles)\n", 0xFF00FFFF);
	clk[0x68/4] = 1u << 5;
	clk[0x98/4] = 1u << 5;
	delay(10000);
	ctrl = trng[0];
	trng[0] = ctrl | 1u;
	uint64_t start = ticks();
	int ready = 0;
	for (unsigned i = 0; i < 10000000; i++) {
		if (trng[0] & (1u << 31)) { ready = 1; break; }
	}
	uint64_t elapsed = ticks() - start;
	putdec("elapsed=", elapsed, 0xFF00FFFF);
	puthex("TRNG[0]=", trng[0], ready ? 0xFF00FF00 : 0xFFFF0000);
	puthex("TRNG[2]=", trng[2], ready ? 0xFF00FF00 : 0xFFFF0000);
	putstr(ready ? "TRNG WORKS!\n" : "TRNG timeout\n", ready ? 0xFF00FF00 : 0xFFFF0000);

	putstr("\nHold POWER to reboot\n", 0xFFFFFFFF);
	for (;;) __asm__ volatile("wfe");
}
