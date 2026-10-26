/* Dandelion framebuffer console implementation */
#include "fb.h"
#include "font.h"
#include "pstore.h"
#include <stddef.h>
#include <stddef.h>

/* Freestanding memcpy */
void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--) *d++ = *s++;
	return dst;
}

static int cursor_x, cursor_y;

#ifdef QEMU_VIRT
/*
 * QEMU's virt machine has no display and no MSM framebuffer; the only
 * observable channel is the PL011 at 0x09000000.  Mirror the console there so
 * the shim can be exercised without the phone.
 */
#define PL011_DR (*(volatile uint32_t *)0x09000000UL)

static void uart_putc(char ch)
{
	if (ch == '\n')
		PL011_DR = '\r';
	PL011_DR = (unsigned char)ch;
}
#endif

void fb_init(void)
{
	cursor_x = 0;
	cursor_y = 0;
}

void fb_clear(uint32_t color)
{
#ifdef QEMU_VIRT
	(void)color;
	return;
#else
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(FB_BASE + (uint64_t)y * FB_STRIDE);
		for (int x = 0; x < FB_W; x++)
			row[x] = color;
	}
	cursor_x = 0;
	cursor_y = 0;
#endif
}

void fb_scroll(void)
{
#ifdef QEMU_VIRT
	return;
#else
	const int shift = 18;

	for (int y = 0; y < FB_H - shift; y++) {
		volatile uint32_t *dst = (volatile uint32_t *)(FB_BASE + (uint64_t)y * FB_STRIDE);
		volatile uint32_t *src = (volatile uint32_t *)(FB_BASE + (uint64_t)(y + shift) * FB_STRIDE);
		for (int x = 0; x < FB_W; x++)
			dst[x] = src[x];
	}
	for (int y = FB_H - shift; y < FB_H; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(FB_BASE + (uint64_t)y * FB_STRIDE);
		for (int x = 0; x < FB_W; x++)
			row[x] = CLR_BLACK;
	}
#endif
}

void fb_newline(void)
{
	cursor_x = 0;
	cursor_y += 18;
#ifdef QEMU_VIRT
	/* The UART is the "screen"; there is nothing to scroll. */
#else
	if (cursor_y + 18 > FB_H) {
		fb_scroll();
		cursor_y = FB_H - 18;
	}
#endif
}

void fb_putc(char ch, uint32_t fg)
{
#ifdef QEMU_VIRT
	(void)fg;
	uart_putc(ch);
	return;
#else
	if (ch == '\n') { fb_newline(); return; }
	if (ch < 0x20 || ch > 0x7e) ch = '?';
	if (cursor_x + 8 > VIS_W || cursor_y + 16 > FB_H) return;

	const unsigned char *g = font8x16[ch - 0x20];
	int px = 8 + cursor_x;
	for (int gy = 0; gy < 16; gy++) {
		volatile uint32_t *row = (volatile uint32_t *)(FB_BASE + (uint64_t)(cursor_y + gy) * FB_STRIDE);
		for (int gx = 0; gx < 8; gx++) {
			if (g[gy] & (0x80 >> gx))
				row[px + gx] = fg;
		}
	}
	cursor_x += 8;
#endif
}

void fb_puts(const char *s, uint32_t fg)
{
	ps_puts(s);  /* Also write to pstore */
	while (*s) {
		if (*s == '\n') { fb_newline(); s++; continue; }
		fb_putc(*s, fg);
		s++;
	}
}

void fb_puthex(const char *label, uint64_t val, uint32_t fg)
{
	ps_puthex(label, val);  /* Also write to pstore */
	static const char hex[] = "0123456789ABCDEF";
	fb_puts(label, fg);
	fb_putc(' ', fg);
	fb_putc('0', fg);
	fb_putc('x', fg);
	for (int s = 60; s >= 0; s -= 4)
		fb_putc(hex[(val >> s) & 0xf], fg);
	fb_newline();
}

void fb_putdec(const char *label, uint64_t val, uint32_t fg)
{
	ps_putdec(label, val);  /* Also write to pstore */
	char buf[20];
	int i = 0;
	if (val == 0) { buf[i++] = '0'; }
	else {
		uint64_t v = val;
		char tmp[20];
		int j = 0;
		while (v) { tmp[j++] = '0' + (v % 10); v /= 10; }
		while (j > 0) buf[i++] = tmp[--j];
	}
	buf[i] = 0;
	fb_puts(label, fg);
	fb_putc(' ', fg);
	fb_puts(buf, fg);
	fb_newline();
}
