/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the dandelion-xnu authors
 *
 * Dandelion (Xiaomi Redmi 9A, MT6765/MT6762G) DSI0 display groundwork.
 *
 * UNTESTED.  Nothing in this file has run on the phone.  It exists so that the
 * register map, the panel timing arithmetic and the order of the bring-up steps
 * are written down in one place and can be reviewed before anybody flashes
 * anything.  See docs/DISPLAY-PLAN.md for the ladder and for the provenance of
 * every number below.
 *
 * Provenance summary (full citations in docs/DISPLAY-PLAN.md):
 *   - Base addresses:      device DTS mt6765.dts (dsi0@14014000, mipi_tx0@11c80000)
 *                          and docs/HARDWARE_ANALYSIS.md
 *   - Register offsets:    MediaTek LK, platform/mt6765/include/platform/
 *                          ddp_reg_dsi.h and ddp_reg_mipi.h
 *   - Timing/PHY formulas: MediaTek LK, platform/mt6765/ddp_dsi.c
 *                          (DSI_Config_VDO_Timing, DSI_PHY_clk_setting,
 *                          DSI_PHY_TIMCONFIG)
 *   - Panel parameters:    Xiaomi vendor kernel, dandelion-q-oss @8974b91,
 *                          nt36525b_vdo_hdp_boe_helitai.c (lcm_get_params)
 *   - Framebuffer window:  bootshim/fb.h and docs/NEXT-M3-GOP.md
 */
#ifndef _DANDELION_DISPLAY_H_
#define _DANDELION_DISPLAY_H_

#include <stdint.h>

/*
 * Physical addresses.  All of these are inside the "dispsys" power domain; on
 * this device LK has already powered it on and left DSI0 running in video mode
 * by the time the kernel starts (docs/NEXT-M3-GOP.md, "photo evidence").
 */
#define DANDELION_DISP_MMSYS_BASE   0x14000000ULL  /* mmsys_config           */
#define DANDELION_DISP_MUTEX0_BASE  0x14001000ULL
#define DANDELION_DISP_OVL0_BASE    0x1400b000ULL
#define DANDELION_DISP_RDMA0_BASE   0x1400d000ULL
#define DANDELION_DISP_COLOR0_BASE  0x1400f000ULL
#define DANDELION_DISP_DITHER0_BASE 0x14013000ULL
#define DANDELION_DSI0_BASE         0x14014000ULL  /* 4 KiB window           */
#define DANDELION_MIPITX0_BASE      0x11c80000ULL  /* DSI PHY / PLL          */
#define DANDELION_DISP_PWM_BASE     0x1100e000ULL  /* backlight PWM          */

/* The panel scans out of this window; the console already writes here. */
#define DANDELION_FB_PA             0x7ec50000ULL
#define DANDELION_FB_SIZE           0x00fb0000ULL  /* reserved-memory size   */
#define DANDELION_FB_STRIDE         2944U          /* 736 pixels * 4 bytes   */
#define DANDELION_FB_SCANOUT_W      736U
#define DANDELION_FB_VISIBLE_W      720U
#define DANDELION_FB_VISIBLE_H      1600U
#define DANDELION_FB_BYTES_PER_PX   4U

/*
 * Display-related GPIOs, all from the vendor kernel's
 * custom_common.dtsi &pio / &mtkfb pinctrl states.  MTK GPIO numbers, not Linux
 * global numbers: the pin mux registers live at 0x10005000.
 */
#define DANDELION_GPIO_LCD_BIAS_ENP   168U  /* TPS65132 VPOS enable  */
#define DANDELION_GPIO_LCD_BIAS_ENN   173U  /* TPS65132 VNEG enable  */
#define DANDELION_GPIO_LCD_BL_EN      154U  /* backlight enable      */
#define DANDELION_GPIO_LCD_LDO18_EN   178U  /* 1.8 V panel rail      */
#define DANDELION_GPIO_LCM_RST         45U  /* panel reset, active low */
#define DANDELION_GPIO_TP_RST         174U  /* touch reset (see TOUCH-PLAN) */

/* TPS65132 LCD bias regulator: i2c6 @ 0x1100d000, slave address 0x3e. */
#define DANDELION_I2C6_BASE          0x1100d000ULL
#define DANDELION_TPS65132_ADDR      0x3eU

