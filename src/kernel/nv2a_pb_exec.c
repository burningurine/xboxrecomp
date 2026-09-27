/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
#include "frame_stats.h"
#include "nv2a_backend.h"

/* Optional GPU backend (see nv2a_backend.h). While one is registered the
 * executor still tracks all state and kernel-visible side effects, but its
 * own CPU/GLES rasterisers are skipped: the backend draws the same stream. */
static const NV2ABackend *s_backend;
void nv2a_backend_register(const NV2ABackend *backend) { s_backend = backend; }
const NV2ABackend *nv2a_backend(void) { return s_backend; }
void nv2a_pb_exec_segment_end(void)
{
    if (s_backend && s_backend->flush)
        s_backend->flush();
}
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "../nv2a/nv2a_pgraph_gles.h"   /* GLES 3D path for object-space batches */

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818

/* GPU->CPU sync. A title renders a batch, releases a semaphore with a value,
 * and the CPU spins on that memory word until it appears -- "the GPU has
 * finished this frame". Nothing here released it, so a title that gates its
 * next frame (or a scene's init) on the semaphore hangs with the GPU idle.
 * Written the moment the executor reaches the release: it has, by then, carried
 * out everything before it, which is exactly what the value means. */
#define NV097_SET_CONTEXT_DMA_SEMAPHORE       0x01A4
#define NV097_SET_SEMAPHORE_OFFSET            0x1D6C
#define NV097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  D3DCOLOR */

/* Fixed-function transform methods. The composite matrix is modelview x
 * projection (clip space); the viewport offset/scale map NDC to the surface. */
#define NV097_SET_COMPOSITE_MATRIX        0x0680   /* 16 floats, row-major */
#define NV097_SET_VIEWPORT_OFFSET         0x0A20   /* 4 floats             */
#define NV097_SET_VIEWPORT_SCALE          0x0AF0   /* 4 floats             */
#define NV097_SET_TRANSFORM_EXECUTION_MODE 0x1E94  /* MODE in bits 0..1    */
/* Vertex-program constants: the uniforms a title's transform program reads.
 * A 2D/UI program is almost always oPos = mul(v0, matrix), the matrix sitting
 * in four consecutive constant registers -- so capturing the block lets the
 * position be transformed without running the microcode. */
#define NV097_SET_TRANSFORM_CONSTANT      0x0B80   /* up to 32 floats/run  */
#define NV097_SET_TRANSFORM_CONSTANT_LOAD 0x1EA4   /* starting register    */
#define NV2A_VP_CONSTS                    192      /* c[0..191]            */
/* Vertex-program microcode: the transform program itself, 4 dwords per
 * instruction. Executed per vertex (nv2a_vp.inc.c) to turn object-space
 * positions into clip space -- the only way Blinx's programmable-pipeline
 * geometry can be placed, since it uses no composite matrix. */
#define NV097_SET_TRANSFORM_PROGRAM       0x0B00   /* 32 dwords/run        */
#define NV097_SET_TRANSFORM_PROGRAM_LOAD  0x1E9C   /* start instruction    */
#define NV097_SET_TRANSFORM_PROGRAM_START 0x1EA0   /* exec entry point     */
#define NV2A_VP_MAX_INSN                  136      /* hardware program size */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
#define IMM_VERTEX_DWORDS 7

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  4096
#define NV_MAX_INLINE   4096            /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint16_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    float      imm_pos[4];
    uint32_t   imm_diffuse;
    float      imm_tex[2];
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    Texture  tex;
    /* Fixed-function transform state. A title running true fixed-function T&L
     * (SET_TRANSFORM_EXECUTION_MODE MODE=0) hands over object-space positions
     * plus the composite matrix and viewport that turn them into screen pixels.
     * Without applying them the raw buffer coordinates land wherever the model
     * sits -- Blinx's UI came out at x -348..222 on a 640-wide surface. Captured
     * here and applied to attribute 0 in fetch_position(). */
    float    composite[16];    /* SET_COMPOSITE_MATRIX 0x0680, row-major       */
    float    vp_offset[4];     /* SET_VIEWPORT_OFFSET  0x0A20                   */
    float    vp_scale[4];      /* SET_VIEWPORT_SCALE   0x0AF0                   */
    uint32_t xform_mode;       /* SET_TRANSFORM_EXECUTION_MODE 0x1E94, MODE&3   */
    int      composite_set, viewport_set;
    int      vp_pretransformed;/* batch's program emits screen space, not clip  */
    /* Vertex-program constant registers, and the register the next
     * SET_TRANSFORM_CONSTANT run writes from. A UI program's transform matrix
     * lives in four of these; which four is what the microcode selects. */
    float    xform_const[NV2A_VP_CONSTS * 4];
    uint32_t const_load;
    /* The vertex program itself: 4 dwords per instruction, a write cursor that
     * the LOAD method resets, and the entry point START selects. Run per vertex
     * by vp_execute() to transform object space to clip space -- the general
     * answer that a single affine cannot be, because different batches author
     * their geometry in different object spaces. */
    uint32_t vp_prog[NV2A_VP_MAX_INSN * 4];
    uint32_t vp_prog_cursor;
    uint32_t vp_prog_len;
    uint32_t vp_prog_start;
    /* GPU->CPU semaphore. SET_SEMAPHORE_OFFSET names where in the semaphore
     * DMA object the release value goes; the release writes it there, so a
     * title spinning on that word for "the GPU has finished this batch" -- the
     * standard D3D8 way to gate the next frame or a scene's resource setup --
     * sees it and moves on instead of hanging with the GPU already idle. */
    uint32_t sema_offset;
    uint32_t sema_releases;
} s_gpu;

/* Reinterpret a method parameter's bits as the float the title wrote. Matrix
 * and viewport methods carry IEEE-754 floats in the 32-bit parameter word. */
