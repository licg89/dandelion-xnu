/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the dandelion-xnu authors
 *
 * Dandelion (Xiaomi Redmi 9A, MT6765/MT6762G) Novatek NT36525B touch
 * groundwork.
 *
 * UNTESTED.  Nothing here has run on the phone.  The point is to have the bus,
 * the reset sequence and the one register report written down in code that can
 * be reviewed before it is wired into a kernel.
 *
 * Provenance (full citations in docs/TOUCH-PLAN.md):
 *   - Bus, GPIOs, address:  device DTS touchscreen.dtsi (vendor kernel
 *                           dandelion-q-oss @8974b91) and mt6765.dts (spi3)
 *   - Protocol and reset:   Xiaomi vendor kernel, drivers/input/touchscreen/
 *                           mediatek/nt36xxx_spi/ (nt36xxx.c, nt36xxx.h,
 *                           nt36xxx_mem_map.h), GPL-2.0, no code copied --
 *                           the constants and the sequences are facts about the
 *                           chip, the comments below say which is which.
 *   - Report layout:        same driver, nvt_ts_work_func()
 *   - Structural reference: UniXNU/ShengNovatekTouch (Apache-2.0), which is the
 *                           NT36532E, a different chip in the same family: its
 *                           memory map and its raw-matrix decoding do NOT apply
 *                           here, its layering (protocol / transport / core)
 *                           does.
 */
#ifndef _DANDELION_TOUCH_H_
#define _DANDELION_TOUCH_H_

#include <stdint.h>

/*
 * The touch controller is a Novatek NT36525B: the same die that drives the
 * panel (TDDI), reached over SPI3 -- NOT over I2C, which is what
 * docs/UEFI-DANDELION-PLAN.md used to say.  See docs/TOUCH-PLAN.md section 0.
 */
#define DANDELION_SPI3_BASE        0x11013000ULL
#define DANDELION_TOUCH_GPIO_RST   174U   /* novatek,reset-gpio  */
#define DANDELION_TOUCH_GPIO_IRQ   0U     /* novatek,irq-gpio    */
#define DANDELION_TOUCH_EINT       13U    /* interrupts = <13 2>, falling */
#define DANDELION_TOUCH_SPI_HZ     9600000U
#define DANDELION_TOUCH_MAX_FINGERS 10U
#define DANDELION_TOUCH_MAX_X      720U
#define DANDELION_TOUCH_MAX_Y      1600U
#define DANDELION_TOUCH_MAX_PRESSURE 1000U

/* Novatek register addresses (from the device tree, not from the driver). */
#define NVT_ADDR_SWRST_N8     0x03f0feU   /* novatek,swrst-n8-addr   */
#define NVT_ADDR_SPI_RD_FAST  0x03f310U   /* novatek,spi-rd-fast-addr */
#define NVT_ADDR_ENG_RST      0x7fff80U   /* nt36xxx.c: ENG_RST_ADDR  */
#define NVT_ADDR_TRIM_ID      0x1f64eU    /* nt36xxx.c: chip id read  */

/* Event buffer for the NT36525/NT36525B map (nt36xxx_mem_map.h). */
#define NVT_EVENT_BUF_ADDR    0x11a00U
#define NVT_EVENT_MAP_HOST_CMD        0x50U
#define NVT_EVENT_MAP_HANDSHAKING     0x51U
#define NVT_EVENT_MAP_RESET_COMPLETE  0x60U
#define NVT_EVENT_MAP_FWINFO          0x78U
#define NVT_EVENT_MAP_PROJECTID       0x9aU

/* One report is 65 bytes plus the index byte; one dummy byte on the wire. */
#define NVT_POINT_DATA_LEN    65U
#define NVT_DUMMY_BYTES       1U
#define NVT_FINGER_STRIDE     6U

