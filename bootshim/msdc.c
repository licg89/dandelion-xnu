/* Dandelion boot shim - polled-PIO eMMC log channel (implementation)
 *
 * Design notes, all of them load-bearing:
 *
 *  - LK has just loaded *this very image* through MSDC0, so the controller is
 *    clocked, the pads are muxed, the card is selected with RCA 1 and it is in
 *    the transfer state.  We therefore inherit rather than re-initialise: no
 *    CMD0, no CMD1, no re-tuning.  That is also what NativeMsdc does.
 *
 *  - The only thing we change is the clock divider, and we slow it down.  A
 *    wrong sampling point at 200 MHz turns into a CRC error; at 400 kHz there
 *    is no sampling point to get wrong.  CKMOD and SDC_CFG.BUSWIDTH are left
 *    exactly as LK set them, because the card's own bus width lives in
 *    EXT_CSD[183] and the two must keep matching.
 *
 *  - Nothing is written until the target sector has been read back as 0xff,
 *    and what is written is read back and compared.  A wrong LBA therefore
 *    costs a failed test, not a partition.
 *
 *  - Polling only, MMU off: no cache maintenance, no interrupts, no DMA.
 *    Deadlines come from CNTPCT_EL0 and every loop has one, because a shim
 *    that busy-waits forever is a brick.
 *
 * Register numbers and the command-word encoding come from
 * docs/research/mtk-msdc-pio.md, which cites the vendor driver shipped for
 * this exact phone (Xiaomi_Kernel_OpenSource, dandelion-q-oss), Linux'
 * mtk-sd.c, and mouseos/NativeMsdc.
 */
#include <stdint.h>
#include <stddef.h>

#include "fb.h"
#include "log_block.h"
#include "msdc.h"

#define MSDC0_BASE	0x11230000UL

/* ---- registers --------------------------------------------------------- */
#define R_CFG		0x000u
#define R_IOCON		0x004u
#define R_PS		0x008u
#define R_INT		0x00Cu
#define R_INTEN		0x010u
#define R_FIFOCS	0x014u
#define R_TXDATA	0x018u
#define R_RXDATA	0x01Cu
#define R_SDC_CFG	0x030u
#define R_SDC_CMD	0x034u
#define R_SDC_ARG	0x038u
#define R_SDC_STS	0x03Cu
#define R_RESP0		0x040u
#define R_RESP1		0x044u
#define R_RESP2		0x048u
#define R_RESP3		0x04Cu
#define R_BLK_NUM	0x050u
#define R_DMA_CTRL	0x098u
#define R_DMA_CFG	0x09Cu
#define R_VERSION	0x114u
#define R_ECO_VER	0x118u

/* ---- MSDC_CFG ---------------------------------------------------------- */
#define CFG_MODE		(1u << 0)
#define CFG_RST			(1u << 2)
#define CFG_PIO			(1u << 3)
#define CFG_CKSTB		(1u << 7)
#define CFG_CKDIV(d)		(((uint32_t)(d) & 0xFFFu) << 8)
#define CFG_CKDIV_MASK		(0xFFFu << 8)
#define CFG_CKMOD_MASK		(3u << 20)

/* ---- SDC_STS ----------------------------------------------------------- */
#define STS_SDCBUSY		(1u << 0)
#define STS_CMDBUSY		(1u << 1)

/* ---- MSDC_INT ---------------------------------------------------------- */
#define INT_CMDRDY		(1u << 8)
#define INT_CMDTMO		(1u << 9)
#define INT_RSPCRCERR		(1u << 10)
#define INT_XFER_COMPL		(1u << 12)
#define INT_DATTMO		(1u << 14)
#define INT_DATCRCERR		(1u << 15)
#define INT_CMD_ALL		(INT_CMDRDY | INT_CMDTMO | INT_RSPCRCERR)
#define INT_DAT_ALL		(INT_XFER_COMPL | INT_DATTMO | INT_DATCRCERR)

/* ---- MSDC_FIFOCS ------------------------------------------------------- */
#define FIFOCS_RXCNT(x)		((x) & 0xFFu)
#define FIFOCS_TXCNT(x)		(((x) >> 16) & 0xFFu)
#define FIFOCS_CLR		(1u << 31)
#define FIFO_DEPTH		128u		/* mtk_sd.h: MSDC_FIFO_SZ */

/* ---- SDC_CFG ----------------------------------------------------------- */
#define SDC_BUSWIDTH_MASK	(3u << 16)
#define SDC_DTOC_MASK		(0xFFu << 24)
#define SDC_WRDTOC_MASK		(0x1FFFu << 2)

/* ---- command words (SDC_CMD) ------------------------------------------- */
#define CMDW(opc, rsp, dtype, rw, blklen, extra) \
	((uint32_t)(opc) | ((uint32_t)(rsp) << 7) | ((uint32_t)(dtype) << 11) | \
	 ((uint32_t)(rw) << 13) | ((uint32_t)(blklen) << 16) | (uint32_t)(extra))

#define CMD6_W		CMDW(6, 7, 0, 0, 0, 0)		/* R1b            */
#define CMD8_W		CMDW(8, 1, 1, 0, 512, 0)	/* read 512 B     */
#define CMD12_W		CMDW(12, 7, 0, 0, 0, (1u << 14))	/* R1b + stop     */
#define CMD13_W		CMDW(13, 1, 0, 0, 0, 0)		/* R1             */
#define CMD17_W		CMDW(17, 1, 1, 0, 512, 0)	/* read one block */
#define CMD18_W		CMDW(18, 1, 2, 0, 512, 0)	/* read many      */
#define CMD24_W		CMDW(24, 1, 1, 1, 512, 0)	/* write one      */
#define CMD25_W		CMDW(25, 1, 2, 1, 512, 0)	/* write many     */

