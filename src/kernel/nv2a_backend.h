/*
 * nv2a_backend.h - optional GPU backend behind the pushbuffer executor.
 *
 * The executor (nv2a_pb_exec.c) always walks the pushbuffer and keeps the
 * kernel-visible side effects (flips, fences, semaphores) exactly as before.
 * A host may register a backend that renders the same method stream on the
 * real GPU (the Blinx Android build registers xemu's Vulkan PGRAPH). While a
 * backend is registered the executor's own CPU/GLES rasterisers are skipped,
 * and the presenter asks the backend to present instead of blitting the
 * guest framebuffer.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

typedef struct NV2ABackend {
    const char *name;
    /* One pushbuffer method (subchannel, method offset, parameter). */
    void (*method)(uint32_t subch, uint32_t method, uint32_t param);
    /* Scan-out changed (AvSetDisplayMode): physical address, pitch, size. */
    void (*scanout)(uint32_t phys, uint32_t pitch, uint32_t width, uint32_t height);
    /* Presenter thread: show the current frame on `window` (ANativeWindow* on
     * Android). 1 = presented, 0 = nothing yet, -1 = unavailable. */
    int  (*present)(void *window, int stretch);
    /* The presentation window went away. */
    void (*window_lost)(void);
} NV2ABackend;

void nv2a_backend_register(const NV2ABackend *backend);
const NV2ABackend *nv2a_backend(void);   /* NULL when none is registered */

#endif /* NV2A_BACKEND_H */
