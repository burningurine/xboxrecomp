/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_fb_width; r.bottom = (LONG)s_fb_height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_fb_va && s_fb_pitch && s_rgb) {
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {
            /* One dump, a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            static int frames;
            if (dump && ++frames == 600)
                xbox_FramebufferDumpBmp(dump);
        }
        Sleep(16);
    }

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#elif defined(__ANDROID__)
/* Android: same job as the Windows GDI window (read the guest framebuffer out of
 * guest RAM and put it on screen), but onto the app's Surface via GLES3/EGL —
 * a textured full-screen quad. This is the live display path on Android (the
 * CPU rasteriser in nv2a_pb_exec.c draws into the guest FB; this shows it).
 * Off unless RECOMP_FB_WINDOW is set. */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/log.h>
#ifndef XR_LOG_TAG
#define XR_LOG_TAG "xr"   /* logcat tag prefix: the title's library name, set by its build */
#endif
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xr_android_window(void);   /* android_runtime/host/jni_bridge.c */
#include "../nv2a/nv2a_pgraph_gles.h"          /* GLES 3D overlay (object-space batches) */
#include "../kernel/frame_stats.h"
#include "../kernel/nv2a_backend.h"
#include <time.h>

#define FBLOG(...) __android_log_print(ANDROID_LOG_INFO, XR_LOG_TAG "-fb", __VA_ARGS__)

static volatile int      s_fb_running;
static volatile uint32_t s_fb_va, s_fb_pitch;
static uint32_t          s_fb_width = 640, s_fb_height = 480;

/* Output aspect: 0 = original 4:3, pillarboxed on the 16:9 panel (default --
 * the game renders 640x480 and stretching it distorts everything); 1 = stretch
 * to fill. RECOMP_ASPECT=stretch at boot, or fb_present_set_aspect() from the
 * Java options menu. */
static volatile int s_aspect_stretch = -1;
void fb_present_set_aspect(int stretch) { s_aspect_stretch = stretch ? 1 : 0; }

static void fb_dest_rect(int sw, int sh, int *x, int *y, int *w, int *h)
{
    if (s_aspect_stretch < 0) {
        const char *a = getenv("RECOMP_ASPECT");
        s_aspect_stretch = (a && a[0] == 's') ? 1 : 0;
    }
    if (s_aspect_stretch || sw <= 0 || sh <= 0) { *x = 0; *y = 0; *w = sw; *h = sh; return; }
    if (sw * 3 >= sh * 4) { *h = sh; *w = sh * 4 / 3; }   /* wider than 4:3: pillarbox */
    else                  { *w = sw; *h = sw * 3 / 4; }   /* taller: letterbox */
    *x = (sw - *w) / 2; *y = (sh - *h) / 2;
}

static double fb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");
    const NV2ABackend *be = nv2a_backend();
    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch) s_fb_pitch = pitch;
    if (be && be->scanout)
        be->scanout(s_fb_va, s_fb_pitch, s_fb_width, s_fb_height);
}

/* A registered GPU backend (nv2a_backend.h) presents through its own API
 * (Vulkan swapchain) -- an ANativeWindow takes one producer API at a time, so
 * this thread then never creates an EGL surface. Returns 0 if the backend
 * cannot present, and the caller falls back to the EGL path. */
static int fb_thread_backend(const NV2ABackend *be)
{
    ANativeWindow *had = NULL;
    FBLOG("presenter: using GPU backend '%s'", be->name ? be->name : "?");
    while (s_fb_running) {
        ANativeWindow *w = (ANativeWindow *)xr_android_window();
        if (w != had && had && be->window_lost)
            be->window_lost();
        had = w;
        if (!w) { usleep(50000); continue; }
        double t0 = fb_now_ms();
        int r = be->present(w, s_aspect_stretch == 1);
        if (r < 0) {
            FBLOG("presenter: backend cannot present; falling back to GLES");
            if (be->window_lost) be->window_lost();
            return 0;
        }
        if (r == 0) { usleep(8000); continue; }   /* nothing rendered yet */
        fs_host_present(fb_now_ms() - t0);        /* FIFO present paces to vsync */
    }
    return 1;
}

static GLuint fb_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof log, &n, log);
               FBLOG("shader compile: %.*s", n, log); }
    return s;
}

