/*
 * frame_stats.c - per-frame performance counters for the debug overlay.
 * See frame_stats.h. Producers are lock-free counter bumps; the 1-second
 * window roll and the frame-time ring take a small spinlock.
 */
#include "frame_stats.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#  include <unistd.h>
#endif
#if defined(__ANDROID__)
#  include <android/log.h>
#  ifndef XR_LOG_TAG
#  define XR_LOG_TAG "xr"   /* logcat tag prefix: the title's library name, set by its build */
#  endif
#endif

extern void kernel_vblank_get_stats(uint64_t *fired, uint64_t *dropped);  /* kernel_bridge.c */

/* ---- tiny portable primitives ------------------------------------------ */

static int64_t fs_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

#if defined(_WIN32)
static volatile LONG s_lock;
static void fs_lock(void)   { while (InterlockedExchange(&s_lock, 1)) YieldProcessor(); }
static void fs_unlock(void) { InterlockedExchange(&s_lock, 0); }
#define FS_ADD(p, v) InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v))
#else
static volatile int s_lock;
static void fs_lock(void)   { while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void fs_unlock(void) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); }
#define FS_ADD(p, v) __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)
#endif

/* ---- state ---------------------------------------------------------------- */

/* Current (accumulating) window. 32-bit counters: a 1 s window never wraps. */
static volatile int32_t s_flips, s_draws, s_verts, s_texups, s_texbytes, s_presents;
static volatile int32_t s_present_us;        /* summed composition time */
static int64_t s_win_start_ns;
static uint64_t s_vbl_at_start;

/* Frame-time ring (guest flip-to-flip, ms). */
static float   s_ft[FS_HISTORY];
static int     s_ft_head, s_ft_count;
static int64_t s_last_flip_ns;
static float   s_ft_min = 1e9f, s_ft_max;
static double  s_ft_sum;
static int     s_ft_n;

/* Last completed window. */
static struct {
    int valid;
    double secs;
    double guest_fps, host_fps, vbl_hz;
    double ft_min, ft_avg, ft_max;
    double draws_pf, verts_pf;
    int texups, texkb;
    double present_ms;
    uint64_t vbl_dropped;
    long rss_mb;
    char state[96];
} s_snap;
static unsigned s_snap_seq, s_logged_seq;

static fs_state_fn s_state_fn;
static volatile int s_overlay = 1;

void fs_set_state_fn(fs_state_fn fn) { s_state_fn = fn; }
void fs_set_overlay(int on) { s_overlay = on ? 1 : 0; }
int  fs_overlay(void) { return s_overlay; }

/* ---- producers ------------------------------------------------------------ */

void fs_guest_flip(void)
{
    int64_t now = fs_now_ns();
    FS_ADD(&s_flips, 1);
    fs_lock();
    if (s_last_flip_ns) {
        float ms = (float)((now - s_last_flip_ns) / 1e6);
        s_ft[s_ft_head] = ms;
        s_ft_head = (s_ft_head + 1) % FS_HISTORY;
        if (s_ft_count < FS_HISTORY) s_ft_count++;
        if (ms < s_ft_min) s_ft_min = ms;
        if (ms > s_ft_max) s_ft_max = ms;
        s_ft_sum += ms;
        s_ft_n++;
    }
    s_last_flip_ns = now;
    fs_unlock();
}

void fs_draw(uint32_t verts)
{
    FS_ADD(&s_draws, 1);
    FS_ADD(&s_verts, (int32_t)verts);
}

void fs_tex_upload(uint32_t bytes)
{
    FS_ADD(&s_texups, 1);
    FS_ADD(&s_texbytes, (int32_t)bytes);
}

void fs_host_present(double present_ms)
{
    FS_ADD(&s_presents, 1);
    FS_ADD(&s_present_us, (int32_t)(present_ms * 1000.0));
}

/* ---- window roll ---------------------------------------------------------- */

static long fs_rss_mb(void)
{
#if defined(__linux__) || defined(__ANDROID__)
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
    fclose(f);
    return rss * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
#else
    return 0;
#endif
}

static int32_t take(volatile int32_t *p)
{
#if defined(_WIN32)
    return InterlockedExchange((volatile LONG *)p, 0);
#else
    return __atomic_exchange_n(p, 0, __ATOMIC_RELAXED);
#endif
}