static float param_as_float(uint32_t p)
{
    float f;
    memcpy(&f, &p, sizeof f);
    return f;
}

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.tex.offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.tex.color;
        s_tex_use[s_tex_use_count].width   = s_gpu.tex.width;
        s_tex_use[s_tex_use_count].height  = s_gpu.tex.height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static void note_unhandled(uint32_t method, uint32_t param)
{
    int i;

    /* With a renderer backend the method was already handed over; the
     * inventory only serves the CPU raster's bring-up (7% of the game thread
     * on the RP6 at scene 9 -- a linear search per method). */
    if (s_backend)
        return;
    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 1:                                  /* S1: normalised signed short */
        /* 16-bit signed, mapped to [-1,1]. Titles pack normals and often
         * texture coordinates this way; unhandled it read as zero, which pins a
         * batch's texcoords at (0,0) -- the corner -- so a DXT sprite sampled its
         * transparent border and drew black while its texture decoded fine. */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case 5:                                  /* S32K: signed short, unnormalised */
        /* 16-bit signed as an integer value; the program scales it (a title's
         * texcoords arrive as raw shorts that oT0 = v * const turns into [0,1]). */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* CMP: 11/11/10 packed, normalised */
        /* Three signed components in one dword -- x,y in 11 bits, z in 10 --
         * each normalised to [-1,1]. Xbox packs normals and low-precision
         * positions this way; unhandled it read as zero, so a batch using it
         * drew degenerate (a flat black triangle). Size is implicitly 3. */
        uint32_t v = *(const uint32_t *)p;
        int32_t x = (int32_t)(v << 21) >> 21;   /* bits 0..10   */
        int32_t y = (int32_t)(v << 10) >> 21;   /* bits 11..21  */
        int32_t z = (int32_t)(v)       >> 22;   /* bits 22..31  */
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        out[3] = 1.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* ===========================================================================
 * Software NV2A vertex-program interpreter.
 *
 * Blinx transforms every batch with a vertex program (execution mode 2) and
 * different batches author their geometry in different object spaces, so no
 * single affine can place them all -- only running each batch's own program
 * does. The 128-bit instruction encoding is the canonical NV2A layout
 * (xboxdevwiki NV2A/Vertex_Shader: word 0 unused, fields in words 1-3, bit 0 =
 * LSB) -- NOT src/d3d/d3d8_vsh.c, whose field offsets are wrong (they decode
 * real microcode as all-NOP). This executes the program, producing the
 * clip-space position (oPos) per vertex. The perspective divide and viewport
 * that follow are fetch_position's, exactly as on the hardware.
 * =========================================================================== */

/* `count` bits at bit `start` of a 128-bit instruction (4 dwords, bit 0 = LSB
 * of word 0) -- the same convention as d3d8_vsh.c's vsh_extract. */
static uint32_t vsh_bits(const uint32_t *insn, int start, int count)
{
    int w = start / 32, o = start % 32;
    uint32_t mask = (count == 32) ? 0xFFFFFFFFu : ((1u << count) - 1);
    if (o + count <= 32)
        return (insn[w] >> o) & mask;
    return ((insn[w] >> o) | (insn[w + 1] << (32 - o))) & mask;
}

/* One source operand, swizzled and negated. `mux` (register file) 2 = input
 * v[input_idx], 3 = const c[const_idx (+a0 if relative)], else temp r[temp_reg].
 * `swx` is the bit of swizzle component X; Y/Z/W are two bits lower each. */
static void vsh_src(const uint32_t *insn, int neg_bit, int mux_start, int temp_reg,
                    int swx, int input_idx, int const_idx, int rel, int a0,
                    const float vin[16][4], const float r[16][4], float out[4])
{
    uint32_t mux = vsh_bits(insn, mux_start, 2);
    const float *base;
    int sw[4], i;

    sw[0] = vsh_bits(insn, swx,     2); sw[1] = vsh_bits(insn, swx - 2, 2);
    sw[2] = vsh_bits(insn, swx - 4, 2); sw[3] = vsh_bits(insn, swx - 6, 2);
    if (mux == 2) {
        base = vin[input_idx & 15];
    } else if (mux == 3) {
        int ci = const_idx + (rel ? a0 : 0);
        if (ci < 0) ci = 0; else if (ci >= NV2A_VP_CONSTS) ci = NV2A_VP_CONSTS - 1;
        base = &s_gpu.xform_const[ci * 4];
    } else {
        base = r[temp_reg & 15];
    }
    for (i = 0; i < 4; i++) out[i] = base[sw[i]];
    if (vsh_bits(insn, neg_bit, 1))
        for (i = 0; i < 4; i++) out[i] = -out[i];
}

static void vsh_write(float *dst, const float v[4], uint32_t mask)
{
    if (mask & 0x8) dst[0] = v[0];
    if (mask & 0x4) dst[1] = v[1];
    if (mask & 0x2) dst[2] = v[2];
    if (mask & 0x1) dst[3] = v[3];
}

/* Run the loaded program for one vertex; write clip-space oPos to `opos`, and,
 * when the caller asks for them, the program's diffuse output oD0 to `odiff` and
 * texture coordinate 0 (oT0) to `otex` -- both computed by the program, not read
 * from a raw attribute. Returns 1 if the program wrote oPos, else 0. */
static int vp_execute(uint32_t index, float opos[4], float odiff[4], float otex[4])
{
    float r[16][4], o[16][4], vin[16][4];
    int owrote[16];
    int a0 = 0, i, pc, guard;

    if (!s_gpu.vp_prog_len)
        return 0;

    memset(r, 0, sizeof r);
    memset(o, 0, sizeof o);
    memset(owrote, 0, sizeof owrote);
    for (i = 0; i < 16; i++) {
        vin[i][0] = vin[i][1] = vin[i][2] = 0.0f; vin[i][3] = 1.0f;
        if (s_gpu.attr[i].offset && s_gpu.attr[i].stride)
            fetch_attr(&s_gpu.attr[i], index, vin[i]);
    }

    {
        static int dumped = 0;
        if (!dumped && getenv("RECOMP_VP_DEBUG")) {
            int p;
            dumped = 1;
            fprintf(stderr, "[VPPROG] start=%u len=%u\n",
                    s_gpu.vp_prog_start, s_gpu.vp_prog_len);
            for (p = (int)s_gpu.vp_prog_start;
                 p < NV2A_VP_MAX_INSN && p < (int)s_gpu.vp_prog_start + 24; p++) {
                const uint32_t *w = &s_gpu.vp_prog[p * 4];
                fprintf(stderr, "  I%03d %08X %08X %08X %08X mac=%u ilu=%u"
                        " const=%u inp=%u Amux=%u Areg=%u oreg=%u dmux=%u"
                        " mmask=%X omask=%X fin=%u\n",
                        p, w[0], w[1], w[2], w[3],
                        vsh_bits(w,53,4), vsh_bits(w,57,3), vsh_bits(w,45,8),
                        vsh_bits(w,41,4), vsh_bits(w,90,2), vsh_bits(w,92,4),
                        vsh_bits(w,99,8)&0xF, vsh_bits(w,98,1),
                        vsh_bits(w,120,4), vsh_bits(w,108,4), vsh_bits(w,96,1));
                if (vsh_bits(w, 96, 1)) break;
            }
        }
    }

    pc = (int)s_gpu.vp_prog_start;
    for (guard = 0; pc >= 0 && pc < NV2A_VP_MAX_INSN && guard < 256; pc++, guard++) {
        const uint32_t *insn = &s_gpu.vp_prog[pc * 4];
        /* Canonical NV2A layout (xboxdevwiki): word 0 is unused; fields live in
         * words 1-3, bit 0 = LSB. Source register files by mux: 2=input,
         * 3=const, else temp. Output register goes to MAC (dmux 0) or ILU
         * (dmux 1); the temp register takes MAC (mac_mask) and ILU (ilu_mask). */
        uint32_t mac = vsh_bits(insn, 53, 4);
        uint32_t ilu = vsh_bits(insn, 57, 3);
        int const_idx = (int)vsh_bits(insn, 45, 8);
        int input_idx = (int)vsh_bits(insn, 41, 4);
        int rel = (int)vsh_bits(insn, 97, 1);
        int is_final = (int)vsh_bits(insn, 96, 1);
        int dtemp = (int)vsh_bits(insn, 116, 4);
        uint32_t mac_mask = vsh_bits(insn, 120, 4);
        uint32_t ilu_mask = vsh_bits(insn, 112, 4);
        uint32_t out_mask = vsh_bits(insn, 108, 4);
        uint32_t out_reg  = vsh_bits(insn, 99, 8) & 0xF;
        int dmux = (int)vsh_bits(insn, 98, 1);
        int creg = (int)(vsh_bits(insn, 64, 2) | (vsh_bits(insn, 126, 2) << 2));
        float A[4], B[4], C[4], res[4];
        int j;

        vsh_src(insn, 40,  90, (int)vsh_bits(insn, 92, 4), 38, input_idx, const_idx, rel, a0, vin, r, A);
        vsh_src(insn, 89,  75, (int)vsh_bits(insn, 77, 4), 87, input_idx, const_idx, rel, a0, vin, r, B);
        vsh_src(insn, 74, 124, creg,                       72, input_idx, const_idx, rel, a0, vin, r, C);

        if (mac != 0) {
            float d;
            switch (mac) {
            case 1:  for (j=0;j<4;j++) res[j]=A[j]; break;                          /* MOV */
            case 2:  for (j=0;j<4;j++) res[j]=A[j]*B[j]; break;                     /* MUL */
            case 3:  for (j=0;j<4;j++) res[j]=A[j]+C[j]; break;                     /* ADD */
            case 4:  for (j=0;j<4;j++) res[j]=A[j]*B[j]+C[j]; break;                /* MAD */
            case 5:  d=A[0]*B[0]+A[1]*B[1]+A[2]*B[2];        for(j=0;j<4;j++)res[j]=d; break; /* DP3 */
            case 6:  d=A[0]*B[0]+A[1]*B[1]+A[2]*B[2]+B[3];   for(j=0;j<4;j++)res[j]=d; break; /* DPH */
            case 7:  d=A[0]*B[0]+A[1]*B[1]+A[2]*B[2]+A[3]*B[3]; for(j=0;j<4;j++)res[j]=d; break; /* DP4 */
            case 8:  res[0]=1.0f; res[1]=A[1]*B[1]; res[2]=A[2]; res[3]=B[3]; break; /* DST */
            case 9:  for(j=0;j<4;j++)res[j]=A[j]<B[j]?A[j]:B[j]; break;             /* MIN */
            case 10: for(j=0;j<4;j++)res[j]=A[j]>B[j]?A[j]:B[j]; break;             /* MAX */
            case 11: for(j=0;j<4;j++)res[j]=A[j]<B[j]?1.0f:0.0f; break;             /* SLT */
            case 12: for(j=0;j<4;j++)res[j]=A[j]>=B[j]?1.0f:0.0f; break;            /* SGE */
            case 13: a0=(int)floorf(A[0]); break;                                  /* ARL */
            default: for(j=0;j<4;j++)res[j]=0.0f; break;
            }
            if (mac != 13) {
                vsh_write(r[dtemp & 15], res, mac_mask);
                if (dmux == 0 && out_mask && out_reg <= 12) {
                    vsh_write(o[out_reg], res, out_mask);
                    if (out_reg == 0) owrote[0] = 1;
                }
            }
        }

        if (ilu != 0) {
            float v = C[0], s;
            switch (ilu) {
            case 1:  for(j=0;j<4;j++)res[j]=C[j]; break;                           /* MOV */
            case 2:  s=(v!=0.0f)?1.0f/v:0.0f;               for(j=0;j<4;j++)res[j]=s; break; /* RCP */
            case 3:  s=(v!=0.0f)?1.0f/v:0.0f;               for(j=0;j<4;j++)res[j]=s; break; /* RCC~ */
            case 4:  s=1.0f/sqrtf(v<0.0f?-v:(v==0.0f?1e-30f:v)); for(j=0;j<4;j++)res[j]=s; break; /* RSQ */
            case 5:  s=exp2f(v);                            for(j=0;j<4;j++)res[j]=s; break; /* EXP */
            case 6:  s=(v>0.0f)?log2f(v):0.0f;              for(j=0;j<4;j++)res[j]=s; break; /* LOG */
            case 7:  for(j=0;j<4;j++)res[j]=1.0f; break;                           /* LIT~ */
            default: for(j=0;j<4;j++)res[j]=0.0f; break;
            }
            {
                vsh_write(r[dtemp & 15], res, ilu_mask);
                if (dmux == 1 && out_mask && out_reg <= 12) {
                    vsh_write(o[out_reg], res, out_mask);
                    if (out_reg == 0) owrote[0] = 1;
                }
            }
        }

        if (is_final)
            break;
    }

    {
        static int dbg = -1, n = 0;
        if (dbg < 0) dbg = getenv("RECOMP_VP_DEBUG") ? 1 : 0;
        if (dbg && n < 10) {
            const float *c = s_gpu.xform_const;
            n++;
            fprintf(stderr,
                "[VP] start=%u v0=(%.2f,%.2f,%.2f,%.2f) c98=(%.2f,%.2f,%.2f,%.2f)"
                " oPos=(%.2f,%.2f,%.2f,%.4f) vp_scale=(%.3f,%.3f,%.3f)"
                " vp_off=(%.3f,%.3f,%.3f) wrote=%d\n",
                s_gpu.vp_prog_start,
                vin[0][0],vin[0][1],vin[0][2],vin[0][3],
                c[98*4],c[98*4+1],c[98*4+2],c[98*4+3],
                o[0][0],o[0][1],o[0][2],o[0][3],
                s_gpu.vp_scale[0],s_gpu.vp_scale[1],s_gpu.vp_scale[2],
                s_gpu.vp_offset[0],s_gpu.vp_offset[1],s_gpu.vp_offset[2],
                owrote[0]);
        }
    }

    /* oD0 is output register 3, oT0 is register 9 (NV2A output-mux order). */
    if (odiff) { odiff[0]=o[3][0]; odiff[1]=o[3][1]; odiff[2]=o[3][2]; odiff[3]=o[3][3]; }
    if (otex)  { otex[0]=o[9][0];  otex[1]=o[9][1];  otex[2]=o[9][2];  otex[3]=o[9][3]; }

    if (!owrote[0])
        return 0;
    opos[0]=o[0][0]; opos[1]=o[0][1]; opos[2]=o[0][2]; opos[3]=o[0][3];
    return 1;
}

/* The vertex-program transform, chosen from the environment once.
 *
 * Blinx transforms its UI with a vertex program (MODE 2), not a composite
 * matrix, so its object-space positions have to be mapped some other way. Two
 * knobs, so the mapping can be found live against the oracle without a rebuild
 * for each guess:
 *
 *   RECOMP_VP_MATRIX_SLOT=N  -- position x the four constant registers c[N..N+3]
 *     (the matrix the microcode multiplies by), row-vector; add
 *     RECOMP_VP_MATRIX_TRANSPOSE=1 to read them column-wise. This is the exact
 *     transform once N is known from the program.
 *   RECOMP_VP_NDC=kx,ky,bx,by -- a plain affine object->NDC (ndc = obj*k + b),
 *     for a 2D UI whose program is only a scale and offset. Enough to place the
 *     sprites while the exact matrix is still being pinned down.
 *
 * Fills clip[] (pre-divide) and returns 1 when configured, else 0 (raw). */
static int vp_clip_from_env(const float in[4], float clip[4])
{
    static int inited, kind, slot, transpose;
    static float k[4];

    if (!inited) {
        const char *s;
        inited = 1;
        if ((s = getenv("RECOMP_VP_MATRIX_SLOT")) != NULL) {
            slot = atoi(s);
            transpose = getenv("RECOMP_VP_MATRIX_TRANSPOSE") ? 1 : 0;
            if (slot >= 0 && slot + 3 < NV2A_VP_CONSTS)
                kind = 2;
        } else if ((s = getenv("RECOMP_VP_NDC")) != NULL) {
            k[0] = k[1] = 1.0f; k[2] = k[3] = 0.0f;
            sscanf(s, "%f,%f,%f,%f", &k[0], &k[1], &k[2], &k[3]);
            kind = 1;
        } else if (!getenv("RECOMP_VP_RAW")) {
            /* Default: the object->NDC affine that lands Blinx's UI. Its
             * program authors 2D geometry in a screen-centred pixel space, so
             * ndc = obj*(1/half_w, -1/half_h) composes with the standard
             * viewport (scale 320/-240, offset ~320/240) to put it in
             * 0..640 x 0..480. RECOMP_VP_RAW=1 disables it (draw raw), and
             * RECOMP_VP_NDC overrides the constants. */
            k[0] = 1.0f / 320.0f; k[1] = -1.0f / 240.0f; k[2] = k[3] = 0.0f;
            kind = 1;
        }
    }

    if (kind == 2) {
        const float *c = &s_gpu.xform_const[slot * 4];
        int i, j;
        for (j = 0; j < 4; j++) {
            clip[j] = 0.0f;
            for (i = 0; i < 4; i++)
                clip[j] += in[i] * (transpose ? c[i * 4 + j] : c[j * 4 + i]);
        }
        return 1;
    }
    if (kind == 1) {
        clip[0] = in[0] * k[0] + k[2];
        clip[1] = in[1] * k[1] + k[3];
        clip[2] = in[2];
        clip[3] = 1.0f;
        return 1;
    }
    return 0;
}

/* The raw transform output for vertex `index`, before the perspective divide
 * and viewport: for a fixed-function batch (MODE 0) the composite matrix, for a
 * vertex-program batch (MODE 2) the program itself (or the affine fallback).
 * Returns 1 with `clip` filled if a transform ran, 0 if the coordinates are raw
 * (already in `out`), -1 if attribute 0 could not be read. */
static int vp_raw_clip(uint32_t index, float out[4], float clip[4])
{
    float in[4];
    int i, j;

    if (!fetch_attr(&s_gpu.attr[0], index, out))
        return -1;
    if (s_gpu.inline_active || !s_gpu.viewport_set)
        return 0;

    in[0] = out[0]; in[1] = out[1]; in[2] = out[2]; in[3] = 1.0f;

    if (s_gpu.xform_mode == 0 && s_gpu.composite_set) {
        const float *m = s_gpu.composite;
        for (j = 0; j < 4; j++) {
            clip[j] = 0.0f;
            for (i = 0; i < 4; i++)
                clip[j] += in[i] * m[i * 4 + j];
        }
        return 1;
    }
    if (s_gpu.xform_mode == 2) {
        static int interp = -1;
        if (interp < 0)
            interp = getenv("RECOMP_VP_NOINTERP") ? 0 : 1;
        if (interp && vp_execute(index, clip, NULL, NULL))
            return 1;
        if (vp_clip_from_env(in, clip))
            return 1;
    }
    return 0;
}

/* Decide, once per batch, whether its program emits screen-space coordinates
 * rather than clip space. Blinx's UI program does the whole transform itself --
 * it adds the viewport offset (c98 = 320,240) to a screen-centred position and
 * carries RHW in w -- so its output is already surface pixels; re-applying the
 * perspective divide and viewport would transform it a second time and throw it
 * off the surface. A clip-space batch spans the NDC cube (|xy/w| ~ 1); a
 * pre-transformed one spans the surface in pixels (hundreds). Sampled so every
 * vertex in the batch is then read the same way. */
static void classify_pretransformed(void)
{
    float o[4], clip[4], maxabs = 0.0f;
    uint32_t k, step, n = 0;

    s_gpu.vp_pretransformed = 0;
    /* Spread the samples across the whole batch: a leading degenerate triangle
     * (all vertices at one point) must not be the only thing looked at. */
    step = s_gpu.idx_count > 96 ? s_gpu.idx_count / 96 : 1;
    for (k = 0; k < s_gpu.idx_count; k += step) {
        if (vp_raw_clip(s_gpu.idx[k], o, clip) != 1)
            continue;
        {
            float w  = clip[3] != 0.0f ? clip[3] : 1.0f;
            float ax = clip[0] / w, ay = clip[1] / w;
            if (ax < 0.0f) ax = -ax;
            if (ay < 0.0f) ay = -ay;
            if (ax > maxabs) maxabs = ax;
            if (ay > maxabs) maxabs = ay;
            n++;
        }
    }
    /* NDC lands within ~[-1,1]; a program that already applied the viewport
     * lands its coordinates in surface pixels (hundreds). Any vertex far outside
     * the cube means the batch is pre-transformed -- max magnitude, not span, so
     * one degenerate sample can't hide a screen-space batch. */
    if (n && maxabs > 4.0f)
        s_gpu.vp_pretransformed = 1;

    if (getenv("RECOMP_VP_DEBUG")) {
        static int m = 0;
        if (m++ < 16)
            fprintf(stderr, "[CLASSIFY] idx=%u samples=%u maxabs_ndc=%.1f -> %s\n",
                    s_gpu.idx_count, n, maxabs,
                    s_gpu.vp_pretransformed ? "screen" : "clip");
    }
}

/* Position of vertex `index`, in surface pixels: the raw transform, then either
 * the program's own screen space used directly (pre-transformed batches) or the
 * perspective divide + viewport that map the clip cube to the surface. */
static int fetch_position(uint32_t index, float out[4])
{
    float clip[4];
    int r = vp_raw_clip(index, out, clip);

    if (r < 0) return 0;
    if (r == 0) return 1;

    if (s_gpu.vp_pretransformed) {
        out[0] = clip[0]; out[1] = clip[1]; out[2] = clip[2]; out[3] = clip[3];
        return 1;
    }

    if (clip[3] != 0.0f) {
        float iw = 1.0f / clip[3];
        clip[0] *= iw; clip[1] *= iw; clip[2] *= iw;
    }
    out[0] = clip[0] * s_gpu.vp_scale[0] + s_gpu.vp_offset[0];
    out[1] = clip[1] * s_gpu.vp_scale[1] + s_gpu.vp_offset[1];
    out[2] = clip[2] * s_gpu.vp_scale[2] + s_gpu.vp_offset[2];
    out[3] = clip[3];
    /* Diagnostic for the blown-up-coordinate path: a clip-space vertex whose w is
     * ~0 or negative divides to a huge/mirrored screen position (no near-plane
     * clipping here). Logs the first few so a w-near-0 clip case can be told from
     * a VP that emits garbage clip coords. RECOMP_XFORM_LOG=1. */
    {
        static int xf_log = -1;
        static unsigned xf_n;
        if (xf_log < 0) xf_log = getenv("RECOMP_XFORM_LOG") != NULL;
        if (xf_log && xf_n < 48 && (out[0] < -64 || out[0] > 704
                                 || out[1] < -64 || out[1] > 544)) {
            /* Also show the draw number, the vertex index, and the raw attr-0
             * (position) input, so a constant output can be traced: same v0 for
             * different idx = a fetch that isn't applying idx*stride on this path
             * (constant input); varying v0 but constant screen = a VP issue. */
            float v0[4] = { 0, 0, 0, 1 };
            if (s_gpu.attr[0].offset && s_gpu.attr[0].stride)
                fetch_attr(&s_gpu.attr[0], index, v0);
            fprintf(stderr, "  [XFORM] draw%u idx%u v0=(%.2f,%.2f,%.2f) a0(t%u "
                    "s%u str%u) clipw=%.4f ndc(%.3f,%.3f) -> screen(%.1f,%.1f)\n",
                    s_gpu.draws, index, v0[0], v0[1], v0[2],
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride,
                    out[3], clip[0], clip[1], out[0], out[1]);
            xf_n++;
            fflush(stderr);
        }
    }
    return 1;
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + dma_resolve(s_gpu.drawn_offset
                                                ? s_gpu.drawn_offset
                                                : s_gpu.color_offset)
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Present the COMPLETED frame, not the buffer being cleared. A title that
     * double-buffers renders into the back buffer (color_offset) and displays
     * the other; pointing the presenter at color_offset here -- the buffer this
     * very CLEAR is about to wipe -- shows a black/half-drawn buffer (the GLES
     * presenter came up black on the RP6 for exactly this, while the guest FB
     * held a full 0xFFFFFFFF-bright frame). drawn_offset is the surface the last
     * batch actually finished into = the current front buffer, which is also
     * what the FB dump reads. The window reads the resolved address, not the
     * DMA-object offset. Falls back to color_offset before the first draw. */
    xbox_FramebufferWindowSet(dma_resolve(s_gpu.drawn_offset ? s_gpu.drawn_offset
                                                             : s_gpu.color_offset),
                              s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return c % size;
    return c >= size ? size - 1 : c;       /* clamp, and everything else */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!s_gpu.tex.valid)
        return 0;
    u = wrap_coord(u, s_gpu.tex.width,  s_gpu.tex.addr_u);
    v = wrap_coord(v, s_gpu.tex.height, s_gpu.tex.addr_v);

    fmt = s_gpu.tex.color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(mem + s_gpu.tex.offset, fmt, u, v,
                                     s_gpu.tex.width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + s_gpu.tex.offset;
        u = swizzle_offset(u, v, s_gpu.tex.width, s_gpu.tex.height);
    } else {
        p = mem + s_gpu.tex.offset + (size_t)v * s_gpu.tex.pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    default:
        return 0;
    }
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, s_gpu.tex.offset, s_gpu.tex.color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.tex.color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = dma_resolve(s_gpu.color_offset);

    if (surface_hits_image(base, (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    return 1;
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    row = s_surface + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.tex.valid;
    /* Diagnostic: the first texel each of the first few textured triangles
     * actually fetches, so a texture that dumps as real art but paints black can
     * be told apart -- real texel here means the sampler is right and the loss is
     * in the write/present; a black texel here means wrong coords or texture.
     * RECOMP_TEXEL_LOG=1. */
    static int texel_log = -1;
    static unsigned texel_log_n;
    int logged = 0;
    if (texel_log < 0)
        texel_log = getenv("RECOMP_TEXEL_LOG") != NULL;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    /* Level screen-space-black diagnostic (RECOMP_RASTERPROBE): gated to level
     * scenes (0x51A0FC&0x7F >= 5) so it skips the intro/menu tris that already
     * work and captures the warp-level's black tris -- logs textured flag, tex
     * fmt, uv0, the texel it would sample, the flat argb, bpp and surface, to
     * tell texel-black (uv/fmt) from argb-black from a wrong-surface write. */
    {
        static int rp = -1;
        if (rp < 0) rp = getenv("RECOMP_RASTERPROBE") != NULL;
        if (rp) {
            uint32_t scene = (*(const uint32_t *)(mem + 0x0051A0FCu)) & 0x7Fu;
            static unsigned rp_n;
            if (scene >= 5u && rp_n < 64u) {
                uint32_t t0 = 0; int got = 0;
                if (textured) {
                    float su0 = uv[0][0] < 0.0f ? 0.0f : uv[0][0];
                    float sv0 = uv[0][1] < 0.0f ? 0.0f : uv[0][1];
                    got = sample_texture((uint32_t)su0, (uint32_t)sv0, &t0);
                }
                fprintf(stderr, "  [RASTERP] sc%u tri%u textured=%d valid=%d "
                        "fmt0x%02X uv0(%.1f,%.1f) argb=%08X texel=%08X(g%d) "
                        "bpp=%u co=0x%08X\n",
                        scene, rp_n++, textured, s_gpu.tex.valid, s_gpu.tex.color,
                        uv ? uv[0][0] : -1.0f, uv ? uv[0][1] : -1.0f,
                        argb, t0, got, bpp, s_gpu.color_offset);
                fflush(stderr);
            }
        }
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    if (texel_log && !logged && texel_log_n < 64) {
                        fprintf(stderr, "  [TEXEL] tri%u tex@0x%08X fmt0x%02X "
                                "%ux%u uv(%.1f,%.1f) -> 0x%08X\n",
                                texel_log_n++, s_gpu.tex.offset, s_gpu.tex.color,
                                s_gpu.tex.width, s_gpu.tex.height,
                                su, sv, texel);
                        logged = 1;
                    }
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    /* A program batch computes texture coordinate 0 (oT0); use it rather than a
     * raw attribute, which for Blinx holds the pre-scale input the program then
     * multiplies (oT0 = v4 * c136). Only for a batch that actually samples a
     * texture (a valid texture bound and a texcoord stream set) -- otherwise the
     * batch is flat-shaded from oD0 and must report no texcoords so the raster
     * path does not sample a stale texture over it. */
    if (s_gpu.xform_mode == 2 && s_gpu.vp_prog_len && !s_gpu.inline_active) {
        const VertexAttr *tc = texcoord_attr();
        float dummy[4], tex[4];
        if (!s_gpu.tex.valid || !(tc->offset && tc->stride))
            return 0;
        vp_execute(index, dummy, NULL, tex);
        out[0] = tex[0];
        out[1] = tex[1];
    } else {
        if (!fetch_attr(texcoord_attr(), index, t))
            return 0;
        out[0] = t[0];
        out[1] = t[1];
    }
    if (tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];
    static int checked, have_forced;
    static uint32_t forced;

    /* A vertex program computes the diffuse colour (oD0) rather than reading it
     * from an attribute, so an untextured program batch draws with whatever the
     * raw slot happens to hold -- for Blinx that is 0, i.e. black on black.
     * RECOMP_FORCE_VCOLOR=RRGGBB (or AARRGGBB) forces a colour so the geometry
     * is visible while the colour path is sorted out. */
    if (!checked) {
        const char *s = getenv("RECOMP_FORCE_VCOLOR");
        checked = 1;
        if (s) { forced = (uint32_t)strtoul(s, NULL, 16); have_forced = 1; }
    }
    if (have_forced)
        return forced;

    /* Program batches compute the diffuse colour (oD0); use it rather than the
     * raw attribute the program never fed (which reads black for Blinx). */
    if (s_gpu.xform_mode == 2 && s_gpu.vp_prog_len && !s_gpu.inline_active) {
        float d[4], dummy[4];
        int q, comp[4];
        vp_execute(index, dummy, d, NULL);
        for (q = 0; q < 4; q++) {
            comp[q] = (int)(d[q] * 255.0f + 0.5f);
            if (comp[q] < 0) comp[q] = 0; else if (comp[q] > 255) comp[q] = 255;
        }
        return ((uint32_t)comp[3] << 24) | ((uint32_t)comp[0] << 16)
             | ((uint32_t)comp[1] <<  8) |  (uint32_t)comp[2];
    }

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_position(s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_position(s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types that are triangles under some winding. */
#define NV_PRIM_TRIANGLES      4
#define NV_PRIM_TRIANGLE_STRIP 5
#define NV_PRIM_TRIANGLE_FAN   6
#define NV_PRIM_QUADS          7
#define NV_PRIM_QUAD_STRIP     8

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!fetch_position(i0, p[0])
     || !fetch_position(i1, p[1])
     || !fetch_position(i2, p[2]))
        return;

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

#if defined(__ANDROID__)
/* Emit-side texture cache: one stable GLES texid per bound-texture identity
 * (guest offset + format + dimensions). The first time a texture is seen it is
 * decoded to RGBA8 through the CPU sampler (sample_texture: swizzled/DXT/linear)
 * and registered with the GLES module; thereafter the same identity re-uses its
 * texid. Runs only on the parser thread, so no lock is needed here. */
typedef struct { uint32_t offset, color, w, h, texid; } GlesTexKey;
static GlesTexKey s_gles_tex[512];
static int        s_gles_tex_n;
static uint32_t   s_gles_next_texid = 1;

/* Decode the currently-bound texture (s_gpu.tex) and hand it to the GLES module.
 * Returns a stable texid, or 0 if the texture is not usable (invalid, too big,
 * or an unsupported format the sampler declines). Texture state is constant
 * within a batch, so callers resolve this once per batch. */
static uint32_t gles_texid_for_current(void)
{
    uint32_t w, h, x, y, probe = 0;
    uint8_t *rgba;
    int i;

    if (!s_gpu.tex.valid)
        return 0;
    w = s_gpu.tex.width; h = s_gpu.tex.height;
    if (!w || !h || w > 2048 || h > 2048)      /* cost/sanity bound */
        return 0;
    for (i = 0; i < s_gles_tex_n; i++)
        if (s_gles_tex[i].offset == s_gpu.tex.offset
         && s_gles_tex[i].color  == s_gpu.tex.color
         && s_gles_tex[i].w == w && s_gles_tex[i].h == h)
            return s_gles_tex[i].texid;
    if (s_gles_tex_n >= (int)(sizeof s_gles_tex / sizeof s_gles_tex[0]))
        return 0;                              /* cache full: draw untextured */
    if (!sample_texture(0, 0, &probe))         /* unsupported format */
        return 0;

    rgba = (uint8_t *)malloc((size_t)w * h * 4u);
    if (!rgba)
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t argb = 0xFF000000u;       /* opaque black if a texel declines */
            uint8_t *p = rgba + ((size_t)y * w + x) * 4u;
            sample_texture(x, y, &argb);       /* 0xAARRGGBB */
            p[0] = (uint8_t)((argb >> 16) & 0xFF);   /* R */
            p[1] = (uint8_t)((argb >>  8) & 0xFF);   /* G */
            p[2] = (uint8_t)( argb        & 0xFF);   /* B */
            p[3] = (uint8_t)((argb >> 24) & 0xFF);   /* A */
        }

    {
        uint32_t texid = s_gles_next_texid++;
        nv2a_gles_texture(texid, (int)w, (int)h,
                          (int)s_gpu.tex.addr_u, (int)s_gpu.tex.addr_v, rgba);
        free(rgba);                            /* the module keeps its own copy */
        s_gles_tex[s_gles_tex_n].offset = s_gpu.tex.offset;
        s_gles_tex[s_gles_tex_n].color  = s_gpu.tex.color;
        s_gles_tex[s_gles_tex_n].w = w;
        s_gles_tex[s_gles_tex_n].h = h;
        s_gles_tex[s_gles_tex_n].texid = texid;
        s_gles_tex_n++;
        return texid;
    }
}

/* One object-space triangle to the GLES rasteriser: transform each vertex to
 * clip space with the same software path the classifier uses (vp_raw_clip), and
 * let GLES place it with a real depth buffer. When a texture is bound and the
 * batch carries texcoords, pass normalised UVs + texid so GLES samples it;
 * otherwise fall back to the flat vertex colour (drawn untextured, not undrawn).
 * Android-only: the Windows oracle renders object-space via nv2a_pgraph_d3d11. */
/* [GLESDIAG] diagnostic counters: why level object-space batches emit few tris. */
static unsigned long g_gd_vpfail, g_gd_trisok;
static void gles_emit_clip_tri(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t texid)
{
    float o[4], c0[4], c1[4], c2[4];
    float uv0[2], uv1[2], uv2[2];
    if (vp_raw_clip(i0, o, c0) != 1) { g_gd_vpfail++; return; }
    if (vp_raw_clip(i1, o, c1) != 1) { g_gd_vpfail++; return; }
    if (vp_raw_clip(i2, o, c2) != 1) { g_gd_vpfail++; return; }
    g_gd_trisok++;
    if (texid
     && fetch_texcoord(i0, uv0) && fetch_texcoord(i1, uv1) && fetch_texcoord(i2, uv2)) {
        /* fetch_texcoord yields texel units (see its scaling); normalise for GL. */
        float iw = s_gpu.tex.width  ? 1.0f / (float)s_gpu.tex.width  : 0.0f;
        float ih = s_gpu.tex.height ? 1.0f / (float)s_gpu.tex.height : 0.0f;
        uv0[0] *= iw; uv0[1] *= ih; uv1[0] *= iw; uv1[1] *= ih; uv2[0] *= iw; uv2[1] *= ih;
        nv2a_gles_tri_tex(c0, c1, c2, uv0, uv1, uv2, vertex_color(i0), texid);
        return;
    }
    nv2a_gles_tri(c0, c1, c2, vertex_color(i0));
}

/* Emit an object-space (vertex-program) batch to GLES. Mirrors raster_batch's
 * primitive decode; the CPU path skips these batches entirely. The bound
 * texture is constant across the batch, so it is resolved once here. */
static void gles_emit_object_batch(void)
{
    uint32_t i;
    uint32_t texid = gles_texid_for_current();
    static unsigned long gd_batches, gd_prim[16], gd_idxlow, gd_tex0;
    gd_batches++;
    if (s_gpu.prim < 16u) gd_prim[s_gpu.prim]++;
    if (s_gpu.idx_count < 3u) gd_idxlow++;
    if (!texid) gd_tex0++;
    if ((gd_batches % 2000u) == 0u) {
        fprintf(stderr, "  [GLESDIAG] batches=%lu prim[1..10]=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu "
                "idxlow=%lu tex0=%lu | trisOK=%lu vpFAIL=%lu\n", gd_batches,
                gd_prim[1],gd_prim[2],gd_prim[3],gd_prim[4],gd_prim[5],gd_prim[6],gd_prim[7],
                gd_prim[8],gd_prim[9],gd_prim[10], gd_idxlow, gd_tex0, g_gd_trisok, g_gd_vpfail);
        fflush(stderr);
    }
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            gles_emit_clip_tri(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2], texid);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            gles_emit_clip_tri(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2], texid);
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_QUADS:
    case NV_PRIM_QUAD_STRIP:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            gles_emit_clip_tri(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1], texid);
        break;
    default:
        break;
    }
}
#endif /* __ANDROID__ */

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    /* Classify screen-space vs clip before any position is read, so every
     * vertex of the batch is transformed the same way. */
    classify_pretransformed();
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        /* The CPU raster can't place object-space 3D (no depth/perspective).
         * Hand it to the GLES rasteriser instead, which can. */
#if defined(__ANDROID__)
        if (nv2a_gles_enabled())
            gles_emit_object_batch();
#endif
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.tex.valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_QUADS:
    case NV_PRIM_QUAD_STRIP:
        /* A fan and a quad both rasterise as a triangle fan around index 0;
         * for a quad that is exactly its two triangles. */
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* The vertex format of the first few batches: which attribute streams are
     * set, and each one's type -- so a batch that reads as constant/zero can be
     * traced to an attribute type fetch_attr does not decode (types 1/5 short
     * were the culprit; 6 is compressed 11/11/10 and still unhandled).
     * RECOMP_ATTR_LOG=1. */
    {
        static int attr_log = -1, attr_n = 0;
        if (attr_log < 0)
            attr_log = getenv("RECOMP_ATTR_LOG") != NULL;
        if (attr_log && attr_n < 200) {
            int k;
            attr_n++;
            for (k = 0; k < NV_VERTEX_ATTRS; k++)
                if (s_gpu.attr[k].stride)
                    fprintf(stderr, "  [ATTR] batch%d v%d type=%u size=%u "
                            "stride=%u off=0x%08X\n", attr_n, k,
                            s_gpu.attr[k].type, s_gpu.attr[k].size,
                            s_gpu.attr[k].stride, s_gpu.attr[k].offset);
        }
    }

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_position(s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    raster_batch();

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.pitch, s_gpu.tex.color,
                    d3d8_format_is_swizzled(s_gpu.tex.color), s_gpu.tex.valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                        s_gpu.tex.pitch, s_gpu.tex.color, s_gpu.tex.valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. */
    s_gpu.attr[0].type = 2; s_gpu.attr[0].size = 4;   /* position float4  */
    s_gpu.attr[0].offset = 0;
    s_gpu.attr[3].type = 0; s_gpu.attr[3].size = 4;   /* diffuse D3DCOLOR */
    s_gpu.attr[3].offset = 16;
    s_gpu.attr[9].type = 2; s_gpu.attr[9].size = 2;   /* texcoord0 float2 */
    s_gpu.attr[9].offset = 20;
    s_gpu.attr[0].stride = s_gpu.attr[3].stride = s_gpu.attr[9].stride =
        IMM_VERTEX_DWORDS * 4;

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append it in the layout draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at],     s_gpu.imm_pos, 4 * sizeof(float));
    memcpy(&s_gpu.inline_buf[at + 4], &s_gpu.imm_diffuse, sizeof(uint32_t));
    memcpy(&s_gpu.inline_buf[at + 5], s_gpu.imm_tex, 2 * sizeof(float));
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    v.u = param;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        uint32_t c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 3)                      /* w completes the vertex */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        uint32_t c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 2) {                    /* z completes it, w is implicitly 1 */
            s_gpu.imm_pos[3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M;
        if (off / 8 == 9)                /* attribute 9 is texture coord 0 */
            s_gpu.imm_tex[(off % 8) / 4] = v.f;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        uint32_t attr = off / 16, c = (off % 16) / 4;
        if (attr == 0) {
            s_gpu.imm_pos[c] = v.f;
            if (c == 3)
                imm_emit_vertex();
        } else if (attr == 9 && c < 2) {
            s_gpu.imm_tex[c] = v.f;
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA4UB) / 4 == 3)   /* diffuse */
            s_gpu.imm_diffuse = param;
        return 1;
    }
    return 0;
}
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    static int s_verbose = -1;
    if (s_verbose < 0) s_verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    if (s_verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_backend && s_backend->method)
        s_backend->method(subch, method, param);

    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (!s_backend)
            clear_surface(param);