static void *fb_thread(void *unused)
{
    (void)unused;

    {
        const NV2ABackend *be = nv2a_backend();
        if (be && be->present && fb_thread_backend(be))
            return NULL;
    }

    /* EGL display + config + context persist for the whole thread; the window
     * SURFACE is (re)created whenever the app's Surface appears, changes, or is
     * lost (screen lock, rotation), so the standalone app survives those. */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        FBLOG("eglInitialize failed 0x%x", eglGetError()); s_fb_running = 0; return NULL; }
    const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        FBLOG("eglChooseConfig failed"); s_fb_running = 0; return NULL; }
    EGLint vid = 0; eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        FBLOG("eglCreateContext failed 0x%x", eglGetError()); s_fb_running = 0; return NULL; }

    static const char *VS =
        "#version 300 es\n"
        "layout(location=0) in vec2 p;\n"
        "out vec2 uv;\n"
        "void main(){ uv = vec2((p.x+1.0)*0.5, (1.0-p.y)*0.5); gl_Position = vec4(p,0.0,1.0); }\n";
    static const char *FS =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 uv; uniform sampler2D t; out vec4 c;\n"
        "void main(){ c = texture(t, uv); }\n";

    ANativeWindow *cur   = NULL;
    EGLSurface     surf  = EGL_NO_SURFACE;
    int            gl    = 0;                 /* GL resources created yet? */
    GLuint         prog = 0, vao = 0, vbo = 0, tex = 0;
    uint32_t      *rgba = NULL;
    EGLint         sw = (EGLint)s_fb_width, sh = (EGLint)s_fb_height;
    int            pulse = 0;

    while (s_fb_running) {
        ANativeWindow *w = (ANativeWindow *)xr_android_window();
        if (w != cur) {                       /* Surface appeared / changed / lost */
            if (surf != EGL_NO_SURFACE) {
                eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(dpy, surf);
                surf = EGL_NO_SURFACE;
            }
            cur = w;
            if (w) {
                ANativeWindow_setBuffersGeometry(w, 0, 0, vid);
                surf = eglCreateWindowSurface(dpy, cfg, w, NULL);
                if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
                    FBLOG("surface bring-up failed 0x%x; retrying", eglGetError());
                    if (surf != EGL_NO_SURFACE) { eglDestroySurface(dpy, surf); surf = EGL_NO_SURFACE; }
                    cur = NULL;                /* force a retry next tick */
                    usleep(50000); continue;
                }
                /* Present on the display's vsync (the loop used to add a fixed
                 * 16 ms sleep on top of the swap wait, halving the rate). */
                eglSwapInterval(dpy, 1);
                eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
                eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
                if (sw <= 0) sw = (EGLint)s_fb_width;
                if (sh <= 0) sh = (EGLint)s_fb_height;
                if (!gl) {                     /* one-time GL resources (need a current ctx) */
                    prog = glCreateProgram();
                    glAttachShader(prog, fb_compile(GL_VERTEX_SHADER, VS));
                    glAttachShader(prog, fb_compile(GL_FRAGMENT_SHADER, FS));
                    glBindAttribLocation(prog, 0, "p");
                    glLinkProgram(prog);
                    static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
                    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
                    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
                    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
                    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
                    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    rgba = (uint32_t *)malloc((size_t)s_fb_width * s_fb_height * 4);
                    gl = 1;
                }
                FBLOG("presenter surface up: %dx%d", sw, sh);
            }
        }
        if (surf == EGL_NO_SURFACE) { usleep(50000); continue; }   /* no Surface: idle-wait */

        double t_compose = fb_now_ms();
        int dx, dy, dw, dh;
        fb_dest_rect(sw, sh, &dx, &dy, &dw, &dh);
        uint32_t va = s_fb_va, pitch = s_fb_pitch;
        if (va && pitch && rgba) {
            const uint8_t *base = (const uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());
            uint32_t bpp = pitch / s_fb_width;
            for (uint32_t y = 0; y < s_fb_height; y++) {
                const uint8_t *row = base + (size_t)y * pitch;
                uint32_t *dst = rgba + (size_t)y * s_fb_width;
                if (bpp == 4) {              /* X8R8G8B8 (BGRA in memory) -> RGBA */
                    const uint32_t *p = (const uint32_t *)row;
                    for (uint32_t x = 0; x < s_fb_width; x++) {
                        uint32_t v = p[x];
                        dst[x] = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
                    }
                } else if (bpp == 2) {       /* R5G6B5 -> RGBA */
                    const uint16_t *p = (const uint16_t *)row;
                    for (uint32_t x = 0; x < s_fb_width; x++) {
                        uint16_t v = p[x];
                        uint32_t r = ((v >> 11) & 0x1F) * 255u / 31u;
                        uint32_t g = ((v >>  5) & 0x3F) * 255u / 63u;
                        uint32_t b = ( v        & 0x1F) * 255u / 31u;
                        dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
                    }
                } else {
                    memset(dst, 0, (size_t)s_fb_width * 4);
                }
            }
            /* Diagnostic: exactly what the presenter reads + converts, so a black
             * screen despite bright guest draws is pinnable in one line -- the
             * source fb_va/pitch/bpp and whether the converted RGBA has content.
             * If brightest is bright but the screen is black, the loss is in the
             * GL upload/draw; if it's 0, the presenter is reading the wrong (or a
             * cleared) surface. */
            {
                static unsigned dcount;
                if ((dcount++ % 120) == 0) {
                    uint32_t mx = 0, nz = 0, i, n = s_fb_width * s_fb_height;
                    for (i = 0; i < n; i++) {
                        if (rgba[i] & 0x00FFFFFFu) nz++;
                        if (rgba[i] > mx) mx = rgba[i];
                    }
                    FBLOG("present fb_va=0x%08X pitch=%u bpp=%u brightest=0x%08X "
                          "nonzero=%u/%u", va, pitch, bpp, mx, nz, n);
                }
            }
            {
                /* Brightness-triggered one-shot dump of the exact RGBA the
                 * presenter shows. A transient bright frame (e.g. the title that
                 * only shows for ~1s before the game settles to a dark menu) can
                 * be captured without racing screencap timing: dump the first
                 * frame whose content exceeds the threshold, then stop. Writes a
                 * PPM (P6) to RECOMP_FB_BRIGHTDUMP; threshold via
                 * RECOMP_FB_BRIGHTDUMP_THR (default 180000 nonzero px). */
                const char *bd = getenv("RECOMP_FB_BRIGHTDUMP");
                static int bright_done;
                if (bd && !bright_done) {
                    uint32_t nz2 = 0, i, n = s_fb_width * s_fb_height;
                    for (i = 0; i < n; i++) if (rgba[i] & 0x00FFFFFFu) nz2++;
                    uint32_t thr = 180000;
                    const char *t = getenv("RECOMP_FB_BRIGHTDUMP_THR");
                    if (t) thr = (uint32_t)strtoul(t, NULL, 0);
                    if (nz2 > thr) {
                        FILE *f = fopen(bd, "wb");
                        if (f) {
                            fprintf(f, "P6\n%u %u\n255\n", s_fb_width, s_fb_height);
                            for (i = 0; i < n; i++) {
                                uint32_t v = rgba[i];
                                unsigned char rgb[3] = {
                                    (unsigned char)(v & 0xFFu),
                                    (unsigned char)((v >> 8) & 0xFFu),
                                    (unsigned char)((v >> 16) & 0xFFu) };
                                fwrite(rgb, 1, 3, f);
                            }
                            fclose(f);
                            bright_done = 1;
                            FBLOG("BRIGHTDUMP wrote %s nz=%u thr=%u", bd, nz2, thr);
                        } else {
                            FBLOG("BRIGHTDUMP fopen(%s) failed", bd);
                        }
                    }
                }
            }
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)s_fb_width, (GLsizei)s_fb_height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glViewport(0, 0, sw, sh);
            glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
            glViewport(dx, dy, dw, dh);
            glUseProgram(prog); glBindVertexArray(vao);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        } else {
            /* No FB address yet: a clear that visibly pulses so it is obvious the
             * presenter is alive and owns the surface (vs a black/absent one). */
            float b = 0.25f + 0.25f * (float)((++pulse % 60) < 30);
            glViewport(0, 0, sw, sh);
            glClearColor(0.10f, 0.10f, b, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        }
        /* Composite the GLES 3D layer (object-space batches the CPU raster
         * can't place) over the guest framebuffer we just blitted. No-op unless
         * RECOMP_GLES_3D is set. Runs here because this thread owns the context. */
        nv2a_gles_render(dx, dy, dw, dh);
        fs_host_present(fb_now_ms() - t_compose);

        if (!eglSwapBuffers(dpy, surf)) {       /* Surface lost (e.g. torn down) → rebuild */
            FBLOG("eglSwapBuffers failed 0x%x; dropping surface", eglGetError());
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(dpy, surf);
            surf = EGL_NO_SURFACE; cur = NULL;
        }
    }

    if (surf != EGL_NO_SURFACE) { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                                  eglDestroySurface(dpy, surf); }
    free(rgba);
    eglDestroyContext(dpy, ctx);
    return NULL;
}

void xbox_FramebufferWindowStart(void)
{
    if (!getenv("RECOMP_FB_WINDOW")) return;
    if (__atomic_exchange_n(&s_fb_running, 1, __ATOMIC_SEQ_CST)) return;   /* already running */
    pthread_t th;
    if (pthread_create(&th, NULL, fb_thread, NULL) == 0) pthread_detach(th);
    else s_fb_running = 0;
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
#endif