/* ---- R1 ---------------------------------------------------------------- */
#define R1_READY_FOR_DATA	(1u << 8)
#define R1_STATE(s)		(((s) >> 9) & 0xFu)
#define R1_STATE_TRAN		4u
#define R1_FATAL_MASK		0xFFFFA080u

/* ---- eMMC -------------------------------------------------------------- */
#define MMC_RCA			1u
#define EXT_CSD_PART_ACCESS	179u
#define EXT_CSD_BUS_WIDTH	183u
#define EXT_CSD_HS_TIMING	185u
#define EXT_CSD_CACHE_CTRL	33u
#define EXT_CSD_SECTOR_COUNT	212u

/* ---- chunking ---------------------------------------------------------- */
#define CHUNK_BLOCKS		128u		/* 64 KiB per CMD18/CMD25 */
#define BOUNCE_BYTES		4096u

/* ---- where to put the log ---------------------------------------------- */
#define MSDC_SCAN_STEP		128u		/* 64 KiB between candidates  */
#define MSDC_SCAN_MAX		240u		/* covers ~15 MiB of the part */
#define MSDC_TEXT_MAX		(256u * 1024u)

enum {
	MSDC_OK = 0,
	MSDC_ETMO = -1,
	MSDC_ECRC = -2,
	MSDC_EBUSY = -3,
	MSDC_ESTATE = -4,
	MSDC_EARG = -5,
	MSDC_ENOCLK = -6,
	MSDC_ENOPART = -7,
	MSDC_ENOTERASED = -8,
	MSDC_EIO = -9,
	MSDC_ESPACE = -10
};

static uint32_t msdc_ready;
static uint32_t msdc_log_base;
static uint32_t msdc_lba_offset = MSDC_LOG_LBA_OFFSET;

/* ---------------------------------------------------------------------- */
/* Register access.  One barrier per register write keeps the controller   */
/* and the CPU in step without the cost of a barrier per FIFO word.        */
/* ---------------------------------------------------------------------- */

static inline void
dsb(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(MSDC0_BASE + off);
}

static inline void
wr(uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)(MSDC0_BASE + off) = value;
	dsb();
}

static inline void
wr_fifo(uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)(MSDC0_BASE + R_TXDATA) = value;
}

/* ---------------------------------------------------------------------- */
/* Byte-wise helpers.  With the MMU off every access is Device-nGnRnE, so   */
/* an unaligned wide store faults; the log header is therefore assembled    */
/* one byte at a time instead of by casting a packed struct over it.        */
/* ---------------------------------------------------------------------- */

