/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the dandelion-xnu authors
 *
 * Dandelion DSI0 display groundwork.  UNTESTED: no line of this has executed on
 * the phone.  It is deliberately split so that the parts that are pure
 * arithmetic (timing, byte counts, PLL dividers) can be checked against the
 * values in docs/DISPLAY-PLAN.md by reading the file, and the parts that touch
 * hardware are one small function each.
 *
 * Everything here is derived from three sources, cited per function:
 *   [LK]  MediaTek LK, platform/mt6765/ (ddp_dsi.c, include/platform/
 *         ddp_reg_dsi.h, ddp_reg_mipi.h, include/platform/mt_reg_base.h)
 *   [VK]  Xiaomi vendor kernel dandelion-q-oss @8974b91, nt36525b_*.c
 *         (panel parameters and DCS init sequence)
 *   [HW]  numbers measured on this phone, as recorded in docs/
 */
#include "dandelion_display.h"

/* ------------------------------------------------------------------ */
/* Panel defaults (see the header for provenance)                      */
/* ------------------------------------------------------------------ */

void
dandelion_panel_defaults(struct dandelion_panel *p)
{
	if (!p) {
		return;
	}
	p->width = 720;
	p->height = 1600;
	p->hsa = 4;
	p->hbp = 85;
	p->hfp = 85;
	p->vsa = 2;
	p->vbp = 8;
	p->vfp = 10;
	p->bllp = 0;
	p->lanes = 4;
	p->bpp = 3;             /* RGB888 */
	p->ssc_disable = 1;
	p->pll_clock_mhz = 277;
	p->physical_width_um = 68040;
	p->physical_height_um = 151200;
}

/* ------------------------------------------------------------------ */
/* Pure arithmetic                                                     */
/* ------------------------------------------------------------------ */

/*
 * [LK] DSI_Config_VDO_Timing().  The byte-count registers are not the pixel
 * counts: each is (pixels * bytes-per-pixel) minus a fixed packet overhead, then
 * rounded up to a multiple of four.  LK has two branches; this panel is
 * SYNC_PULSE_VDO_MODE (not SYNC_EVENT and not BURST), which is the second one,
 * where the sync and porch overheads are subtracted separately.
 *
 * For the defaults this yields 4 / 248 / 244 / 0 -- see the table in
 * docs/DISPLAY-PLAN.md section 3.
 */
void
dandelion_dsi_horizontal_wc(const struct dandelion_panel *p,
    uint32_t *hsa_wc, uint32_t *hbp_wc, uint32_t *hfp_wc, uint32_t *bllp_wc)
{
	uint32_t bpp;
	int32_t hsa, hbp, hfp, bllp;

	bpp = p->bpp ? p->bpp : 3;

	hsa = (int32_t)(p->hsa * bpp) - 10;
	hbp = (int32_t)(p->hbp * bpp) - 10;
	hfp = (int32_t)(p->hfp * bpp) - 12;
	bllp = (int32_t)(p->bllp * bpp);

	/* LK asserts these stay positive; a negative value would mean the panel
	 * timing is not what this code was written for. */
	if (hsa < 0) {
		hsa = 0;
	}
	if (hbp < 0) {
		hbp = 0;
	}
	if (hfp < 0) {
		hfp = 0;
	}
	if (bllp < 0) {
		bllp = 0;
	}

	if (hsa_wc) {
		*hsa_wc = ((uint32_t)hsa + 3U) & ~3U;
	}
	if (hbp_wc) {
		*hbp_wc = ((uint32_t)hbp + 3U) & ~3U;
	}
	if (hfp_wc) {
		*hfp_wc = ((uint32_t)hfp + 3U) & ~3U;
	}
	if (bllp_wc) {
		*bllp_wc = ((uint32_t)bllp + 3U) & ~3U;
	}
}

