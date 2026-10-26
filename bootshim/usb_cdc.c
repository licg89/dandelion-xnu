/* Dandelion boot shim - USB CDC-ACM debug console (implementation)
 *
 * MediaTek's USB2 device controller is a Mentor MUSB core ("mediatek,
 * mt6765-usb20" in the device tree, register block at 0x11200000, PHY at
 * 0x11cc0000).  The bootloader already brings the PHY and clocks up for
 * fastboot, so this file only drives the controller's device-mode logic:
 * enumeration on EP0, then a CDC-ACM data interface whose bulk IN endpoint
 * carries the console text.
 *
 * Everything is polled.  There is no interrupt setup and no DMA: the shim
 * calls usb_poll() from its loops and usb_cdc_putc() for every character the
 * framebuffer console prints, so a hung guest still shows its last words on
 * the panel and a live guest streams them to the host.
 */
#include <stdint.h>
#include "usb_cdc.h"

#define MUSB_BASE        0x11200000UL
#define USB_PHY_BASE     0x11CC0800UL
#define TOPCK_BASE       0x10000000UL
#define INFRAAO_BASE     0x10001000UL
#define APMIX_BASE       0x1000C000UL
#define PERICFG_BASE     0x10003000UL

/* PHY "a60810" registers we touch (see docs/USB_CONSOLE.md) */
#define PHY_ACR0         0x00
#define PHY_ACR6         0x18
#define PHY_ACR4         0x20
#define PHY_DTM0         0x68
#define PHY_DTM1         0x6C
#define PHY_VERSIONC     0xF0

#define PHY_DEV_ACTIVE   0x00003F2Fu

/* Checkpoints for the on-panel summary. */
static uint32_t phy_versionc;
static uint32_t phy_acr6_after;
static uint32_t phy_dtm1_after;
static uint32_t musb_devctl_after;

/* --- core registers ---------------------------------------------------- */
#define MUSB_FADDR       (MUSB_BASE + 0x00)
#define MUSB_POWER       (MUSB_BASE + 0x01)
#define MUSB_INTRTX      (MUSB_BASE + 0x02)      /* 16 bit */
#define MUSB_INTRRX      (MUSB_BASE + 0x04)
#define MUSB_INTRTXE     (MUSB_BASE + 0x06)
#define MUSB_INTRRXE     (MUSB_BASE + 0x08)
#define MUSB_INTRUSB     (MUSB_BASE + 0x0A)
#define MUSB_INTRUSBE    (MUSB_BASE + 0x0B)
#define MUSB_FRAME       (MUSB_BASE + 0x0C)
#define MUSB_INDEX       (MUSB_BASE + 0x0E)
#define MUSB_DEVCTL      (MUSB_BASE + 0x60)

/* --- indexed endpoint registers ---------------------------------------- */
#define MUSB_TXMAXP      (MUSB_BASE + 0x10)
#define MUSB_TXCSR       (MUSB_BASE + 0x12)
#define MUSB_RXMAXP      (MUSB_BASE + 0x14)
#define MUSB_RXCSR       (MUSB_BASE + 0x16)
#define MUSB_RXCOUNT     (MUSB_BASE + 0x18)
#define MUSB_FIFO        (MUSB_BASE + 0x20)

/* POWER bits */
#define POWER_ISOUPDATE  0x80
#define POWER_SOFTCONN   0x40
#define POWER_HSEN       0x20
#define POWER_HSMODE     0x10
#define POWER_RESET      0x08
#define POWER_RESUME     0x04
#define POWER_SUSPENDM   0x02

/* INTRUSB bits */
#define INTR_VBUS        0x80
#define INTR_SESSREQ     0x40
#define INTR_DISCON      0x20
#define INTR_CONN        0x10
#define INTR_SOF         0x08
#define INTR_RESET       0x04
#define INTR_RESUME      0x02
#define INTR_SUSPEND     0x01

/* TXCSR bits (device mode) */
#define TXCSR_TXPKTRDY   0x0001
#define TXCSR_CLRDT      0x0040
#define TXCSR_FIFONOTEMPTY 0x0200
#define TXCSR_MODE       0x2000
#define TXCSR_AUTOSET    0x8000
#define TXCSR_FLUSHFIFO  0x0008

