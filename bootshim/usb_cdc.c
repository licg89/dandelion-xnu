/* MT6762G USB CDC-ACM debug console
 * Minimal USB device implementation for debug output.
 * Uses MTK MUSB controller at 0x11200000.
 *
 * When connected to PC via USB, appears as a CDC-ACM serial device.
 * PC sees: /dev/ttyACM0 (Linux) or COM port (Windows)
 * Baud rate: irrelevant (USB), use 115200 in terminal
 */
#include <stdint.h>
#include "usb_cdc.h"

/* MUSB registers */
#define MUSB_BASE       0x11200000UL
#define MUSB_FADDR      (MUSB_BASE + 0x000)
#define MUSB_POWER      (MUSB_BASE + 0x001)
#define MUSB_INTRTX     (MUSB_BASE + 0x002)
#define MUSB_INTRRX     (MUSB_BASE + 0x004)
#define MUSB_INTRTXE    (MUSB_BASE + 0x006)
#define MUSB_INTRRXE    (MUSB_BASE + 0x008)
#define MUSB_INTRUSB    (MUSB_BASE + 0x00A)
#define MUSB_INTRUSBE   (MUSB_BASE + 0x00B)
#define MUSB_FRAME      (MUSB_BASE + 0x00C)
#define MUSB_INDEX      (MUSB_BASE + 0x00E)
#define MUSB_TXMAXP     (MUSB_BASE + 0x010)
#define MUSB_TXCSR      (MUSB_BASE + 0x012)
#define MUSB_RXMAXP     (MUSB_BASE + 0x014)
#define MUSB_RXCSR      (MUSB_BASE + 0x016)
#define MUSB_COUNT0     (MUSB_BASE + 0x018)
#define MUSB_FIFO0      (MUSB_BASE + 0x020)
#define MUSB_DEVCTL     (MUSB_BASE + 0x041)
#define MUSB_TXFIFOSZ   (MUSB_BASE + 0x042)
#define MUSB_RXFIFOSZ   (MUSB_BASE + 0x043)
#define MUSB_TXFIFOADD  (MUSB_BASE + 0x044)
#define MUSB_RXFIFOADD  (MUSB_BASE + 0x046)
#define MUSB_VEND0      (MUSB_BASE + 0x047)
#define MUSB_EP0_FIFO   (MUSB_BASE + 0x000)  /* indexed by INDEX */

/* Indexed endpoint registers (selected by INDEX) */
#define MUSB_TXMAXP_I   (MUSB_BASE + 0x010)
#define MUSB_TXCSR_I    (MUSB_BASE + 0x012)
#define MUSB_RXMAXP_I   (MUSB_BASE + 0x014)
#define MUSB_RXCSR_I    (MUSB_BASE + 0x016)
#define MUSB_TXFIFOSZ_I (MUSB_BASE + 0x042)
#define MUSB_RXFIFOSZ_I (MUSB_BASE + 0x043)
#define MUSB_TXFIFOADD_I(MUSB_BASE + 0x044)
#define MUSB_RXFIFOADD_I(MUSB_BASE + 0x046)

/* USB PHY registers */
#define USB_PHY_BASE    0x11280000UL

/* Clock gate for USB */
#define TOPCKGEN_BASE   0x10000000UL

static volatile uint8_t *musb = (volatile uint8_t *)MUSB_BASE;

/* CDC-ACM descriptors */
static const uint8_t device_desc[] = {
    18, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,
    0xED, 0x1D, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 1
};
static const uint8_t config_desc[] = {
    /* Configuration descriptor */
    9, 0x02, 67, 0, 2, 1, 0, 0x80, 50,
    /* IAD */
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,
    /* CDC Control interface */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    /* Header functional */
    5, 0x24, 0x00, 0x10, 0x01,
    /* ACM functional */
    4, 0x24, 0x02, 0x02,
    /* Union functional */
    5, 0x24, 0x06, 0, 1,
    /* Notification endpoint */
    7, 0x05, 0x83, 0x03, 8, 0, 16,
    /* CDC Data interface */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    /* Data OUT endpoint */
    7, 0x05, 0x01, 0x02, 64, 0, 0,
    /* Data IN endpoint */
    7, 0x05, 0x82, 0x02, 64, 0, 0,
};

static uint8_t usb_address = 0;
static uint8_t ep0_buf[64];
static int ep0_state = 0;  /* 0=idle, 1=status */
static const uint8_t *desc_ptr = 0;
static int desc_remaining = 0;

/* USB string descriptors */
static const uint8_t string_lang[] = {4, 0x03, 0x09, 0x04};
static const uint8_t string_product[] = {
    30, 0x03, 'D',0,'a',0,'n',0,'d',0,'e',0,'l',0,'i',0,'o',0,'n',0,' ',0,'D',0,'e',0,'b',0,'u',0,'g',0
};

static void musb_write(uint32_t reg, uint8_t val) { *(volatile uint8_t *)reg = val; }
static uint8_t musb_read(uint32_t reg) { return *(volatile uint8_t *)reg; }

static void usb_select_ep(uint8_t ep) { musb_write(MUSB_INDEX, ep); }