static int
same_bytes(const void *a, const void *b, uint32_t count)
{
	const uint8_t *x = (const uint8_t *)a;
	const uint8_t *y = (const uint8_t *)b;

	for (uint32_t i = 0; i < count; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static void
put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void
put_le64(uint8_t *p, uint64_t value)
{
	put_le32(p, (uint32_t)value);
	put_le32(p + 4, (uint32_t)(value >> 32));
}

/* ---------------------------------------------------------------------- */
/* Time.  Deadlines only, so microsecond resolution is plenty, and the     */
/* fallback keeps the shim alive if CNTFRQ_EL0 was never programmed.       */
/* ---------------------------------------------------------------------- */

static inline uint64_t
now_us(void)
{
	uint64_t count, frequency;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(count));
	if (!frequency)
		return count / 10u;			/* assume 10 MHz */
	return (count / frequency) * 1000000u +
	       ((count % frequency) * 1000000u) / frequency;
}

static inline uint64_t
deadline_ms(uint32_t ms)
{
	return now_us() + (uint64_t)ms * 1000u;
}

static inline int
expired(uint64_t deadline)
{
	return now_us() > deadline;
}

static void
delay_ms(uint32_t ms)
{
	uint64_t deadline = deadline_ms(ms);

	while (!expired(deadline))
		;
}

/* ---------------------------------------------------------------------- */
/* Panel output.  gic_probe.c established these helpers; keep the same     */
/* shape so the two reports look alike.                                    */
/* ---------------------------------------------------------------------- */

static void
out(const char *text, uint32_t color)
{
	fb_puts(text, color);
}

static void
outhex(unsigned long value)
{
	static const char digits[] = "0123456789abcdef";
	char buffer[19];
	unsigned length = 0;
	int started = 0;

	buffer[length++] = '0';
	buffer[length++] = 'x';
	for (int shift = 60; shift >= 0; shift -= 4) {
		unsigned nibble = (unsigned)((value >> shift) & 0xful);
		if (nibble != 0 || started || shift == 0) {
			buffer[length++] = digits[nibble];
			started = 1;
		}
	}
	buffer[length] = '\0';
	out(buffer, CLR_WHITE);
}

static void
outdec(unsigned long value)
{
	char buffer[21];
	unsigned length = 0;

	if (value == 0)
		buffer[length++] = '0';
	while (value != 0) {
		buffer[length++] = (char)('0' + (value % 10));
		value /= 10;
	}
	for (unsigned i = 0; i < length / 2; i++) {
		char swap = buffer[i];
		buffer[i] = buffer[length - 1 - i];
		buffer[length - 1 - i] = swap;
	}
	buffer[length] = '\0';
	out(buffer, CLR_WHITE);
}

static void
field_hex(const char *label, unsigned long value)
{
	out("  ", CLR_WHITE);
	out(label, CLR_CYAN);
	out(" ", CLR_WHITE);
	outhex(value);
	fb_newline();
}

static void
field_dec(const char *label, unsigned long value)
{
	out("  ", CLR_WHITE);
	out(label, CLR_CYAN);
	out(" ", CLR_WHITE);
	outdec(value);
	fb_newline();
}

/* ---------------------------------------------------------------------- */
/* Host primitives                                                        */
/* ---------------------------------------------------------------------- */

static int
wait_sts_clear(uint32_t mask, uint32_t ms)
{
	uint64_t deadline = deadline_ms(ms);

	while (rd(R_SDC_STS) & mask)
		if (expired(deadline))
			return MSDC_EBUSY;
	return MSDC_OK;
}

static int
fifo_clear(void)
{
	uint64_t deadline = deadline_ms(20);

	wr(R_FIFOCS, rd(R_FIFOCS) | FIFOCS_CLR);
	while (rd(R_FIFOCS) & FIFOCS_CLR)
		if (expired(deadline))
			return MSDC_ETMO;
	return MSDC_OK;
}

/* Issue one command and poll MSDC_INT for its completion. */
static int
msdc_cmd(uint32_t cmdw, uint32_t arg, uint32_t *r1, uint32_t ms)
{
	uint32_t opcode = cmdw & 0x3Fu;
	uint64_t deadline;
	int rc;

	/* CMD12/CMD13 may run while DAT0 is busy; everything else needs the
	 * whole controller idle. */
	rc = wait_sts_clear((opcode == 12u || opcode == 13u) ?
			    STS_CMDBUSY : STS_SDCBUSY, 100);
	if (rc)
		return rc;

	wr(R_INT, INT_CMD_ALL);			/* W1C: drop stale flags */
	wr(R_SDC_ARG, arg);
	wr(R_SDC_CMD, cmdw);

	deadline = deadline_ms(ms);
	for (;;) {
		uint32_t status = rd(R_INT) & INT_CMD_ALL;

		if (status) {
			wr(R_INT, status);
			if (status & INT_CMDTMO)
				return MSDC_ETMO;
			if (status & INT_RSPCRCERR)
				return MSDC_ECRC;
			if (r1)
				*r1 = rd(R_RESP0);
			return MSDC_OK;
		}
		if (expired(deadline))
			return MSDC_ETMO;
	}
}

/* CMD13 until the card is in TRAN with READY_FOR_DATA. */
static int
wait_card_ready(uint32_t ms)
{
	uint64_t deadline = deadline_ms(ms);

	for (;;) {
		uint32_t r1 = 0;
		int rc = msdc_cmd(CMD13_W, MMC_RCA << 16, &r1, 50);

		if (rc == MSDC_OK) {
			if (r1 & R1_FATAL_MASK)
				return MSDC_ESTATE;
			if (R1_STATE(r1) == R1_STATE_TRAN &&
			    (r1 & R1_READY_FOR_DATA))
				return MSDC_OK;
		}
		if (expired(deadline))
			return MSDC_ETMO;
		delay_ms(1);
	}
}

static void
fifo_push(const uint8_t *p, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes; i += 4) {
		uint32_t word;

		__builtin_memcpy(&word, p + i, 4);	/* little-endian out */
		wr_fifo(word);
	}
	dsb();
}

/* ---------------------------------------------------------------------- */
/* Data phase: one implementation, used by CMD8 / CMD17 / CMD18 and by     */
/* CMD24 / CMD25.                                                         */
/* ---------------------------------------------------------------------- */

static int
data_read(uint32_t cmdw, uint32_t arg, uint8_t *dst, uint32_t blocks)
{
	uint32_t response = 0, left = blocks << 9, off = 0;
	uint64_t deadline;
	int rc, done = 0;

	rc = wait_sts_clear(STS_SDCBUSY, 100);
	if (rc)
		return rc;
	rc = fifo_clear();
	if (rc)
		return rc;
	wr(R_BLK_NUM, blocks);
	wr(R_INT, INT_DAT_ALL);
	if (!(rd(R_CFG) & CFG_PIO))
		wr(R_CFG, rd(R_CFG) | CFG_PIO);

	rc = msdc_cmd(cmdw, arg, &response, 100);
	if (rc)
		goto stop;
	if (response & R1_FATAL_MASK) {
		rc = MSDC_ESTATE;
		goto stop;
	}

	deadline = deadline_ms(3000);
	for (;;) {
		uint32_t status = rd(R_INT) & INT_DAT_ALL;

		if (status) {
			wr(R_INT, status);
			if (status & INT_DATTMO) {
				rc = MSDC_ETMO;
				goto stop;
			}
			if (status & INT_DATCRCERR) {
				rc = MSDC_ECRC;
				goto stop;
			}
			if (status & INT_XFER_COMPL)
				done = 1;
		}
		if (done && left == 0)
			break;

		for (;;) {
			uint32_t count = FIFOCS_RXCNT(rd(R_FIFOCS));

			if (!count)
				break;
			if (count > left)
				count = left;
			if ((count & 3u) == 0) {
				for (uint32_t i = 0; i < count; i += 4) {
					uint32_t word = rd(R_RXDATA);

					__builtin_memcpy(dst + off, &word, 4);
					off += 4;
				}
			} else {
				for (uint32_t i = 0; i < count; i++)
					dst[off++] = (uint8_t)rd(R_RXDATA);
			}
			left -= count;
		}
		if (expired(deadline)) {
			rc = MSDC_ETMO;
			goto stop;
		}
	}

	if (blocks > 1) {
		uint32_t r1 = 0;

		rc = msdc_cmd(CMD12_W, 0, &r1, 250);
	}
	return rc;

stop:
	if (blocks > 1) {
		uint32_t r1 = 0;

		(void)msdc_cmd(CMD12_W, 0, &r1, 250);
	}
	(void)fifo_clear();
	wr(R_INT, 0xFFFFFFFFu);
	return rc;
}

