/*
 * usb_gamepad.c -- an Xbox controller, as the console's USB stack expects it.
 *
 * Descriptors and the input report, from the USB 2.0 specification for the
 * standard requests and from the device's own published interface class for
 * the rest. The gamepad is not a HID device: it reports interface class 0x58
 * subclass 0x42, which is Microsoft's own, and its report has a fixed layout
 * rather than one described by a HID report descriptor. That is why there is
 * no report descriptor here and why nothing asks for one.
 *
 * Input comes from the host through the existing xbox_input layer, so a real
 * pad plugged into the PC drives this one.
 */
#include "usb_gamepad.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t s_device_desc[18] = {
    18,             /* bLength                                    */
    0x01,           /* bDescriptorType: DEVICE                    */
    0x10, 0x01,     /* bcdUSB 1.10                                */
    0x00,           /* bDeviceClass: per interface                */
    0x00,           /* bDeviceSubClass                            */
    0x00,           /* bDeviceProtocol                            */
    0x08,           /* bMaxPacketSize0: 8                         */
    0x5E, 0x04,     /* idVendor  0x045E Microsoft                 */
    0x89, 0x02,     /* idProduct 0x0289 Controller S              */
    0x21, 0x01,     /* bcdDevice                                  */
    0x00,           /* iManufacturer: none                        */
    0x00,           /* iProduct: none                             */
    0x00,           /* iSerialNumber: none                        */
    0x01            /* bNumConfigurations                         */
};

/* Configuration, interface and both endpoints, in the one block a
 * GET_DESCRIPTOR(CONFIGURATION) returns. wTotalLength covers all of it. */
static const uint8_t s_config_desc[32] = {
    /* configuration */
    9, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 50,
    /* interface: class 0x58 subclass 0x42, the Xbox gamepad's own */
    9, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,
    /* endpoint 0x81 IN, interrupt, 32 bytes, 4 ms */
    7, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    /* endpoint 0x02 OUT, interrupt, 32 bytes, 4 ms -- rumble */
    7, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04
};

static uint8_t s_address;
static uint8_t s_configuration;

uint8_t usb_gamepad_address(void) { return s_address; }

/* ---- control transfers ------------------------------------------------- */

#define REQ_GET_STATUS         0x00
#define REQ_CLEAR_FEATURE      0x01
#define REQ_SET_FEATURE        0x03
#define REQ_SET_ADDRESS        0x05
#define REQ_GET_DESCRIPTOR     0x06
#define REQ_GET_CONFIGURATION  0x08
#define REQ_SET_CONFIGURATION  0x09
#define REQ_SET_INTERFACE      0x0B

