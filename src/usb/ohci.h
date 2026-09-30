/*
 * ohci.h -- the Xbox USB host controllers, with a gamepad plugged in.
 *
 * A title reaches its gamepad through XAPI, which is statically linked into
 * the image and drives the OHCI controller registers directly rather than
 * going through anything this runtime can shim. So the runtime is the
 * hardware: two OHCI 1.0a controllers whose registers answer (through
 * platform/mmio_trap.c), which walk the endpoint and transfer descriptor lists
 * the driver builds, and which interrupt through the kernel's device lines.
 * An Xbox controller (usb_gamepad.c) sits on the first controller's root port
 * 3, which XAPI calls player 1, and reports the host's pad (xbox_InputGetState).
 *
 * Written from the OHCI 1.0a specification and traces of the titles' own
 * drivers. See ohci.c.
 */
#ifndef XBOX_OHCI_H
#define XBOX_OHCI_H

#include <stdint.h>

/* The two MCPX host controllers, as the XDK addresses them. */
#define XBOX_OHCI0_BASE   0xFED00000u
#define XBOX_OHCI1_BASE   0xFED08000u
#define XBOX_OHCI_SIZE    0x00001000u   /* 4 KB, the length XAPI registers */

/* Bring the controllers up: trap their registers and start the controller
 * thread. Call once guest memory is mapped and before the title runs. Safe to
 * call more than once; does nothing unless RECOMP_USB is set.
 * RECOMP_USB_TRACE=1 logs register accesses and control requests. */
void xbox_OhciInit(void);

/* 1 if the address is inside a controller this model owns. */
int  xbox_OhciOwnsAddress(uint32_t xbox_va);

/* Report counts at exit, so a run says whether the driver ever looked. */
void xbox_OhciReport(void);

#endif /* XBOX_OHCI_H */