static int
data_write(uint32_t cmdw, uint32_t arg, const uint8_t *src, uint32_t blocks)
{
	uint32_t response = 0, left = blocks << 9, off = 0;
	uint64_t deadline;
	int rc, done = 0;

	rc = wait_sts_clear(STS_SDCBUSY, 100);
	if (rc)
		return rc;
	rc = fifo_clear();
	if (rc)
		return rc;
	wr(R_BLK_NUM, blocks);
	wr(R_INT, INT_DAT_ALL);
	if (!(rd(R_CFG) & CFG_PIO))
		wr(R_CFG, rd(R_CFG) | CFG_PIO);

	rc = msdc_cmd(cmdw, arg, &response, 100);
	if (rc)
		goto stop;
	if (response & R1_FATAL_MASK) {
		rc = MSDC_ESTATE;
		goto stop;
	}

	deadline = deadline_ms(3000);
	for (;;) {
		uint32_t status = rd(R_INT) & INT_DAT_ALL;

		if (status) {
			wr(R_INT, status);
			if (status & INT_DATTMO) {
				rc = MSDC_ETMO;
				goto stop;
			}
			if (status & INT_DATCRCERR) {
				rc = MSDC_ECRC;
				goto stop;
			}
			if (status & INT_XFER_COMPL)
				done = 1;
		}
		if (done && left == 0)
			break;

		/* MediaTek's own policy: refill only when the FIFO is empty. */
		if (left && FIFOCS_TXCNT(rd(R_FIFOCS)) == 0) {
			uint32_t burst = left < FIFO_DEPTH ? left : FIFO_DEPTH;

			fifo_push(src + off, burst);
			off += burst;
			left -= burst;
		}
		if (expired(deadline)) {
			rc = MSDC_ETMO;
			goto stop;
		}
	}

	deadline = deadline_ms(2000);
	while (FIFOCS_TXCNT(rd(R_FIFOCS)) != 0) {
		if (expired(deadline)) {
			rc = MSDC_ETMO;
			goto stop;
		}
	}

	if (blocks > 1) {
		uint32_t r1 = 0;

		rc = msdc_cmd(CMD12_W, 0, &r1, 250);
		if (rc)
			goto stop;
	}

	return wait_card_ready(5000);

stop:
	if (blocks > 1) {
		uint32_t r1 = 0;

		(void)msdc_cmd(CMD12_W, 0, &r1, 250);
	}
	(void)fifo_clear();
	wr(R_INT, 0xFFFFFFFFu);
	return rc;
}

/* ---------------------------------------------------------------------- */
/* Chunked sector access                                                  */
/* ---------------------------------------------------------------------- */

int
msdc_read_sectors(uint32_t lba, void *dst, uint32_t bytes)
{
	uint8_t *p = (uint8_t *)dst;

	if (!msdc_ready || !dst || !bytes || (bytes & 511u))
		return msdc_ready ? MSDC_EARG : MSDC_ENOCLK;

	while (bytes) {
		uint32_t blocks = bytes >> 9;
		int rc;

		if (blocks > CHUNK_BLOCKS)
			blocks = CHUNK_BLOCKS;
		rc = data_read(blocks > 1 ? CMD18_W : CMD17_W, lba, p, blocks);
		if (rc)
			return rc;
		lba += blocks;
		p += blocks << 9;
		bytes -= blocks << 9;
	}
	return MSDC_OK;
}

int
msdc_write_sectors(uint32_t lba, const void *data, uint32_t bytes)
{
	const uint8_t *p = (const uint8_t *)data;

	if (!msdc_ready || !data || !bytes || (bytes & 511u))
		return msdc_ready ? MSDC_EARG : MSDC_ENOCLK;

	while (bytes) {
		uint32_t blocks = bytes >> 9;
		int rc;

		if (blocks > CHUNK_BLOCKS)
			blocks = CHUNK_BLOCKS;
		rc = data_write(blocks > 1 ? CMD25_W : CMD24_W, lba, p, blocks);
		if (rc)
			return rc;
		lba += blocks;
		p += blocks << 9;
		bytes -= blocks << 9;
	}
	return MSDC_OK;
}

/* ---------------------------------------------------------------------- */
/* Bring-up                                                               */
/* ---------------------------------------------------------------------- */