/*
 * [LK] DSI_PHY_TIMCONFIG().  ui is one DSI unit interval in ns, cycle_time is
 * one byte-clock period in ns; both are integer-divided exactly as LK does it
 * (LK's "+ 0x01" is part of the formula, not a rounding convenience).
 *
 * Field positions are from ddp_reg_dsi.h:
 *   TIMCON0: DA_HS_TRAIL 31:24, DA_HS_ZERO 23:16, DA_HS_PREP 15:8, LPX 7:0
 *   TIMCON1: DA_HS_EXIT 31:24, TA_GET 23:16, TA_SURE 15:8, TA_GO 7:0
 *   TIMCON2: CLK_HS_TRAIL 31:24, CLK_HS_ZERO 23:16, DA_HS_SYNC 15:8,
 *            CONT_DET 7:0
 *   TIMCON3: CLK_HS_EXIT 23:16, CLK_HS_POST 15:8, CLK_HS_PREP 7:0
 */
void
dandelion_dsi_phy_timcon(const struct dandelion_panel *p,
    uint32_t *timcon0, uint32_t *timcon1, uint32_t *timcon2,
    uint32_t *timcon3)
{
	uint32_t data_rate, ui, cycle_time;
	uint32_t hs_trail, hs_prpr, hs_zero, lpx;
	uint32_t ta_get, ta_sure, ta_go, da_hs_exit;
	uint32_t clk_trail, clk_zero, clk_hs_prpr, clk_hs_exit, clk_hs_post;

	data_rate = p->pll_clock_mhz * 2U;      /* DDR: PLL_CLOCK is per-lane MHz */
	if (data_rate == 0) {
		data_rate = 554;
	}

	ui = 1000U / data_rate + 1U;
	cycle_time = 8000U / data_rate + 1U;

	hs_trail = (4U * ui + 0x50U) / cycle_time;
	if (hs_trail < 1U) {
		hs_trail = 1U;
	}

	hs_prpr = (0x40U + 5U * ui) / cycle_time;
	if (hs_prpr < 1U) {
		hs_prpr = 1U;
	}

	hs_zero = (0xc8U + 0x0aU * ui) / cycle_time;
	if (hs_zero > hs_prpr) {
		hs_zero -= hs_prpr;
	}

	lpx = 0x50U / cycle_time;
	if (lpx < 1U) {
		lpx = 1U;
	}

	ta_get = 5U * lpx;
	ta_sure = 3U * lpx / 2U;
	ta_go = 4U * lpx;
	da_hs_exit = 2U * lpx;

	clk_trail = 0x60U / cycle_time + 1U;
	if (clk_trail < 2U) {
		clk_trail = 2U;
	}
	clk_zero = 0x190U / cycle_time;
	clk_hs_prpr = 0x40U / cycle_time;
	if (clk_hs_prpr < 1U) {
		clk_hs_prpr = 1U;
	}
	clk_hs_exit = 2U * lpx;
	clk_hs_post = (0x60U + 0x34U * ui) / cycle_time;

	if (timcon0) {
		*timcon0 = ((hs_trail & 0xffU) << 24) | ((hs_zero & 0xffU) << 16) |
		    ((hs_prpr & 0xffU) << 8) | (lpx & 0xffU);
	}
	if (timcon1) {
		*timcon1 = ((da_hs_exit & 0xffU) << 24) |
		    ((ta_get & 0xffU) << 16) | ((ta_sure & 0xffU) << 8) |
		    (ta_go & 0xffU);
	}
	if (timcon2) {
		/* DA_HS_SYNC and CONT_DET stay zero, as in LK for this panel. */
		*timcon2 = ((clk_trail & 0xffU) << 24) |
		    ((clk_zero & 0xffU) << 16);
	}
	if (timcon3) {
		*timcon3 = ((clk_hs_exit & 0xffU) << 16) |
		    ((clk_hs_post & 0xffU) << 8) | (clk_hs_prpr & 0xffU);
	}
}

/*
 * [LK] DSI_PHY_clk_setting().  The MIPI TX PLL is a fractional-N PLL clocked
 * from 26 MHz; pcw_ratio and posdiv are chosen from the per-lane data rate
 * band.  554 Mbps lands in the 500..1000 band: pcw_ratio = 4, posdiv = 2.
 *
 * The register value is the integer part in bits 24..30 and three fractional
 * bytes below it.  For 554 Mbps this works out to 0x553b13b1 -- but that is a
 * derived number, so the ladder's step D0 reads MIPITX_PLL_CON0 back from the
 * running device and compares before anything is written.
 */
