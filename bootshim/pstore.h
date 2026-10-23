/* Pstore logger - writes shim output to persistent RAM
 * After reboot, read via: adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0'
 * Or directly: adb shell su -c 'dd if=/dev/block/by-name/userdata bs=1 skip=OFFSET count=SIZE'
 */
#ifndef DANDELION_PSTORE_H
#define DANDELION_PSTORE_H

#include <stdint.h>
#include <stddef.h>

/* pstore/ramoops area from device tree: 0x47c90000, size 0xe0000 */
#ifndef PSTORE_BASE
#define PSTORE_BASE   0x47C90000UL
#endif
#ifndef PSTORE_SIZE
#define PSTORE_SIZE   0x000E0000
#endif

/* Ramoops record header (simplified) */
struct pstore_record {
    uint64_t size;
    uint64_t type;   /* 1 = console, 2 = dmesg, etc */
};

/*
 * The platform test payload runs before XNU and must not touch the ramoops
 * area, so it is built with -DPSTORE_ENABLED=0.
 */
#ifndef PSTORE_ENABLED
#define PSTORE_ENABLED 1
#endif

#if PSTORE_ENABLED

static char _ps_buf[8192];
static int _ps_pos = 0;

static void _ps_flush(void)
{
    if (_ps_pos == 0) return;
    volatile uint8_t *ps = (volatile uint8_t *)PSTORE_BASE;

    /* Try multiple formats to find what works */

    /* Format 1: Simple text with header */
    /* Write magic "PSLOG" + size + text */
    ps[0] = 'P'; ps[1] = 'S'; ps[2] = 'L'; ps[3] = 'O'; ps[4] = 'G';
    ps[5] = (_ps_pos >> 0) & 0xff;
    ps[6] = (_ps_pos >> 8) & 0xff;
    ps[7] = 0; ps[8] = 0; ps[9] = 0;

    for (int i = 0; i < _ps_pos && (10 + i) < 0x10000; i++)
        ps[10 + i] = (uint8_t)_ps_buf[i];

    /* Also write as persistent_ram_buffer format */
    volatile uint32_t *ps32 = (volatile uint32_t *)PSTORE_BASE;
    ps32[0x1000/4] = 0x43474244;  /* "DBGC" signature at offset 0x1000 */
    ps32[0x1004/4] = 0;            /* start */
    ps32[0x1008/4] = _ps_pos;      /* size */
    for (int i = 0; i < _ps_pos && i < 0x8000; i++)
        ps[0x100C + i] = (uint8_t)_ps_buf[i];

    _ps_pos = 0;
}

static inline void ps_char(char c)
{
    if (_ps_pos < (int)sizeof(_ps_buf) - 1)
        _ps_buf[_ps_pos++] = c;
    if (c == '\n' || _ps_pos >= (int)sizeof(_ps_buf) - 64)
        _ps_flush();
}

static inline void ps_puts(const char *s)
{
    while (*s) ps_char(*s++);
}

static inline void ps_puthex(const char *label, uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    ps_puts(label);
    ps_puts(" 0x");
    for (int s = 60; s >= 0; s -= 4)
        ps_char(hex[(val >> s) & 0xf]);
    ps_char('\n');
}

static inline void ps_putdec(const char *label, uint64_t val)
{
    ps_puts(label);
    ps_puts(" ");
    char buf[20]; int i = 0;
    if (val == 0) buf[i++] = '0';
    else { uint64_t v = val; char tmp[20]; int j = 0;
        while (v) { tmp[j++] = '0' + (v % 10); v /= 10; }
        while (j > 0) buf[i++] = tmp[--j]; }
    buf[i] = 0;
    ps_puts(buf);
    ps_char('\n');
}

#else /* !PSTORE_ENABLED */

/*
 * No ramoops area is available (or it must not be touched).  Keep the same
 * interface so fb.c and the shim can be linked unchanged.
 */
static inline void _ps_flush(void) { }
static inline void ps_char(char c) { (void)c; }
static inline void ps_puts(const char *s) { (void)s; }
static inline void ps_puthex(const char *label, uint64_t val)
{
    (void)label; (void)val;
}
static inline void ps_putdec(const char *label, uint64_t val)
{
    (void)label; (void)val;
}

#endif /* PSTORE_ENABLED */

#endif /* DANDELION_PSTORE_H */
