/* Unit test for src/kernel/vblank_pacer.h: the vblank cadence is the guest's
 * game clock, so its long-run rate must be exact regardless of how coarsely or
 * jittery the host loop polls it. */
#include <stdio.h>
#include <stdlib.h>
#include "vblank_pacer.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Drive the pacer for `secs` of simulated time, polling every `step_ns` plus
 * pseudo-random jitter in [0, jitter_ns). Returns vblanks fired. */
static uint64_t run(vbl_pacer *p, int64_t t0, double secs, int64_t step_ns, int64_t jitter_ns, int64_t *t_end)
{
    int64_t t = t0, end = t0 + (int64_t)(secs * 1e9);
    unsigned seed = 12345;
    uint64_t start = p->fired;
    while (t < end) {
        while (vbl_pacer_poll(p, t)) {}
        seed = seed * 1103515245u + 12345u;
        t += step_ns + (jitter_ns ? (int64_t)(seed % (unsigned)jitter_ns) : 0);
    }
    if (t_end) *t_end = t;
    return p->fired - start;
}

int main(void)
{
    vbl_pacer p;

    /* 1. Ideal sleeper that wakes exactly at each deadline: 10 s -> 599 vblanks. */
    vbl_pacer_init(&p, VBL_PERIOD_NTSC_NS);
    {
        int64_t t = 1000000000LL; uint64_t n = 0;
        vbl_pacer_poll(&p, t);                 /* start */
        while (t < 1000000000LL + 10000000000LL) {
            t += vbl_pacer_wait_ns(&p, t);
            n += (uint64_t)vbl_pacer_poll(&p, t);
        }
        CHECK(n >= 598 && n <= 600, "ideal: %llu vblanks in 10s (want 599+-1)", (unsigned long long)n);
    }

    /* 2. The OLD failure mode: a 10 ms poll quantum + 0..6 ms jitter. The old
     *    now+16 deadline produced ~50 Hz here; absolute deadlines must hold 59.94. */
    vbl_pacer_init(&p, VBL_PERIOD_NTSC_NS);
    {
        uint64_t n = run(&p, 5, 10.0, 10000000LL, 6000000LL, NULL);
        CHECK(n >= 597 && n <= 601, "coarse jittery poll: %llu vblanks in 10s (want ~599)", (unsigned long long)n);
        CHECK(p.dropped == 0, "coarse poll dropped %llu", (unsigned long long)p.dropped);
    }

    /* 3. A 50 ms stall (3 periods) is caught up without drops. */
    vbl_pacer_init(&p, VBL_PERIOD_NTSC_NS);
    {
        int64_t t; uint64_t a = run(&p, 0, 1.0, 1000000LL, 0, &t);
        t += 50000000LL;                       /* stall */
        uint64_t b = run(&p, t, 1.0, 1000000LL, 0, NULL);
        CHECK(p.dropped == 0, "50ms stall dropped %llu", (unsigned long long)p.dropped);
        CHECK(a + b >= 121 && a + b <= 123, "50ms stall total %llu (want ~122 over 2.05s)", (unsigned long long)(a + b));
    }

    /* 4. A 1 s stall resyncs (no 60-vblank burst) and counts the drops. */
    vbl_pacer_init(&p, VBL_PERIOD_NTSC_NS);
    {
        int64_t t; run(&p, 0, 0.5, 1000000LL, 0, &t);
        uint64_t before = p.fired;
        t += 1000000000LL;
        int burst = 0;
        while (vbl_pacer_poll(&p, t)) burst++;
        CHECK(burst == 1, "1s stall delivered a burst of %d (want 1)", burst);
        CHECK(p.dropped >= 50 && p.dropped <= 62, "1s stall dropped %llu (want ~59)", (unsigned long long)p.dropped);
        CHECK(p.fired == before + 1, "fired bookkeeping");
    }

    /* 5. PAL period. */
    vbl_pacer_init(&p, VBL_PERIOD_PAL_NS);
    {
        uint64_t n = run(&p, 0, 10.0, 3000000LL, 1000000LL, NULL);
        CHECK(n >= 499 && n <= 501, "PAL: %llu vblanks in 10s (want 500)", (unsigned long long)n);
    }

    if (failures) { printf("vblank_pacer: %d FAILED\n", failures); return 1; }
    printf("vblank_pacer: all tests passed\n");
    return 0;
}