/* Roll the window if >= 1 s has passed. Returns 1 if a new snapshot exists. */
static int fs_roll(void)
{
    int64_t now = fs_now_ns();
    uint64_t vbl = 0, vdrop = 0;
    double secs;
    int32_t flips, draws, verts, texups, texbytes, presents, pus;

    kernel_vblank_get_stats(&vbl, &vdrop);
    if (!s_win_start_ns) {
        s_win_start_ns = now;
        s_vbl_at_start = vbl;
        return 0;
    }
    secs = (now - s_win_start_ns) / 1e9;
    if (secs < 1.0) return 0;

    flips = take(&s_flips); draws = take(&s_draws); verts = take(&s_verts);
    texups = take(&s_texups); texbytes = take(&s_texbytes);
    presents = take(&s_presents); pus = take(&s_present_us);

    fs_lock();
    s_snap.valid = 1;
    s_snap.secs = secs;
    s_snap.guest_fps = flips / secs;
    s_snap.host_fps = presents / secs;
    s_snap.vbl_hz = (double)(vbl - s_vbl_at_start) / secs;
    s_snap.vbl_dropped = vdrop;
    s_snap.ft_min = s_ft_n ? s_ft_min : 0;
    s_snap.ft_max = s_ft_n ? s_ft_max : 0;
    s_snap.ft_avg = s_ft_n ? s_ft_sum / s_ft_n : 0;
    s_snap.draws_pf = flips ? (double)draws / flips : draws;
    s_snap.verts_pf = flips ? (double)verts / flips : verts;
    s_snap.texups = texups;
    s_snap.texkb = texbytes / 1024;
    s_snap.present_ms = presents ? pus / 1000.0 / presents : 0;
    s_ft_min = 1e9f; s_ft_max = 0; s_ft_sum = 0; s_ft_n = 0;
    s_snap_seq++;
    fs_unlock();

    s_snap.rss_mb = fs_rss_mb();
    s_snap.state[0] = 0;
    if (s_state_fn) s_state_fn(s_snap.state, sizeof s_snap.state);

    s_win_start_ns = now;
    s_vbl_at_start = vbl;
    return 1;
}

/* ---- consumers ------------------------------------------------------------ */

int fs_format(char *buf, size_t n)
{
    fs_roll();
    if (!n) return 0;
    if (!s_snap.valid) return snprintf(buf, n, "stats: warming up");
    return snprintf(buf, n,
        "Game %.1f fps  frame %.1f/%.1f/%.1f ms\n"
        "Display %.1f fps  compose %.2f ms\n"
        "VBlank %.2f Hz (dropped %llu)\n"
        "Draws %.0f/f  verts %.0f/f  tex up %d (%d KB)\n"
        "RSS %ld MB  %s",
        s_snap.guest_fps, s_snap.ft_min, s_snap.ft_avg, s_snap.ft_max,
        s_snap.host_fps, s_snap.present_ms,
        s_snap.vbl_hz, (unsigned long long)s_snap.vbl_dropped,
        s_snap.draws_pf, s_snap.verts_pf, s_snap.texups, s_snap.texkb,
        s_snap.rss_mb, s_snap.state);
}

int fs_frame_times(float *out, int max)
{
    int n, i, start;
    fs_lock();
    n = s_ft_count < max ? s_ft_count : max;
    start = (s_ft_head - n + FS_HISTORY) % FS_HISTORY;
    for (i = 0; i < n; i++) out[i] = s_ft[(start + i) % FS_HISTORY];
    fs_unlock();
    return n;
}

void fs_log_tick(void)
{
    fs_roll();
    if (!s_snap.valid || s_logged_seq == s_snap_seq) return;
    s_logged_seq = s_snap_seq;
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, XR_LOG_TAG "-stats",
#else
    fprintf(stderr,
#endif
        "[STATS] game=%.1ffps ft=%.1f/%.1f/%.1fms display=%.1ffps compose=%.2fms vbl=%.2fHz drop=%llu "
        "draws=%.0f verts=%.0f texup=%d/%dKB rss=%ldMB %s\n",
        s_snap.guest_fps, s_snap.ft_min, s_snap.ft_avg, s_snap.ft_max,
        s_snap.host_fps, s_snap.present_ms, s_snap.vbl_hz,
        (unsigned long long)s_snap.vbl_dropped, s_snap.draws_pf, s_snap.verts_pf,
        s_snap.texups, s_snap.texkb, s_snap.rss_mb, s_snap.state);
}