static void usb_ep0_send(const uint8_t *data, int len)
{
    if (len > 64) len = 64;
    for (int i = 0; i < len; i++)
        musb_write(MUSB_FIFO0, data[i]);
    musb_write(MUSB_COUNT0, len);
    uint16_t txcsr = musb_read(MUSB_TXCSR_I) | 0x0002; /* TXPKTRDY */
    musb_write(MUSB_TXCSR_I+1, txcsr >> 8);
    musb_write(MUSB_TXCSR_I, txcsr & 0xff);
}

static void usb_ep0_stall(void)
{
    uint16_t txcsr = musb_read(MUSB_TXCSR_I) | 0x0010 | 0x0004;
    musb_write(MUSB_TXCSR_I+1, txcsr >> 8);
    musb_write(MUSB_TXCSR_I, txcsr & 0xff);
}

static void handle_ep0_setup(const uint8_t *setup)
{
    uint8_t type = setup[0] & 0x60;
    uint8_t req = setup[1];
    uint16_t wValue = setup[2] | (setup[3] << 8);
    uint16_t wIndex = setup[4] | (setup[5] << 8);
    uint16_t wLength = setup[6] | (setup[7] << 8);

    if (type == 0x00) { /* Standard device request */
        switch (req) {
        case 0x00: /* GET_STATUS */
            ep0_buf[0] = 0; ep0_buf[1] = 0;
            usb_ep0_send(ep0_buf, 2);
            break;
        case 0x05: /* SET_ADDRESS */
            usb_address = wValue & 0x7F;
            ep0_state = 1;
            usb_ep0_send(0, 0);
            break;
        case 0x06: { /* GET_DESCRIPTOR */
            uint8_t dtype = wValue >> 8;
            if (dtype == 0x01) { desc_ptr = device_desc; desc_remaining = device_desc[0]; }
            else if (dtype == 0x02) { desc_ptr = config_desc; desc_remaining = config_desc[2] | (config_desc[3]<<8); }
            else if (dtype == 0x03) {
                if (wValue == 0) { desc_ptr = string_lang; desc_remaining = string_lang[0]; }
                else if (wValue == 1) { desc_ptr = string_product; desc_remaining = string_product[0]; }
                else { desc_ptr = string_lang; desc_remaining = string_lang[0]; }
            }
            else { usb_ep0_stall(); return; }
            int send_len = desc_remaining < wLength ? desc_remaining : wLength;
            usb_ep0_send(desc_ptr, send_len);
            desc_ptr += send_len;
            desc_remaining -= send_len;
            break;
        }
        case 0x08: /* GET_CONFIGURATION */
            ep0_buf[0] = 1;
            usb_ep0_send(ep0_buf, 1);
            break;
        case 0x09: /* SET_CONFIGURATION */
            ep0_state = 1;
            usb_ep0_send(0, 0);
            break;
        default:
            usb_ep0_stall();
            break;
        }
    } else if (type == 0x20) { /* Class request */
        ep0_state = 1;
        usb_ep0_send(0, 0);
    } else {
        usb_ep0_stall();
    }
}

void usb_cdc_init(void)
{
    /* Enable USB clock */
    volatile uint32_t *clk = (volatile uint32_t *)TOPCKGEN_BASE;
    clk[0x98/4] = (1u << 26);  /* SETCG: enable USB clock */

    /* Reset MUSB */
    musb_write(MUSB_POWER, 0x08);  /* RESET */
    for (volatile int i = 0; i < 100000; i++);
    musb_write(MUSB_POWER, 0x00);

    /* Set device mode */
    musb_write(MUSB_DEVCTL, 0x00);

    /* Enable interrupts */
    musb_write(MUSB_INTRUSBE, 0x07);  /* reset, resume, suspend */
    musb_write(MUSB_INTRTXE, 0x0001); /* EP0 */
    musb_write(MUSB_INTRRXE, 0x0000);

    /* Configure EP1 for bulk IN (debug output) */
    usb_select_ep(1);
    musb_write(MUSB_TXMAXP_I, 64);
    musb_write(MUSB_TXMAXP_I+1, 0);
    musb_write(MUSB_TXFIFOSZ_I, 0x04);  /* 64 bytes */
    musb_write(MUSB_TXFIFOADD_I, 0x10); /* FIFO address */
    musb_write(MUSB_TXFIFOADD_I+1, 0);

    /* Enable EP1 TX interrupt */
    musb_write(MUSB_INTRTXE, 0x0003);  /* EP0 + EP1 */
}

int usb_cdc_putc(char c)
{
    /* Wait for EP1 TX ready */
    for (volatile int i = 0; i < 1000000; i++) {
        uint16_t txcsr = musb_read(MUSB_TXCSR_I) | (musb_read(MUSB_TXCSR_I+1) << 8);
        if (!(txcsr & 0x0002)) break;  /* TXPKTRDY clear */
    }

    usb_select_ep(1);
    musb_write(MUSB_FIFO0 + 4, c);  /* EP1 FIFO */
    uint16_t txcsr = 0x0002 | 0x4000;  /* TXPKTRDY | AUTOSET */
    musb_write(MUSB_TXCSR_I+1, txcsr >> 8);
    musb_write(MUSB_TXCSR_I, txcsr & 0xff);
    return 0;
}

int usb_cdc_puts(const char *s)
{
    while (*s) usb_cdc_putc(*s++);
    return 0;
}