#if defined(__ANDROID__)
        if (nv2a_gles_enabled())
            nv2a_gles_clear(s_gpu.clear_color);
#endif
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            fs_draw(s_gpu.imm_count ? s_gpu.imm_count
                    : s_gpu.inline_count ? s_gpu.inline_count : s_gpu.idx_count);
            if (s_backend)
                ;                       /* the backend rendered it */
            else if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;

    case NV097_DRAW_ARRAYS:
        /* Non-indexed sequential draw: no index stream at all, just a start
         * vertex and a count (stored biased by one), and several of these can
         * arrive back-to-back inside one BEGIN_END. This is how a title that
         * uses DRAW_ARRAYS instead of ARRAY_ELEMENT submits geometry -- so
         * without it the executor saw the BEGIN_END pair with nothing attached
         * and reported `draws 0` while every array-drawn batch went past it
         * (the same failure the immediate-vertex methods above once had).
         * Expand each run into the same index list the rasteriser reads; the
         * vertices themselves are then fetched from the bound vertex arrays.
         * idx[] is 16-bit and capped at NV_MAX_INDICES, which bounds a batch to
         * the range the rest of this file already assumes. */
        if (s_gpu.prim) {
            uint32_t start = param & 0x00FFFFFF;
            uint32_t count = ((param >> 24) & 0xFF) + 1;
            uint32_t k;
            for (k = 0; k < count && s_gpu.idx_count < NV_MAX_INDICES; k++)
                s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + k);
        }
        break;

    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        /* MODE 0 = fixed-function T&L (transform by the composite matrix here),
         * MODE 2 = a vertex program runs instead (not executed yet, so those
         * batches stay object-space and are skipped). Blinx sets both across a
         * frame, so this is read per batch to know which path a batch is on. */
        s_gpu.xform_mode = param & 0x3;
        break;

    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        /* The constant register the next SET_TRANSFORM_CONSTANT run starts at. */
        s_gpu.const_load = param;
        break;

    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        /* Instruction slot the next program run writes from; 4 dwords each. */
        s_gpu.vp_prog_cursor = param * 4;
        break;

    case NV097_SET_TRANSFORM_PROGRAM_START:
        s_gpu.vp_prog_start = param;
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        return;

    case NV097_FLIP_STALL:
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        fs_guest_flip();
        /* Frame boundary: publish the object-space batch built this frame to
         * the presenter's GL thread. */