/*
 * EP0 uses CSR0, which has a different bit layout from the data endpoints:
 * bit0 is RXPKTRDY, bit1 is TXPKTRDY, bit6 acknowledges a SETUP packet and
 * bit7 signals DATAEND (the status stage of a control transfer).  Writing the
 * data-endpoint TXCSR value (0x0001) here sets RXPKTRDY instead of TXPKTRDY,
 * so nothing is ever transmitted and the host never enumerates - which is
 * exactly what the earlier attempt did.
 */
#define CSR0_RXPKTRDY     0x0001
#define CSR0_TXPKTRDY     0x0002
#define CSR0_OPRQ         0x0004
#define CSR0_SERVICEDSETUP 0x0040
#define CSR0_DATAEND      0x0080
#define CSR0_SENTSTALL    0x0100

/* FIFO sizing registers (indexed) */
#define MUSB_TXFIFOSZ     (MUSB_BASE + 0x62)
#define MUSB_TXMAXP_      (MUSB_BASE + 0x10)
#define MUSB_TXFIFOADD    (MUSB_BASE + 0x64)

/* RXCSR bits (device mode) */
#define RXCSR_RXPKTRDY   0x0001
#define RXCSR_FIFOFULL   0x0002
#define RXCSR_FLUSHFIFO  0x0010
#define RXCSR_REQPKT     0x0020
#define RXCSR_RXSTALL    0x0080
#define RXCSR_AUTOREQ    0x4000
#define RXCSR_AUTOCLEAR  0x8000

/* --- descriptors -------------------------------------------------------- */

static const uint8_t device_desc[] = {
	18, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,
	0xED, 0x1D, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 1
};

static const uint8_t config_desc[] = {
	9, 0x02, 67, 0, 2, 1, 0, 0x80, 50,
	8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,
	9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
	5, 0x24, 0x00, 0x10, 0x01,
	4, 0x24, 0x02, 0x02,
	5, 0x24, 0x06, 0, 1,
	7, 0x05, 0x83, 0x03, 8, 0, 16,
	9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
	7, 0x05, 0x01, 0x02, 64, 0, 0,
	7, 0x05, 0x82, 0x02, 64, 0, 0,
};

static const uint8_t string_lang[] = { 4, 0x03, 0x09, 0x04 };
static const uint8_t string_product[] = {
	30, 0x03,
	'D', 0, 'a', 0, 'n', 0, 'd', 0, 'e', 0, 'l', 0, 'i', 0, 'o', 0, 'n', 0,
	' ', 0, 'l', 0, 'o', 0, 'g', 0
};

/* --- state -------------------------------------------------------------- */

static int configured;
static int connected;
static uint32_t request_count;
static uint32_t last_request;
static uint32_t bytes_out;
static uint32_t event_count;

static const uint8_t *desc_ptr;
static int desc_remaining;

static uint8_t r8(uint32_t reg) { return *(volatile uint8_t *)(uintptr_t)reg; }
static void w8(uint32_t reg, uint8_t value) { *(volatile uint8_t *)(uintptr_t)reg = value; }
static uint16_t r16(uint32_t reg) { return *(volatile uint16_t *)(uintptr_t)reg; }
static void w16(uint32_t reg, uint16_t value) { *(volatile uint16_t *)(uintptr_t)reg = value; }

static uint32_t r32(uint32_t reg) { return *(volatile uint32_t *)(uintptr_t)reg; }
static void w32(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)reg = value;
	__asm__ volatile("dsb sy" ::: "memory");
}
static void setclr(uint32_t reg, uint32_t clear, uint32_t set)
{
	uint32_t value = r32(reg);
	value &= ~clear;
	value |= set;
	w32(reg, value);
}

/* Busy-wait using the EL0 physical counter (13 MHz on this SoC). */
static void udelay(unsigned microseconds)
{
	unsigned long frequency;
	unsigned long start;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	if (frequency == 0) {
		frequency = 13000000;
	}
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
	start += (frequency / 1000000ul) * microseconds;
	while (1) {
		unsigned long now;
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
		if ((long)(now - start) >= 0) {
			break;
		}
	}
}