static int
host_bringup(struct msdc_report *r, uint32_t divider)
{
	uint32_t cfg, sdc;
	uint64_t deadline;

	/* MSDC_CFG first: if the clock is gated nothing else may be touched.
	 * LK has just loaded this image through this controller, so this is a
	 * sanity check rather than a real risk. */
	r->cfg = rd(R_CFG);
	if (!(r->cfg & CFG_CKSTB))
		return MSDC_ENOCLK;
	r->host_ok = 1;

	r->sdc_cfg = rd(R_SDC_CFG);
	r->version = rd(R_VERSION);
	r->eco = rd(R_ECO_VER);
	r->dma_ctrl = rd(R_DMA_CTRL);
	r->dma_cfg = rd(R_DMA_CFG);

	wr(R_INTEN, 0);				/* polling only */
	wr(R_INT, 0xFFFFFFFFu);

	/* Slow the bus down.  CKMOD is inherited on purpose: the card is still
	 * in whatever timing mode LK negotiated, and at this clock none of it
	 * matters. */
	cfg = r->cfg;
	cfg &= ~CFG_CKDIV_MASK;
	cfg |= CFG_MODE | CFG_PIO | CFG_CKDIV(divider);
	wr(R_CFG, cfg);

	deadline = deadline_ms(20);
	while (!(rd(R_CFG) & CFG_CKSTB))
		if (expired(deadline))
			return MSDC_ENOCLK;
	r->clock_div = divider;
	r->cfg_after = rd(R_CFG);

	/* Generous hardware backstops; software deadlines are the real limit. */
	sdc = rd(R_SDC_CFG);
	sdc &= ~(SDC_DTOC_MASK | SDC_WRDTOC_MASK);
	sdc |= (0xFFu << 24) | (0x1FFFu << 2);
	wr(R_SDC_CFG, sdc);
	r->sdc_after = rd(R_SDC_CFG);

	return fifo_clear();
}

/* The inherited bus width has to keep matching EXT_CSD[183]; we never change
 * it, but a mismatch means LK left something odd behind and writing would be
 * writing through a bus the card is not listening on. */
static int
check_bus_width(struct msdc_report *r)
{
	uint32_t host = (rd(R_SDC_CFG) & SDC_BUSWIDTH_MASK) >> 16;

	r->host_bus = host;
	return host == (r->ext_bus & 3u) ? MSDC_OK : MSDC_ESTATE;
}

static int
ext_csd_read(struct msdc_report *r)
{
	static uint8_t ext[512] __attribute__((aligned(8)));
	int rc;

	rc = data_read(CMD8_W, 0, ext, 1);
	if (rc)
		return rc;

	r->ext_rev = ext[192];
	r->ext_part = ext[EXT_CSD_PART_ACCESS] & 7u;
	r->ext_bus = ext[EXT_CSD_BUS_WIDTH];
	r->ext_hs = ext[EXT_CSD_HS_TIMING];
	r->ext_cache = ext[EXT_CSD_CACHE_CTRL] & 1u;
	r->ext_sectors = (uint32_t)ext[EXT_CSD_SECTOR_COUNT] |
			 ((uint32_t)ext[EXT_CSD_SECTOR_COUNT + 1] << 8) |
			 ((uint32_t)ext[EXT_CSD_SECTOR_COUNT + 2] << 16) |
			 ((uint32_t)ext[EXT_CSD_SECTOR_COUNT + 3] << 24);
	return MSDC_OK;
}

/* CMD6 write-byte, then wait for the card to come back to TRAN. */
static int
emmc_switch(uint32_t index, uint32_t value, uint32_t ms)
{
	uint32_t response = 0;
	int rc;

	rc = msdc_cmd(CMD6_W, (3u << 24) | (index << 16) | (value << 8),
		      &response, 300);
	if (rc)
		return rc;
	if (response & R1_FATAL_MASK)
		return MSDC_ESTATE;
	return wait_card_ready(ms);
}

/* ---------------------------------------------------------------------- */
/* GPT                                                                    */
/* ---------------------------------------------------------------------- */

static int
partition_name_is(const uint8_t *entry, const char *name)
{
	const uint16_t *utf16 = (const uint16_t *)(const void *)(entry + 56);
	unsigned i;

	/* GPT partition names are 36 UTF-16LE code units, NUL padded. */
	for (i = 0; name[i]; i++) {
		uint16_t unit;

		__builtin_memcpy(&unit, (const uint8_t *)utf16 + i * 2, 2);
		if (unit != (uint16_t)(unsigned char)name[i])
			return 0;
	}
	{
		uint16_t terminator;

		__builtin_memcpy(&terminator, (const uint8_t *)utf16 + i * 2, 2);
		return terminator == 0;
	}
}

static uint64_t
get_le64(const uint8_t *p)
{
	return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
	       ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
	       ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
	       ((uint64_t)p[7] << 56);
}

static uint32_t
get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int
gpt_find(const char *name, uint32_t *lba_out, uint32_t *sectors_out,
	 uint32_t *entries_seen)
{
	static uint8_t header[512] __attribute__((aligned(8)));
	static uint8_t table[16384] __attribute__((aligned(8)));
	uint64_t entries_lba;
	uint32_t count, size, bytes;
	int rc;

	rc = msdc_read_sectors(1, header, 512);
	if (rc)
		return rc;
	if (!same_bytes(header, "EFI PART", 8))
		return MSDC_ENOPART;

	entries_lba = get_le64(header + 72);
	count = get_le32(header + 80);
	size = get_le32(header + 84);
	*entries_seen = count;

	if (!count || !size || size < 128 || count > 128)
		return MSDC_ENOPART;

	bytes = count * size;
	if (bytes > sizeof(table))
		return MSDC_EARG;
	bytes = (bytes + 511u) & ~511u;		/* the sector layer wants 512s */

	rc = msdc_read_sectors((uint32_t)entries_lba, table, bytes);
	if (rc)
		return rc;

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *entry = table + i * size;

		if (!partition_name_is(entry, name))
			continue;
		*lba_out = (uint32_t)get_le64(entry + 32);
		*sectors_out = (uint32_t)(get_le64(entry + 40) -
					  get_le64(entry + 32) + 1);
		return MSDC_OK;
	}
	return MSDC_ENOPART;
}

