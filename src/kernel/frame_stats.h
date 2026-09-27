/*
 * frame_stats.h - per-frame performance counters for the debug overlay.
 *
 * Platform-neutral (no GL, no Android). Producers call the fs_* hooks from
 * wherever the event happens; consumers pull a formatted summary:
 *   - Android: MainActivity polls blinx_stats_text()/frame times over JNI and
 *     draws the overlay + frame-time graph (TouchPadView), and a 1 Hz
 *     "blinx-stats" logcat line is emitted.
 *   - Windows oracle: the same 1 Hz line goes to stderr.
 *
 * "Guest frame" = one NV2A FLIP_STALL (the title finished and swapped a frame).
 * "Host present" = one presenter-thread eglSwapBuffers.
 */
#ifndef FRAME_STATS_H
#define FRAME_STATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FS_HISTORY 240          /* guest frame times kept for the graph */

void fs_guest_flip(void);                 /* guest completed a frame */
void fs_draw(uint32_t verts);             /* one draw call (BEGIN_END close) */
void fs_tex_upload(uint32_t bytes);       /* one texture upload to the host GPU */
void fs_host_present(double present_ms);  /* presenter swapped; ms spent composing */

/* Optional game-state line (e.g. Blinx scene id). Called at ~1 Hz. */
typedef int (*fs_state_fn)(char *buf, size_t n);
void fs_set_state_fn(fs_state_fn fn);

/* Summary of the last completed 1-second window, newline separated.
 * Returns the length written. Rolls the window if a second has elapsed. */
int fs_format(char *buf, size_t n);

/* Copy the most recent guest frame times (ms, oldest first). Returns count. */
int fs_frame_times(float *out, int max);

/* Emit the 1 Hz one-line summary to the log if a new window completed. */
void fs_log_tick(void);

/* Overlay visibility (set from the Java options menu). */
void fs_set_overlay(int on);
int  fs_overlay(void);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_STATS_H */