#if defined(__ANDROID__)
        if (nv2a_gles_enabled())
            nv2a_gles_frame();
#endif
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_CONTEXT_DMA_SEMAPHORE:
        /* Which DMA object the semaphore word lives in. The offset below is
         * taken relative to it, and this runtime resolves both through the same
         * window, so the handle needs no separate lookup -- accepting it here
         * just keeps it out of the "unhandled" tally. */
        return;

    case NV097_SET_SEMAPHORE_OFFSET:
        s_gpu.sema_offset = param;
        return;

    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE: {
        /* By the time the executor reaches this method it has already carried
         * out every batch queued before it -- which is exactly what the release
         * value asserts to the CPU. Write it now, so a title spinning on that
         * word for "GPU finished" completes instead of hanging with the GPU
         * idle. Native byte order: the guest reads it back with an ordinary
         * 32-bit load and compares against the value it just supplied.
         *
         * Guarded on a non-zero offset: the semaphore lives at its DMA object's
         * base + this offset, and resolving the DMA object (SET_CONTEXT_DMA_
         * SEMAPHORE, a RAMHT handle) is not done here. With offset 0 the fall-
         * back resolves to the contiguous base and would scribble the release
         * value over the arena, so only honour an explicit offset -- which is
         * also the only case a title that truly polls the semaphore produces.
         * (Blinx submits releases with offset 0 but polls a notifier/fence, not
         * this word, so skipping the write costs it nothing.) */
        s_gpu.sema_releases++;
        if (s_gpu.sema_offset) {
            uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
            uint32_t addr = dma_resolve(s_gpu.sema_offset);
            *(volatile uint32_t *)(mem + addr) = param;
        }
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr,
                        "  [GPU] semaphore release #%u: offset=0x%08X value=0x%08X%s\n",
                        s_gpu.sema_releases, s_gpu.sema_offset, param,
                        s_gpu.sema_offset ? "" : "  (skipped: no offset)");
                fflush(stderr);
            }
        }
        return;
    }

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge; the rest (mirror,
         * border) fall back to clamp, which is wrong at an edge rather than
         * wrong everywhere. */
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        record_tex_reg(method, param);
        break;

    default:
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (method >= NV097_SET_COMPOSITE_MATRIX
                && method < NV097_SET_COMPOSITE_MATRIX + 16 * 4) {
            /* The 4x4 clip-space matrix, one float per auto-incrementing word. */
            s_gpu.composite[(method - NV097_SET_COMPOSITE_MATRIX) / 4] =
                param_as_float(param);
            s_gpu.composite_set = 1;
        } else if (method >= NV097_SET_VIEWPORT_OFFSET
                && method < NV097_SET_VIEWPORT_OFFSET + 4 * 4) {
            s_gpu.vp_offset[(method - NV097_SET_VIEWPORT_OFFSET) / 4] =
                param_as_float(param);
            s_gpu.viewport_set = 1;
        } else if (method >= NV097_SET_VIEWPORT_SCALE
                && method < NV097_SET_VIEWPORT_SCALE + 4 * 4) {
            s_gpu.vp_scale[(method - NV097_SET_VIEWPORT_SCALE) / 4] =
                param_as_float(param);
            s_gpu.viewport_set = 1;
        } else if (method >= NV097_SET_TRANSFORM_CONSTANT
                && method < NV097_SET_TRANSFORM_CONSTANT + 32 * 4) {
            /* One float per auto-incrementing word, into the register the last
             * LOAD selected: float i lands at register (load + i/4), component
             * i%4. Bounded to the register file. */
            uint32_t f = s_gpu.const_load * 4
                       + (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
            if (f < NV2A_VP_CONSTS * 4)
                s_gpu.xform_const[f] = param_as_float(param);
        } else if (method >= NV097_SET_TRANSFORM_PROGRAM
                && method < NV097_SET_TRANSFORM_PROGRAM + 32 * 4) {
            /* Program dwords stream in order from the LOAD cursor; the method's
             * own low address just repeats per 32-dword run, so advance a single
             * cursor rather than trusting it. */
            if (s_gpu.vp_prog_cursor < NV2A_VP_MAX_INSN * 4) {
                s_gpu.vp_prog[s_gpu.vp_prog_cursor++] = param;
                if (s_gpu.vp_prog_cursor > s_gpu.vp_prog_len)
                    s_gpu.vp_prog_len = s_gpu.vp_prog_cursor;
            }
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}