/* ---------------------------------------------------------------------- */
/* The log block                                                          */
/* ---------------------------------------------------------------------- */

/*
 * Shared staging buffer.  4 KiB is the smallest unit that makes the 128-byte
 * FIFO depth irrelevant and keeps the per-transfer command overhead low.
 */
static uint8_t msdc_bounce[BOUNCE_BYTES] __attribute__((aligned(8)));

/* Is [lba, lba+sectors) entirely erased?  Checked in 4 KiB reads. */
static int
span_is_blank(uint32_t lba, uint32_t sectors)
{
	while (sectors) {
		uint32_t chunk = sectors > (BOUNCE_BYTES / 512) ?
				 (BOUNCE_BYTES / 512) : sectors;
		uint32_t bytes = chunk << 9;

		if (msdc_read_sectors(lba, msdc_bounce, bytes))
			return 0;
		for (uint32_t i = 0; i < bytes; i++)
			if (msdc_bounce[i] != 0xFFu)
				return 0;
		lba += chunk;
		sectors -= chunk;
	}
	return 1;
}

/*
 * Find a place to put the log.
 *
 * The first test boot proved that flashinfo is not blank past its vendor
 * header (the sector at +1 MiB held real data), so scan for a blank run
 * instead of assuming one.  Candidates are examined one sector per 64 KiB so
 * the scan costs a few hundred small reads; the full span is only read when a
 * candidate looks empty.
 *
 * A slot that already carries one of our headers is reused as-is, which keeps
 * the boot counter counting up instead of starting over.
 */
static int
find_slot(struct msdc_report *r, uint32_t part_lba, uint32_t part_sectors,
	  uint32_t need_bytes, uint32_t *out_lba)
{
	uint32_t need = (need_bytes + 511u) >> 9;
	uint32_t probes = 0;

	for (uint32_t offset = MSDC_LOG_LBA_OFFSET;
	     offset + need <= part_sectors && probes < MSDC_SCAN_MAX;
	     offset += MSDC_SCAN_STEP, probes++) {
		uint32_t lba = part_lba + offset;
		static uint8_t probe[512] __attribute__((aligned(8)));
		int blank = 1;

		r->scan_count = probes + 1;
		if (msdc_read_sectors(lba, probe, 512))
			return MSDC_EIO;

		if (probes == 0) {
			/* Record what is actually there: the first attempt is
			 * the one whose assumption we want to check. */
			for (unsigned i = 0; i < 4; i++)
				r->probe_hex[i] = get_le32(probe + i * 4);
			for (unsigned i = 0; i < 512; i++)
				if (probe[i] != 0xFFu)
					r->probe_nonff++;
		}

		if (same_bytes(probe + SLOG_OFF_MAGIC, SLOG_MAGIC, 8)) {
			r->reused = 1;
			r->erased_ok = 0;
			*out_lba = lba;
			return MSDC_OK;
		}

		for (unsigned i = 0; i < sizeof(probe); i++) {
			if (probe[i] != 0xFFu) {
				blank = 0;
				break;
			}
		}
		if (!blank)
			continue;

		if (!span_is_blank(lba, need))
			continue;

		r->erased_ok = 1;
		r->reused = 0;
		*out_lba = lba;
		return MSDC_OK;
	}
	return MSDC_ESPACE;
}

static int
write_text(uint32_t base_lba, const char *text, uint32_t length,
	   uint32_t *written)
{
	uint32_t offset = 0;

	while (offset < length) {
		uint32_t chunk = length - offset;
		uint32_t blocks;
		int rc;

		if (chunk > sizeof(msdc_bounce))
			chunk = sizeof(msdc_bounce);
		__builtin_memcpy(msdc_bounce, text + offset, chunk);
		for (uint32_t i = chunk; i < sizeof(msdc_bounce); i++)
			msdc_bounce[i] = 0;
		blocks = (chunk + 511u) >> 9;
		rc = msdc_write_sectors(base_lba + (offset >> 9), msdc_bounce,
				       blocks << 9);
		if (rc) {
			*written = offset;
			return rc;
		}
		offset += blocks << 9;
	}
	*written = length;
	return MSDC_OK;
}

/*
 * Stage 1: bring the controller up from LK's state, prove we can talk to the
 * card, and pick a blank run to write into.  Read-only - nothing is written
 * here, so a wrong assumption costs a failed test rather than a partition.
 *
 * `span_bytes` is the space we want to reserve for the text; the caller knows
 * roughly how long the log is before it starts, and the report that prepare()
 * itself prints is added on top of that by the caller's slack.
 */
