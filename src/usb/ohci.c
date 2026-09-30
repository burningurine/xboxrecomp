/*
 * ohci.c -- the MCPX's two OHCI 1.0a USB host controllers, with an Xbox
 * controller plugged into the first.
 *
 * A title reaches its gamepad through XAPI, which is statically linked into
 * the image and drives the controller registers directly, so the only way to
 * give every title a working pad -- without finding and patching each one's
 * input code -- is to be the hardware XAPI expects: a controller whose
 * registers answer, which walks the endpoint and transfer descriptor lists the
 * driver builds in memory, and which interrupts when transfers complete.
 *
 * Register semantics and the list walking follow the OHCI 1.0a specification
 * (sections 4 and 7). Where the console's own driver has an opinion, the
 * comments say what it does. Written from the specification and from traces
 * of the titles' own drivers (Half-Life 2, Blinx).
 *
 * The console, as XAPI sees it:
 *   USB0  registers 0xFED00000, interrupt vector 1, four root hub ports, which
 *         are the four controller sockets. XAPI calls root port 3 player 1,
 *         then 4, 1, 2 (xemu wires its pads the same way).
 *   USB1  registers 0xFED08000, vector 9, nothing plugged in.
 * The pad here sits on USB0 root port 3.
 *
 * Addresses in the controller's registers and descriptors are physical. The
 * driver makes them with MmGetPhysicalAddress: its own lists and the HCCA
 * come from the contiguous window (0x80000000+P -> P), while data buffers can
 * be anywhere its callers keep them -- XAPI's setup packets live in the
 * image. xbox_PhysToHost follows each back the way the kernel converted it.
 *
 * Threads: the registers are served on the guest thread that touched them
 * (a trapped access, platform/mmio_trap.c); the lists are walked by one
 * controller thread, once per millisecond frame. A spinlock keeps the two
 * apart -- the register side runs inside a fault handler, where a sleeping
 * lock is the wrong tool. Interrupts go out through the kernel's device
 * interrupt lines (xbox_SetInterruptLine): the kernel timer thread calls the
 * title's ISR while a line is high, with the guest stack and TIB that needs.
 *
 * Off unless RECOMP_USB is set. RECOMP_USB_TRACE=1 logs register accesses and
 * control requests.
 */
#include "ohci.h"
#include "usb_gamepad.h"
#include "../kernel/kernel.h"
#include "../kernel/xbox_memory_layout.h"
#include "../platform/mmio_trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

extern void xbox_SetInterruptLine(uint32_t vector, int level);

/* ---- OHCI 1.0a operational registers, by byte offset ------------------- */
#define HcRevision              0x00
#define HcControl               0x04
#define HcCommandStatus         0x08
#define HcInterruptStatus       0x0C
#define HcInterruptEnable       0x10
#define HcInterruptDisable      0x14
#define HcHCCA                  0x18
#define HcPeriodCurrentED       0x1C
#define HcControlHeadED         0x20
#define HcControlCurrentED      0x24
#define HcBulkHeadED            0x28
#define HcBulkCurrentED         0x2C
#define HcDoneHead              0x30
#define HcFmInterval            0x34
#define HcFmRemaining           0x38
#define HcFmNumber              0x3C
#define HcPeriodicStart         0x40
#define HcLSThreshold           0x44
#define HcRhDescriptorA         0x48
#define HcRhDescriptorB         0x4C
#define HcRhStatus              0x50
#define HcRhPortStatus1         0x54
#define OHCI_REG_MAX            0x64    /* through HcRhPortStatus4 */

/* HcControl */
#define CTL_PLE                 0x00000004u   /* periodic list enable        */
#define CTL_IE                  0x00000008u   /* isochronous enable          */
#define CTL_CLE                 0x00000010u   /* control list enable         */
#define CTL_BLE                 0x00000020u   /* bulk list enable            */
#define CTL_HCFS_MASK           0x000000C0u   /* functional state            */
#define CTL_HCFS_RESET          0x00000000u
#define CTL_HCFS_OPERATIONAL    0x00000080u
#define CTL_HCFS_SUSPEND        0x000000C0u

/* HcCommandStatus */
#define CS_HCR                  0x00000001u   /* host controller reset       */
#define CS_CLF                  0x00000002u   /* control list filled         */
#define CS_BLF                  0x00000004u   /* bulk list filled            */