/* Reset-state machine values (RST_COMPLETE_STATE). */
#define NVT_RESET_STATE_INIT        0xa0U
#define NVT_RESET_STATE_REK         0xa1U
#define NVT_RESET_STATE_REK_FINISH  0xa2U
#define NVT_RESET_STATE_NORMAL_RUN  0xa3U
#define NVT_RESET_STATE_MAX         0xafU

/*
 * Transport.  The caller supplies SPI and GPIO access because the same code has
 * to run in a shim, in XNU, and in a host test harness with a fake chip.
 */
struct dandelion_nvt_bus {
	void *ctx;
	/*
	 * Full-duplex SPI mode 0, 8 bits per word, chip select held for the whole
	 * transfer.  rx may be NULL for a write.  Returns 0 on success.
	 */
	int  (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len);
	void (*delay_ms)(void *ctx, unsigned ms);
	/* Drive the reset line; level 0 = asserted (reset held). */
	void (*reset_pin)(void *ctx, unsigned level);
	/* Read the interrupt line, for the polled path.  1 = asserted. */
	int  (*irq_level)(void *ctx);
};

/* One decoded finger. */
struct dandelion_touch_point {
	uint8_t id;        /* 1..10 as reported by the chip */
	uint8_t status;    /* 1 = enter/moving, 2 = moving (see decode) */
	uint16_t x;
	uint16_t y;
	uint8_t weight;    /* contact area, 1..255 */
	uint16_t pressure; /* 1..1000 */
};

struct dandelion_touch_report {
	unsigned count;
	struct dandelion_touch_point point[DANDELION_TOUCH_MAX_FINGERS];
};

struct dandelion_nvt_info {
	uint8_t fw_ver;
	uint8_t x_num;
	uint8_t y_num;
	uint16_t abs_x_max;
	uint16_t abs_y_max;
	uint8_t pid_high;
	uint8_t pid_low;
};

enum dandelion_touch_status {
	DANDELION_TOUCH_OK = 0,
	DANDELION_TOUCH_ERR_ARG = -1,
	DANDELION_TOUCH_ERR_BUS = -2,
	DANDELION_TOUCH_ERR_NOT_ID = -3,
	DANDELION_TOUCH_ERR_TIMEOUT = -4,
	DANDELION_TOUCH_ERR_EMPTY = -5
};

/* Raw register access: page select, then one byte at (addr & 0x7f). */
int dandelion_nvt_set_page(const struct dandelion_nvt_bus *bus, uint32_t addr);
int dandelion_nvt_write8(const struct dandelion_nvt_bus *bus, uint32_t addr,
    uint8_t value);
int dandelion_nvt_read(const struct dandelion_nvt_bus *bus, uint32_t addr,
    uint8_t *out, uint32_t len);

/* Reset sequences, in the order the vendor driver uses them. */
int dandelion_nvt_hw_reset(const struct dandelion_nvt_bus *bus);
int dandelion_nvt_eng_reset(const struct dandelion_nvt_bus *bus);
int dandelion_nvt_bootloader_reset(const struct dandelion_nvt_bus *bus);

/* Probe: reset, read the trim ID, read the firmware info block. */
int dandelion_nvt_identify(const struct dandelion_nvt_bus *bus);
int dandelion_nvt_read_info(const struct dandelion_nvt_bus *bus,
    struct dandelion_nvt_info *info);
int dandelion_nvt_wait_reset_state(const struct dandelion_nvt_bus *bus,
    uint8_t want, unsigned polls);

/* One interrupt's worth of data: read 66 bytes, decode up to 10 fingers. */
int dandelion_nvt_read_report(const struct dandelion_nvt_bus *bus,
    uint8_t raw[NVT_POINT_DATA_LEN + 1U]);
int dandelion_nvt_decode_report(const uint8_t *raw,
    struct dandelion_touch_report *out);

/* Probe-and-read-once, the whole of ladder step T3 in one call. */
int dandelion_touch_probe_and_read(const struct dandelion_nvt_bus *bus,
    struct dandelion_touch_report *report);

const char *dandelion_touch_status_name(int status);

#endif /* _DANDELION_TOUCH_H_ */
