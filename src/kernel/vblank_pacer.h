/*
 * vblank_pacer.h - drift-free vertical-blank cadence for the NV2A frame clock.
 *
 * WHY
 * ---
 * Xbox titles pace their game loop off the vblank interrupt: the guest's vblank
 * callback bumps a frame counter and the main loop spins until it has advanced
 * by 1 (60 fps) or 2 (30 fps). So the host vblank RATE is the game's clock --
 * a slow vblank makes the whole game run slow, independent of CPU speed.
 *
 * The first implementation ticked from a Sleep(10) loop against a
 * GetTickCount64() deadline of now+16 ms. Deadline "now + period" drifts by one
 * loop quantum per vblank, so the real period was ~20 ms (~50 Hz, measured on
 * the RP6) -- the game ran ~17-20% slow.
 *
 * HOW
 * ---
 * Absolute deadlines on a monotonic nanosecond clock: next += period, never
 * next = now + period. A late loop iteration fires immediately and the NEXT
 * deadline is unchanged, so short stalls are caught up and the long-run rate is
 * exact. A stall longer than VBL_MAX_BACKLOG periods (a debugger break, a
 * multi-second load hitch) resyncs instead of firing a burst, and the skipped
 * vblanks are counted in `dropped` (real hardware would also "lose" them from
 * the game's point of view if it were not servicing interrupts).
 *
 * Pure logic, no OS calls: unit-tested in tests/vblank_pacer.
 */
#ifndef VBLANK_PACER_H
#define VBLANK_PACER_H

#include <stdint.h>

/* NTSC Xbox: 60000/1001 Hz = 59.94 Hz -> 16,683,333 ns. */
#define VBL_PERIOD_NTSC_NS  16683333LL
/* PAL-50 titles: 20 ms. */
#define VBL_PERIOD_PAL_NS   20000000LL
/* Max vblanks we are willing to deliver back-to-back to catch up. */
#define VBL_MAX_BACKLOG     4

typedef struct vbl_pacer {
    int64_t  period_ns;   /* vblank period */
    int64_t  next_ns;     /* absolute deadline of the next vblank; 0 = unstarted */
    uint64_t fired;       /* vblanks delivered */
    uint64_t dropped;     /* vblanks skipped by a resync after a long stall */
} vbl_pacer;

static inline void vbl_pacer_init(vbl_pacer *p, int64_t period_ns)
{
    p->period_ns = period_ns > 0 ? period_ns : VBL_PERIOD_NTSC_NS;
    p->next_ns = 0;
    p->fired = 0;
    p->dropped = 0;
}

/* Returns 1 if a vblank is due at `now_ns` (and consumes it), else 0.
 * Call it repeatedly; when it returns 1 call it again before sleeping, since
 * a late caller may owe more than one vblank. */
static inline int vbl_pacer_poll(vbl_pacer *p, int64_t now_ns)
{
    if (p->next_ns == 0) {                     /* first call: start the clock */
        p->next_ns = now_ns + p->period_ns;
        return 0;
    }
    if (now_ns < p->next_ns)
        return 0;
    if (now_ns - p->next_ns > (int64_t)VBL_MAX_BACKLOG * p->period_ns) {
        /* Too far behind: skip the backlog, keep phase relative to now. */
        int64_t behind = (now_ns - p->next_ns) / p->period_ns;
        p->dropped += (uint64_t)behind;
        p->next_ns += behind * p->period_ns;
    }
    p->next_ns += p->period_ns;
    p->fired++;
    return 1;
}

/* Nanoseconds until the next deadline (0 if already due, or unstarted). */
static inline int64_t vbl_pacer_wait_ns(const vbl_pacer *p, int64_t now_ns)
{
    if (p->next_ns == 0 || now_ns >= p->next_ns)
        return 0;
    return p->next_ns - now_ns;
}

#endif /* VBLANK_PACER_H */