/*
 * Power up MediaTek's "a60810" USB2 PHY so the controller can see the bus.
 *
 * The bootloader leaves the PHY gated after fastboot, so MUSB stays readable
 * while DEVCTL reads 0 (its VBUS/BDEVICE bits come straight from the PHY's
 * UTMI comparators).  Sequence and values are taken from the vendor kernel
 * (drivers/misc/mediatek/usb20/mt6765/usb20_phy.c, mtk-phy-a60810.h).
 */
static int usb_phy_bringup(void)
{
	/* Phase 1: clocks.  Read-modify-write only: 0x104 and apmixed 0x14 are
	 * shared with camera/display clocks. */
	w32(INFRAAO_BASE + 0x84, 1u << 8);              /* CG_CLR ICUSB on */
	setclr(TOPCK_BASE + 0x90, 0, 1u << 16);         /* usb_top_sel = univpll3_d4 */
	w32(TOPCK_BASE + 0x004, 1u << 22);              /* commit the mux */
	w32(TOPCK_BASE + 0x98, 1u << 23);               /* ungate usb_top_sel */
	setclr(TOPCK_BASE + 0x104, 0, (1u << 8) | (1u << 9));
	setclr(APMIX_BASE + 0x14, 0, 1u << 4);          /* SSUSB26M on */
	udelay(50);

	/* Phase 2: USB IP soft reset (also resets MUSB, so it runs before it). */
	setclr(PERICFG_BASE + 0x00, 0, 1u << 29);
	udelay(10000);
	setclr(PERICFG_BASE + 0x00, 1u << 29, 0);
	udelay(100);

	/* Phase 3: is the PHY register file alive at all? */
	phy_versionc = r32(USB_PHY_BASE + PHY_VERSIONC);
	if (phy_versionc == 0 && r32(USB_PHY_BASE + PHY_VERSIONC + 0x800) == 0) {
		return -1;
	}

	/* Phase 4: PHY power-on ("M17_USB_PWR"). */
	udelay(50);
	setclr(USB_PHY_BASE + PHY_DTM0, 1u << 26, 0);           /* FORCE_UART_EN */
	setclr(USB_PHY_BASE + PHY_DTM1, 1u << 16, 0);           /* UART_EN */
	setclr(USB_PHY_BASE + PHY_ACR4, (1u << 9) | (1u << 8), 0);
	setclr(USB_PHY_BASE + PHY_ACR6, 1u << 23, 0);           /* BC11_SW_EN */
	setclr(USB_PHY_BASE + PHY_ACR4, 0, 1u << 18);           /* DP_100K_MODE */
	setclr(USB_PHY_BASE + PHY_ACR4, (1u << 16) | (1u << 17), 0);
	setclr(USB_PHY_BASE + PHY_ACR6, 0, 1u << 20);           /* OTG_VBUSCMP_EN */
	setclr(USB_PHY_BASE + PHY_DTM0, 1u << 18, 0);           /* FORCE_SUSPENDM */
	setclr(USB_PHY_BASE + PHY_ACR6, 0xFFu << 24, 0x40u << 24); /* PHY_REV */
	udelay(800);
	phy_acr6_after = r32(USB_PHY_BASE + PHY_ACR6);

	/* Phase 5: force device mode, which makes DEVCTL report VBUS. */
	w32(USB_PHY_BASE + PHY_DTM1,
	    (r32(USB_PHY_BASE + PHY_DTM1) & ~0x00003F12u) | PHY_DEV_ACTIVE);
	udelay(10);
	setclr(USB_PHY_BASE + PHY_ACR6, 0xF0u, 0xF0u);          /* DISCTH max */
	phy_dtm1_after = r32(USB_PHY_BASE + PHY_DTM1);
	return 0;
}

static void select_ep(uint8_t ep) { w8(MUSB_INDEX, ep); }

static void fifo_write(const uint8_t *data, int length)
{
	for (int i = 0; i < length; i++) {
		w8(MUSB_FIFO, data[i]);
	}
}

static void fifo_read(uint8_t *data, int length)
{
	for (int i = 0; i < length; i++) {
		data[i] = r8(MUSB_FIFO);
	}
}