void
dandelion_dsi_pll_div(const struct dandelion_panel *p,
    uint32_t *pll_con0, uint32_t *pcw_ratio_out, uint32_t *posdiv_out)
{
	uint32_t data_rate, pcw_ratio, posdiv, rem, f1, f2, f3, integer;
	uint32_t scaled;

	data_rate = p->pll_clock_mhz * 2U;
	if (data_rate == 0) {
		data_rate = 554;
	}

	if (data_rate >= 2000U) {
		pcw_ratio = 1;
		posdiv = 0;
	} else if (data_rate >= 1000U) {
		pcw_ratio = 2;
		posdiv = 1;
	} else if (data_rate >= 500U) {
		pcw_ratio = 4;
		posdiv = 2;
	} else if (data_rate > 250U) {
		pcw_ratio = 8;
		posdiv = 3;
	} else {
		pcw_ratio = 16;
		posdiv = 4;
	}

	scaled = data_rate * pcw_ratio;
	integer = scaled / 26U;
	rem = scaled % 26U;

	f1 = (256U * rem) / 26U;
	rem = (256U * rem) % 26U;
	f2 = (256U * rem) / 26U;
	rem = (256U * rem) % 26U;
	f3 = (256U * rem) / 26U;

	if (pll_con0) {
		*pll_con0 = ((integer & 0xffU) << 24) | ((f1 & 0xffU) << 16) |
		    ((f2 & 0xffU) << 8) | (f3 & 0xffU);
	}
	if (pcw_ratio_out) {
		*pcw_ratio_out = pcw_ratio;
	}
	if (posdiv_out) {
		*posdiv_out = posdiv;
	}
}

/* ------------------------------------------------------------------ */
/* Read-back and comparison (ladder step D0)                           */
/* ------------------------------------------------------------------ */

int
dandelion_dsi_snapshot(const struct dandelion_dsi_bus *bus,
    struct dandelion_dsi_snapshot *out)
{
	uint64_t dsi, phy;

	if (!bus || !out || !bus->read32) {
		return DANDELION_DISP_ERR_ARG;
	}
	dsi = DANDELION_DSI0_BASE;
	phy = DANDELION_MIPITX0_BASE;

	out->start = bus->read32(bus->ctx, dsi + DSI_START);
	out->status = bus->read32(bus->ctx, dsi + DSI_STATUS);
	out->intsta = bus->read32(bus->ctx, dsi + DSI_INTSTA);
	out->com_con = bus->read32(bus->ctx, dsi + DSI_COM_CON);
	out->mode_con = bus->read32(bus->ctx, dsi + DSI_MODE_CON);
	out->txrx_con = bus->read32(bus->ctx, dsi + DSI_TXRX_CON);
	out->pscon = bus->read32(bus->ctx, dsi + DSI_PSCON);
	out->vsa_nl = bus->read32(bus->ctx, dsi + DSI_VSA_NL);
	out->vbp_nl = bus->read32(bus->ctx, dsi + DSI_VBP_NL);
	out->vfp_nl = bus->read32(bus->ctx, dsi + DSI_VFP_NL);
	out->vact_nl = bus->read32(bus->ctx, dsi + DSI_VACT_NL);
	out->size_con = bus->read32(bus->ctx, dsi + DSI_SIZE_CON);
	out->hsa_wc = bus->read32(bus->ctx, dsi + DSI_HSA_WC);
	out->hbp_wc = bus->read32(bus->ctx, dsi + DSI_HBP_WC);
	out->hfp_wc = bus->read32(bus->ctx, dsi + DSI_HFP_WC);
	out->bllp_wc = bus->read32(bus->ctx, dsi + DSI_BLLP_WC);
	out->phy_lccon = bus->read32(bus->ctx, dsi + DSI_PHY_LCCON);
	out->phy_ld0con = bus->read32(bus->ctx, dsi + DSI_PHY_LD0CON);
	out->timcon0 = bus->read32(bus->ctx, dsi + DSI_PHY_TIMCON0);
	out->timcon1 = bus->read32(bus->ctx, dsi + DSI_PHY_TIMCON1);
	out->timcon2 = bus->read32(bus->ctx, dsi + DSI_PHY_TIMCON2);
	out->timcon3 = bus->read32(bus->ctx, dsi + DSI_PHY_TIMCON3);
	out->mipitx_lane_con = bus->read32(bus->ctx, phy + MIPITX_LANE_CON);
	out->mipitx_pll_con0 = bus->read32(bus->ctx, phy + MIPITX_PLL_CON0);
	out->mipitx_pll_con1 = bus->read32(bus->ctx, phy + MIPITX_PLL_CON1);

