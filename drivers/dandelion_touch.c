/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the dandelion-xnu authors
 *
 * Dandelion Novatek NT36525B touch groundwork.  UNTESTED: this has never run on
 * the phone, and the register report has never been read out of this chip.  See
 * docs/TOUCH-PLAN.md for the ladder and for what still has to be measured.
 *
 * The protocol is Novatek's "host download" SPI protocol, which is a plain
 * register window: select a 128-byte page with command 0xff, then read or write
 * bytes at (address & 0x7f) inside it.  The top bit of the first byte selects
 * read (0) or write (1).
 */
#include "dandelion_touch.h"

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

/*
 * [VK] nvt_set_page(): 0xff, then the address split as (addr >> 15) and
 * (addr >> 7).  The low 7 bits are the index inside the page and are sent as
 * the first byte of the next transfer.
 */
int
dandelion_nvt_set_page(const struct dandelion_nvt_bus *bus, uint32_t addr)
{
	uint8_t buf[3];

	if (!bus || !bus->xfer) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	buf[0] = 0xffU;
	buf[1] = (uint8_t)((addr >> 15) & 0xffU);
	buf[2] = (uint8_t)((addr >> 7) & 0xffU);

	return bus->xfer(bus->ctx, buf, 0, 3) == 0 ?
	    DANDELION_TOUCH_OK : DANDELION_TOUCH_ERR_BUS;
}

int
dandelion_nvt_write8(const struct dandelion_nvt_bus *bus, uint32_t addr,
    uint8_t value)
{
	uint8_t buf[2];
	int rc;

	if (!bus || !bus->xfer) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	rc = dandelion_nvt_set_page(bus, addr);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	/* [VK] SPI_WRITE_MASK: the top bit of the index byte marks a write. */
	buf[0] = (uint8_t)((addr & 0x7fU) | 0x80U);
	buf[1] = value;

	return bus->xfer(bus->ctx, buf, 0, 2) == 0 ?
	    DANDELION_TOUCH_OK : DANDELION_TOUCH_ERR_BUS;
}

/*
 * [VK] CTP_SPI_READ(): the index byte goes out with the top bit cleared, the
 * transfer is one byte longer than the data because of the dummy byte, and the
 * payload comes back starting at rx[2] (one index byte, one dummy byte).
 */
int
dandelion_nvt_read(const struct dandelion_nvt_bus *bus, uint32_t addr,
    uint8_t *out, uint32_t len)
{
	/* Large enough for the biggest transfer in this file: one whole report
	 * (66 bytes) plus the dummy byte. */
	uint8_t tx[NVT_POINT_DATA_LEN + 1U + NVT_DUMMY_BYTES];
	uint8_t rx[NVT_POINT_DATA_LEN + 1U + NVT_DUMMY_BYTES];
	int rc;
	uint32_t i;

	if (!bus || !bus->xfer || !out || len == 0 || len > sizeof(tx)) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	rc = dandelion_nvt_set_page(bus, addr);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	/* The whole transfer, dummy byte included, has to be defined: the dummy
	 * byte is what clocks the last payload byte out of the chip. */
	for (i = 0; i < len + NVT_DUMMY_BYTES; i++) {
		tx[i] = 0;
		rx[i] = 0;
	}
	tx[0] = (uint8_t)(addr & 0x7fU);   /* SPI_READ_MASK: top bit clear */

	rc = bus->xfer(bus->ctx, tx, rx, len + NVT_DUMMY_BYTES);
	if (rc != 0) {
		return DANDELION_TOUCH_ERR_BUS;
	}

	for (i = 0; i < len; i++) {
		out[i] = rx[i + 2U];
	}

	return DANDELION_TOUCH_OK;
}

/* ------------------------------------------------------------------ */
/* Reset sequences                                                     */
/* ------------------------------------------------------------------ */

/*
 * Drive the reset line through the bus, if the caller wired it up.
 *
 * NOTE: the vendor driver does *not* do this at probe time -- it has
 * NVT_TOUCH_SUPPORT_HW_RST 0 and relies on the power-on reset -- so on this
 * device this sequence is a fallback for the case where the panel's own reset
 * has already happened and the controller is left in an unknown state.  The
 * delays are the vendor's resume sequence: reset high, 10 ms, and the caller
 * must then wait another 10 ms before the first SPI access.
 */
