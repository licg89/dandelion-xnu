/* Dandelion boot shim - durable log block format (shared with the host tool)
 *
 * The panel is the only output this board has, and photographing it once per
 * boot does not scale.  The shim and the kernel therefore also write their log
 * into a raw partition (flashinfo, 16 MiB, unused by Android) using a polled
 * eMMC writer, in a small self-describing format that tools/read_log_block.py
 * turns back into text on the host.
 *
 * The header is the SHLOG001 layout used by UniXNU's BootLog (magic, version,
 * sequence, timestamps, msgbuf size and the slot ring pointers), so their host
 * decoder can be used as a cross-check and so the format is not invented here
 * for no reason.
 *
 *   block 0        : struct slog_header
 *   blocks 1..N    : text slots, each SLOG_SLOT_SIZE bytes, written in order
 *
 * The header is written LAST (after the text), so a dump where the magic is
 * missing means "the writer never finished" rather than "there is no log".
 */
#ifndef DANDELION_LOG_BLOCK_H
#define DANDELION_LOG_BLOCK_H

#include <stdint.h>

#define SLOG_SECTOR        512u
#define SLOG_MAGIC         "DLXLOG01"
#define SLOG_VERSION       1u
#define SLOG_SLOT_SIZE     (64u * 1024u)
#define SLOG_SLOTS         8u              /* 512 KiB of text */
#define SLOG_HEADER_SIZE   SLOG_SECTOR
#define SLOG_TOTAL_SIZE    (SLOG_HEADER_SIZE + SLOG_SLOTS * SLOG_SLOT_SIZE)

struct slog_header {
	char     magic[8];                     /* "DLXLOG01"               */
	uint32_t version;
	uint32_t header_bytes;                 /* sizeof(struct slog_header) */
	uint32_t boot;                         /* boot counter             */
	uint32_t slot_count;
	uint32_t slot_size;
	uint32_t next_slot;                    /* where the writer stopped */
	uint32_t text_bytes;                   /* bytes valid across slots */
	uint64_t cntpct;                       /* timestamp, counter ticks */
	uint64_t cntfrq;
	uint64_t payload_pa;                   /* kernel entry, for context */
	uint32_t kernel_end_pa;
	uint32_t shim_flags;
	char     note[64];                     /* free-form, nul padded    */
	uint8_t  reserved[SLOG_HEADER_SIZE - (8 + 4 * 6 + 8 * 3 + 4 + 64)];
};
_Static_assert(sizeof(struct slog_header) == SLOG_HEADER_SIZE,
    "slog_header must fill exactly one sector");

#endif /* DANDELION_LOG_BLOCK_H */