	return DANDELION_DISP_OK;
}

/*
 * Compare what the hardware reports against what this panel needs.
 *
 * Only the fields that are unambiguous are compared: the video-mode line counts,
 * the four byte counts, and the four PHY timing registers.  MODE_CON, TXRX_CON
 * and PSCON are deliberately NOT compared field by field, because the encoding
 * of some of their fields (pixel format, RGB swap) is not settled from the
 * sources we have; they are dumped instead and a human reads them.
 */
int
dandelion_dsi_snapshot_check(const struct dandelion_panel *p,
    const struct dandelion_dsi_snapshot *snap,
    uint32_t *first_bad, uint32_t *expected, uint32_t *actual)
{
	uint32_t hsa_wc, hbp_wc, hfp_wc, bllp_wc;
	uint32_t t0, t1, t2, t3;
	int bad = 0;

	struct {
		uint32_t reg;
		uint32_t want;
		uint32_t got;
	} check[8];

	if (!p || !snap) {
		return DANDELION_DISP_ERR_ARG;
	}

	dandelion_dsi_horizontal_wc(p, &hsa_wc, &hbp_wc, &hfp_wc, &bllp_wc);
	dandelion_dsi_phy_timcon(p, &t0, &t1, &t2, &t3);

	check[0].reg = DSI_VSA_NL;   check[0].want = p->vsa;    check[0].got = snap->vsa_nl;
	check[1].reg = DSI_VBP_NL;   check[1].want = p->vbp;    check[1].got = snap->vbp_nl;
	check[2].reg = DSI_VFP_NL;   check[2].want = p->vfp;    check[2].got = snap->vfp_nl;
	check[3].reg = DSI_VACT_NL;  check[3].want = p->height; check[3].got = snap->vact_nl;
	check[4].reg = DSI_HSA_WC;   check[4].want = hsa_wc;    check[4].got = snap->hsa_wc;
	check[5].reg = DSI_HBP_WC;   check[5].want = hbp_wc;    check[5].got = snap->hbp_wc;
	check[6].reg = DSI_HFP_WC;   check[6].want = hfp_wc;    check[6].got = snap->hfp_wc;
	check[7].reg = DSI_PHY_TIMCON0; check[7].want = t0;     check[7].got = snap->timcon0;

	for (unsigned i = 0; i < sizeof(check) / sizeof(check[0]); i++) {
		if (check[i].want != check[i].got) {
			if (bad == 0) {
				if (first_bad) {
					*first_bad = check[i].reg;
				}
				if (expected) {
					*expected = check[i].want;
				}
				if (actual) {
					*actual = check[i].got;
				}
			}
			bad++;
		}
	}

	/* TIMCON1..3 are reported the same way, but only counted. */
	if (snap->timcon1 != t1) {
		bad++;
	}
	if (snap->timcon2 != t2) {
		bad++;
	}
	if (snap->timcon3 != t3) {
		bad++;
	}

	return bad;
}

/* ------------------------------------------------------------------ */
/* Framebuffer (ladder step D1)                                        */
/* ------------------------------------------------------------------ */

/*
 * [HW] The panel scans out 736 pixels per line while 720 are visible, so the
 * destination pointer must advance by DANDELION_FB_STRIDE, not by width * 4.
 * Writing with a 720-pixel pitch is exactly the bug that made the UEFI console
 * sheared (docs/NEXT-M3-GOP.md, "photo evidence").
 */
