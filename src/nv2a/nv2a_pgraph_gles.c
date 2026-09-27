/*
 * nv2a_pgraph_gles.c - GLES 3.0 rasteriser for NV2A batches on Android.
 *
 * WHY THIS EXISTS
 * ---------------
 * The CPU rasteriser in kernel/nv2a_pb_exec.c only draws PRE-TRANSFORMED
 * (screen-space) batches into the guest framebuffer; it SKIPS every batch that
 * arrives in object space (a vertex-program batch), because a software raster
 * with no depth buffer and no perspective correction cannot place 3D correctly.
 * That skip is why gameplay 3D is dark on device.
 *
 * This module fills that gap on the GPU. It does NOT translate NV2A vertex
 * microcode to GLSL (the hard, error-prone part): nv2a_pb_exec.c already runs
 * the vertices through its proven software transform (vp_raw_clip -> vp_execute
 * / composite matrix) to get CLIP-SPACE positions. We take those clip-space
 * triangles, upload them, and let GLES rasterise them WITH a real depth buffer
 * and perspective-correct interpolation -- which is exactly what the software
 * path lacked.
 *
 * PHASE 1 (texturing)
 * -------------------
 * The parser thread decodes each bound texture to RGBA8 through the SAME proven
 * sampler the CPU path uses (kernel/nv2a_pb_exec.c: sample_texture, which knows
 * swizzled/DXT/linear), and registers it here under a stable id. Triangles carry
 * per-vertex normalised UVs plus that id. The presenter thread uploads each
 * texture to GL once and draws the frame as runs of same-texture triangles,
 * sampling with perspective-correct UVs. A tri with id 0, or an id that never
 * uploaded, falls back to the flat vertex colour -- untextured, not undrawn,
 * matching the CPU path's "drawn wrong beats not drawn" rule. Register-combiner
 * shading (modulate/blend by vertex colour, multitexture) is a later phase.
 *
 * THREADING
 * ---------
 * NV2A methods are decoded on the guest/parser thread (nv2a_pb_exec_method).
 * The only EGL context lives on fb_present.c's presenter thread. GL is
 * thread-affine, so we cannot issue GL from the parser thread. Instead:
 *   - PRODUCER (parser thread): nv2a_gles_clear() / nv2a_gles_tri[_tex]() append
 *     into a "building" command buffer; nv2a_gles_texture() records decoded
 *     texels; nv2a_gles_frame() publishes the buffer to "ready".
 *   - CONSUMER (presenter thread, context current): nv2a_gles_render() drains
 *     the "ready" buffer, uploads any new textures, draws it into an FBO, and
 *     composites the FBO over the guest framebuffer that fb_present blitted.
 * A mutex guards the hand-off; the buffers are double-buffered so the producer
 * can build frame N+1 while the consumer draws frame N. The texture cache only
 * ever grows and is never freed (Phase 1 static-texture assumption), so its
 * decoded pixel pointers are stable and the GL id is written only by the
 * consumer -- letting the consumer upload outside the lock.
 *
 * Gated by RECOMP_GLES_3D so it is inert until explicitly enabled.
 */

#include <stdint.h>

#if defined(__ANDROID__)

#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/frame_stats.h"
#include <stdio.h>
#include <android/log.h>

#include "nv2a_pgraph_gles.h"

#define GLLOG(...) __android_log_print(ANDROID_LOG_INFO, "blinx-gl3d", __VA_ARGS__)

/* One vertex handed to GL: clip-space position, packed RGBA colour, UV. */
typedef struct { float x, y, z, w; float r, g, b, a; float u, v; } GlVertex;

/* A command buffer for one frame. Triangles accumulate as flat GlVertex triples
 * (3 per triangle); a parallel tri_tex[] holds one texture id per triangle so
 * the consumer can draw runs of same-texture triangles. The clear colour is
 * whatever the last CLEAR_SURFACE set. */
typedef struct {
    GlVertex *verts;
    uint32_t  count;      /* vertices used   */
    uint32_t  cap;        /* vertices alloc  */
    uint32_t *tri_tex;    /* one texid per triangle (count/3 used) */
    uint32_t  tri_count;  /* triangles used  */
    uint32_t  tri_cap;    /* triangles alloc */
    float     clear_r, clear_g, clear_b, clear_a;
    int       have_clear;
} GlCmdBuf;