/* HcInterruptStatus / Enable */
#define INTR_SO                 0x00000001u   /* scheduling overrun          */
#define INTR_WDH                0x00000002u   /* writeback done head         */
#define INTR_SF                 0x00000004u   /* start of frame              */
#define INTR_RD                 0x00000008u   /* resume detected             */
#define INTR_UE                 0x00000010u   /* unrecoverable error         */
#define INTR_FNO                0x00000020u   /* frame number overflow       */
#define INTR_RHSC               0x00000040u   /* root hub status change      */
#define INTR_OC                 0x40000000u   /* ownership change            */
#define INTR_MIE                0x80000000u   /* master interrupt enable     */
#define INTR_SOURCES            0x4000007Fu

/* HcRhPortStatus, as read */
#define PORT_CCS                0x00000001u   /* current connect status      */
#define PORT_PES                0x00000002u   /* port enable status          */
#define PORT_PSS                0x00000004u   /* port suspend status         */
#define PORT_PRS                0x00000010u   /* port reset status           */
#define PORT_PPS                0x00000100u   /* port power status           */
#define PORT_CSC                0x00010000u   /* connect status change       */
#define PORT_PESC               0x00020000u   /* enable status change        */
#define PORT_PSSC               0x00040000u   /* suspend status change       */
#define PORT_PRSC               0x00100000u   /* reset status change         */
#define PORT_CHANGES            0x001F0000u
/* ... and as written: set/clear requests by bit position, not a value. */
#define PORT_W_CLEAR_ENABLE     0x00000001u
#define PORT_W_SET_ENABLE       0x00000002u
#define PORT_W_SET_SUSPEND      0x00000004u
#define PORT_W_CLEAR_SUSPEND    0x00000008u
#define PORT_W_SET_RESET        0x00000010u
#define PORT_W_SET_POWER        0x00000100u
#define PORT_W_CLEAR_POWER      0x00000200u

/* The HCCA, the 256-byte block the driver shares with the controller. */
#define HCCA_INTERRUPT_TABLE    0x00          /* 32 ED pointers              */
#define HCCA_FRAME_NUMBER       0x80          /* 16 bits                     */
#define HCCA_DONE_HEAD          0x84

/* Endpoint descriptor: four dwords. */
#define ED_FA(d0)               ((d0) & 0x7Fu)
#define ED_EN(d0)               (((d0) >> 7) & 0xFu)
#define ED_D(d0)                (((d0) >> 11) & 3u)
#define ED_SKIP                 (1u << 14)
#define ED_ISO                  (1u << 15)
#define ED_MPS(d0)              (((d0) >> 16) & 0x7FFu)
#define ED_HEAD_HALT            0x1u
#define ED_HEAD_CARRY           0x2u
#define PTR_MASK                0xFFFFFFF0u

/* General transfer descriptor: four dwords; the condition code is in the top
 * four bits of the first. */
#define TD_R                    (1u << 18)    /* buffer rounding: short OK   */
#define TD_DP(d0)               (((d0) >> 19) & 3u)
#define TD_DI(d0)               (((d0) >> 21) & 7u)
#define TD_T(d0)                (((d0) >> 24) & 3u)
#define PID_SETUP               0u
#define PID_OUT                 1u
#define PID_IN                  2u

#define CC_NOERROR              0u
#define CC_STALL                4u
#define CC_NOT_RESPONDING       5u
#define CC_DATA_UNDERRUN        9u

#define OHCI_PORTS              4
#define PAD_PORT                2             /* root port 3: XAPI's player 1 */

typedef struct {
    UsbSetup setup;
    int      active;          /* a SETUP was seen; the transfer is open    */
    int      has_in_data;     /* a device-to-host request's answer is ready */
    int      stalled;
    uint8_t  in[256];         /* that answer                                */
    int      in_len, in_off;
    uint8_t  out[64];         /* a host-to-device request's data stage      */
    int      out_len;
} ControlXfer;

typedef struct {
    uint32_t base;            /* guest VA of the register block             */
    uint32_t vector;
    int      index;
    uint32_t reg[OHCI_REG_MAX / 4];
    volatile long lock;
    int      line;            /* what the interrupt line was last set to    */
    int      plugged;         /* the pad has arrived on PAD_PORT            */
    uint64_t frame;           /* frames since the controller started        */
    uint64_t last_ms;         /* guest clock of the last frame processed    */
    uint32_t done_head;       /* retired TDs not yet written back, newest first */
    int      done_delay;      /* frames until they are (7: no interrupt)    */
    unsigned reads, writes, frames_run;
    ControlXfer ctl;          /* endpoint 0 of the one device               */
} OhciController;

static OhciController s_hc[2];
static int s_enabled;
static int s_trace;

/* ---- small helpers ----------------------------------------------------- */