/* Send one packet on the currently selected endpoint. */
static void ep_write(const uint8_t *data, int length)
{
	fifo_write(data, length);
	w16(MUSB_TXCSR, TXCSR_TXPKTRDY);
}

/* --- EP0 control transfers (CSR0 layout) -------------------------------- */

static void ep0_ack_status(void)
{
	/*
	 * Status stage of a control write: a zero length IN packet, which on EP0
	 * is TXPKTRDY together with DATAEND so the core closes the transfer.
	 */
	select_ep(0);
	w16(MUSB_TXCSR, CSR0_TXPKTRDY | CSR0_DATAEND);
}

static void ep0_send(const uint8_t *data, int length)
{
	if (length > 64) {
		length = 64;
	}
	select_ep(0);
	fifo_write(data, length);
	w16(MUSB_TXCSR, CSR0_TXPKTRDY);
}

/* Finish a control-IN transfer once the last data packet has gone out. */
static void ep0_data_end(void)
{
	select_ep(0);
	w16(MUSB_TXCSR, CSR0_TXPKTRDY | CSR0_DATAEND);
}

static void ep0_stall(void)
{
	select_ep(0);
	w16(MUSB_TXCSR, CSR0_OPRQ);     /* STALL for EP0 */
}

static void handle_setup(const uint8_t *setup)
{
	uint8_t type = setup[0] & 0x60;
	uint8_t req = setup[1];
	uint16_t value = (uint16_t)(setup[2] | (setup[3] << 8));
	uint16_t length = (uint16_t)(setup[6] | (setup[7] << 8));

	request_count++;
	last_request = ((uint32_t)req << 16) | (uint32_t)value;

	if (type == 0x00) {                             /* standard */
		switch (req) {
		case 0x00:                              /* GET_STATUS */
		{
			static const uint8_t status[2] = { 0, 0 };
			ep0_send(status, 2);
			break;
		}
		case 0x05:                              /* SET_ADDRESS */
			w8(MUSB_FADDR, value & 0x7f);
			ep0_ack_status();
			break;
		case 0x06:                              /* GET_DESCRIPTOR */
		{
			uint8_t dtype = (uint8_t)(value >> 8);
			int total;

			if (dtype == 0x01) {
				desc_ptr = device_desc;
				total = device_desc[0];
			} else if (dtype == 0x02) {
				desc_ptr = config_desc;
				total = config_desc[2] | (config_desc[3] << 8);
			} else if (dtype == 0x03) {
				if ((value & 0xff) == 0x01) {
					desc_ptr = string_product;
					total = string_product[0];
				} else {
					desc_ptr = string_lang;
					total = string_lang[0];
				}
			} else {
				ep0_stall();
				return;
			}
			desc_remaining = total;
			{
				int send = total < length ? total : length;
				ep0_send(desc_ptr, send);
				desc_ptr += send;
				desc_remaining -= send;
			}
			break;
		}
		case 0x08:                              /* GET_CONFIGURATION */
		{
			static uint8_t config = 1;
			ep0_send(&config, 1);
			break;
		}
		case 0x09:                              /* SET_CONFIGURATION */
			configured = 1;
			ep0_ack_status();
			break;
		default:
			ep0_stall();
			break;
		}
		return;
	}

	/* CDC class requests (SET_LINE_CODING, SET_CONTROL_LINE_STATE, ...) and
	 * any vendor request: acknowledge without data. */
	ep0_ack_status();
}

/* --- public interface --------------------------------------------------- */

/*
 * Service EP0.  On EP0 the status/control register is CSR0 (0x12), not the
 * data-endpoint RXCSR (0x16): RXPKTRDY is bit0, a SETUP packet is acknowledged
 * by clearing RXPKTRDY and setting SERVICEDSETUP, and the receive byte count
 * is in COUNT0 (0x18).
 */