int
dandelion_nvt_hw_reset(const struct dandelion_nvt_bus *bus)
{
	if (!bus || !bus->reset_pin) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	bus->reset_pin(bus->ctx, 0);
	if (bus->delay_ms) {
		bus->delay_ms(bus->ctx, 10);
	}
	bus->reset_pin(bus->ctx, 1);
	if (bus->delay_ms) {
		bus->delay_ms(bus->ctx, 10);
	}

	return DANDELION_TOUCH_OK;
}

/*
 * [VK] nvt_eng_reset(): write 0x5a to ENG_RST_ADDR (0x7fff80) and wait 1 ms
 * ("tMCU_Idle2TP_REX_Hi after TP_RST").  The vendor driver runs this *before*
 * raising the reset line.
 */
int
dandelion_nvt_eng_reset(const struct dandelion_nvt_bus *bus)
{
	int rc;

	rc = dandelion_nvt_write8(bus, NVT_ADDR_ENG_RST, 0x5aU);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}
	if (bus->delay_ms) {
		bus->delay_ms(bus->ctx, 1);
	}

	return DANDELION_TOUCH_OK;
}

/*
 * [VK] nvt_bootloader_reset(): 0x69 to SWRST_N8_ADDR, wait 5 ms
 * ("tBRST2FR after Bootload RST"), then clear SPI_RD_FAST if the device tree
 * gave us one (0x3f310 on dandelion).
 */
int
dandelion_nvt_bootloader_reset(const struct dandelion_nvt_bus *bus)
{
	int rc;

	rc = dandelion_nvt_write8(bus, NVT_ADDR_SWRST_N8, 0x69U);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}
	if (bus->delay_ms) {
		bus->delay_ms(bus->ctx, 5);
	}

	rc = dandelion_nvt_write8(bus, NVT_ADDR_SPI_RD_FAST, 0x00U);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	return DANDELION_TOUCH_OK;
}

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */

/*
 * [VK] nvt_ts_check_chip_ver_trim(): after a bootloader reset, read seven bytes
 * at 0x1f64e and compare bytes 1..6 against the trim table.
 *
 * The NT36525/NT36525B row in that table is
 *     id = {0xff, 0xff, 0xff, 0x25, 0x65, 0x03}, mask = {0,0,0,1,1,1}
 * so only the last three bytes are compared: the chip must report 0x25, 0x65,
 * 0x03 -- that is "36525" plus the 0x03 family byte.
 *
 * TODO: the vendor driver retries this five times with a 10 ms gap, and only a
 * match tells it which memory map to use (NT36525 -> EVENT_BUF_ADDR 0x11a00).
 * A mismatch must be treated as "not our chip", not as "retry harder".
 */
int
dandelion_nvt_identify(const struct dandelion_nvt_bus *bus)
{
	uint8_t buf[7];
	int rc;
	unsigned attempt;

	if (!bus) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	for (attempt = 0; attempt < 5U; attempt++) {
		rc = dandelion_nvt_bootloader_reset(bus);
		if (rc != DANDELION_TOUCH_OK) {
			return rc;
		}

		rc = dandelion_nvt_read(bus, NVT_ADDR_TRIM_ID, buf, 7);
		if (rc == DANDELION_TOUCH_OK) {
			if (buf[4] == 0x25U && buf[5] == 0x65U &&
			    buf[6] == 0x03U) {
				return DANDELION_TOUCH_OK;
			}
		}

		if (bus->delay_ms) {
			bus->delay_ms(bus->ctx, 10);
		}
	}

	return DANDELION_TOUCH_ERR_NOT_ID;
}

/*
 * [VK] nvt_get_fw_info(): seventeen bytes from the event buffer at 0x78.
 * buf[1] fw version, buf[3]/buf[4] sensor counts, buf[5..6] and buf[7..8] the
 * maximum X and Y the firmware reports.  The vendor driver treats
 * (fw_ver + ~fw_ver) != 0xff as a broken block, retries three times, and falls
 * back to 720x1600 -- which is exactly this panel.
 */
int
dandelion_nvt_read_info(const struct dandelion_nvt_bus *bus,
    struct dandelion_nvt_info *info)
{
	uint8_t buf[17];
	int rc;

