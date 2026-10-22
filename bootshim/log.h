/* Persistent log writer - writes to ramoops area for post-reboot reading */
#ifndef DANDELION_LOG_H
#define DANDELION_LOG_H

#include <stdint.h>
#include <stddef.h>

/* Ramoops area: 0x47c90000, size 896KB (from device tree mblock-11-pstore) */
#define PSTORE_BASE   0x47c90000UL
#define PSTORE_SIZE   0xe0000

/* Simple log format: [4-byte length][data...] */
static char _log_buf[8192];
static int _log_pos = 0;

static inline void _log_flush(void)
{
    if (_log_pos == 0) return;
    volatile char *pstore = (volatile char *)PSTORE_BASE;
    /* Write length header */
    pstore[0] = (_log_pos >> 0) & 0xff;
    pstore[1] = (_log_pos >> 8) & 0xff;
    pstore[2] = (_log_pos >> 16) & 0xff;
    pstore[3] = (_log_pos >> 24) & 0xff;
    /* Write data */
    for (int i = 0; i < _log_pos && i < (int)sizeof(_log_buf); i++)
        pstore[4 + i] = _log_buf[i];
    /* End marker */
    pstore[4 + _log_pos] = 'E';
    pstore[4 + _log_pos + 1] = 'N';
    pstore[4 + _log_pos + 2] = 'D';
    pstore[4 + _log_pos + 3] = '\n';
    _log_pos = 0;
}

static inline void log_char(char c)
{
    if (_log_pos < (int)sizeof(_log_buf) - 1)
        _log_buf[_log_pos++] = c;
    if (c == '\n' || _log_pos >= (int)sizeof(_log_buf) - 128)
        _log_flush();
}

static inline void log_puts(const char *s)
{
    while (*s) log_char(*s++);
}

static inline void log_puthex(const char *label, uint64_t val)
{
    static const char hex[] = "0123456789ABCDEF";
    log_puts(label);
    log_puts(" 0x");
    for (int s = 60; s >= 0; s -= 4)
        log_char(hex[(val >> s) & 0xf]);
    log_char('\n');
}

static inline void log_putdec(const char *label, uint64_t val)
{
    log_puts(label);
    log_puts(" ");
    char buf[20]; int i = 0;
    if (val == 0) buf[i++] = '0';
    else { uint64_t v = val; char tmp[20]; int j = 0;
        while (v) { tmp[j++] = '0' + (v % 10); v /= 10; }
        while (j > 0) buf[i++] = tmp[--j]; }
    buf[i] = 0;
    log_puts(buf);
    log_char('\n');
}

#endif /* DANDELION_LOG_H */