/* ---- DSI0 register offsets (ddp_reg_dsi.h, base 0x14014000) ------------- */
#define DSI_START             0x000U
#define DSI_STATUS            0x004U
#define DSI_INTEN             0x008U
#define DSI_INTSTA            0x00cU
#define DSI_COM_CON           0x010U
#define DSI_MODE_CON          0x014U
#define DSI_TXRX_CON          0x018U
#define DSI_PSCON             0x01cU
#define DSI_VSA_NL            0x020U
#define DSI_VBP_NL            0x024U
#define DSI_VFP_NL            0x028U
#define DSI_VACT_NL           0x02cU
#define DSI_SIZE_CON          0x038U
#define DSI_HSA_WC            0x050U
#define DSI_HBP_WC            0x054U
#define DSI_HFP_WC            0x058U
#define DSI_BLLP_WC           0x05cU
#define DSI_CMDQ_CON          0x060U
#define DSI_RX_DATA03         0x074U
#define DSI_RX_DATA47         0x078U
#define DSI_RX_RACK           0x084U
#define DSI_PHY_LCPAT         0x100U
#define DSI_PHY_LCCON         0x104U
#define DSI_PHY_LD0CON       0x108U
#define DSI_PHY_TIMCON0       0x110U
#define DSI_PHY_TIMCON1       0x114U
#define DSI_PHY_TIMCON2       0x118U
#define DSI_PHY_TIMCON3       0x11cU
#define DSI_VM_CMD_CON        0x130U
#define DSI_CMDQ              0x200U

/* DSI_START bit 0 starts video mode; bit 16 starts a VM command transfer. */
#define DSI_START_DSI_START       (1U << 0)
#define DSI_START_VM_CMD_START    (1U << 16)

/* DSI_INTSTA bit 31 is "controller busy"; the rest are event flags. */
#define DSI_INTSTA_DSI_BUSY       (1U << 31)
#define DSI_INTSTA_BUFFER_UNDERRUN (1U << 12)

/* ---- MIPI TX (DSI PHY) register offsets (base 0x11c80000) --------------- */
#define MIPITX_LANE_CON       0x00cU
#define MIPITX_PLL_PWR        0x028U
#define MIPITX_PLL_CON0       0x02cU
#define MIPITX_PLL_CON1       0x030U
#define MIPITX_PLL_CON2       0x034U
#define MIPITX_PLL_CON3       0x038U
#define MIPITX_PLL_CON4       0x03cU
#define MIPITX_PHY_SEL0       0x040U

/* Values written by LK's DSI_PHY_clk_setting(); kept verbatim, not guessed. */
#define MIPITX_PLL_CON4_INIT  0x00ff12e0U
#define MIPITX_LANE_CON_A     0x3fff0180U   /* BG_LPF_EN=0, BG_CORE_EN=1 */
#define MIPITX_LANE_CON_B     0x3fff0080U   /* BG_LPF_EN=1               */

/*
 * Panel parameters.  Defaults are the NT36525B in the vendor kernel's
 * nt36525b_vdo_hdp_boe_helitai.c lcm_get_params().  dandelion ships at least
 * three NT36525B module variants (dijing / helitai / panda-shengchao) plus a
 * FT8006S and an HX83102D; see docs/DISPLAY-PLAN.md section 2.  Which one is
 * fitted has to be read from the panel (DCS 0x04) before these are trusted.
 */
struct dandelion_panel {
	uint16_t width;              /* visible pixels              720  */
	uint16_t height;             /* visible lines              1600  */
	uint16_t hsa;                /* horizontal sync active        4  */
	uint16_t hbp;                /* horizontal back porch        85  */
	uint16_t hfp;                /* horizontal front porch       85  */
	uint16_t vsa;                /* vertical sync active          2  */
	uint16_t vbp;                /* vertical back porch           8  */
	uint16_t vfp;                /* vertical front porch         10  */
	uint16_t bllp;               /* blanking-low-power lines      0  */
	uint8_t  lanes;              /* 4                              */
	uint8_t  bpp;                /* 3 = RGB888                     */
	uint8_t  ssc_disable;        /* 1 (no spread spectrum)         */
	uint32_t pll_clock_mhz;      /* 277; data rate = 2x = 554 Mbps */
	uint16_t physical_width_um;  /* 68040                          */
	uint16_t physical_height_um; /* 151200                         */
};

/*
 * Accessors.  The caller supplies them because the same code has to run in
 * three very different places: a bare-metal shim with the MMU off, XNU after
 * ml_io_map(), and a host test harness.
 */