static GlCmdBuf        s_build;                 /* producer writes here */
static GlCmdBuf        s_ready;                 /* consumer reads here  */
static int             s_ready_valid;           /* s_ready holds an unconsumed frame */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static int s_enabled = -1;   /* -1 unknown, 0 off, 1 on (RECOMP_GLES_3D) */

int nv2a_gles_enabled(void)
{
    if (s_enabled < 0)
        s_enabled = getenv("RECOMP_GLES_3D") != NULL;
    return s_enabled;
}

/* ---- texture cache (append-only; see THREADING) ------------------------- */

#define GLTEX_MAX 1024
typedef struct {
    uint32_t texid;       /* producer: stable identity from the emit side */
    int      w, h;        /* producer */
    GLenum   wrap_u, wrap_v; /* producer */
    uint8_t *rgba;        /* producer: owned copy, w*h*4 bytes, R,G,B,A; stable */
    GLuint   gl;          /* consumer only: 0 until uploaded */
} GlTexEntry;

static GlTexEntry s_tex[GLTEX_MAX];
static int        s_tex_n;             /* count; only grows, guarded by s_lock */

static GLenum wrap_of(int addr)
{
    return addr == 1 ? GL_REPEAT : GL_CLAMP_TO_EDGE;   /* 1=wrap, else clamp */
}

void nv2a_gles_texture(uint32_t texid, int w, int h,
                       int addr_u, int addr_v, const void *rgba8)
{
    if (!nv2a_gles_enabled() || !texid || w <= 0 || h <= 0 || !rgba8) return;
    size_t bytes = (size_t)w * (size_t)h * 4u;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_tex_n; i++)
        if (s_tex[i].texid == texid) { pthread_mutex_unlock(&s_lock); return; }
    if (s_tex_n >= GLTEX_MAX) { pthread_mutex_unlock(&s_lock); return; }
    uint8_t *copy = (uint8_t *)malloc(bytes);
    if (!copy) { pthread_mutex_unlock(&s_lock); return; }
    memcpy(copy, rgba8, bytes);
    GlTexEntry *e = &s_tex[s_tex_n];
    e->texid = texid; e->w = w; e->h = h;
    e->wrap_u = wrap_of(addr_u); e->wrap_v = wrap_of(addr_v);
    e->rgba = copy; e->gl = 0;
    s_tex_n++;                     /* publish last: entry fully built above */
    pthread_mutex_unlock(&s_lock);
}

/* ---- producer side (parser thread) ------------------------------------- */

static void cmdbuf_reset(GlCmdBuf *b)
{
    b->count = 0;
    b->tri_count = 0;
    b->have_clear = 0;
}

/* Append one triangle (3 verts + its texid) to the building buffer. */
static void cmdbuf_push_tri(GlCmdBuf *b, const GlVertex v[3], uint32_t texid)
{
    if (b->count + 3 > b->cap) {
        uint32_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->count + 3) ncap *= 2;
        GlVertex *nv = (GlVertex *)realloc(b->verts, (size_t)ncap * sizeof(GlVertex));
        if (!nv) return;              /* drop on OOM rather than crash */
        b->verts = nv; b->cap = ncap;
    }
    if (b->tri_count + 1 > b->tri_cap) {
        uint32_t ncap = b->tri_cap ? b->tri_cap * 2 : 2048;
        uint32_t *nt = (uint32_t *)realloc(b->tri_tex, (size_t)ncap * sizeof(uint32_t));
        if (!nt) return;
        b->tri_tex = nt; b->tri_cap = ncap;
    }
    b->verts[b->count++] = v[0];
    b->verts[b->count++] = v[1];
    b->verts[b->count++] = v[2];
    b->tri_tex[b->tri_count++] = texid;
}