#define DESC_DEVICE            0x01
#define DESC_CONFIGURATION     0x02
#define DESC_STRING            0x03

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len,
                    uint16_t wLength)
{
    /* A device sends the smaller of what was asked for and what it has. */
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

int usb_gamepad_control(const UsbSetup *setup, uint8_t *out, int max)
{
    int is_in = (setup->bmRequestType & 0x80) != 0;
    int type  = (setup->bmRequestType >> 5) & 3;   /* 0 standard, 1 class */

    if (type == 0) {
        switch (setup->bRequest) {
        case REQ_GET_DESCRIPTOR:
            switch (setup->wValue >> 8) {
            case DESC_DEVICE:
                return copy_out(out, max, s_device_desc,
                                (int)sizeof s_device_desc, setup->wLength);
            case DESC_CONFIGURATION:
                return copy_out(out, max, s_config_desc,
                                (int)sizeof s_config_desc, setup->wLength);
            case DESC_STRING:
                /* No string descriptors. Stalling is the correct answer and
                 * the one a host expects; returning an empty descriptor gets
                 * read as a malformed one. */
                return -1;
            default:
                return -1;
            }

        case REQ_SET_ADDRESS:
            s_address = (uint8_t)(setup->wValue & 0x7F);
            return 0;                    /* zero-length status stage */

        case REQ_SET_CONFIGURATION:
            s_configuration = (uint8_t)(setup->wValue & 0xFF);
            return 0;

        case REQ_GET_CONFIGURATION:
            if (!is_in || max < 1) return -1;
            out[0] = s_configuration;
            return 1;

        case REQ_GET_STATUS:
            /* Bus-powered, no remote wakeup. */
            if (!is_in || max < 2) return -1;
            out[0] = 0; out[1] = 0;
            return 2;

        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
        case REQ_SET_INTERFACE:
            return 0;

        default:
            return -1;
        }
    }

    /* Class requests. The Xbox pad answers a vendor-defined capabilities
     * request on the interface; anything else is not ours to guess at. */
    return -1;
}

/* ---- the input report -------------------------------------------------- */

/* The host's own pad, through the layer that already maps one to XInput.
 * A real controller plugged into the PC drives this emulated one. */
#include "../input/xinput_xbox.h"

/*
 * The Xbox report is 20 bytes and fixed:
 *
 *   0      report id, always 0
 *   1      length, always 20
 *   2      digital buttons: dpad, start, back, thumb clicks
 *   3      reserved
 *   4..11  analog buttons A B X Y Black White, then the two triggers
 *   12..19 four signed 16-bit stick axes, little endian
 */
int usb_gamepad_report(uint8_t *out, int max)
{
    XBOX_INPUT_STATE state;
    const XBOX_GAMEPAD *g;
    int i;

    if (max < 20)
        return 0;
    memset(out, 0, 20);
    out[0] = 0;
    out[1] = 20;

    /* A disconnected host pad is not an error here: the device is present on
     * the bus either way, it just reports nothing pressed -- so build a zeroed
     * report rather than bailing, which also lets the force-pad test below
     * drive input on a host with no pad at all. */
    if (xbox_InputGetState(0, &state) == 0) {
        g = &state.Gamepad;
        out[2] = (uint8_t)(g->wButtons & 0xFF);
        out[3] = (uint8_t)((g->wButtons >> 8) & 0xFF);
        for (i = 0; i < 8; i++)
            out[4 + i] = g->bAnalogButtons[i];
        out[12] = (uint8_t)(g->sThumbLX & 0xFF);
        out[13] = (uint8_t)((g->sThumbLX >> 8) & 0xFF);
        out[14] = (uint8_t)(g->sThumbLY & 0xFF);
        out[15] = (uint8_t)((g->sThumbLY >> 8) & 0xFF);
        out[16] = (uint8_t)(g->sThumbRX & 0xFF);
        out[17] = (uint8_t)((g->sThumbRX >> 8) & 0xFF);
        out[18] = (uint8_t)(g->sThumbRY & 0xFF);
        out[19] = (uint8_t)((g->sThumbRY >> 8) & 0xFF);
    }

    /* Progression test: force a button so an input-gated screen advances with
     * no real pad. RECOMP_FORCE_PAD=start|a|both (default both). Held only after
     * a grace period, so the guest first sees "released" and then a press EDGE
     * -- a title that advances on a Start press, not a level, still fires. */
    {
        static int mode = -1;
        static unsigned calls;
        if (mode < 0) {
            const char *s = getenv("RECOMP_FORCE_PAD");
            if (!s)                              mode = 0;
            else if (strstr(s, "start"))         mode = 1;
            else if (strcmp(s, "a") == 0)        mode = 2;
            else                                 mode = 3;   /* both / anything */
        }
        if (mode && ++calls > 180) {
            if (mode & 1) out[2] |= 0x10;        /* Start (digital bit 4)      */
            if (mode & 2) out[4]  = 0xFF;        /* A (analog button 0)        */
        }
    }

    /* Is the guest actually polling input? This function is only reached when
     * the guest's USB driver runs an IN transfer on the pad's interrupt
     * endpoint, so a rising poll count proves the whole input chain (OHCI ->
     * report -> guest XID driver) is live -- and the button bytes prove the
     * forced/real press is what the guest reads. RECOMP_PAD_LOG=1. */
    {
        static int plog = -1;
        static unsigned pn, prev_pressed;
        unsigned pressed;
        if (plog < 0) plog = getenv("RECOMP_PAD_LOG") != NULL;
        if (plog) {
            pressed = out[2] | out[3] | out[4] | out[5] | out[6] | out[7];
            pn++;
            if ((pressed && !prev_pressed) || (pn % 1000u) == 0u) {
                fprintf(stderr, "  [PAD] poll#%u digital=%02X%02X A=%02X B=%02X"
                        " X=%02X Y=%02X %s\n", pn, out[3], out[2], out[4],
                        out[5], out[6], out[7],
                        pressed ? "<-- PRESS reaches guest" : "(idle)");
                fflush(stderr);
            }
            prev_pressed = pressed;
        }
    }
    return 20;
}