	if (!bus || !info) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	rc = dandelion_nvt_read(bus, NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_FWINFO,
	    buf, sizeof(buf));
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	info->fw_ver = buf[1];
	info->x_num = buf[3];
	info->y_num = buf[4];
	info->abs_x_max = (uint16_t)((buf[5] << 8) | buf[6]);
	info->abs_y_max = (uint16_t)((buf[7] << 8) | buf[8]);

	if ((uint8_t)(buf[1] + buf[2]) != 0xffU) {
		info->fw_ver = 0;
		info->x_num = 18;
		info->y_num = 32;
		info->abs_x_max = (uint16_t)DANDELION_TOUCH_MAX_X;
		info->abs_y_max = (uint16_t)DANDELION_TOUCH_MAX_Y;
		return DANDELION_TOUCH_ERR_BUS;
	}

	/* [VK] nvt_read_pid(): two bytes at the project-ID event offset. */
	rc = dandelion_nvt_read(bus, NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_PROJECTID,
	    buf, 3);
	if (rc == DANDELION_TOUCH_OK) {
		info->pid_low = buf[1];
		info->pid_high = buf[2];
	}

	return DANDELION_TOUCH_OK;
}

/*
 * [VK] nvt_check_fw_reset_state(): poll the reset-complete byte until it is in
 * [want, 0xaf].  One poll every 10 ms; the vendor driver allows ten polls for
 * the initial state and fifty afterwards.
 */