static void drain_ep0(void)
{
	select_ep(0);

	for (int guard = 0; guard < 8; guard++) {
		uint16_t csr0 = r16(MUSB_TXCSR);
		if (!(csr0 & CSR0_RXPKTRDY)) {
			break;
		}

		uint16_t count = r16(MUSB_RXCOUNT) & 0x7ff;
		uint8_t buffer[64];

		if (count > sizeof(buffer)) {
			count = sizeof(buffer);
		}
		fifo_read(buffer, count);

		/* Acknowledge the packet; a SETUP packet also needs
		 * SERVICEDSETUP so the core accepts the next one. */
		w16(MUSB_TXCSR, (uint16_t)(CSR0_RXPKTRDY | CSR0_SERVICEDSETUP));

		if (count == 8) {
			handle_setup(buffer);
		}
		event_count++;
	}
}

/* Continue a multi-packet control-IN transfer when the previous packet left. */
static void service_ep0_tx(void)
{
	select_ep(0);
	if (desc_remaining > 0 && desc_ptr) {
		uint16_t csr0 = r16(MUSB_TXCSR);
		if (!(csr0 & CSR0_TXPKTRDY)) {
			int send = desc_remaining > 64 ? 64 : desc_remaining;
			ep0_send(desc_ptr, send);
			desc_ptr += send;
			desc_remaining -= send;
			event_count++;
			if (desc_remaining == 0) {
				ep0_data_end();
			}
		}
	}
}

void usb_poll(void)
{
	uint8_t intr = r8(MUSB_INTRUSB);

	if (intr) {
		event_count++;
		w8(MUSB_INTRUSB, intr);
		if (intr & INTR_RESET) {
			connected = 1;
			configured = 0;
			w8(MUSB_FADDR, 0);
			desc_remaining = 0;
			select_ep(0);
			w16(MUSB_TXCSR, TXCSR_CLRDT);
			w16(MUSB_RXCSR, 0);
		}
		if (intr & INTR_DISCON) {
			connected = 0;
			configured = 0;
		}
	}

	drain_ep0();
	service_ep0_tx();
}

/*
 * One-line register summary, refreshed by the shim's status loop so a single
 * photograph always carries the current state:
 *
 *   usb: P=xx I=xx F=xx D=xx T=xxxx R=xxxx W=xxxx PHY=xxxxxxxx cfg=N reqs=N
 *
 * W is a write/read test on EP1 TXMAXP: if it does not return 0x1234 the
 * controller is not writable at all (clock gated, or the wrong IP), which is
 * a different problem from "the host is not talking to us".
 */
void usb_cdc_dump(void)
{
	extern void fb_puts(const char *s, uint32_t fg);
	extern void fb_puthex_raw(uint64_t val);
	extern void fb_putdec_raw(uint64_t val);
	extern void fb_newline(void);
	uint16_t write_test;
	uint32_t phy_cc, phy_28;

	select_ep(1);
	w16(MUSB_TXMAXP, 0x1234);
	write_test = r16(MUSB_TXMAXP);
	w16(MUSB_TXMAXP, 64);
	select_ep(0);

	phy_cc = *(volatile uint32_t *)(uintptr_t)0x11CC0000UL;
	phy_28 = *(volatile uint32_t *)(uintptr_t)0x11280000UL;

	/* The USB verdict line: PHY alive, PHY forced to device mode, DEVCTL.
	 * Success is DEVCTL == 0x98 (BDEVICE | VBUS = 11). */
	fb_puts("usb phy ver=", 0xFF00FFFF);
	fb_puthex_raw(phy_versionc);
	fb_puts(" acr6=", 0xFF00FFFF);
	fb_puthex_raw(phy_acr6_after);
	fb_puts(" dtm1=", 0xFF00FFFF);
	fb_puthex_raw(phy_dtm1_after);
	fb_newline();
	fb_puts("usb devctl=", 0xFF00FFFF);
	fb_puthex_raw(r8(MUSB_DEVCTL));
	fb_puts(" power=", 0xFF00FFFF);
	fb_puthex_raw(r8(MUSB_POWER));
	fb_puts(" intr=", 0xFF00FFFF);
	fb_puthex_raw(r8(MUSB_INTRUSB));
	fb_puts(" cfg=", 0xFF00FFFF);
	fb_putdec_raw((uint64_t)configured);
	fb_newline();

	/* Two short lines: 80 columns is the visible width of the panel. */
	fb_puts("usb ctl P=", 0xFF00FFFF);
	fb_puthex_raw(r8(MUSB_POWER));
	fb_puts(" D=", 0xFF00FFFF);
	fb_puthex_raw(r8(MUSB_DEVCTL));
	fb_puts(" I=", 0xFF00FFFF);
	fb_puthex_raw(r8(MUSB_INTRUSB));
	fb_puts(" W=", 0xFF00FFFF);
	fb_puthex_raw(write_test);
	fb_newline();

	fb_puts("usb phy cc=", 0xFF00FFFF);
	fb_puthex_raw(phy_cc);
	fb_puts(" 1128=", 0xFF00FFFF);
	fb_puthex_raw(phy_28);
	fb_puts(" cfg=", 0xFF00FFFF);
	fb_putdec_raw((uint64_t)configured);
	fb_puts(" req=", 0xFF00FFFF);
	fb_putdec_raw(request_count);
	fb_newline();

	/* Are the clock controllers reachable at all?  A live APB means the PHY
	 * reading 0 is a gate/mapping problem rather than "nothing is powered". */
	fb_puts("clk top=", 0xFF00FFFF);
	fb_puthex_raw(*(volatile uint32_t *)(uintptr_t)0x10000000UL);
	fb_puts(" infra=", 0xFF00FFFF);
	fb_puthex_raw(*(volatile uint32_t *)(uintptr_t)0x10001000UL);
	fb_puts(" sif=", 0xFF00FFFF);
	fb_puthex_raw(*(volatile uint32_t *)(uintptr_t)0x11210000UL);
	fb_newline();
}