void
dandelion_fb_fill(uint32_t rgb)
{
	volatile uint32_t *row;
	uint32_t y, x;

	for (y = 0; y < DANDELION_FB_VISIBLE_H; y++) {
		row = (volatile uint32_t *)(uintptr_t)
		    (DANDELION_FB_PA + (uint64_t)y * DANDELION_FB_STRIDE);
		for (x = 0; x < DANDELION_FB_VISIBLE_W; x++) {
			row[x] = rgb;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Programming (ladder steps D2 and D3)                                */
/* ------------------------------------------------------------------ */

int
dandelion_dsi_program_timing(const struct dandelion_dsi_bus *bus,
    const struct dandelion_panel *p)
{
	uint32_t hsa_wc, hbp_wc, hfp_wc, bllp_wc;

	if (!bus || !bus->write32 || !p) {
		return DANDELION_DISP_ERR_ARG;
	}

	dandelion_dsi_horizontal_wc(p, &hsa_wc, &hbp_wc, &hfp_wc, &bllp_wc);

	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_VSA_NL, p->vsa);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_VBP_NL, p->vbp);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_VFP_NL, p->vfp);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_VACT_NL, p->height);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_HSA_WC, hsa_wc);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_HBP_WC, hbp_wc);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_HFP_WC, hfp_wc);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_BLLP_WC, bllp_wc);

	return DANDELION_DISP_OK;
}

int
dandelion_dsi_program_phy(const struct dandelion_dsi_bus *bus,
    const struct dandelion_panel *p)
{
	uint32_t t0, t1, t2, t3, pll_con0, pcw_ratio, posdiv, val;
	uint64_t phy = DANDELION_MIPITX0_BASE;
	uint64_t dsi = DANDELION_DSI0_BASE;
	unsigned i;

	if (!bus || !bus->write32 || !bus->read32 || !p) {
		return DANDELION_DISP_ERR_ARG;
	}

	dandelion_dsi_phy_timcon(p, &t0, &t1, &t2, &t3);
	dandelion_dsi_pll_div(p, &pll_con0, &pcw_ratio, &posdiv);
	(void)pcw_ratio;

	/*
	 * [LK] DSI_PHY_clk_setting() order: bias/bandgap, PLL power, PCW and
	 * post-divider, then PLL enable.  The impedance calibration that LK does
	 * from the efuse is NOT reproduced here -- it needs the MIPI TX efuse
	 * block (0x11c50190/94/98) and is step D3 work, not D2.
	 */
	bus->write32(bus->ctx, phy + MIPITX_PLL_CON4, MIPITX_PLL_CON4_INIT);
	bus->write32(bus->ctx, phy + MIPITX_LANE_CON, MIPITX_LANE_CON_A);
	if (bus->delay_ms) {
		bus->delay_ms(bus->ctx, 1);
	}
	bus->write32(bus->ctx, phy + MIPITX_LANE_CON, MIPITX_LANE_CON_B);

	bus->write32(bus->ctx, phy + MIPITX_PLL_CON0, pll_con0);

	/* PLL_CON1: keep the reserved bits, replace POSDIV (bits 4:0). */
	val = bus->read32(bus->ctx, phy + MIPITX_PLL_CON1);
	val = (val & ~0x1fU) | (posdiv & 0x1fU);
	bus->write32(bus->ctx, phy + MIPITX_PLL_CON1, val);

	if (bus->delay_ms) {
		bus->delay_ms(bus->ctx, 1);
	}
	val = bus->read32(bus->ctx, phy + MIPITX_PLL_CON1);
	bus->write32(bus->ctx, phy + MIPITX_PLL_CON1, val | (1U << 0));

	/* PHY timing, then the byte counts again: LK writes the byte counts
	 * before and after the PHY configuration because the LP-per-line
	 * recalculation reads them back. */
	bus->write32(bus->ctx, dsi + DSI_PHY_TIMCON0, t0);
	bus->write32(bus->ctx, dsi + DSI_PHY_TIMCON1, t1);
	bus->write32(bus->ctx, dsi + DSI_PHY_TIMCON2, t2);
	bus->write32(bus->ctx, dsi + DSI_PHY_TIMCON3, t3);

	/*
	 * TODO(D3): still missing from this function, all of it requiring the
	 * D0 read-back before it can be written with any confidence --
	 *   1. DSI_TXRX_CON.LANE_NUM (bits 5:2).  LK encodes it as
	 *      (1 << lanes) - 1, so 4 lanes is 0xf [LK ddp_dsi.c, DSI_exit_ULPS],
	 *      but LK only writes that field on the ULPS path; the value the
	 *      device actually runs with has to be read first.
	 *   2. DSI_PSCON pixel format / RGB swap (PS_SEL bits 18:16) for RGB888
	 *      with "MSB first, padding on LSB" [VK lcm_get_params].
	 *   3. MIPI TX impedance calibration from the efuse words at
	 *      0x11c50190 / 0x11c50194 / 0x11c50198 [LK DSI_PHY_clk_setting].
	 *      Not guessed here on purpose: the efuse contents differ per die.
	 */
	(void)i;

	return DANDELION_DISP_OK;
}

