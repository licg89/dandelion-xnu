/* Pstore logger - writes shim output to persistent RAM
 *
 * The stock kernel registers the region at 0x47c90000 as a standard ramoops
 * backend ("pstore: Registered ramoops as persistent store backend", console
 * zone 0x40000 bytes at the start of the region), so the shim can hand its log
 * to the *next* boot by writing a plain persistent_ram buffer header there:
 *
 *     struct persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; }
 *
 * After the next Android boot the text shows up as
 * /sys/fs/pstore/console-ramoops-0.
 *
 * The writer is deliberately stateless - the cursor lives in the header in
 * DRAM, not in a static variable - because this header is included by more
 * than one translation unit (shim.c and fb.c) and both must share it.  It also
 * writes every byte straight through, so a crash still leaves the log behind.
 */
#ifndef DANDELION_PSTORE_H
#define DANDELION_PSTORE_H

#include <stdint.h>
#include <stddef.h>

/* ramoops region and console zone, both from the stock kernel command line */
#ifndef PSTORE_BASE
#define PSTORE_BASE	0x47C90000UL
#endif
#ifndef PSTORE_ZONE_SIZE
#define PSTORE_ZONE_SIZE 0x00040000UL
#endif

#define PS_SIG		0x43474244U	/* "DBGC" */
#define PS_HDR_SIZE	12
#define PS_MAX		(PSTORE_ZONE_SIZE - PS_HDR_SIZE - 0x1000)

/*
 * The platform test payload runs before XNU and must not touch the ramoops
 * area, so it is built with -DPSTORE_ENABLED=0.
 */
#ifndef PSTORE_ENABLED
#define PSTORE_ENABLED 1
#endif

#if PSTORE_ENABLED

static inline volatile uint32_t *ps_hdr(void)
{
	return (volatile uint32_t *)PSTORE_BASE;
}

static inline void ps_reset(void)
{
	ps_hdr()[0] = PS_SIG;
	ps_hdr()[1] = 0;
	ps_hdr()[2] = 0;
}

static inline void _ps_flush(void)
{
	/* every byte is already in DRAM */
}

static inline void ps_char(char c)
{
	volatile uint32_t *h = ps_hdr();
	if (h[0] != PS_SIG)
		ps_reset();
	uint32_t n = h[2];
	if (n >= PS_MAX)
		return;
	((volatile uint8_t *)(PSTORE_BASE + PS_HDR_SIZE))[n] = (uint8_t)c;
	h[2] = n + 1;
}

static inline void ps_puts(const char *s)
{
	while (*s)
		ps_char(*s++);
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
	char buf[21];
	int i = 0;
	if (val == 0) {
		buf[i++] = '0';
	} else {
		char tmp[20];
		int j = 0;
		while (val) {
			tmp[j++] = '0' + (val % 10);
			val /= 10;
		}
		while (j > 0)
			buf[i++] = tmp[--j];
	}
	buf[i] = 0;
	ps_puts(buf);
	ps_char('\n');
}

#else /* !PSTORE_ENABLED */

/*
 * No ramoops area is available (or it must not be touched).  Keep the same
 * interface so fb.c and the shim can be linked unchanged.
 */
static inline void ps_reset(void) { }
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