int usb_cdc_init(void)
{
	/*
	 * Power the PHY first.  The bootloader gates it after fastboot, and
	 * without it DEVCTL stays 0 no matter what the controller is told to do
	 * (its VBUS/BDEVICE bits come from the PHY's UTMI comparators).
	 */
	int phy_error = usb_phy_bringup();

	w8(MUSB_INTRUSBE, 0x00);
	w16(MUSB_INTRTXE, 0x0000);

	/* Soft reset the core, then come back in device mode. */
	w8(MUSB_POWER, POWER_RESET);
	for (volatile int i = 0; i < 200000; i++) {
	}
	w8(MUSB_POWER, 0);
	for (volatile int i = 0; i < 200000; i++) {
	}

	w8(MUSB_FADDR, 0);
	select_ep(0);
	w16(MUSB_TXMAXP, 64);
	w16(MUSB_RXMAXP, 64);
	w16(MUSB_TXCSR, TXCSR_MODE);
	w16(MUSB_RXCSR, 0);
	w8(MUSB_INDEX, 0);

	/* Bulk IN endpoint for the log stream: EP1 IN, 64 byte packets. */
	select_ep(1);
	w16(MUSB_TXMAXP, 64);
	w16(MUSB_TXCSR, TXCSR_MODE | TXCSR_CLRDT);
	select_ep(0);

	/* Connect: clearing and setting SOFTCONN makes the host re-enumerate
	 * with our descriptors instead of the bootloader's. */
	w8(MUSB_POWER, 0);
	udelay(500);
	w8(MUSB_POWER, POWER_SOFTCONN | POWER_HSEN);
	udelay(500);
	w8(MUSB_DEVCTL, 0x00);
	udelay(100);

	musb_devctl_after = r8(MUSB_DEVCTL);

	connected = 0;
	configured = 0;
	desc_remaining = 0;
	return phy_error;
}

void usb_cdc_putc(char c)
{
	if (!configured) {
		return;
	}
	usb_poll();

	select_ep(1);
	uint16_t txcsr = r16(MUSB_TXCSR);
	if (txcsr & TXCSR_FIFONOTEMPTY) {
		return;                 /* host has not drained the previous packet */
	}
	w8(MUSB_FIFO, (uint8_t)c);
	w16(MUSB_TXCSR, TXCSR_TXPKTRDY);
	bytes_out++;
}

int usb_cdc_ready(void)
{
	return configured;
}

void usb_cdc_stats(uint32_t *requests, uint32_t *last, uint32_t *bytes,
		   uint32_t *events)
{
	if (requests) {
		*requests = request_count;
	}
	if (last) {
		*last = last_request;
	}
	if (bytes) {
		*bytes = bytes_out;
	}
	if (events) {
		*events = event_count;
	}
}
