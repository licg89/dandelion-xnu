/* Dandelion boot shim - USB CDC-ACM debug console
 *
 * The phone has no accessible UART, but it does have the USB2 controller that
 * the bootloader already uses for fastboot (mediatek,mt6765-usb20 at
 * 0x11200000, PHY at 0x11cc0000).  The shim re-uses that hardware as a debug
 * console: it enumerates as a CDC-ACM device (so the host gets a COM port or
 * /dev/ttyACM0) and then mirrors everything the framebuffer console prints
 * onto the bulk IN endpoint.
 *
 * The PHY and the USB clock are left exactly as the bootloader set them up -
 * only the controller's device-mode logic is (re)initialised here, which is
 * what makes this feasible without a full MTK USB PHY bring-up.
 *
 * The console is polling based: USB interrupts are serviced from usb_poll(),
 * which the shim calls from its idle loops (and from fb_putc() while a line is
 * being written), so no host-side driver work and no interrupt setup is
 * needed.
 */
#ifndef DANDELION_USB_CDC_H
#define DANDELION_USB_CDC_H

#include <stdint.h>

/* Bring the controller up in device mode and print a register summary on the
 * framebuffer.  Returns 0 when the controller answered. */
int usb_cdc_init(void);

/* Service pending USB events.  Safe to call from anywhere, any time. */
void usb_poll(void);

/* Mirror one character to the CDC bulk IN endpoint.  Drops the byte when the
 * console has not been enumerated yet, so the shim can call it unconditionally. */
void usb_cdc_putc(char c);

/* Has the host configured us (SET_CONFIGURATION seen)? */
int usb_cdc_ready(void);

/* Diagnostics: how many control requests were served, last request type,
 * number of bytes pushed to the host. */
void usb_cdc_stats(uint32_t *requests, uint32_t *last_request,
		   uint32_t *bytes_out, uint32_t *events);

/* Print the controller and PHY registers on the framebuffer. */
void usb_cdc_dump(void);

#endif