int
msdc_log_prepare(struct msdc_report *r, uint32_t span_bytes)
{
	uint32_t flashinfo_lba = MSDC_FALLBACK_LBA;
	uint32_t flashinfo_sectors = MSDC_FALLBACK_SECTORS;
	uint32_t base_lba = 0;
	int rc;

	/* Wipe the report first: a partially filled struct printed after a
	 * failure must not claim anything that did not happen. */
	for (unsigned i = 0; i < sizeof(*r); i++)
		((uint8_t *)r)[i] = 0;
	r->target_lba = 0;
	msdc_ready = 0;

	if (span_bytes > MSDC_TEXT_MAX)
		span_bytes = MSDC_TEXT_MAX;
	r->span_bytes = span_bytes;

	rc = host_bringup(r, 260);		/* ~385 kHz @ MSDC PLL/2 */
	if (rc) {
		r->status_rc = rc;
		return rc;
	}

	/* Fast path: is the card still selected and idle in TRAN? */
	r->status_rc = msdc_cmd(CMD13_W, MMC_RCA << 16, &r->status_r1, 100);
	if (r->status_rc == MSDC_OK &&
	    (r->status_r1 & R1_FATAL_MASK) == 0 &&
	    R1_STATE(r->status_r1) == R1_STATE_TRAN &&
	    (r->status_r1 & R1_READY_FOR_DATA)) {
		/*
		 * Yes.  From here the controller and card are consistent, so
		 * the sector layer can be used.
		 */
		msdc_ready = 1;
	}

	if (!msdc_ready)
		return r->status_rc ? r->status_rc : MSDC_ESTATE;

	r->ext_rc = ext_csd_read(r);
	if (r->ext_rc)
		return r->ext_rc;

	if (r->ext_part != 0) {
		/* A boot or RPMB partition is selected; writing here would hit
		 * the wrong place.  Select the user area. */
		rc = emmc_switch(EXT_CSD_PART_ACCESS, 0, 1000);
		if (rc)
			return rc;
		r->ext_part = 0;
	}

	r->bus_ok = check_bus_width(r) == MSDC_OK;

	/* Locate the partition.  The GPT is authoritative; the measured LBA is
	 * only a fallback for a partition table we cannot parse. */
	r->gpt_rc = gpt_find("flashinfo", &flashinfo_lba, &flashinfo_sectors,
			     &r->gpt_entries);
	r->gpt_found = r->gpt_rc == MSDC_OK;
	if (!r->gpt_found) {
		flashinfo_lba = MSDC_FALLBACK_LBA;
		flashinfo_sectors = MSDC_FALLBACK_SECTORS;
	}
	r->flashinfo_lba = flashinfo_lba;
	r->flashinfo_sectors = flashinfo_sectors;

	if (flashinfo_sectors < msdc_lba_offset + 2048u)
		return MSDC_EARG;

	/* A mismatch here means LK left the controller driving a bus the card is
	 * not listening on, so a write would land as garbage. */
	if (!r->bus_ok)
		return MSDC_ESTATE;

	rc = find_slot(r, flashinfo_lba, flashinfo_sectors,
		       SLOG_HEADER_SIZE + span_bytes, &base_lba);
	if (rc)
		return rc;

	r->target_lba = base_lba;
	msdc_log_base = base_lba;
	return MSDC_OK;
}

/*
 * Stage 2: write the captured text, then the header, then read both back.
 * Called after the report has been printed, so the log contains its own
 * bring-up report.
 */
int
msdc_log_commit(struct msdc_report *r, const char *text, uint32_t length)
{
	static uint8_t header_sector[SLOG_HEADER_SIZE]
		__attribute__((aligned(8)));
	static uint8_t verify[512] __attribute__((aligned(8)));
	uint32_t base_lba = msdc_log_base, boot = 0;
	int rc;

	if (!base_lba)
		return MSDC_EARG;

	/* Trailing bytes of the log are dropped rather than truncated mid-word;
	 * the decoder treats the byte count as authoritative anyway. */
	if (length > SLOG_SLOTS * SLOG_SLOT_SIZE)
		length = SLOG_SLOTS * SLOG_SLOT_SIZE;
	if (r->span_bytes && length > r->span_bytes)
		length = r->span_bytes;

	/* Keep the boot counter running across test boots; the second pass of the
	 * same boot must not bump it again. */
	if (r->committed)
		boot = r->boot;
	else if (msdc_read_sectors(base_lba, verify, 512) == MSDC_OK &&
		 same_bytes(verify + SLOG_OFF_MAGIC, SLOG_MAGIC, 8))
		boot = get_le32(verify + SLOG_OFF_BOOT) + 1;
	r->boot = boot;

	/* Text first, header last: a dump without the magic means the writer
	 * never finished, which is exactly the signal we want. */
	rc = write_text(base_lba + 1, text, length, &r->text_bytes);
	r->write_rc = rc;
	if (rc)
		return rc;

	/* Read the text back and compare, so "written" means "verified". */
	{
		uint32_t checked = 0;

		while (checked < length) {
			uint32_t chunk = length - checked;

			if (chunk > sizeof(verify))
				chunk = sizeof(verify);
			rc = msdc_read_sectors(base_lba + 1 + (checked >> 9),
					       verify, 512);
			if (rc) {
				r->verify_rc = rc;
				return rc;
			}
			if (!same_bytes(verify, text + checked, chunk)) {
				r->verify_rc = MSDC_EIO;
				return MSDC_EIO;
			}
			checked += 512;
		}
	}
	r->verify_rc = MSDC_OK;

	for (unsigned i = 0; i < sizeof(header_sector); i++)
		header_sector[i] = 0;
	__builtin_memcpy(header_sector + SLOG_OFF_MAGIC, SLOG_MAGIC, 8);
	put_le32(header_sector + SLOG_OFF_VERSION, SLOG_VERSION);
	put_le32(header_sector + SLOG_OFF_HDR_BYTES, SLOG_HEADER_SIZE);
	put_le32(header_sector + SLOG_OFF_BOOT, boot);
	put_le32(header_sector + SLOG_OFF_SLOT_COUNT, SLOG_SLOTS);
	put_le32(header_sector + SLOG_OFF_SLOT_SIZE, SLOG_SLOT_SIZE);
	put_le32(header_sector + SLOG_OFF_NEXT_SLOT,
		 (length + SLOG_SLOT_SIZE - 1) / SLOG_SLOT_SIZE);
	put_le32(header_sector + SLOG_OFF_TEXT_BYTES, length);
	{
		uint64_t ticks, frequency;

		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
		put_le64(header_sector + SLOG_OFF_CNTPCT, ticks);
		put_le64(header_sector + SLOG_OFF_CNTFRQ, frequency);
	}
	put_le64(header_sector + SLOG_OFF_PAYLOAD_PA, 0);
	put_le32(header_sector + SLOG_OFF_KERNEL_END, 0);
	put_le32(header_sector + SLOG_OFF_SHIM_FLAGS, 0);
	__builtin_memcpy(header_sector + SLOG_OFF_NOTE, "shim msdc pio", 13);

	rc = msdc_write_sectors(base_lba, header_sector, SLOG_HEADER_SIZE);
	r->header_rc = rc;
	if (rc)
		return rc;

	/* Flush the card cache if it is on, otherwise a reset can lose the tail
	 * even though every write reported success. */
	if (r->ext_cache)
		r->flush_rc = emmc_switch(32u, 1u, 30000);
	else
		r->flush_rc = MSDC_OK;

	/* Final gate: the header must read back with the right magic. */
	if (msdc_read_sectors(base_lba, verify, 512) != MSDC_OK ||
	    !same_bytes(verify + SLOG_OFF_MAGIC, SLOG_MAGIC, 8))
		return MSDC_EIO;

	r->committed = 1;
	return MSDC_OK;
}

