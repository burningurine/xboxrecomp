/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

static void note(uint32_t subch, uint32_t method)
{
    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].method == method && s_seen[i].subch == subch) {
            s_seen[i].count++;
            return;
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk [start_va, end_va), stopping early at a JUMP; *jump_off receives the
 * jump's target (a pushbuffer offset, i.e. a physical address) or ~0u. */
static void pb_scan_core(uint32_t start_va, uint32_t end_va, uint32_t *jump_off)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va;
    *jump_off = ~0u;
    uint32_t words = 0, jumps = 0, unknown = 0;

    static int s_scan = -1, s_trace = -1;   /* env is fixed at start-up */
    if (s_exec_enabled < 0) {
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
        s_scan  = getenv("RECOMP_PB_SCAN") != NULL;
        s_trace = getenv("RECOMP_PB_METHOD_TRACE") != NULL;
    }
    if (!(s_scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            jumps++;
            *jump_off = ((w & 3u) == 1u) ? (w & 0xFFFFFFFCu)   /* JUMP     */
                                         : (w & 0x1FFFFFFCu);  /* OLD_JUMP */
            break;                            /* a jump ends this segment */
        }
        if ((w & 3u) == 2u || (w & 0xFFFF0003u) == 0x00020000u)
            continue;
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Bounded method+arg trace (RECOMP_PB_METHOD_TRACE) to capture
                 * the exact push-buffer method sequence for GPU bring-up. Off by
                 * default; stops after a cap so it never floods a long run. */
                {
                    static int _mt = -1, _mtn = 0;
                    if (_mt < 0)
                        _mt = getenv("RECOMP_PB_METHOD_TRACE") ? 1 : 0;
                    /* Focus on geometry-range methods (vertex arrays 0x0B00-0x0B7C
                     * and the 0x1700-0x18FC vertex/begin-end/draw/inline block) so
                     * the cap is not spent on the per-frame matrix/state setup.
                     * RECOMP_PB_METHOD_TRACE=all traces everything instead. */
                    if (_mt && _mtn < 520) {
                        static int _all = -1, _vp = -1;
                        int geo;
                        if (_vp < 0)
                            _vp = getenv("RECOMP_PB_VP_DUMP") ? 1 : 0;
                        /* RECOMP_PB_VP_DUMP: capture the VP microcode instead --
                         * SET_TRANSFORM_PROGRAM words (0x0B00-0x0B7C), the program
                         * load index (0x1E9C) and draw markers (0x1810). */
                        if (_vp)
                            geo = (m >= 0x0B00 && m <= 0x0B7C)
                               || (m == 0x1E9C) || (m == 0x1810);
                        else
                        /* Vertex arrays / BEGIN_END / draws (0x1700-0x18FC),
                         * transform-execution-mode (0x1E94), the composite matrix
                         * (0x0680-0x06BC), viewport offset (0x0A20-0x0A2C) and
                         * viewport scale (0x0AF0-0x0AFC), plus the transform
                         * constants (0x0B80-0x0BFC + 0x1EA4 load). NOT the 0x0B00
                         * TRANSFORM_PROGRAM microcode unless RECOMP_PB_VP_DUMP. */
                        geo = (m >= 0x1700 && m <= 0x18FC) || (m == 0x1E94)
                               || (m >= 0x0680 && m <= 0x06BC)
                               || (m >= 0x0A20 && m <= 0x0A2C)
                               || (m >= 0x0AF0 && m <= 0x0AFC)
                               || (m == 0x1EA4)                 /* const load idx */
                               || (m >= 0x0B80 && m <= 0x0BFC); /* transform consts */
                        if (_all < 0) {
                            const char *e = getenv("RECOMP_PB_METHOD_TRACE");
                            _all = (e && !strcmp(e, "all")) ? 1 : 0;
                        }
                        if (_all || geo) {
                            _mtn++;
                            fprintf(stderr,
                                    "  [PBM] subch=%u m=0x%04X p=0x%08X\n",
                                    subch, m, *(const uint32_t *)(mem + va));
                            fflush(stderr);
                        }
                    }
                }
                /* Per-draw context: for the first several DRAW_ARRAYS, log the
                 * active texture offset (0x1B00), transform-execution-mode
                 * (0x1E94) and vertex-array offset (0x1720) + vertex0 position,
                 * so the batch using a given texture (e.g. the 128x256 startup
                 * art) can be found with its exact transform inputs. */
                if (s_trace) {
                    static uint32_t _voff = 0, _tex = 0, _mode = 0, _vpstart = 0;
                    static uint32_t _vsx = 0, _vsy = 0, _vox = 0, _voy = 0;
                    static int _nd = 0;
                    if (m == 0x1720) _voff = *(const uint32_t *)(mem + va);
                    if (m == 0x1B00) _tex  = *(const uint32_t *)(mem + va);
                    if (m == 0x1E94) _mode = *(const uint32_t *)(mem + va);
                    if (m == 0x1EA0) _vpstart = *(const uint32_t *)(mem + va);
                    if (m == 0x0AF0) _vsx = *(const uint32_t *)(mem + va);
                    if (m == 0x0AF4) _vsy = *(const uint32_t *)(mem + va);
                    if (m == 0x0A20) _vox = *(const uint32_t *)(mem + va);
                    if (m == 0x0A24) _voy = *(const uint32_t *)(mem + va);
                    if (m == 0x1810 && _nd < 16) {
                        float sx, sy, ox, oy;
                        memcpy(&sx, &_vsx, 4); memcpy(&sy, &_vsy, 4);
                        memcpy(&ox, &_vox, 4); memcpy(&oy, &_voy, 4);
                        int _nd2 = ++_nd;
                        fprintf(stderr,
                            "  [DRAW#%d] tex=0x%08X mode=0x%X vp_start=%u"
                            " vp_scale=(%.2f,%.2f) vp_off=(%.2f,%.2f)\n", _nd2,
                            _tex, _mode & 3u, _vpstart, sx, sy, ox, oy);
                        fflush(stderr);
                    }
                }
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        unknown++;
    }

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t jump_off;
    pb_scan_core(start_va, end_va, &jump_off);
}

/* Consume everything from from_va up to put_va, following JUMPs. D3D's
 * pushbuffer is a ring: at its end the title writes a JUMP back to the start
 * and carries on, so PUT comes out below the last position. Scanning only
 * [last, put) when put > last dropped every segment that crossed a wrap --
 * the ring's tail and head -- and with them any state or vertex-program upload
 * that happened to span it (xemu then asserts on the half-loaded program).
 * phys_base maps a jump target (physical) back to a guest VA. */
void nv2a_pb_drain(uint32_t from_va, uint32_t put_va, uint32_t phys_base)
{
    uint32_t va = from_va;
    for (int hops = 0; hops < 8 && va != put_va; hops++) {
        uint32_t jump_off;
        uint32_t end = (va < put_va) ? put_va : va + 0x400000u;
        pb_scan_core(va, end, &jump_off);
        if (jump_off == ~0u)
            break;                            /* reached PUT (or the bound) */
        va = phys_base | (jump_off & 0x0FFFFFFFu);
    }
}