void nv2a_gles_clear(uint32_t argb)
{
    if (!nv2a_gles_enabled()) return;
    pthread_mutex_lock(&s_lock);
    s_build.clear_a = (float)((argb >> 24) & 0xFF) / 255.0f;
    s_build.clear_r = (float)((argb >> 16) & 0xFF) / 255.0f;
    s_build.clear_g = (float)((argb >>  8) & 0xFF) / 255.0f;
    s_build.clear_b = (float)( argb        & 0xFF) / 255.0f;
    s_build.have_clear = 1;
    pthread_mutex_unlock(&s_lock);
}

/* Unpack an ARGB8888 flat colour into a GlVertex (position/uv filled by caller). */
static void set_color(GlVertex *v, uint32_t argb)
{
    float a = (float)((argb >> 24) & 0xFF) / 255.0f;
    if (a == 0.0f) a = 1.0f;          /* many batches leave A=0; treat as opaque */
    v->r = (float)((argb >> 16) & 0xFF) / 255.0f;
    v->g = (float)((argb >>  8) & 0xFF) / 255.0f;
    v->b = (float)( argb        & 0xFF) / 255.0f;
    v->a = a;
}

/* clip[i] = {x,y,z,w} clip-space position of vertex i; argb is the flat colour. */
void nv2a_gles_tri(const float clip0[4], const float clip1[4], const float clip2[4],
                   uint32_t argb)
{
    static const float z2[2] = { 0.0f, 0.0f };
    nv2a_gles_tri_tex(clip0, clip1, clip2, z2, z2, z2, argb, 0u);
}

void nv2a_gles_tri_tex(const float clip0[4], const float clip1[4], const float clip2[4],
                       const float uv0[2], const float uv1[2], const float uv2[2],
                       uint32_t argb, uint32_t texid)
{
    if (!nv2a_gles_enabled()) return;
    const float *c[3]  = { clip0, clip1, clip2 };
    const float *uv[3] = { uv0,   uv1,   uv2   };
    GlVertex tri[3];
    for (int i = 0; i < 3; i++) {
        tri[i].x = c[i][0]; tri[i].y = c[i][1]; tri[i].z = c[i][2];
        tri[i].w = c[i][3] != 0.0f ? c[i][3] : 1.0f;
        tri[i].u = uv[i][0]; tri[i].v = uv[i][1];
        set_color(&tri[i], argb);
    }
    pthread_mutex_lock(&s_lock);
    cmdbuf_push_tri(&s_build, tri, texid);
    pthread_mutex_unlock(&s_lock);
}

/* Guest buffer flip: publish the batch just built for the presenter to draw. */
void nv2a_gles_frame(void)
{
    if (!nv2a_gles_enabled()) return;
    pthread_mutex_lock(&s_lock);
    /* Swap building <-> ready storage so we don't copy the vertex array. The
     * whole struct swaps, so verts[] and tri_tex[] move together. */
    GlCmdBuf tmp = s_ready;
    s_ready = s_build;
    s_build = tmp;
    s_ready_valid = 1;
    cmdbuf_reset(&s_build);           /* keep s_build storage (reuse) */
    pthread_mutex_unlock(&s_lock);
}

/* ---- consumer side (presenter GL thread, context current) --------------- */

static int    s_gl_init;
static GLuint s_prog, s_vbo, s_vao;
static GLuint s_fbo, s_fbo_tex, s_fbo_depth;
static int    s_fbo_w, s_fbo_h;
static GLint  s_u_use_tex, s_u_samp;

/* Fullscreen-quad program to composite the FBO colour over the guest FB. */
static GLuint s_comp_prog, s_comp_vbo, s_comp_vao;
static GLint  s_comp_samp;

static GLuint gl_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof log, &n, log);
               GLLOG("shader: %.*s", n, log); }
    return s;
}

static GLuint gl_program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, gl_compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, gl_compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; GLsizei n = 0; glGetProgramInfoLog(p, sizeof log, &n, log);
               GLLOG("link: %.*s", n, log); }
    return p;
}

