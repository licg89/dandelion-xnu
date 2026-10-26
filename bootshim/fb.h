/* Dandelion framebuffer console header */
#ifndef DANDELION_FB_H
#define DANDELION_FB_H
#include <stdint.h>

#define FB_W        736
#define FB_H        1600
#define FB_STRIDE   2944
#define FB_BASE     0x7ec50000UL
#define VIS_W       720

/* RGBA8888 colors */
#define CLR_BLACK   0xFF000000
#define CLR_WHITE   0xFFFFFFFF
#define CLR_RED     0xFFFF0000
#define CLR_GREEN   0xFF00FF00
#define CLR_BLUE    0xFF0000FF
#define CLR_YELLOW  0xFFFFFF00
#define CLR_CYAN    0xFF00FFFF
#define CLR_MAGENTA 0xFFFF00FF

void fb_init(void);
void fb_clear(uint32_t color);
void fb_scroll(void);
void fb_putc(char ch, uint32_t fg);
void fb_puts(const char *s, uint32_t fg);
void fb_puthex(const char *label, uint64_t val, uint32_t fg);
void fb_putdec(const char *label, uint64_t val, uint32_t fg);
/* Same, without the trailing newline (for compact one-line status output). */
void fb_puthex_raw(uint64_t val);
void fb_putdec_raw(uint64_t val);
void fb_newline(void);

#endif
