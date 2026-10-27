/* Dandelion boot shim - durable log block format (shared with the host tool)
 *
 * The panel is the only output this board has, and photographing it once per
 * boot does not scale.  The shim therefore also writes its log into a raw
 * partition (flashinfo, 16 MiB, unused by Android) using a polled eMMC
 * writer, in a small self-describing format that tools/read_log_block.py
 * turns back into text on the host.
 *
 * The header is the SHLOG001 layout used by UniXNU's BootLog (magic, version,
 * sequence, timestamps, msgbuf size and the slot ring pointers), so their host
 * decoder can be used as a cross-check and so the format is not invented here
 * for no reason.
 *
 *   block 0        : header, one 512-byte sector
 *   blocks 1..N    : text slots, each SLOG_SLOT_SIZE bytes, written in order
 *
 * The header is written LAST (after the text), so a dump where the magic is
 * missing means "the writer never finished" rather than "there is no log".
 *
 * The header is described by byte offsets rather than by a struct on purpose:
 * the shim runs with the MMU off, where every access is Device-nGnRnE and an
 * unaligned 64-bit store faults.  A packed C struct would produce exactly
 * those unaligned accesses (cntpct sits at 0x24), so the writer assembles the
 * sector with explicit little-endian put_le32()/put_le64() calls instead.
 * tools/read_log_block.py parses "<8sIIIIIIIQQQII64s", which is this same
 * layout with no padding.
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

/* field offsets inside the header sector */
#define SLOG_OFF_MAGIC      0x00u   /*  8 bytes, not NUL terminated          */
#define SLOG_OFF_VERSION    0x08u   /* u32                                   */
#define SLOG_OFF_HDR_BYTES  0x0Cu   /* u32                                   */
#define SLOG_OFF_BOOT       0x10u   /* u32, boot counter                     */
#define SLOG_OFF_SLOT_COUNT 0x14u   /* u32                                   */
#define SLOG_OFF_SLOT_SIZE  0x18u   /* u32                                   */
#define SLOG_OFF_NEXT_SLOT  0x1Cu   /* u32, where the writer stopped         */
#define SLOG_OFF_TEXT_BYTES 0x20u   /* u32, bytes valid across slots         */
#define SLOG_OFF_CNTPCT     0x24u   /* u64, counter ticks                    */
#define SLOG_OFF_CNTFRQ     0x2Cu   /* u64                                   */
#define SLOG_OFF_PAYLOAD_PA 0x34u   /* u64, kernel entry, for context        */
#define SLOG_OFF_KERNEL_END 0x3Cu   /* u32                                   */
#define SLOG_OFF_SHIM_FLAGS 0x40u   /* u32                                   */
#define SLOG_OFF_NOTE       0x44u   /* 64 bytes, NUL padded                  */

#endif /* DANDELION_LOG_BLOCK_H */