struct dandelion_dsi_bus {
	void *ctx;
	void     (*write32)(void *ctx, uint64_t phys, uint32_t value);
	uint32_t (*read32)(void *ctx, uint64_t phys);
	void     (*delay_ms)(void *ctx, unsigned ms);
	/* DCS command over the DSI command queue; returns 0 on success. */
	int      (*dcs_write)(void *ctx, uint8_t cmd, const uint8_t *para, uint8_t count);
	/* DCS read; returns the number of bytes read, or a negative error. */
	int      (*dcs_read)(void *ctx, uint8_t cmd, uint8_t *out, uint8_t len);
};

/* What a read-back of the live DSI0 state looks like (ladder step D0). */
struct dandelion_dsi_snapshot {
	uint32_t start;
	uint32_t status;
	uint32_t intsta;
	uint32_t com_con;
	uint32_t mode_con;
	uint32_t txrx_con;
	uint32_t pscon;
	uint32_t vsa_nl;
	uint32_t vbp_nl;
	uint32_t vfp_nl;
	uint32_t vact_nl;
	uint32_t size_con;
	uint32_t hsa_wc;
	uint32_t hbp_wc;
	uint32_t hfp_wc;
	uint32_t bllp_wc;
	uint32_t phy_lccon;
	uint32_t phy_ld0con;
	uint32_t timcon0;
	uint32_t timcon1;
	uint32_t timcon2;
	uint32_t timcon3;
	uint32_t mipitx_lane_con;
	uint32_t mipitx_pll_con0;
	uint32_t mipitx_pll_con1;
};

/* Result codes.  Every step of the ladder returns one of these. */
enum dandelion_display_status {
	DANDELION_DISP_OK = 0,
	DANDELION_DISP_ERR_ARG = -1,
	DANDELION_DISP_ERR_BUS = -2,      /* a read or write did not stick  */
	DANDELION_DISP_ERR_TIMEOUT = -3,  /* DSI stayed busy / no RX data   */
	DANDELION_DISP_ERR_NOT_ID = -4,   /* panel ID did not match         */
	DANDELION_DISP_ERR_UNIMPLEMENTED = -5
};

void dandelion_panel_defaults(struct dandelion_panel *p);

/* D0: read the whole DSI0 + MIPITX state into *out. */
int dandelion_dsi_snapshot(const struct dandelion_dsi_bus *bus,
                           struct dandelion_dsi_snapshot *out);

/*
 * Compare a snapshot against the values this panel needs.  Returns the number
 * of fields that differ and stores the first mismatching offset in *first_bad
 * (0 when everything matches).
 */
int dandelion_dsi_snapshot_check(const struct dandelion_panel *p,
                                 const struct dandelion_dsi_snapshot *snap,
                                 uint32_t *first_bad, uint32_t *expected,
                                 uint32_t *actual);

/* Pure arithmetic, no I/O: the four byte-count registers of video mode. */
void dandelion_dsi_horizontal_wc(const struct dandelion_panel *p,
                                 uint32_t *hsa_wc, uint32_t *hbp_wc,
                                 uint32_t *hfp_wc, uint32_t *bllp_wc);

/* Pure arithmetic, no I/O: DSI_PHY_TIMCON0..3 as LK computes them. */
void dandelion_dsi_phy_timcon(const struct dandelion_panel *p,
                              uint32_t *timcon0, uint32_t *timcon1,
                              uint32_t *timcon2, uint32_t *timcon3);

/* Pure arithmetic, no I/O: MIPITX PLL dividers for the panel's data rate. */
void dandelion_dsi_pll_div(const struct dandelion_panel *p,
                           uint32_t *pll_con0, uint32_t *pcw_ratio,
                           uint32_t *posdiv);

/* D1: fill the whole scanout buffer with one colour, stride-aware. */
void dandelion_fb_fill(uint32_t rgb);

/* D2/D3: program timing + PHY and (re)start video mode. */
int dandelion_dsi_program_timing(const struct dandelion_dsi_bus *bus,
                                 const struct dandelion_panel *p);
int dandelion_dsi_program_phy(const struct dandelion_dsi_bus *bus,
                              const struct dandelion_panel *p);
int dandelion_dsi_start(const struct dandelion_dsi_bus *bus);

/* D3: the panel's own DCS init sequence. */
int dandelion_panel_init(const struct dandelion_dsi_bus *bus);
int dandelion_panel_read_id(const struct dandelion_dsi_bus *bus,
                            uint8_t id[3]);
int dandelion_panel_set_backlight(const struct dandelion_dsi_bus *bus,
                                  uint8_t level);

/* One-line human-readable name for a status code, for kernel logs. */
const char *dandelion_display_status_name(int status);

#endif /* _DANDELION_DISPLAY_H_ */
