/*
 * usb_gamepad.h -- the device on the other end of the wire.
 *
 * A host controller moves bytes; it does not know what they mean. This is the
 * thing that answers them: an Xbox controller, as the console's own USB stack
 * expects to find it -- standard descriptors over endpoint 0, the XID class
 * requests on its interface, a 20-byte report on its interrupt IN endpoint and
 * a 6-byte rumble report on its interrupt OUT endpoint.
 *
 * Kept apart from ohci.c because it is a different concern. The controller
 * walks descriptor lists and raises interrupts and would do the same for a
 * memory unit or a headset; this knows one device and nothing about lists.
 */
#ifndef XBOX_USB_GAMEPAD_H
#define XBOX_USB_GAMEPAD_H

#include <stdint.h>

/* USB setup packet, as it arrives in a SETUP transfer's buffer. */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} UsbSetup;

/* What a transfer can come back as, besides a byte count. */
#define USB_STALL  (-1)   /* the request or endpoint is not supported      */
#define USB_NAK    (-2)   /* nothing to send yet; the host retries later   */

/* A bus reset of the port the pad is on: back to address 0, unconfigured,
 * and the next report goes out whatever it holds. */
void usb_gamepad_reset(void);

/* Answer a control request. For a host-to-device request with a data stage,
 * `data`/`data_len` hold what the host sent; for a device-to-host one the
 * answer goes to `out` (at most `max` bytes).
 *
 * Returns the number of bytes written to `out`, or USB_STALL if the request is
 * not one this device answers -- which the caller reports as a stall rather
 * than as a short transfer, because those mean different things to a driver.
 */
int usb_gamepad_control(const UsbSetup *setup, const uint8_t *data, int data_len,
                        uint8_t *out, int max);

/* An IN transaction on a non-control endpoint: the 20-byte input report, or
 * USB_NAK when nothing changed since the last one delivered (what the real pad
 * does; it keeps the guest's driver from running for every 4 ms poll). */
int usb_gamepad_in(int endpoint, uint8_t *out, int max);

/* An OUT transaction on a non-control endpoint: a rumble report. Returns the
 * number of bytes taken. */
int usb_gamepad_out(int endpoint, const uint8_t *data, int len);

/* Fill in the 20-byte input report as it stands. Returns the byte count. */
int usb_gamepad_report(uint8_t *out, int max);

/* The address the host assigned with SET_ADDRESS, 0 until it does. */
uint8_t usb_gamepad_address(void);

#endif /* XBOX_USB_GAMEPAD_H */