/* Convenience for callers that do not need the report in the log itself. */
int
msdc_log_run(struct msdc_report *r, const char *text, uint32_t length)
{
	int rc = msdc_log_prepare(r, length);

	if (rc)
		return rc;
	return msdc_log_commit(r, text, length);
}

/* ---------------------------------------------------------------------- */
/* Report                                                                 */
/* ---------------------------------------------------------------------- */

void
msdc_report_print(const struct msdc_report *r)
{
	fb_puts("\n--- MSDC0 (eMMC log channel) ---\n", CLR_YELLOW);

	out("  controller    ", CLR_CYAN);
	out(r->host_ok ? "clocked" : "NO CLOCK", r->host_ok ? CLR_GREEN : CLR_RED);
	out("  version ", CLR_CYAN);
	outhex(r->version);
	out("  eco ", CLR_CYAN);
	outhex(r->eco);
	fb_newline();

	field_hex("cfg in     ", r->cfg);
	field_hex("cfg out    ", r->cfg_after);
	field_hex("clock div  ", r->clock_div);
	field_hex("sdc in     ", r->sdc_cfg);
	field_hex("sdc out    ", r->sdc_after);
	field_hex("dma ctrl   ", r->dma_ctrl);
	field_hex("dma cfg    ", r->dma_cfg);

	out("  status     ", CLR_CYAN);
	outhex(r->status_r1);
	out("  rc ", CLR_CYAN);
	outhex((unsigned long)(int32_t)r->status_rc);
	fb_newline();

	if (r->ext_rc == 0) {
		out("  ext csd     rev ", CLR_CYAN);
		outdec(r->ext_rev);
		out("  part ", CLR_CYAN);
		outdec(r->ext_part);
		out("  bus ", CLR_CYAN);
		outdec(r->ext_bus);
		out("  hs ", CLR_CYAN);
		outdec(r->ext_hs);
		out("  cache ", CLR_CYAN);
		outdec(r->ext_cache);
		fb_newline();
		field_dec("ext sectors ", r->ext_sectors);
		field_dec("host bus    ", r->host_bus);
	} else {
		field_hex("ext csd rc  ", (unsigned long)(int32_t)r->ext_rc);
	}

	field_hex("gpt rc     ", (unsigned long)(int32_t)r->gpt_rc);
	field_dec("gpt entries ", r->gpt_entries);
	out("  flashinfo   ", CLR_CYAN);
	outdec(r->flashinfo_lba);
	out(" + ", CLR_CYAN);
	outdec(r->flashinfo_sectors);
	out(r->gpt_found ? " (gpt)" : " (fallback)", CLR_WHITE);
	fb_newline();

	out("  log lba     ", CLR_CYAN);
	outdec(r->target_lba);
	out("  erased ", CLR_CYAN);
	out(r->erased_ok ? "yes" : "no", r->erased_ok ? CLR_GREEN : CLR_YELLOW);
	out("  reused ", CLR_CYAN);
	out(r->reused ? "yes" : "no", CLR_WHITE);
	fb_newline();

	field_dec("scan probes ", r->scan_count);
	out("  probe[0]    ", CLR_CYAN);
	for (unsigned i = 0; i < 4; i++) {
		outhex(r->probe_hex[i]);
		out(" ", CLR_WHITE);
	}
	out("nonff ", CLR_CYAN);
	outdec(r->probe_nonff);
	fb_newline();

	field_hex("write rc   ", (unsigned long)(int32_t)r->write_rc);
	field_hex("verify rc  ", (unsigned long)(int32_t)r->verify_rc);
	field_hex("header rc  ", (unsigned long)(int32_t)r->header_rc);
	field_hex("flush rc   ", (unsigned long)(int32_t)r->flush_rc);
	field_dec("text bytes ", r->text_bytes);
	field_dec("boot count ", r->boot);

	if (r->committed)
		fb_puts("  committed to flashinfo\n", CLR_GREEN);
	else
		fb_puts("  bring-up only, nothing written yet\n", CLR_YELLOW);
}