static void ensure_fbo(int w, int h)
{
    if (s_fbo && s_fbo_w == w && s_fbo_h == h) return;
    if (s_fbo) { glDeleteFramebuffers(1, &s_fbo); glDeleteTextures(1, &s_fbo_tex);
                 glDeleteRenderbuffers(1, &s_fbo_depth); s_fbo = 0; }
    glGenTextures(1, &s_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &s_fbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_fbo_depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) GLLOG("FBO incomplete 0x%x", st);
    s_fbo_w = w; s_fbo_h = h;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLLOG("3D FBO %dx%d ready", w, h);
}

static void gl_init(void)
{
    static const char *VS =
        "#version 300 es\n"
        "layout(location=0) in vec4 pos;\n"   /* D3D clip-space x,y,z,w (z in [0,w]) */
        "layout(location=1) in vec4 col;\n"
        "layout(location=2) in vec2 uv;\n"
        "out vec4 vcol; out vec2 vuv;\n"
        /* Xbox/D3D clip space uses z in [0,w]; GL expects z in [-w,w]. Remap
         * z -> 2z - w so depth is correct. x,y,w pass through unchanged.
         * (Revisit Y-orientation/winding once gameplay geometry is on screen.) */
        "void main(){ vcol = col; vuv = uv; gl_Position = vec4(pos.xy, 2.0*pos.z - pos.w, pos.w); }\n";
    static const char *FS =
        "#version 300 es\n"
        "precision highp float;\n"
        "in vec4 vcol; in vec2 vuv; out vec4 o;\n"
        "uniform int uUseTex; uniform sampler2D uTex;\n"
        /* Phase 1: textured => texel; untextured => flat vertex colour.
         * Combiner-style modulate (texel * vcol) comes with the combiner phase. */
        "void main(){ o = (uUseTex != 0) ? texture(uTex, vuv) : vcol; }\n";
    s_prog = gl_program(VS, FS);
    s_u_use_tex = glGetUniformLocation(s_prog, "uUseTex");
    s_u_samp    = glGetUniformLocation(s_prog, "uTex");
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVertex), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GlVertex), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), (void *)(8 * sizeof(float)));
    glBindVertexArray(0);

    static const char *CVS =
        "#version 300 es\n"
        "layout(location=0) in vec2 p; out vec2 uv;\n"
        "void main(){ uv = (p+1.0)*0.5; gl_Position = vec4(p,0.0,1.0); }\n";
    static const char *CFS =
        "#version 300 es\n"
        "precision highp float;\n"
        "in vec2 uv; uniform sampler2D t; out vec4 o;\n"
        "void main(){ o = texture(t, uv); }\n";
    s_comp_prog = gl_program(CVS, CFS);
    s_comp_samp = glGetUniformLocation(s_comp_prog, "t");
    static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glGenVertexArrays(1, &s_comp_vao);
    glGenBuffers(1, &s_comp_vbo);
    glBindVertexArray(s_comp_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindVertexArray(0);

    s_gl_init = 1;
    GLLOG("3D GLES pipeline initialised");
}

/* Find a cached texture by id within the first `n` (snapshotted) entries, and
 * upload it to GL on first use. Returns its GL id, or 0 if not usable. */
static GLuint gltex_gl(uint32_t texid, int n)
{
    if (!texid) return 0;
    for (int i = 0; i < n; i++) {
        GlTexEntry *e = &s_tex[i];
        if (e->texid != texid) continue;
        if (e->gl) return e->gl;               /* already uploaded (consumer-only) */
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, e->w, e->h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, e->rgba);
        fs_tex_upload((uint32_t)(e->w * e->h * 4));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, e->wrap_u);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, e->wrap_v);
        e->gl = id;                            /* only this thread writes gl */
        return id;
    }
    return 0;
}

