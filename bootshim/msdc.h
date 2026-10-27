/* Dandelion boot shim - polled-PIO eMMC log channel (interface)
 *
 * The panel is the only output this board has, and photographing it does not
 * scale.  Both USB routes are dead ends (see docs/DEAD-ENDS.md), so the shim
 * writes its log into the unused 16 MiB "flashinfo" partition instead: after
 * the test boot we go back to Android and dump that partition with adb, which
 * needs no cooperation from the phone at boot time.
 *
 * Everything here is read-only until the very last step, and that step is
 * guarded: the target sector must read back as erased (0xff) before a single
 * byte is written, and the result is read back and compared.
 */
#ifndef DANDELION_MSDC_H
#define DANDELION_MSDC_H

#include <stdint.h>
#include <stddef.h>

/*
 * Where the log lives inside the partition.
 *
 * The first 64 KiB of flashinfo holds a vendor "DOWNLOAD INFORMATION!!"
 * record, so the log starts 1 MiB in; everything past the first 64 KiB was
 * verified 0xff-filled on the real device (docs/NIGHT-LOG-1027.md).
 */
#define MSDC_LOG_LBA_OFFSET	2048u		/* +1 MiB in 512 B sectors */

/*
 * Fallback for the partition start, measured on the device with
 * /sys/class/block/mmcblk0p43/start.  Only used when the GPT read fails; the
 * GPT is authoritative because Android may repartition on an OTA.
 */
#define MSDC_FALLBACK_LBA	122109919u
#define MSDC_FALLBACK_SECTORS	32768u		/* 16 MiB */

/* Outcome of one staged bring-up.  Printed on the panel and written to eMMC. */
struct msdc_report {
	int      host_ok;		/* MSDC_CFG said the clock was running   */
	uint32_t cfg;			/* MSDC_CFG as inherited from LK         */
	uint32_t cfg_after;		/* MSDC_CFG after our re-programming     */
	uint32_t sdc_cfg;		/* SDC_CFG as inherited from LK          */
	uint32_t sdc_after;
	uint32_t dma_ctrl;
	uint32_t dma_cfg;
	uint32_t version;
	uint32_t eco;
	uint32_t clock_div;		/* divider in use when the log was written */

	int      status_rc;		/* CMD13 fast path                       */
	uint32_t status_r1;		/* the R1 word it returned               */

	int      ext_rc;		/* CMD8 (EXT_CSD)                        */
	uint32_t ext_rev;		/* EXT_CSD[192]                          */
	uint32_t ext_part;		/* EXT_CSD[179] & 7: 0 = user area       */
	uint32_t ext_bus;		/* EXT_CSD[183] & 3                      */
	uint32_t ext_hs;		/* EXT_CSD[185]                          */
	uint32_t ext_sectors;		/* EXT_CSD[212..215]                     */
	uint32_t ext_cache;		/* EXT_CSD[33] & 1                       */
	uint32_t host_bus;		/* SDC_CFG BUSWIDTH as LK left it        */
	int      bus_ok;		/* host bus width matches EXT_CSD[183]   */

	int      gpt_rc;		/* GPT scan                              */
	int      gpt_found;		/* partition name matched exactly         */
	uint32_t gpt_entries;		/* entries the header claims             */
	uint32_t flashinfo_lba;		/* partition start as found/located      */
	uint32_t flashinfo_sectors;
	uint32_t target_lba;		/* where the log header goes             */

	int      erased_ok;		/* target sector read back as 0xff       */
	int      reused;		/* target already holds one of our logs  */
	int      write_rc;		/* text slots                            */
	int      verify_rc;		/* read-back comparison                  */
	int      header_rc;		/* header sector                         */
	int      flush_rc;		/* CMD6 FLUSH_CACHE                      */
	uint32_t text_bytes;		/* bytes of log actually written         */
	uint32_t boot;			/* boot counter out of the old header    */
	int      committed;		/* the log is on the medium              */
};

/*
 * Stage the whole thing: bring the controller up from LK's state, verify the
 * card, find flashinfo, write `text` as a DLXLOG block, read it back.
 *
 * Returns 0 when the log is on the medium and verified.  Never panics: every
 * failure is recorded in `report` and the shim carries on to the panel.
 */
int msdc_log_run(struct msdc_report *report, const char *text, uint32_t length);

/*
 * The same thing in two halves, because the panel report is part of the log
 * we want to capture: prepare() is read-only and fills in everything up to
 * "the target sector is erased", the caller prints that report, and commit()
 * then writes the captured text (which now includes the report).
 */
int msdc_log_prepare(struct msdc_report *report);
int msdc_log_commit(struct msdc_report *report, const char *text,
		    uint32_t length);

/* Print a report on the panel (also mirrored into the pstore log). */
void msdc_report_print(const struct msdc_report *report);

/* Low level, exposed for bring-up experiments. */
int msdc_write_sectors(uint32_t lba, const void *data, uint32_t bytes);
int msdc_read_sectors(uint32_t lba, void *dst, uint32_t bytes);

#endif /* DANDELION_MSDC_H */