int
dandelion_dsi_start(const struct dandelion_dsi_bus *bus)
{
	uint32_t intsta;
	unsigned spins;

	if (!bus || !bus->write32 || !bus->read32) {
		return DANDELION_DISP_ERR_ARG;
	}

	/* [LK] DSI_Start(): clear then set DSI_START.DSI_START. */
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_START, 0);
	bus->write32(bus->ctx, DANDELION_DSI0_BASE + DSI_START,
	    DSI_START_DSI_START);

	/* [LK] DSI_WaitForNotBusy(): poll DSI_INTSTA.DSI_BUSY. */
	for (spins = 0; spins < 1000U; spins++) {
		intsta = bus->read32(bus->ctx, DANDELION_DSI0_BASE + DSI_INTSTA);
		if ((intsta & DSI_INTSTA_DSI_BUSY) == 0) {
			if (intsta & DSI_INTSTA_BUFFER_UNDERRUN) {
				return DANDELION_DISP_ERR_BUS;
			}
			return DANDELION_DISP_OK;
		}
		if (bus->delay_ms) {
			bus->delay_ms(bus->ctx, 1);
		}
	}

	return DANDELION_DISP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* Panel DCS sequence (ladder step D3)                                 */
/* ------------------------------------------------------------------ */

/*
 * [VK] nt36525b_vdo_hdp_boe_helitai.c, init_setting[].
 *
 * These are the panel's own DCS commands, in the order the vendor driver sends
 * them.  0xff is "page select" on this controller: 0x23 selects the backlight
 * (PWM frequency) page, 0x10 the main page.  0x35 = set tear on, 0x51 = write
 * display brightness, 0x53 = write control display, 0x55 = write CABC, 0x68 =
 * unknown vendor command, 0x29 = display on, 0x11 = sleep out.
 *
 * The vendor driver sends 0x29 *before* 0x11 and then waits 100 ms; that order
 * is reproduced verbatim rather than "fixed", because the panel is the authority
 * on what it accepts.
 *
 * TODO: this table is one of at least three NT36525B module variants on this
 * device (dijing / helitai / panda-shengchao, plus FT8006S and HX83102D
 * alternatives).  Read the panel ID first (dandelion_panel_read_id) and only
 * then decide whether this table applies.  See docs/DISPLAY-PLAN.md section 2.
 */
struct dandelion_dcs_cmd {
	uint8_t cmd;
	uint8_t count;
	uint8_t para[4];
};

/* A count of 0xff means "delay, count = milliseconds". */
#define DCS_DELAY 0xffU

static const struct dandelion_dcs_cmd dandelion_panel_init_table[] = {
	{ 0xff, 1, { 0x23, 0, 0, 0 } },   /* page: backlight            */
	{ DCS_DELAY, 1, { 0, 0, 0, 0 } }, /* 1 ms                       */
	{ 0xfb, 1, { 0x01, 0, 0, 0 } },
	{ 0x07, 1, { 0x20, 0, 0, 0 } },   /* PWM frequency, 30 kHz band */
	{ 0x08, 1, { 0x07, 0, 0, 0 } },
	{ 0x09, 1, { 0x02, 0, 0, 0 } },
	{ 0xff, 1, { 0x10, 0, 0, 0 } },   /* page: main                 */
	{ 0xfb, 1, { 0x01, 0, 0, 0 } },
	{ 0x35, 1, { 0x00, 0, 0, 0 } },   /* set tear on                */
	{ 0x51, 1, { 0x00, 0, 0, 0 } },   /* brightness 0               */
	{ 0x53, 1, { 0x2c, 0, 0, 0 } },   /* write control display      */
	{ 0x55, 1, { 0x00, 0, 0, 0 } },   /* write CABC                 */
	{ 0x68, 2, { 0x02, 0x01, 0, 0 } },
	{ 0x29, 0, { 0, 0, 0, 0 } },      /* display on                 */
	{ 0x11, 0, { 0, 0, 0, 0 } },      /* sleep out                  */
	{ DCS_DELAY, 100, { 0, 0, 0, 0 } },
};