void nv2a_gles_render(int vx, int vy, int surf_w, int surf_h)
{
    if (!nv2a_gles_enabled()) return;

    /* Grab the ready frame (if any). We render the most recent published frame;
     * if the producer hasn't flipped since last time, re-draw the last one so
     * the 3D layer doesn't flicker between guest flips. */
    static GlVertex *scratch;   static uint32_t scratch_cap;
    static uint32_t *tri_scr;   static uint32_t tri_scr_cap;
    pthread_mutex_lock(&s_lock);
    int      have    = s_ready_valid;
    uint32_t nverts  = s_ready.count;
    uint32_t ntris   = s_ready.tri_count;
    int      tex_n   = s_tex_n;              /* snapshot: entries below this are final */
    if (nverts > scratch_cap) {
        GlVertex *ns = (GlVertex *)realloc(scratch, (size_t)nverts * sizeof(GlVertex));
        if (ns) { scratch = ns; scratch_cap = nverts; } else nverts = ntris = 0;
    }
    if (ntris > tri_scr_cap) {
        uint32_t *nt = (uint32_t *)realloc(tri_scr, (size_t)ntris * sizeof(uint32_t));
        if (nt) { tri_scr = nt; tri_scr_cap = ntris; } else nverts = ntris = 0;
    }
    if (nverts) memcpy(scratch, s_ready.verts,   (size_t)nverts * sizeof(GlVertex));
    if (ntris)  memcpy(tri_scr, s_ready.tri_tex, (size_t)ntris  * sizeof(uint32_t));
    s_ready_valid = 0;
    pthread_mutex_unlock(&s_lock);

    if (!have && !s_fbo) return;      /* nothing ever produced yet */
    if (!s_gl_init) gl_init();
    int w = surf_w > 0 ? surf_w : 640, h = surf_h > 0 ? surf_h : 480;
    ensure_fbo(w, h);

    /* --- draw the 3D batch into the FBO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, w, h);
    /* Clear to TRANSPARENT so the composite only shows drawn 3D over the guest
     * FB. (When 3D becomes the whole scene we'll honour have_clear/clear_*.) */
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    uint32_t drawn = 0, textured_tris = 0;
    if (ntris >= 1 && nverts >= 3) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_CULL_FACE);       /* winding unknown for now */
        glUseProgram(s_prog);
        glUniform1i(s_u_samp, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * sizeof(GlVertex),
                     scratch, GL_STREAM_DRAW);
        /* Draw as runs of consecutive same-texture triangles: one glDrawArrays
         * per run, binding that run's texture (or none for id 0 / not-uploaded). */
        uint32_t t = 0;
        while (t < ntris) {
            uint32_t tid = tri_scr[t];
            uint32_t e = t + 1;
            while (e < ntris && tri_scr[e] == tid) e++;
            GLuint gltex = gltex_gl(tid, tex_n);
            if (gltex) { glUniform1i(s_u_use_tex, 1); glBindTexture(GL_TEXTURE_2D, gltex);
                         textured_tris += (e - t); }
            else       { glUniform1i(s_u_use_tex, 0); }
            glDrawArrays(GL_TRIANGLES, (GLint)(t * 3), (GLsizei)((e - t) * 3));
            t = e;
        }
        glBindVertexArray(0);
        drawn = ntris;
    }

    /* --- composite FBO colour over whatever is on the default framebuffer --- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(vx, vy, w, h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_comp_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
    glUniform1i(s_comp_samp, 0);
    glBindVertexArray(s_comp_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDisable(GL_BLEND);

    {
        static unsigned n;
        if ((n++ % 120) == 0)
            GLLOG("3D render: %u tris (%u textured, %u verts, %d tex cached)",
                  drawn, textured_tris, nverts, tex_n);
    }
}

#else  /* !__ANDROID__ : stubs so shared callers link on the oracle */

#include "nv2a_pgraph_gles.h"
int  nv2a_gles_enabled(void) { return 0; }
void nv2a_gles_clear(uint32_t argb) { (void)argb; }
void nv2a_gles_tri(const float a[4], const float b[4], const float c[4], uint32_t d)
{ (void)a; (void)b; (void)c; (void)d; }
void nv2a_gles_texture(uint32_t texid, int w, int h, int au, int av, const void *px)
{ (void)texid; (void)w; (void)h; (void)au; (void)av; (void)px; }
void nv2a_gles_tri_tex(const float a[4], const float b[4], const float c[4],
                       const float u0[2], const float u1[2], const float u2[2],
                       uint32_t argb, uint32_t texid)
{ (void)a; (void)b; (void)c; (void)u0; (void)u1; (void)u2; (void)argb; (void)texid; }
void nv2a_gles_frame(void) {}
void nv2a_gles_render(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

#endif