static void hc_lock(OhciController *hc)
{
#if defined(_MSC_VER)
    while (_InterlockedCompareExchange(&hc->lock, 1, 0) != 0)
        YieldProcessor();
#else
    while (__atomic_exchange_n(&hc->lock, 1, __ATOMIC_ACQUIRE))
        ;
#endif
}

static void hc_unlock(OhciController *hc)
{
#if defined(_MSC_VER)
    _InterlockedExchange(&hc->lock, 0);
#else
    __atomic_store_n(&hc->lock, 0, __ATOMIC_RELEASE);
#endif
}

#define REG(hc, off) ((hc)->reg[(off) / 4])

/* Physical address -> host pointer, for `bytes` bytes (kernel/
 * xbox_memory_layout.c, xbox_PhysToHost: the contiguous window, or the
 * buffer's own address when MmGetPhysicalAddress converted it that way).
 * Every address here came from guest memory, so none of them are trusted:
 * the driver has been seen writing 0xCCCCCCCC (MSVC's uninitialised fill)
 * into HcControlHeadED during bring-up, and following that would take the
 * runtime down with a fault the title itself never had. */
static uint8_t *phys(uint32_t pa, uint32_t bytes)
{
    return xbox_PhysToHost(pa, bytes);
}

static uint32_t rd32(uint32_t pa)
{
    uint8_t *p = phys(pa, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

static void wr32(uint32_t pa, uint32_t v)
{
    uint8_t *p = phys(pa, 4);
    if (p) memcpy(p, &v, 4);
}

/* Copy between a TD's buffer and `data`. A TD's buffer can cross one page
 * boundary, and then its second part is the page BE is in, not the next page
 * after CBP (OHCI 4.3.1.3.1): physical pages need not be consecutive. */
static void td_buffer_io(uint32_t cbp, uint32_t be, uint8_t *data, int len, int to_guest)
{
    int first = (int)(0x1000u - (cbp & 0xFFFu));
    uint8_t *p;

    if (first > len) first = len;
    p = phys(cbp, (uint32_t)first);
    if (p && first > 0) {
        if (to_guest) memcpy(p, data, (size_t)first);
        else          memcpy(data, p, (size_t)first);
    }
    if (len > first) {
        uint32_t second = be & ~0xFFFu;
        p = phys(second, (uint32_t)(len - first));
        if (p) {
            if (to_guest) memcpy(p, data + first, (size_t)(len - first));
            else          memcpy(data + first, p, (size_t)(len - first));
        }
    }
}

static int td_buffer_len(uint32_t cbp, uint32_t be)
{
    if (!cbp)
        return 0;
    if ((cbp & ~0xFFFu) == (be & ~0xFFFu))
        return (be >= cbp) ? (int)(be - cbp + 1) : 0;
    return (int)(0x1000u - (cbp & 0xFFFu)) + (int)(be & 0xFFFu) + 1;
}

/* The interrupt line follows the status: asserted while an enabled source is
 * pending and the master enable is set. Level-triggered, as OHCI is: the ISR
 * clears the status, the line drops, the kernel stops calling it. */
static void update_line(OhciController *hc)
{
    uint32_t en = REG(hc, HcInterruptEnable);
    int level = (en & INTR_MIE) && (REG(hc, HcInterruptStatus) & en & INTR_SOURCES);
    if (level != hc->line) {
        hc->line = level;
        xbox_SetInterruptLine(hc->vector, level);
    }
}

/* ---- the device on each port ------------------------------------------- */

/* The pad answers at its address once the port is enabled; nothing else is
 * plugged in anywhere. */
static int device_at(OhciController *hc, uint32_t fa)
{
    return hc->index == 0 && hc->plugged
        && (REG(hc, HcRhPortStatus1 + 4 * PAD_PORT) & PORT_PES)
        && fa == usb_gamepad_address();
}

/* ---- register semantics ------------------------------------------------ */

static void hc_reset(OhciController *hc)
{
    int p;

    memset(hc->reg, 0, sizeof hc->reg);
    REG(hc, HcRevision)      = 0x00000010u;      /* OHCI 1.0             */
    REG(hc, HcControl)       = CTL_HCFS_RESET;
    REG(hc, HcFmInterval)    = 0x27782EDFu;      /* 11999, FSMPS default */
    REG(hc, HcPeriodicStart) = 0x00003E67u;      /* 90% of the frame     */
    REG(hc, HcLSThreshold)   = 0x00000628u;
    /* Root hub: OHCI_PORTS downstream, no power switching (so a driver does
     * not wait on a power-on sequence there is nothing to switch), no
     * over-current reporting. */
    REG(hc, HcRhDescriptorA) = (uint32_t)OHCI_PORTS | (1u << 9) | (1u << 12);
    REG(hc, HcRhDescriptorB) = 0;
    for (p = 0; p < OHCI_PORTS; p++)
        REG(hc, HcRhPortStatus1 + 4 * p) = PORT_PPS;
    /* The pad arrives again once the driver is running (see hc_frame). */
    hc->plugged = 0;
    hc->done_head = 0;
    hc->done_delay = 7;
    memset(&hc->ctl, 0, sizeof hc->ctl);
    if (hc->index == 0)
        usb_gamepad_reset();
}

static uint64_t ohci_read(void *dev, uint32_t off, unsigned size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u, v;

    hc_lock(hc);
    hc->reads++;
    v = (aligned < OHCI_REG_MAX) ? hc->reg[aligned / 4] : 0;
    if (aligned == HcFmRemaining) {
        /* Counts down through the frame; the top bit toggles with each
         * frame's FIT. Nothing the drivers seen here depend on. */
        uint32_t fi = REG(hc, HcFmInterval) & 0x3FFFu;
        v = (fi - (uint32_t)((GetTickCount64() * 12u) % (fi + 1)))
          | ((uint32_t)(hc->frame & 1) << 31);
    }
    hc_unlock(hc);

    if (size < 4) {
        v >>= (off & 3u) * 8u;
        v &= (size == 1) ? 0xFFu : 0xFFFFu;
    }
    if (s_trace) {
        static unsigned n;
        if (n++ < 400)
            fprintf(stderr, "  [OHCI%d] read  +0x%02X = %08X\n", hc->index, off, v);
    }
    return v;
}

static void write_port(OhciController *hc, int port, uint32_t v)
{
    uint32_t *ps = &hc->reg[(HcRhPortStatus1 + 4 * port) / 4];

    if (v & PORT_W_CLEAR_ENABLE) *ps &= ~PORT_PES;
    if (v & PORT_W_SET_ENABLE)   *ps |= (*ps & PORT_CCS) ? PORT_PES : 0;
    if (v & PORT_W_SET_SUSPEND)  *ps |= (*ps & PORT_CCS) ? PORT_PSS : 0;
    if (v & PORT_W_CLEAR_SUSPEND) {
        if (*ps & PORT_PSS) *ps |= PORT_PSSC;
        *ps &= ~PORT_PSS;
    }
    if (v & PORT_W_SET_POWER)    *ps |= PORT_PPS;
    if (v & PORT_W_CLEAR_POWER)  *ps &= ~PORT_PPS;
    if (v & PORT_W_SET_RESET) {
        /* A reset completes at once -- there is no wire to settle -- so PRS
         * is never seen set. What the driver checks afterwards is that a
         * present device came back enabled, at address 0, and that the
         * reset-change bit says the reset finished. */
        if (*ps & PORT_CCS) {
            *ps |= PORT_PES;
            if (hc->index == 0 && port == PAD_PORT) {
                usb_gamepad_reset();
                memset(&hc->ctl, 0, sizeof hc->ctl);
            }
        }
        *ps |= PORT_PRSC;
    }
    /* The change bits are write-1-to-clear, in the high half. */
    *ps &= ~(v & PORT_CHANGES);
    if (*ps & PORT_CHANGES)
        REG(hc, HcInterruptStatus) |= INTR_RHSC;
}

static void ohci_write(void *dev, uint32_t off, uint64_t val, unsigned size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u, v = (uint32_t)val;
    uint32_t *r;

    if (size < 4) {           /* merge a narrow write into the register */
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
        uint32_t cur = (aligned < OHCI_REG_MAX) ? hc->reg[aligned / 4] : 0;
        v = (cur & ~mask) | ((v << shift) & mask);
        /* For the write-1 registers only the written bytes may act. */
        if (aligned == HcInterruptStatus || aligned == HcInterruptEnable
         || aligned == HcInterruptDisable || aligned == HcCommandStatus
         || aligned >= HcRhStatus)
            v = (v & mask);
    }
    if (s_trace) {
        static unsigned n;
        if (n++ < 400)
            fprintf(stderr, "  [OHCI%d] write +0x%02X = %08X\n", hc->index, off, v);
    }
    if (aligned >= OHCI_REG_MAX)
        return;

    hc_lock(hc);
    hc->writes++;
    r = &hc->reg[aligned / 4];
    switch (aligned) {
    case HcRevision:
    case HcFmRemaining:
    case HcFmNumber:
    case HcDoneHead:
    case HcRhDescriptorB:
        break;                                   /* read-only here        */

    case HcControl:
        *r = v;
        break;

    case HcCommandStatus:
        /* HCR is self-clearing: the controller resets and drops the bit,
         * and a driver polls for exactly that -- it is the first thing a
         * driver does, so leaving it set is a hang before anything else. */
        if (v & CS_HCR) {
            hc_reset(hc);
            v &= ~CS_HCR;
        }
        *r |= v & (CS_CLF | CS_BLF);
        break;

    case HcInterruptStatus:
        *r &= ~v;                                /* write 1 to clear      */
        break;

    case HcInterruptEnable:
        *r |= v;
        break;

    case HcInterruptDisable:
        REG(hc, HcInterruptEnable) &= ~v;
        break;

    case HcRhDescriptorA:
        /* NumberDownstreamPorts is ours; the driver may set the power and
         * over-current policy bits above it. */
        *r = (*r & 0x000000FFu) | (v & ~0x000000FFu);
        break;

    case HcRhStatus:
        /* LPSC (bit 16) powers every port; nothing else here has an effect. */
        if (v & 0x00010000u) {
            int p;
            for (p = 0; p < OHCI_PORTS; p++)
                REG(hc, HcRhPortStatus1 + 4 * p) |= PORT_PPS;
        }
        break;

    case HcRhPortStatus1:
    case HcRhPortStatus1 + 4:
    case HcRhPortStatus1 + 8:
    case HcRhPortStatus1 + 12:
        write_port(hc, (int)(aligned - HcRhPortStatus1) / 4, v);
        break;

    default:                                     /* plain storage         */
        *r = v;
        break;
    }
    update_line(hc);
    hc_unlock(hc);
}

/* ---- transfers ---------------------------------------------------------- */

/* Move one general TD for the device. Returns the condition code, or -1 for a
 * NAK (the TD stays where it is and is retried in a later frame). *moved is
 * the byte count that went across. */
static int do_td(OhciController *hc, uint32_t ed0, uint32_t td, int *moved)
{
    uint32_t info = rd32(td), cbp = rd32(td + 4), be = rd32(td + 12);
    uint32_t pid = ED_D(ed0);
    int len = td_buffer_len(cbp, be), n;
    uint32_t ep = ED_EN(ed0);
    ControlXfer *c = &hc->ctl;
    uint8_t buf[256];

    *moved = 0;
    if (pid == 0 || pid == 3)
        pid = TD_DP(info);                       /* direction from the TD */
    if (!device_at(hc, ED_FA(ed0)))
        return CC_NOT_RESPONDING;
    if (len > (int)sizeof buf)
        len = (int)sizeof buf;                   /* nothing here is bigger */

    if (ep == 0) {                               /* ---- control ---------- */
        if (pid == PID_SETUP) {
            uint8_t s[8] = {0};
            td_buffer_io(cbp, be, s, len < 8 ? len : 8, 0);
            memset(c, 0, sizeof *c);
            c->setup.bmRequestType = s[0];
            c->setup.bRequest      = s[1];
            c->setup.wValue        = (uint16_t)(s[2] | (s[3] << 8));
            c->setup.wIndex        = (uint16_t)(s[4] | (s[5] << 8));
            c->setup.wLength       = (uint16_t)(s[6] | (s[7] << 8));
            c->active = 1;
            if (c->setup.bmRequestType & 0x80) {
                /* Device-to-host: the answer is known now, and the data
                 * stage may take it in pieces. */
                n = usb_gamepad_control(&c->setup, NULL, 0, c->in, (int)sizeof c->in);
                if (n < 0) c->stalled = 1;
                else { c->in_len = n; c->has_in_data = 1; }
            }
            if (s_trace) {
                fprintf(stderr, "  [OHCI%d] SETUP %02X %02X value %04X index %04X"
                        " len %u%s\n", hc->index, c->setup.bmRequestType,
                        c->setup.bRequest, c->setup.wValue, c->setup.wIndex,
                        c->setup.wLength, c->stalled ? " -> STALL" : "");
                fflush(stderr);
            }
            *moved = len < 8 ? len : 8;
            return CC_NOERROR;
        }
        if (!c->active)
            return CC_STALL;
        if (c->stalled) {
            c->active = 0;
            return CC_STALL;
        }
        if (pid == PID_IN) {
            if (c->has_in_data) {                /* data stage            */
                n = c->in_len - c->in_off;
                if (n > len) n = len;
                if (n > 0)
                    td_buffer_io(cbp, be, c->in + c->in_off, n, 1);
                c->in_off += n;
                *moved = n;
                if (n < len && !(info & TD_R))
                    return CC_DATA_UNDERRUN;
                return CC_NOERROR;
            }
            /* The status stage of a host-to-device request: now it happens.
             * An address takes effect only after this, which is why the
             * status stage itself still went to address 0. */
            n = usb_gamepad_control(&c->setup, c->out, c->out_len, NULL, 0);
            c->active = 0;
            if (s_trace && n < 0) {
                fprintf(stderr, "  [OHCI%d] request %02X %02X -> STALL\n",
                        hc->index, c->setup.bmRequestType, c->setup.bRequest);
                fflush(stderr);
            }
            return n < 0 ? CC_STALL : CC_NOERROR;
        }
        /* OUT: the data stage of a host-to-device request, or the status
         * stage of a device-to-host one. */
        if (c->has_in_data) {
            c->active = 0;
            *moved = len;
            return CC_NOERROR;
        }
        if (len > 0) {
            int room = (int)sizeof c->out - c->out_len;
            n = len < room ? len : room;
            td_buffer_io(cbp, be, c->out + c->out_len, n, 0);
            c->out_len += n;
        }
        *moved = len;
        return CC_NOERROR;
    }

    if (pid == PID_IN) {                         /* ---- interrupt IN ----- */
        n = usb_gamepad_in((int)ep, buf, len);
        if (n == USB_NAK)
            return -1;
        if (n < 0)
            return CC_STALL;
        td_buffer_io(cbp, be, buf, n, 1);
        *moved = n;
        if (n < len && !(info & TD_R))
            return CC_DATA_UNDERRUN;
        return CC_NOERROR;
    }
    if (pid == PID_OUT) {                        /* ---- interrupt OUT ---- */
        td_buffer_io(cbp, be, buf, len, 0);
        usb_gamepad_out((int)ep, buf, len);
        *moved = len;
        return CC_NOERROR;
    }
    return CC_STALL;
}

/* Walk one ED list. Control and bulk EDs run every queued TD they can;
 * periodic EDs run one TD a frame. Returns 1 if any TD was retired. */
static int run_ed_list(OhciController *hc, uint32_t ed, int periodic)
{
    int guard = 0, retired = 0;

    while (ed && ++guard <= 256) {
        uint32_t ed0 = rd32(ed), tail = rd32(ed + 4) & PTR_MASK;
        uint32_t head = rd32(ed + 8), next_ed = rd32(ed + 12) & PTR_MASK;
        int tguard = 0;

        if (!phys(ed, 16) || (ed0 & (ED_SKIP | ED_ISO)) || (head & ED_HEAD_HALT)) {
            ed = next_ed;
            continue;
        }
        while ((head & PTR_MASK) != tail && ++tguard <= 64) {
            uint32_t td = head & PTR_MASK, info, next_td, toggle;
            int moved, cc, packets, mps;

            if (!phys(td, 16))
                break;
            info = rd32(td);
            cc = do_td(hc, ed0, td, &moved);
            if (cc < 0)
                break;                           /* NAK: try again later  */
            if (s_trace && (ED_EN(ed0) == 0 || cc != CC_NOERROR)) {
                static unsigned tn;
                if (tn++ < 300) {
                    fprintf(stderr, "  [OHCI%d] TD %08X fa %u ep %u pid %u len %d moved %d"
                            " R %u DI %u T %u -> cc %d\n", hc->index, td, ED_FA(ed0),
                            ED_EN(ed0), TD_DP(info),
                            td_buffer_len(rd32(td + 4), rd32(td + 12)), moved,
                            (info & TD_R) ? 1u : 0u, TD_DI(info), TD_T(info), cc);
                    fflush(stderr);
                }
            }

            /* The data toggle: the TD's own, or the ED's carry, flipped
             * once per packet that went across. */
            mps = (int)ED_MPS(ed0);
            packets = (moved == 0 || mps == 0) ? 1 : (moved + mps - 1) / mps;
            toggle = (TD_T(info) & 2) ? (TD_T(info) & 1) : ((head & ED_HEAD_CARRY) ? 1 : 0);
            if (cc == CC_NOERROR)
                toggle ^= (uint32_t)(packets & 1);

            /* Retire it: condition code, current buffer pointer (zero when
             * everything moved, else past what did), onto the done queue. */
            next_td = rd32(td + 8) & PTR_MASK;
            {
                uint32_t cbp = rd32(td + 4);
                int len = td_buffer_len(cbp, rd32(td + 12));
                if (moved >= len)
                    cbp = 0;
                else if (moved > 0)
                    cbp += (uint32_t)moved;      /* ponytail: within a page */
                wr32(td + 4, cbp);
            }
            wr32(td, (info & 0x0FFFFFFFu & ~(3u << 26)) | ((uint32_t)cc << 28));
            wr32(td + 8, hc->done_head);
            hc->done_head = td;
            if ((int)TD_DI(info) < hc->done_delay)
                hc->done_delay = (int)TD_DI(info);
            /* A TD retired with an error is reported at once, whatever its
             * interrupt delay (OHCI 6.4.4): the driver asked not to be woken
             * for a routine completion, not for a failed one -- and the rest
             * of that transfer sits behind the halted endpoint, so nothing
             * else would ever wake it. XAPI splits a control read into
             * 8-byte TDs, all DI 7 but the status stage, and meets a
             * DataUnderrun whenever the device has less than it asked for. */
            if (cc != CC_NOERROR)
                hc->done_delay = 0;
            retired = 1;

            head = next_td | (toggle ? ED_HEAD_CARRY : 0);
            if (cc != CC_NOERROR) {
                head |= ED_HEAD_HALT;            /* an error halts the endpoint */
                break;
            }
            if (periodic)
                break;
        }
        wr32(ed + 8, head);
        ed = next_ed;
    }
    return retired;
}

/* One 1 ms frame. */
static void hc_frame(OhciController *hc)
{
    uint32_t control = REG(hc, HcControl), hcca = REG(hc, HcHCCA) & ~0xFFu;
    uint16_t fn;

    hc->frame++;
    hc->frames_run++;
    fn = (uint16_t)hc->frame;
    if (((fn ^ (uint16_t)(fn - 1)) & 0x8000u))
        REG(hc, HcInterruptStatus) |= INTR_FNO;  /* bit 15 changed         */
    REG(hc, HcFmNumber) = fn;
    if (hcca && phys(hcca, 256)) {
        uint8_t *p = phys(hcca + HCCA_FRAME_NUMBER, 4);
        p[0] = (uint8_t)fn; p[1] = (uint8_t)(fn >> 8); p[2] = 0; p[3] = 0;
    }
    REG(hc, HcInterruptStatus) |= INTR_SF;

    /* The pad arrives once the driver is running and listening for it.
     * Presenting it earlier does not work: the driver scans the root hub
     * itself during bring-up and clears the connect change, and a reset
     * clears the interrupt status, so a device that was always there is one
     * that never arrives. A console notices the plug after the controller
     * is running, so that is when it happens here. */
    if (hc->index == 0 && !hc->plugged
     && (REG(hc, HcInterruptEnable) & (INTR_MIE | INTR_RHSC)) == (INTR_MIE | INTR_RHSC)) {
        REG(hc, HcRhPortStatus1 + 4 * PAD_PORT) |= PORT_CCS | PORT_CSC;
        REG(hc, HcInterruptStatus) |= INTR_RHSC;
        hc->plugged = 1;
        fprintf(stderr, "  [OHCI0] controller running; a gamepad arrives on root port %d\n",
                PAD_PORT + 1);
        fflush(stderr);
    }

    if ((control & CTL_PLE) && hcca) {
        uint32_t ed = rd32(hcca + HCCA_INTERRUPT_TABLE + 4u * (fn & 31u)) & PTR_MASK;
        run_ed_list(hc, ed, 1);
    }
    if ((control & CTL_CLE) && (REG(hc, HcCommandStatus) & CS_CLF)) {
        if (!run_ed_list(hc, REG(hc, HcControlHeadED) & PTR_MASK, 0))
            REG(hc, HcCommandStatus) &= ~CS_CLF;   /* nothing left queued */
    }
    if ((control & CTL_BLE) && (REG(hc, HcCommandStatus) & CS_BLF)) {
        if (!run_ed_list(hc, REG(hc, HcBulkHeadED) & PTR_MASK, 0))
            REG(hc, HcCommandStatus) &= ~CS_BLF;
    }

    /* The done queue goes back to the driver once it has taken the last one
     * (WDH clear) and the TDs' interrupt delay has run out (OHCI 6.4.4); the
     * low bit of the written head says another interrupt source is pending
     * as well. TDs that asked for no interrupt at all (DI 7) wait for one
     * that did: written back on their own, nothing would tell the driver,
     * and the next write-back would overwrite them. */
    if (hc->done_head && hc->done_delay < 7) {
        if (hc->done_delay > 0)
            hc->done_delay--;
        if (hc->done_delay == 0 && !(REG(hc, HcInterruptStatus) & INTR_WDH) && hcca) {
            uint32_t other = REG(hc, HcInterruptStatus) & REG(hc, HcInterruptEnable)
                           & INTR_SOURCES & ~INTR_WDH;
            wr32(hcca + HCCA_DONE_HEAD, hc->done_head | (other ? 1u : 0u));
            REG(hc, HcInterruptStatus) |= INTR_WDH;
            hc->done_head = 0;
            hc->done_delay = 7;
        }
    }
    REG(hc, HcDoneHead) = hc->done_head;
}

/* The controller thread. It runs the frames that have elapsed on the guest
 * clock (which stands still while the app is paused), a few at a time, and
 * sets the interrupt lines; the kernel timer thread does the calling. */
static DWORD WINAPI ohci_thread(LPVOID unused)
{
    int running = 0;

    (void)unused;
    for (;;) {
        int i;
        /* Every 2 ms while a controller runs (the pad is polled every 4),
         * and rarely while none does, so an idle bus costs next to nothing. */
        Sleep(running ? 2 : 50);
        running = 0;
        for (i = 0; i < 2; i++) {
            OhciController *hc = &s_hc[i];
            uint64_t now = GetTickCount64();
            int steps = 0;

            hc_lock(hc);
            if ((REG(hc, HcControl) & CTL_HCFS_MASK) != CTL_HCFS_OPERATIONAL) {
                hc->last_ms = now;
            } else {
                running = 1;
                if (now - hc->last_ms > 32)   /* far behind: skip ahead */
                    hc->last_ms = now - 32;
                while (hc->last_ms < now && steps++ < 32) {
                    hc->last_ms++;
                    hc_frame(hc);
                }
            }
            update_line(hc);
            hc_unlock(hc);
        }
    }
    return 0;
}

/* ---- bring-up ----------------------------------------------------------- */

void xbox_OhciInit(void)
{
    static int done;
    int i;

    if (done)
        return;
    done = 1;
    {
        const char *on = getenv("RECOMP_USB");
        if (!on || !strcmp(on, "0"))
            return;
    }
    s_trace = getenv("RECOMP_USB_TRACE") != NULL;

    for (i = 0; i < 2; i++) {
        OhciController *hc = &s_hc[i];
        memset(hc, 0, sizeof *hc);
        hc->index  = i;
        hc->base   = i ? XBOX_OHCI1_BASE : XBOX_OHCI0_BASE;
        hc->vector = i ? 9u : 1u;
        hc_reset(hc);
        /* The registers have to fault to be answered: the aperture is plain
         * memory otherwise, and a controller whose registers read as zeros
         * out of RAM is exactly what this exists to end. */
        if (mmio_trap_register(hc->base, XBOX_OHCI_SIZE, ohci_read, ohci_write, hc) != 0) {
            fprintf(stderr, "  OHCI: cannot trap 0x%08X; USB off\n", hc->base);
            return;
        }
    }
    s_enabled = 1;
    fprintf(stderr, "  OHCI: two controllers at 0x%08X (vector 1) and 0x%08X "
                    "(vector 9), %d ports each; a gamepad on USB0 root port %d\n",
            XBOX_OHCI0_BASE, XBOX_OHCI1_BASE, OHCI_PORTS, PAD_PORT + 1);
    fflush(stderr);
    CloseHandle(CreateThread(NULL, 0, ohci_thread, NULL, 0, NULL));
}

int xbox_OhciOwnsAddress(uint32_t xbox_va)
{
    int i;
    if (!s_enabled)
        return 0;
    for (i = 0; i < 2; i++)
        if (xbox_va >= s_hc[i].base && xbox_va < s_hc[i].base + XBOX_OHCI_SIZE)
            return 1;
    return 0;
}

void xbox_OhciReport(void)
{
    int i;

    if (!s_enabled)
        return;
    for (i = 0; i < 2; i++)
        fprintf(stderr, "  [OHCI%d] %u reads, %u writes, %u frames; HcControl=%08X "
                        "HcIntStatus=%08X HcIntEnable=%08X port%d=%08X\n",
                i, s_hc[i].reads, s_hc[i].writes, s_hc[i].frames_run,
                s_hc[i].reg[HcControl / 4], s_hc[i].reg[HcInterruptStatus / 4],
                s_hc[i].reg[HcInterruptEnable / 4], PAD_PORT + 1,
                s_hc[i].reg[(HcRhPortStatus1 + 4 * PAD_PORT) / 4]);
    fflush(stderr);
}