int
dandelion_panel_init(const struct dandelion_dsi_bus *bus)
{
	const struct dandelion_dcs_cmd *c;
	unsigned i, n;
	int rc;

	if (!bus || !bus->dcs_write) {
		return DANDELION_DISP_ERR_ARG;
	}

	n = (unsigned)(sizeof(dandelion_panel_init_table) /
	    sizeof(dandelion_panel_init_table[0]));

	for (i = 0; i < n; i++) {
		c = &dandelion_panel_init_table[i];
		if (c->cmd == DCS_DELAY) {
			if (bus->delay_ms) {
				bus->delay_ms(bus->ctx, c->count);
			}
			continue;
		}
		rc = bus->dcs_write(bus->ctx, c->cmd, c->para, c->count);
		if (rc != 0) {
			return DANDELION_DISP_ERR_BUS;
		}
	}

	return DANDELION_DISP_OK;
}

/*
 * [VK] lcm_compare_id(): the vendor driver resets the panel, selects page
 * 0x03 with 0x37 0x00, then reads DCS 0x04 and accepts exactly 00 80 00.
 *
 * TODO: this is the check that tells us which module is fitted, but the three
 * NT36525B variants share an ID in the vendor driver -- it accepts all of them
 * with the same three bytes, so an ID match does NOT identify the variant.  The
 * variant distinction in the vendor tree is a build-time choice
 * (CONFIG_CUSTOM_KERNEL_LCM), not a runtime probe.
 */
int
dandelion_panel_read_id(const struct dandelion_dsi_bus *bus, uint8_t id[3])
{
	int n;

	if (!bus || !bus->dcs_read || !id) {
		return DANDELION_DISP_ERR_ARG;
	}

	id[0] = 0;
	id[1] = 0;
	id[2] = 0;
	n = bus->dcs_read(bus->ctx, 0x04, id, 3);
	if (n < 3) {
		return DANDELION_DISP_ERR_BUS;
	}

	if (id[0] != 0x00 || id[1] != 0x80 || id[2] != 0x00) {
		return DANDELION_DISP_ERR_NOT_ID;
	}

	return DANDELION_DISP_OK;
}

/*
 * [VK] lcm_setbacklight_cmdq(): the vendor driver clamps a non-zero level to at
 * least 14 and then scales it by 72/100 before writing DCS 0x51.  Reproduced
 * because a brightness curve that differs from Android's will look wrong even
 * when the register write is right.
 */
int
dandelion_panel_set_backlight(const struct dandelion_dsi_bus *bus, uint8_t level)
{
	uint32_t scaled;
	uint8_t para;

	if (!bus || !bus->dcs_write) {
		return DANDELION_DISP_ERR_ARG;
	}

	if (level != 0 && level <= 14) {
		level = 14;
	}
	scaled = (uint32_t)level * 72U / 100U;
	para = (uint8_t)scaled;

	return bus->dcs_write(bus->ctx, 0x51, &para, 1) == 0 ?
	    DANDELION_DISP_OK : DANDELION_DISP_ERR_BUS;
}

/* ------------------------------------------------------------------ */
/* Logging helper                                                      */
/* ------------------------------------------------------------------ */

const char *
dandelion_display_status_name(int status)
{
	switch (status) {
	case DANDELION_DISP_OK:
		return "ok";
	case DANDELION_DISP_ERR_ARG:
		return "bad argument";
	case DANDELION_DISP_ERR_BUS:
		return "register read/write did not stick";
	case DANDELION_DISP_ERR_TIMEOUT:
		return "DSI stayed busy";
	case DANDELION_DISP_ERR_NOT_ID:
		return "panel ID mismatch";
	case DANDELION_DISP_ERR_UNIMPLEMENTED:
		return "not implemented";
	default:
		return "unknown";
	}
}