int
dandelion_nvt_wait_reset_state(const struct dandelion_nvt_bus *bus, uint8_t want,
    unsigned polls)
{
	uint8_t buf[6];
	unsigned i;
	int rc;

	if (!bus) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	for (i = 0; i < polls; i++) {
		rc = dandelion_nvt_read(bus,
		    NVT_EVENT_BUF_ADDR | NVT_EVENT_MAP_RESET_COMPLETE, buf,
		    sizeof(buf));
		if (rc == DANDELION_TOUCH_OK) {
			if (buf[1] >= want && buf[1] <= NVT_RESET_STATE_MAX) {
				return DANDELION_TOUCH_OK;
			}
		}
		if (bus->delay_ms) {
			bus->delay_ms(bus->ctx, 10);
		}
	}

	return DANDELION_TOUCH_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* One report                                                          */
/* ------------------------------------------------------------------ */

/*
 * [VK] nvt_ts_work_func(): the interrupt handler reads POINT_DATA_LEN + 1 = 66
 * bytes from index 0 of the event-buffer page.  Index 0 is the "handshaking"
 * byte the firmware uses to tell the host it has new data; the ten finger
 * records start at offset 1.
 *
 * The caller must have selected the event-buffer page already (identify and
 * read_info both leave the page there); this function re-selects it so that a
 * report can be read after any other access.
 */
int
dandelion_nvt_read_report(const struct dandelion_nvt_bus *bus,
    uint8_t raw[NVT_POINT_DATA_LEN + 1U])
{
	int rc;

	if (!bus || !raw) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	rc = dandelion_nvt_set_page(bus, NVT_EVENT_BUF_ADDR);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	rc = dandelion_nvt_read(bus, NVT_EVENT_BUF_ADDR,
	    raw, NVT_POINT_DATA_LEN + 1U);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	return DANDELION_TOUCH_OK;
}

/*
 * [VK] nvt_ts_work_func(), decoding half.
 *
 * Each record is six bytes at raw[1 + 6*i]:
 *   [0] bits 7..3 = finger id (1..10), bits 2..0 = status
 *       (1 = enter or moving, 2 = moving, 0 = nothing in this slot)
 *   [1] X bits 11..4
 *   [2] Y bits 11..4
 *   [3] bits 7..4 = X bits 3..0, bits 3..0 = Y bits 3..0
 *   [4] contact area, 0..255 (0 is reported as 1)
 *   [5] pressure low byte; for the first two fingers the high byte is at
 *       raw[63] and raw[64], because the record layout has no room for it
 *
 * A finger with id 0, id > 10, or a status other than 1 or 2 is not a finger.
 * Coordinates outside the panel are dropped, as the vendor driver does.
 */
int
dandelion_nvt_decode_report(const uint8_t *raw,
    struct dandelion_touch_report *out)
{
	unsigned i;
	unsigned pos;
	uint8_t id, status;
	uint32_t x, y, w, p;

	if (!raw || !out) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	out->count = 0;

	for (i = 0; i < DANDELION_TOUCH_MAX_FINGERS; i++) {
		pos = 1U + NVT_FINGER_STRIDE * i;
		id = (uint8_t)(raw[pos] >> 3);
		status = (uint8_t)(raw[pos] & 0x07U);

		if (id == 0 || id > DANDELION_TOUCH_MAX_FINGERS) {
			continue;
		}
		if (status != 1U && status != 2U) {
			continue;
		}

		x = ((uint32_t)raw[pos + 1] << 4) |
		    ((uint32_t)raw[pos + 3] >> 4);
		y = ((uint32_t)raw[pos + 2] << 4) |
		    ((uint32_t)raw[pos + 3] & 0x0fU);

		if (x >= DANDELION_TOUCH_MAX_X || y >= DANDELION_TOUCH_MAX_Y) {
			continue;
		}

		w = raw[pos + 4];
		if (w == 0) {
			w = 1;
		}

		p = raw[pos + 5];
		if (i < 2U) {
			p += (uint32_t)raw[63U + i] << 8;
		}
		if (p > DANDELION_TOUCH_MAX_PRESSURE) {
			p = DANDELION_TOUCH_MAX_PRESSURE;
		}
		if (p == 0) {
			p = 1;
		}

		out->point[out->count].id = id;
		out->point[out->count].status = status;
		out->point[out->count].x = (uint16_t)x;
		out->point[out->count].y = (uint16_t)y;
		out->point[out->count].weight = (uint8_t)w;
		out->point[out->count].pressure = (uint16_t)p;
		out->count++;
	}

	return out->count > 0 ? DANDELION_TOUCH_OK : DANDELION_TOUCH_ERR_EMPTY;
}

/* ------------------------------------------------------------------ */
/* Whole ladder step T3 in one call                                    */
/* ------------------------------------------------------------------ */

/*
 * Bring the controller from "unknown state" to "one report decoded":
 * reset, identify, read the firmware block, wait for the firmware to reach its
 * normal-run state, then read and decode one report.
 *
 * TODO(T3): the report is only valid if the controller has been told to scan.
 * The vendor driver leaves that to the firmware's own default after the reset
 * state reaches NORMAL_RUN; if the first report comes back empty on hardware,
 * the next thing to try is the host command at EVENT_MAP_HOST_CMD (0x50), which
 * UniXNU's NT36532E driver uses as {0xbf, cmd, 0, lo, hi} to start scanning.
 * That command layout is for the NT36532 and has NOT been confirmed for the
 * NT36525B -- do not copy it without reading 0x50 back first.
 */
int
dandelion_touch_probe_and_read(const struct dandelion_nvt_bus *bus,
    struct dandelion_touch_report *report)
{
	uint8_t raw[NVT_POINT_DATA_LEN + 1U];
	struct dandelion_nvt_info info;
	int rc;

	if (!bus || !report) {
		return DANDELION_TOUCH_ERR_ARG;
	}

	rc = dandelion_nvt_identify(bus);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	rc = dandelion_nvt_read_info(bus, &info);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	rc = dandelion_nvt_wait_reset_state(bus, NVT_RESET_STATE_NORMAL_RUN,
	    50);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	rc = dandelion_nvt_read_report(bus, raw);
	if (rc != DANDELION_TOUCH_OK) {
		return rc;
	}

	return dandelion_nvt_decode_report(raw, report);
}

const char *
dandelion_touch_status_name(int status)
{
	switch (status) {
	case DANDELION_TOUCH_OK:
		return "ok";
	case DANDELION_TOUCH_ERR_ARG:
		return "bad argument";
	case DANDELION_TOUCH_ERR_BUS:
		return "SPI transfer failed or firmware block inconsistent";
	case DANDELION_TOUCH_ERR_NOT_ID:
		return "not a Novatek NT36525B (trim id mismatch)";
	case DANDELION_TOUCH_ERR_TIMEOUT:
		return "firmware did not reach normal run";
	case DANDELION_TOUCH_ERR_EMPTY:
		return "no finger in the report";
	default:
		return "unknown";
	}
}
