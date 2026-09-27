/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;

/* Texture-staging window.
 *
 * MmAllocateContiguousMemoryEx serves two very different customers on this
 * title: framebuffers / pushbuffers (raw align 16384), which fb_present scans
 * out and a future NV2A would read, and D3DX texture surfaces (raw align 0x80),
 * which nothing on the Android build ever reads -- there is no GL pgraph, only
 * the Windows D3D11 one, so a texture buffer is write-only there. Both used to
 * come out of the 64 MB physical mirror (xbox_ContiguousAlloc). The ~50 MB
 * resident image bump leaves that window with ~14 MB, which the framebuffers
 * plus a handful of textures exhaust; the next 64 KB texture (a 256x256 DXT5 is
 * exactly 0x10000) then returns 0, the caller builds a half-initialised texture
 * resource, and the warp screen hangs on a wild call through it.
 *
 * The fix is to keep only the scan-out buffers in the mirror and hand texture
 * surfaces a separate host-backed window, off the mirror, so neither starves
 * the other. It is mapped exactly like the device apertures (a fixed VA backed
 * by VirtualAlloc at base + g_memory_offset), so a guest VA in it resolves
 * through XBOX_TO_NATIVE with no special case. It sits in the free gap between
 * the mirror top (0x84000000) and the NV2A aperture (0xFD000000). 64 MB matches
 * the console's entire contiguous budget, which is a generous ceiling for a
 * live texture set; with the free-list below, churn is reclaimed. */
#define XBOX_STAGING_BASE 0x98000000u
#define XBOX_STAGING_SIZE (64u * 1024u * 1024u)
/* The GPU sees a staging texture at (VA & 0x0FFFFFFF): the title's D3D masks
 * the VA itself to form SET_TEXTURE_OFFSET (no MmGetPhysicalAddress). At base
 * 0x90000000 that landed on physical 0..64 MB -- the XBE image -- so the
 * renderer sampled code as texels. At 0x98000000 it lands on 0x08000000, past
 * RAM, where a second view of the same pages makes it the texture data. */
#define XBOX_STAGING_PHYS (XBOX_STAGING_BASE & 0x0FFFFFFFu)
static void *g_staging_memory = NULL;
static void *g_staging_phys_view = NULL;   /* at XBOX_STAGING_PHYS */
static void *g_staging_uc_view = NULL;     /* at 0x80000000 | XBOX_STAGING_PHYS */
static void *g_staging_wc_view = NULL;     /* at 0xF0000000 | XBOX_STAGING_PHYS */

/* What the GPU sees at physical address P, as one host range (the xemu
 * renderer's vram): [0, 64 MB) the contiguous window's storage -- the same
 * bytes as 0x80000000+P and the tiled 0xF0000000+P --, [64, 128 MB) the heap's
 * RAM, [128, 192 MB) the staging window. Guest VA P itself cannot serve: low
 * RAM holds the XBE image, which the contiguous window deliberately does not
 * alias (see the tiled aperture below), so a GPU reading VA P read the image
 * or empty RAM where the title had written its buffers at 0x80000000+P. */
#define XBOX_GPU_PHYS_SPAN 0x0C000000u
static uint8_t *g_gpu_phys_view = NULL;
uint8_t *xbox_GpuPhysView(void) { return g_gpu_phys_view; }
static HANDLE g_staging_mapping = NULL;
/* The contiguous window's backing section. It is a file mapping rather than
 * plain committed memory for one reason: the tiled aperture has to be a
 * second view of the very same bytes, and only a mapping can be mapped
 * twice. See the tiled aperture below for why that matters.
 */
static HANDLE g_contig_mapping = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
/* How much guest address space is mapped.
 *
 * For anything that dereferences an address it read out of guest memory --
 * a descriptor pointer a device model follows, say. Those are attacker-ish
 * input in the only sense that matters here: the title can leave one
 * uninitialised, and 0xCCCCCCCC dereferenced is a crash in the runtime
 * rather than a fault the title would have taken. */
size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}
/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Two counters inside the device, one of which the GPU owns.
 *
 * D3D's swap throttle is a pair: the title bumps "frames submitted" itself and
 * waits for "frames completed", which on hardware only the GPU moves. The Xbox
 * dashboard's is exactly that, at guest 0x000AF121 --
 *
 *     eax = [esi+0x2518]        ; completed
 *     ecx = [esi+0x2B60]        ; submitted
 *     ecx = ecx - eax
 *     if (ecx < 2) proceed      ; else spin on a 400-iteration delay loop
 *
 * -- and with nothing moving completed it spins there forever once two frames
 * are outstanding. That delay loop was 99.8 million of the dashboard's calls,
 * against 35 thousand for the next function down.
 *
 * This differs from xbox_Nv2aFrameCounter, which advances a counter on a 60 Hz
 * clock, in the way that matters for this pair: a free-running counter can
 * pass submitted, and then submitted - completed underflows to about four
 * billion, which is >= 2, and the spin never ends again. Mirroring cannot do
 * that, because completed is only ever whatever submitted already is.
 *
 * It is also simply true here. The pushbuffer is executed at submit, so by the
 * time the title asks whether the frame is finished, it is.
 */
#define XBOX_MAX_COUNTER_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t src_off, dst_off;
} g_counter_mirrors[XBOX_MAX_COUNTER_MIRRORS];
static int g_counter_mirror_count = 0;

int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va,
                           uint32_t src_off, uint32_t dst_off)
{
    if (g_counter_mirror_count >= XBOX_MAX_COUNTER_MIRRORS)
        return -1;
    g_counter_mirrors[g_counter_mirror_count].device_ptr_va = device_ptr_va;
    g_counter_mirrors[g_counter_mirror_count].src_off = src_off;
    g_counter_mirrors[g_counter_mirror_count].dst_off = dst_off;
    g_counter_mirror_count++;
    fprintf(stderr, "  NV2A counter mirror: device at 0x%08X,"
            " +0x%X -> +0x%X\n", device_ptr_va, src_off, dst_off);
    return 0;
}

static void counter_mirrors_tick(void)
{
    for (int i = 0; i < g_counter_mirror_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_counter_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_counter_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_counter_mirrors[i].src_off, 4)
                || !fence_readable(dev + g_counter_mirrors[i].dst_off, 4))
            continue;
        {
            volatile uint32_t *dst =
                (volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].dst_off)
                                      + g_memory_offset);
            uint32_t src =
                *(volatile uint32_t *)((uintptr_t)(dev + g_counter_mirrors[i].src_off)
                                       + g_memory_offset);
            if (*dst != src)
                *dst = src;
        }
    }
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

/* Direct frame counters: a fixed guest VA to bump each frame, for a title whose
 * swap counter sits at a known STATIC address rather than behind a runtime
 * device pointer (Blinx's is inside its interrupt context struct at a fixed VA).
 * Registered from RECOMP_FRAME_COUNTER=<hex>[,<hex>...] so the exact counter can
 * be found and tried live without a rebuild. A direct entry is marked by
 * device_ptr_va == 0, with the VA held in counter_off. */
static void frame_counters_init_env(void)
{
    static int done = 0;
    const char *s;

    if (done)
        return;
    done = 1;
    s = getenv("RECOMP_FRAME_COUNTER");
    while (s && *s) {
        uint32_t va = (uint32_t)strtoul(s, NULL, 0);
        if (va && g_frame_counter_count < XBOX_MAX_FRAME_COUNTERS) {
            g_frame_counters[g_frame_counter_count].device_ptr_va = 0;
            g_frame_counters[g_frame_counter_count].counter_off   = va;
            g_frame_counter_count++;
            fprintf(stderr, "  Frame counter (direct): VA 0x%08X @ %d Hz\n",
                    va, 1000 / XBOX_FRAME_PERIOD_MS);
        }
        s = strchr(s, ',');
        if (s) s++;
    }
}

/* Advance one registered counter (direct VA, or through the device pointer). */
static void frame_counter_bump(int i)
{
    uint32_t addr;

    if (g_frame_counters[i].device_ptr_va == 0) {
        addr = g_frame_counters[i].counter_off;            /* a direct VA */
    } else {
        uint32_t dev;
        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            return;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        addr = dev + g_frame_counters[i].counter_off;
    }
    if (!fence_readable(addr, 4))
        return;
    *(volatile uint32_t *)((uintptr_t)addr + g_memory_offset) += 1;
}

/* A real swap happened: advance every registered counter, and remember when.
 *
 * The timer below exists for a title nothing presents for. Once the
 * pushbuffer executor is actually running flips, the timer is the wrong
 * clock and an actively harmful one: Half-Life 2's loader paces its intro on
 * this count, so a 62 Hz timer against an executor managing a fraction of a
 * frame per second ran the video forward in virtual time far faster than it
 * could be drawn. Only every few hundredth frame was ever presented, each one
 * sampled part way through its own decode -- which looks exactly like a
 * stalling, blocky video rather than a clock running away.
 */
static DWORD g_frame_counter_flip_ms;

void xbox_Nv2aFrameCounterFlip(void)
{
    int i;

    g_frame_counter_flip_ms = GetTickCount();
    if (!g_frame_counter_flip_ms)
        g_frame_counter_flip_ms = 1;          /* 0 means "never" */
    frame_counters_init_env();
    for (i = 0; i < g_frame_counter_count; i++)
        frame_counter_bump(i);
}

/* RECOMP_POKE=<hex VA>:<hex val>[,<va>:<val>...] writes val to a guest global
 * every tick -- a general "force a global to a value" for testing (or applying)
 * a fix. Once the gate's polled VA and its unblocking value are known, poke it
 * and watch the guest advance; if it does, that confirms the gate and the poke
 * itself is a usable stopgap. Distinct from a frame counter, which increments. */
#define XBOX_MAX_POKES 8
static struct { uint32_t va, val; } g_pokes[XBOX_MAX_POKES];
static int g_poke_count = 0;
/* Indirect pokes: [MEM32(base_va) + off] = val -- for a gate cell behind a
 * runtime table pointer (the recurring double-indirect descriptor pattern:
 * scene 0x14 polled [MEM32(0x3F09C0)+0x4C]). RECOMP_POKE_IND=base:off:val[,...] */
static struct { uint32_t base_va, off, val; } g_ipokes[XBOX_MAX_POKES];
static int g_ipoke_count = 0;

static void pokes_init_env(void)
{
    static int done = 0;
    const char *s;

    if (done)
        return;
    done = 1;
    s = getenv("RECOMP_POKE");
    while (s && *s) {
        uint32_t va = (uint32_t)strtoul(s, NULL, 0);
        const char *colon = strchr(s, ':');
        uint32_t val = colon ? (uint32_t)strtoul(colon + 1, NULL, 0) : 0;
        if (va && g_poke_count < XBOX_MAX_POKES) {
            g_pokes[g_poke_count].va  = va;
            g_pokes[g_poke_count].val = val;
            g_poke_count++;
            fprintf(stderr, "  Poke: [0x%08X] = 0x%08X every tick\n", va, val);
        }
        s = strchr(s, ',');
        if (s) s++;
    }
    s = getenv("RECOMP_POKE_IND");
    while (s && *s) {
        uint32_t base = (uint32_t)strtoul(s, NULL, 0);
        const char *c1 = strchr(s, ':');
        uint32_t off = c1 ? (uint32_t)strtoul(c1 + 1, NULL, 0) : 0;
        const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        uint32_t val = c2 ? (uint32_t)strtoul(c2 + 1, NULL, 0) : 0;
        if (base && g_ipoke_count < XBOX_MAX_POKES) {
            g_ipokes[g_ipoke_count].base_va = base;
            g_ipokes[g_ipoke_count].off     = off;
            g_ipokes[g_ipoke_count].val     = val;
            g_ipoke_count++;
            fprintf(stderr, "  Poke(ind): [[0x%08X]+0x%X] = 0x%08X every tick\n",
                    base, off, val);
        }
        s = strchr(s, ',');
        if (s) s++;
    }
}

static void pokes_tick(void)
{
    int i;

    pokes_init_env();
    for (i = 0; i < g_poke_count; i++) {
        if (!fence_readable(g_pokes[i].va, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)g_pokes[i].va + g_memory_offset)
            = g_pokes[i].val;
    }
    for (i = 0; i < g_ipoke_count; i++) {
        uint32_t base, addr;
        if (!fence_readable(g_ipokes[i].base_va, 4))
            continue;
        base = *(volatile uint32_t *)((uintptr_t)g_ipokes[i].base_va
                                      + g_memory_offset);
        if (!base) continue;
        addr = base + g_ipokes[i].off;
        if (!fence_readable(addr, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)addr + g_memory_offset)
            = g_ipokes[i].val;
    }
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    frame_counters_init_env();
    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    /* Something is presenting: let it drive the count instead. Two seconds,
     * because the executor's flips are not evenly spaced and a title that
     * genuinely stops presenting still has to be got moving again. */
    if (g_frame_counter_flip_ms && (now - g_frame_counter_flip_ms) < 2000) {
        g_frame_counter_last_ms = now;
        return;
    }
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++)
        frame_counter_bump(i);
}

/* Register a fence mirror from the environment so the exact device pointer and
 * offsets can be found and tried live without a rebuild, and then baked into a
 * launcher default. RECOMP_FENCE_MIRROR=<devPtrVA>:<putOff>:<getPtrOff>[,...].
 * Blinx's pushbuffer space manager reads its GET from a shadow the GPU ISR would
 * refresh; 0x143B58:0x2C:0x30 mirrors the submit count into it every tick, which
 * is what lets the guest keep draining and advance past the boot screen. */
static void fence_mirrors_init_env(void)
{
    static int done = 0;
    const char *s;

    if (done)
        return;
    done = 1;
    s = getenv("RECOMP_FENCE_MIRROR");
    while (s && *s) {
        uint32_t dev = (uint32_t)strtoul(s, NULL, 0);
        const char *c1 = strchr(s, ':');
        uint32_t put_off = c1 ? (uint32_t)strtoul(c1 + 1, NULL, 0) : 0;
        const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        uint32_t get_off = c2 ? (uint32_t)strtoul(c2 + 1, NULL, 0) : 0;
        if (dev)
            xbox_Nv2aMirrorFence(dev, put_off, get_off);
        s = strchr(s, ',');
        if (s) s++;
    }
}

static void fence_mirrors_tick(void)
{
    fence_mirrors_init_env();
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

/* Where the title last said its framebuffer is, for anything that wants to read
 * guest pixels directly. There was only a setter, so every such reader carried
 * its own hardcoded address instead -- and a title that moves its framebuffer
 * (which is most of them, once it owns one) left that constant pointing at
 * uninitialised memory. A dump taken there is not empty, it is noise, which
 * reads as "the title drew garbage" rather than "you read the wrong page".
 *
 * This is the resolved address, not the physical one AvSetDisplayMode states:
 * the caller resolves before storing, because a physical framebuffer address
 * read directly lands in the loaded image. Getting that wrong is the same bug
 * twice over -- once in the probe below, once in a caller of this. */
uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch)
{
    if (pitch)
        *pitch = s_fb_pitch;
    return s_fb_va;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

/* Register-level flush acknowledgement. Some titles kick a GPU flush by setting
 * a bit in an NV2A register and spin until the GPU clears it. Blinx's
 * sub_00139240 sets bit 0x10000 at [MEM32(0x145778)+0x100410] (a PFIFO cache-
 * pull) and waits for it to clear -- an ack a synchronous executor owes at once,
 * since the work behind the flush is already carried out. Clear the named bit(s)
 * every tick. The register lives behind a device pointer and may sit in the NV2A
 * aperture (reached through regs) or in ordinary guest RAM; both are handled.
 * RECOMP_REG_ACK=<devPtrVA>:<off>:<clearBits>[,...] (hex). */
#define XBOX_MAX_REG_ACKS 4
static struct { uint32_t ptr_va, off, bits; } g_reg_acks[XBOX_MAX_REG_ACKS];
static int g_reg_ack_count = -1;

static void reg_acks_tick(volatile uint32_t *regs)
{
    int i;
    if (g_reg_ack_count < 0) {
        const char *s = getenv("RECOMP_REG_ACK");
        g_reg_ack_count = 0;
        while (s && *s && g_reg_ack_count < XBOX_MAX_REG_ACKS) {
            uint32_t ptr = (uint32_t)strtoul(s, NULL, 0);
            const char *c1 = strchr(s, ':');
            uint32_t off = c1 ? (uint32_t)strtoul(c1 + 1, NULL, 0) : 0;
            const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            uint32_t bits = c2 ? (uint32_t)strtoul(c2 + 1, NULL, 0) : 0;
            if (ptr && bits) {
                g_reg_acks[g_reg_ack_count].ptr_va = ptr;
                g_reg_acks[g_reg_ack_count].off    = off;
                g_reg_acks[g_reg_ack_count].bits   = bits;
                g_reg_ack_count++;
                fprintf(stderr, "  NV2A reg ack: clear 0x%08X at [*(0x%08X)+0x%X]"
                        " every tick\n", bits, ptr, off);
            }
            s = strchr(s, ',');
            if (s) s++;
        }
    }
    for (i = 0; i < g_reg_ack_count; i++) {
        uint32_t base, addr;
        if (!fence_readable(g_reg_acks[i].ptr_va, 4))
            continue;
        base = *(volatile uint32_t *)((uintptr_t)g_reg_acks[i].ptr_va
                                      + g_memory_offset);
        if (!base)
            continue;
        addr = base + g_reg_acks[i].off;
        if (addr >= XBOX_NV2A_BASE && addr < XBOX_NV2A_BASE + 0x01000000u) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + (addr - XBOX_NV2A_BASE));
            *r &= ~g_reg_acks[i].bits;
        } else if (fence_readable(addr, 4)) {
            *(volatile uint32_t *)((uintptr_t)addr + g_memory_offset)
                &= ~g_reg_acks[i].bits;
        }
    }
}

/* Reflect the GPU-idle indicators a title POLLS, through the SAME backing the
 * guest reads: mem+addr (g_memory_offset), not the `regs` alias the NV2A_ACK
 * table writes. On build-win the NV2A aperture the game reads is at mem+0xFDxxxxxx
 * (the path BRIDGE_MEM32 and kernel_vblank_tick use); the `regs` pointer is a
 * different buffer the game never sees, so register acks written there are lost.
 * Every boot gate so far is a spin on such an indicator the executor drains but
 * never reports back. Env-configurable so a newly found poll is a config change,
 * not a rebuild:
 *   RECOMP_GPU_IDLE_MIRROR=<dst>:<src>[,...]   *(dst) = *(src)   (GET=PUT, pos=count)
 *   RECOMP_GPU_IDLE_SET=<addr>:<val>[,...]     *(addr) = val     (STATUS=idle)
 *   RECOMP_GPU_IDLE_CLR=<addr>:<bits>[,...]    *(addr) &= ~bits  (flush ack)
 * Blinx set: MIRROR=0xFD003244:0xFD003240,0xFD400B10:0x143B8C SET=0xFD400700:0
 * CLR=0xFD100410:0x10000  (PFIFO CACHE1 GET=PUT, PGRAPH pos=drained count,
 * PGRAPH_STATUS idle, PFB flush). */
#define XBOX_MAX_IDLE 12
typedef struct { uint32_t a, b; } IdlePair;
static IdlePair g_idle_mirror[XBOX_MAX_IDLE];
static IdlePair g_idle_set[XBOX_MAX_IDLE];
static IdlePair g_idle_clr[XBOX_MAX_IDLE];
static int g_idle_mirror_n = -1, g_idle_set_n = 0, g_idle_clr_n = 0;

/* Host pointer for a guest address in the main/contiguous range (fence_readable)
 * OR the NV2A register aperture (mapped at mem+addr but above g_memory_size, so
 * fence_readable alone rejects it). NULL if not yet mapped. */
static volatile uint32_t *idle_ptr(uint32_t addr)
{
    if (g_memory_base == NULL)
        return NULL;
    if (fence_readable(addr, 4)
            || (addr >= XBOX_NV2A_BASE && addr < XBOX_NV2A_BASE + 0x01000000u))
        return (volatile uint32_t *)((uintptr_t)addr + g_memory_offset);
    return NULL;
}

static int idle_parse_pairs(const char *s, IdlePair *out, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        uint32_t a = (uint32_t)strtoul(s, NULL, 0);
        const char *c = strchr(s, ':');
        uint32_t b = c ? (uint32_t)strtoul(c + 1, NULL, 0) : 0;
        if (a) { out[n].a = a; out[n].b = b; n++; }
        s = strchr(s, ',');
        if (s) s++;
    }
    return n;
}

static void gpu_idle_reflect_tick(void)
{
    int i;
    if (g_idle_mirror_n < 0) {
        g_idle_mirror_n = idle_parse_pairs(getenv("RECOMP_GPU_IDLE_MIRROR"),
                                           g_idle_mirror, XBOX_MAX_IDLE);
        g_idle_set_n = idle_parse_pairs(getenv("RECOMP_GPU_IDLE_SET"),
                                        g_idle_set, XBOX_MAX_IDLE);
        g_idle_clr_n = idle_parse_pairs(getenv("RECOMP_GPU_IDLE_CLR"),
                                        g_idle_clr, XBOX_MAX_IDLE);
        if (g_idle_mirror_n || g_idle_set_n || g_idle_clr_n)
            fprintf(stderr, "  GPU idle reflect (mem+addr): %d mirror, %d set,"
                    " %d clr\n", g_idle_mirror_n, g_idle_set_n, g_idle_clr_n);
    }
    for (i = 0; i < g_idle_mirror_n; i++) {
        volatile uint32_t *d = idle_ptr(g_idle_mirror[i].a);
        volatile uint32_t *s = idle_ptr(g_idle_mirror[i].b);
        if (d && s) *d = *s;
    }
    for (i = 0; i < g_idle_set_n; i++) {
        volatile uint32_t *p = idle_ptr(g_idle_set[i].a);
        if (p) *p = g_idle_set[i].b;
    }
    for (i = 0; i < g_idle_clr_n; i++) {
        volatile uint32_t *p = idle_ptr(g_idle_clr[i].a);
        if (p) *p &= ~g_idle_clr[i].b;
    }
#if defined(_WIN32)
    /* Baked GPU-idle reflect, through mem+addr -- the aperture the guest reads on
     * this build (the `regs` alias the NV2A_ACK/USER-mirror paths write is a
     * separate buffer it never sees). Called both once per ack iteration AND, for
     * low latency, right after the executor drains a submitted segment, so the
     * guest's per-frame GPU-idle sync-spins (Blinx does ~111/frame) clear the
     * instant the work is consumed rather than a scheduler quantum later.
     * Standard indicators first; the last is Blinx's D3D reading a PGRAPH position
     * register against its own submit count, guarded to a small count. */
    {
        volatile uint32_t *d, *s;
        if ((d = idle_ptr(0xFD003244)) && (s = idle_ptr(0xFD003240)))
            *d = *s;                                    /* CACHE1 GET = PUT   */
        if ((d = idle_ptr(0xFD400700))) *d = 0;         /* PGRAPH_STATUS idle */
        if ((d = idle_ptr(0xFD100410))) *d &= ~0x00010000u; /* PFB flush ack  */
        if ((d = idle_ptr(0xFD400B10)) && (s = idle_ptr(0x00143B8C))
                && *s && *s < 0x00010000u)
            *d = *s;                                    /* PGRAPH pos = count */
    }
#endif
    /* Authoritative fence reflect (all platforms), runs last so it wins over the
     * env mirror and the WIN32 block above. Blinx's D3D sync-spin sub_00139120
     * loops until (*(0xFD400B10) ^ *(*(dev+0x30))) & 0x1F == 0 -- the PGRAPH
     * position register's low 5 bits must reach the guest's SUBMITTED fence count
     * at *(*(dev+0x30)), dev = *(0x143B58). On HW the GPU writes 0x400B10 as it
     * retires work; we drain the pushbuffer synchronously (GPU is always idle from
     * the guest's view), so completed == submitted at all times. The legacy source
     * 0x143B8C is a readback slot that no literal writer updates -- it only moves
     * when the guest's computed-pointer fence path happens to write it, so it goes
     * stale and freezes (seen frozen at 7 during the file_select pre-flip wait,
     * hanging the boot). Reflecting the submitted count directly makes the spin
     * clear for any target, at any scene. idle_ptr() returns NULL for any unmapped
     * link (dev==0 pre-device, bad chain), so a broken chain simply leaves
     * 0xFD400B10 as the mirror/WIN32 block set it -- no regression, no fault. */
    {
        volatile uint32_t *pdev = idle_ptr(0x00143B58u);
        if (pdev && *pdev) {
            volatile uint32_t *pptr = idle_ptr(*pdev + 0x30u);
            if (pptr && *pptr) {
                volatile uint32_t *pcount = idle_ptr(*pptr);
                volatile uint32_t *pfence = idle_ptr(0xFD400B10u);
                if (pcount && pfence)
                    *pfence = *pcount;              /* completed = submitted */
            }
        }
    }
}

/* Pushbuffer drain, shared by the ack thread and guest threads that wait for
 * GPU idle (xbox_Nv2aDrainNow). The lock keeps segments in order and the
 * executor single-threaded; a spin lock because each hold is short and the
 * waiter is a busy guest thread anyway. */
static volatile LONG s_drain_lock;
static uint32_t s_drain_last_put;
static DWORD    s_drain_last_ms;

static void drain_lock(void)
{
    /* test-and-test-and-set: spin on a plain read, CAS only when it looks
     * free, so a waiter doesn't bounce the line off the holder */
    for (;;) {
        while (s_drain_lock)
            YieldProcessor();
        if (InterlockedCompareExchange(&s_drain_lock, 1, 0) == 0)
            return;
    }
}
static int drain_trylock(void)
{
    return !s_drain_lock
        && InterlockedCompareExchange(&s_drain_lock, 1, 0) == 0;
}
static void drain_unlock(void) { InterlockedExchange(&s_drain_lock, 0); }

/* Walk whatever the title submitted since the last drain, then reflect GPU
 * idle. Returns 1 if a segment was drained. */
static int nv2a_drain_locked(volatile uint32_t *regs, DWORD now_ms)
{
    extern void nv2a_pb_drain(uint32_t, uint32_t, uint32_t);
    uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
    int drained = 0;
    if (put != s_drain_last_put || (now_ms - s_drain_last_ms) > 2000) {
        /* DMA_PUT holds a PHYSICAL address (Xbox D3D writes VA & 0x0FFFFFFF);
         * the contiguous window is the physical view, so OR its base back.
         * put < last is a ring wrap; the drain follows the JUMP. */
        if (s_drain_last_put && put != s_drain_last_put) {
            nv2a_pb_drain(XBOX_CONTIG_BASE | (s_drain_last_put & 0x0FFFFFFFu),
                          XBOX_CONTIG_BASE | (put & 0x0FFFFFFFu),
                          XBOX_CONTIG_BASE);
            gpu_idle_reflect_tick();
            drained = 1;
        }
        s_drain_last_put = put;
        s_drain_last_ms = now_ms;
    }
    /* GET reports what has been CONSUMED. D3D reuses pushbuffer space behind
     * GET, so moving it before the drain let the title overwrite commands the
     * executor had not read yet; the executor then resumed mid-command in the
     * new stream and fed the renderer garbage (xemu asserted on an unknown
     * fog mode / VS input). */
    *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET) = s_drain_last_put;
    return drained;
}

/* Called by guest threads before they spin on GPU-idle registers. */
void xbox_Nv2aDrainNow(void)
{
    if (!g_nv2a_memory)
        return;
    volatile uint32_t *regs = (volatile uint32_t *)g_nv2a_memory;
    drain_lock();
    nv2a_drain_locked(regs, GetTickCount());
    gpu_idle_reflect_tick();
    drain_unlock();
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    /* Stay hot (no yield) until this tick, refreshed on every drain. A frame is
     * a burst of ~150 draws with a sync + setup gap between each; yielding in
     * those gaps cost a scheduler quantum (~15 ms) PER DRAW. Holding hot across
     * the gaps clears each sync at memory speed; the window lapses only when the
     * guest stops submitting, so an idle guest still costs no core. */
    DWORD hot_until = 0;
    int irq_model = xbox_Nv2aIrqModelOn();
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        /* The interrupt registers first, and outside the drain lock: the
         * title's vblank handler acknowledges PCRTC_INTR_0 and then spins until
         * PMC_INTR_0 follows, and this is where plain RAM follows. */
        xbox_Nv2aIrqReflect();
        /* Drain before reflecting anything: every idle/fence/GET value below
         * claims work is done, so it must describe work that IS done. A guest
         * thread holding the lock is draining right now (possibly waiting on
         * the host GPU); don't spin on it, and don't reflect half-done work --
         * come back shortly. */
        if (!drain_trylock()) {
            if (!xbox_Nv2aIrqInService())
                Sleep(1); /* the holder drains; yielding in a loop cost a core */
            continue;
        }
        if (nv2a_drain_locked(regs, GetTickCount()))
            hot_until = GetTickCount() + 50;   /* ~50 ms hot window */
        drain_unlock();
        {
            extern int xbox_VblankBitsHeld(void);
            int vbl_held = xbox_VblankBitsHeld();
            for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
                volatile uint32_t *r =
                    (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
                uint32_t mask = NV2A_ACK[i].busy_mask;
                /* The interrupt model owns these two (kernel_bridge.c). */
                if (irq_model && (NV2A_ACK[i].offset == 0x000100 ||
                                  NV2A_ACK[i].offset == 0x600100))
                    continue;
                if (vbl_held) {    /* a vblank the title's DPC has yet to see */
                    if (NV2A_ACK[i].offset == 0x000100) mask &= ~(1u << 24);
                    if (NV2A_ACK[i].offset == 0x600100) mask &= ~1u;
                }
                if (*r & mask)
                    *r &= ~mask;
            }
        }
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
            }
        }
        reg_acks_tick(regs);   /* clear any GPU-flush bits a title spins on */
        fence_mirrors_tick();
        gpu_idle_reflect_tick();  /* GPU-idle regs (env + baked), per iteration */
        counter_mirrors_tick();
        frame_counters_tick();
        pokes_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        /* Not gated on the trace flag: nv2a_pb_scan is what drives the
         * executor, and it already returns unless RECOMP_PB_SCAN or
         * RECOMP_PB_EXEC asked for it. Gating the call as well meant
         * RECOMP_PB_EXEC on its own did nothing at all, and the executor
         * only ran when someone happened to also be tracing. */
        {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static uint32_t last_put;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            (void)now_ms;                      /* drained at the loop top */
            if (put != last_put) {
                last_put = put;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                if (s_nv2a_trace) {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                }
                fflush(stderr);
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();

        /* Don't yield while a frame is still streaming in: the guest busy-waits
         * on the GPU-idle regs many times per frame, and a yield here hands the
         * quantum to that spinning thread, so each sync cleared only ~once per
         * scheduler tick (~1 frame every few seconds). Staying hot while a frame
         * is active clears every sync the instant its segment drains; yield only
         * once the title stops submitting, so an idle guest costs no core.
         *
         * The cold-path yield is Sleep(1), not Sleep(0). Sleep(0) only yields to
         * an already-ready thread and returns to us at the scheduler's
         * discretion; when the guest spins on a GPU-idle reg during a DRAINED
         * present (PUT no longer advancing, so the 50 ms hot window has lapsed),
         * that discretion stretched to seconds on device -- the reflect ran once
         * per quantum, so Blinx's warp intro stalled ~8 s per frame and cascaded
         * into a hang. Sleep(1) takes us off the run queue for a bare tick
         * regardless of the spinner, so a drained-but-waiting present's idle/flush
         * condition clears within ~1 ms instead of once per quantum, while an idle
         * guest still costs no core. Active frames are unaffected: they stay hot
         * and never reach this yield. Nor does a vblank in service: its handler
         * is spinning on PMC_INTR_0 until the reflect at the top runs. */
        if (GetTickCount() >= hot_until && !xbox_Nv2aIrqInService())
            Sleep(1);
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_stack_base = XBOX_STACK_BASE_DEFAULT;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* Each guest thread's TIB. The first thread uses the one the loader built;
 * a spawned thread gets its own from xbox_AllocThreadTib(). */
RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

/* The shape of the TLS block the loader built, so a new thread can be
 * given one just like it: where the initialised image data starts, how
 * big the block is, and how big the per-thread structure slot 0 points
 * at is. Zero total means the image had no TLS directory. */
static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0x4000;

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;      /* the same buffer, re-armed */
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    {
        const char *spec = getenv("RECOMP_PEEK");
        char buf[256], *q, *end;
        if (spec && *spec) {
            strncpy(buf, spec, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            fprintf(stderr, "  peek:");
            for (q = buf; *q; ) {
                unsigned long va = strtoul(q, &end, 0);
                if (end == q)
                    break;
                if (va >= XBOX_BASE_ADDRESS && va < XBOX_TOTAL_RAM)
                    fprintf(stderr, " [%08lX]=%08X", va,
                            *(const uint32_t *)(mem + va));
                q = (*end == ',') ? end + 1 : end;
            }
            fprintf(stderr, "\n");
        }
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    /* Large-image auto-size: the guest bump heap (base XBOX_HEAP_BASE=0x00F80000)
     * grows UP and is relocated ABOVE the loaded image (see xbox_HeapAlloc). A
     * title whose (non-PRELOAD) sections push the image near/past RAM — Blinx's
     * MAP/MDL sections reach ~0x031DC780 (~50 MB) — leaves too little room below
     * 64 MB for the heap, so the heap would either overlap the image or OOM.
     * Pre-scan the section headers for the image extent; if image+headroom won't
     * fit under RAM, grow the MAPPED address space (not reported RAM) to the
     * devkit 128 MB so the heap lives entirely above the image. */
    if (g_xbox_map_size == 0 && xbe_size >= 0x0124) {
        uint32_t ib   = *(const uint32_t *)(xbe + XBE_BASE_ADDR_OFFSET);
        uint32_t nsec = *(const uint32_t *)(xbe + XBE_SECTION_COUNT_OFFSET);
        uint32_t shva = *(const uint32_t *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        uint32_t shof = shva - ib;
        uint32_t img_hi = 0;
        if (nsec > 64) nsec = 64;
        for (uint32_t i = 0; i < nsec; i++) {
            if ((uint64_t)shof + (uint64_t)(i + 1) * SECTHDR_SIZE > xbe_size) break;
            const uint8_t *sh = xbe + shof + i * SECTHDR_SIZE;
            uint32_t va = *(const uint32_t *)(sh + SECTHDR_VA);
            uint32_t vs = *(const uint32_t *)(sh + SECTHDR_VSIZE);
            if (va + vs > img_hi) img_hi = va + vs;
        }
        if ((uint64_t)img_hi + (48ull << 20) > (uint64_t)g_xbox_total_ram) {
            g_xbox_map_size = 128u * 1024u * 1024u;   /* devkit-size mapped space */
            fprintf(stderr, "  [MEMLAYOUT] image_hi=0x%08X near/over RAM -> map_size=128MB "
                            "(guest heap relocates above image)\n", img_hi);
        }
    }
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        static const uintptr_t try_bases[] = {
#if defined(__ANDROID__)
            /* Android/arm64 first: the whole Xbox space (base + mirrors at
             * +64 MB*k, the contiguous window at +0x80000000 and the MMIO/flash
             * apertures up to +0xFF000000) must land in ONE free ~4 GB native
             * span. On Android the low 4 GB is full of the process's own
             * mappings (ART heap, dex, libs), which is why the mirrors and the
             * contiguous window failed at 0x10010000..0x80010000. arm64 has a
             * huge VA (libraries sit near ~0x7F_xxxx_xxxx), so a high base like
             * 16/32/64 GB has 4 GB of free room above it. Windows keeps the
             * original low bases below (this whole block is Android-only). */
            0x0000000400000000ULL,  /* 16 GB */
            0x0000000800000000ULL,  /* 32 GB */
            0x0000001000000000ULL,  /* 64 GB */
#endif
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address (Windows) */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        for (int i = 0; try_bases[i] != 0 || i == 0; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                            " (null dereferences fault)\n");
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);

        /* Post-load verify (temp diag): MAP11's manifest descriptor + first record
         * bytes RIGHT AFTER section load, before any runtime activity. If this shows
         * {base=0x01C681C0, count=35} + a "h_sora_r01"-style name, MAP11 loaded
         * correctly from the XBE and something CLOBBERS it later (runtime overwrite);
         * if it's {0,0}/garbage, the section copy itself failed. Distinguishes
         * load-bug vs clobber for the scene-9 hang. */
        {
            uint32_t d = *(const volatile uint32_t *)XBOX_VA(0x01C68620u);
            uint32_t c = *(const volatile uint32_t *)XBOX_VA(0x01C68624u);
            char nm[20];
            for (int k = 0; k < 19; k++) nm[k] = *(const volatile char *)XBOX_VA(0x01C681C0u + (uint32_t)k);
            nm[19] = 0;
            for (int k = 0; k < 19; k++) if ((unsigned char)nm[k] < 32 || (unsigned char)nm[k] > 126) { nm[k] = (nm[k]==0)?0:'.'; }
            fprintf(stderr, "  [MAPCHK] post-load 0x1C68620={base=0x%08X count=%u} 0x1C681C0='%s'\n",
                    d, c, nm);
            fflush(stderr);
        }
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Place the stack area. The default, 0x00780000, sits above the last
     * section of most XBEs; a large image reaches past it and then every
     * guest stack frame lands on the title's own data. Blinx's .data runs to
     * 0x00ABFABC: the main stack's top (0x00F7FFF0) was inside MDLB10, and the
     * kernel timer thread's worker slice (0x00780000-0x007C0000, where every
     * vblank ISR and DPC runs) was inside a .bss pool the title publishes at
     * [0x008EC3EC]. Physical [image_hi, 64 MB) is the contiguous arena, so the
     * area goes at the bottom of the heap region instead, and the heap starts
     * above it (xbox_HeapAlloc). Needs the heap region to exist: the 128 MB map
     * the large-image auto-size picks. Otherwise it stays, with a warning.
     */
    if (g_xbox_image_hi > XBOX_STACK_BASE_DEFAULT) {
        if ((uint64_t)g_memory_size >= (uint64_t)XBOX_CONTIG_SIZE + XBOX_STACK_SIZE + (16u << 20)) {
            g_xbox_stack_base = XBOX_CONTIG_SIZE;
            fprintf(stderr, "  [STACK] image_hi=0x%08X covers the default stack area;"
                            " stack area moved to 0x%08X\n",
                    g_xbox_image_hi, g_xbox_stack_base);
        } else {
            fprintf(stderr, "  [STACK] WARNING: image_hi=0x%08X covers the stack area"
                            " at 0x%08X and there is no room above the image\n",
                    g_xbox_image_hi, g_xbox_stack_base);
        }
    }

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA 0x00761000  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA     0x00760000  /* Fake TLS structure (in BSS) */
        #define FAKE_RWDATA_VA  0x00700000  /* RW engine data area (in BSS) */

        MEM32_INIT(XBOX_FS_BASE + 0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  0x00770000  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA 0x00770200  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_CONTIG_SIZE, NULL);
        g_contig_memory = g_contig_mapping
            ? MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS,
                              0, 0, XBOX_CONTIG_SIZE, (LPVOID)contig_native)
            : NULL;
        if (!g_contig_memory)
            g_contig_memory = VirtualAlloc(
                (LPVOID)contig_native,
                XBOX_CONTIG_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (getenv("RECOMP_AC97_READY")) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. Only the APU's own 512K is unmapped: AC'97 above it
                 * stays plain memory, which is what the codec-ready bit needs.
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                DWORD old_protect;
                if (VirtualProtect((char *)g_mcpx_memory, 0x00080000u,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + 0x00080000u);
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    /* Texture-staging window -- see XBOX_STAGING_BASE. Backed like an aperture:
     * a fixed VA reserved+committed at base + g_memory_offset, so guest writes
     * to a staging surface resolve through XBOX_TO_NATIVE with no special case.
     * If this fails, xbox_StagingAlloc returns 0 and the texture path falls back
     * to the mirror (the pre-fix behaviour), so a failure here is not fatal. */
    {
        uintptr_t staging_native = XBOX_STAGING_BASE + g_memory_offset;
        uintptr_t phys_native    = XBOX_STAGING_PHYS + g_memory_offset;

        g_staging_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            0, (DWORD)XBOX_STAGING_SIZE, NULL);
        if (g_staging_mapping) {
            g_staging_memory = MapViewOfFileEx(g_staging_mapping, FILE_MAP_ALL_ACCESS,
                                               0, 0, XBOX_STAGING_SIZE,
                                               (LPVOID)staging_native);
            if (g_staging_memory)
                g_staging_phys_view = MapViewOfFileEx(
                    g_staging_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    XBOX_STAGING_SIZE, (LPVOID)phys_native);
            /* ...and a third at 0x80000000 | phys: D3D turns a resource's
             * physical address back into a pointer that way (Lock and
             * friends). Without it that pointer hit the RAM mirror there and
             * the title wrote its resources over its own image. Mapped before
             * the RAM mirrors, which then leave these slots alone. */
            if (g_staging_phys_view)
                g_staging_uc_view = MapViewOfFileEx(
                    g_staging_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    XBOX_STAGING_SIZE,
                    (LPVOID)((uintptr_t)(XBOX_STAGING_PHYS | 0x80000000u) + g_memory_offset));
            /* ...and at 0xF0000000 | phys, the tiled/write-combined view the
             * title fills textures through. Without it those writes landed on
             * demand-zero pages past the tiled aperture and every staging
             * texture stayed black. */
            if (g_staging_uc_view)
                g_staging_wc_view = MapViewOfFileEx(
                    g_staging_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    XBOX_STAGING_SIZE,
                    (LPVOID)((uintptr_t)(XBOX_STAGING_PHYS | 0xF0000000u) + g_memory_offset));
        }
        if (!g_staging_memory)
            g_staging_memory = VirtualAlloc(
                (LPVOID)staging_native,
                XBOX_STAGING_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_READWRITE
            );
        if (g_staging_memory && g_staging_phys_view && g_staging_uc_view) {
            /* The views must share pages, not merely exist. */
            volatile uint32_t *a = (volatile uint32_t *)g_staging_memory;
            volatile uint32_t *b = (volatile uint32_t *)g_staging_phys_view;
            volatile uint32_t *c = (volatile uint32_t *)g_staging_uc_view;
            a[1] = 0x5A7A61A5u;
            if (b[1] != 0x5A7A61A5u || c[1] != 0x5A7A61A5u)
                fprintf(stderr, "  WARNING: staging views do NOT alias (GPU view"
                        " %08X, 0x8 view %08X) -- the GPU will sample zeros\n",
                        b[1], c[1]);
            a[1] = 0;
        }
        if (g_staging_memory) {
            fprintf(stderr, "  Texture-staging window: %u MB at Xbox VA "
                    "0x%08X, GPU view at physical 0x%08X%s\n",
                    XBOX_STAGING_SIZE / (1024 * 1024), XBOX_STAGING_BASE,
                    XBOX_STAGING_PHYS,
                    g_staging_phys_view && g_staging_uc_view && g_staging_wc_view ? ""
                        : " (alias FAILED: GPU/D3D views of staging resources are wrong)");
        } else {
            fprintf(stderr, "  WARNING: texture-staging window at 0x%08X failed "
                    "(error %lu); textures fall back to the mirror\n",
                    XBOX_STAGING_BASE, GetLastError());
        }
    }

    /* The GPU-physical view (see XBOX_GPU_PHYS_SPAN). Reserve a free range,
     * release it and map the three views into it; any failure leaves the
     * renderer on the old identity view. */
    if (g_contig_mapping && g_staging_mapping && g_mapping_handle
            && g_memory_size >= 0x08000000u && XBOX_CONTIG_SIZE == 0x04000000u
            && XBOX_STAGING_PHYS == 0x08000000u) {
        uint8_t *b = (uint8_t *)VirtualAlloc(NULL, XBOX_GPU_PHYS_SPAN,
                                             MEM_RESERVE, PAGE_NOACCESS);
        if (b) {
            void *v0, *v1 = NULL, *v2 = NULL;
#if defined(_WIN32)
            VirtualFree(b, 0, MEM_RELEASE);
#else
            VirtualFree(b, XBOX_GPU_PHYS_SPAN, MEM_RELEASE);  /* shim needs the length */
#endif
            v0 = MapViewOfFileEx(g_contig_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                 0x04000000u, b);
            if (v0)
                v1 = MapViewOfFileEx(g_mapping_handle, FILE_MAP_ALL_ACCESS, 0,
                                     0x04000000u, 0x04000000u, b + 0x04000000u);
            if (v1)
                v2 = MapViewOfFileEx(g_staging_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                     XBOX_STAGING_SIZE, b + XBOX_STAGING_PHYS);
            if (v0 && v1 && v2) {
                g_gpu_phys_view = b;
                fprintf(stderr, "  GPU physical view: contiguous|heap|staging at"
                        " host %p (192 MB)\n", (void *)b);
            } else {
                if (v2) UnmapViewOfFile(v2);
                if (v1) UnmapViewOfFile(v1);
                if (v0) UnmapViewOfFile(v0);
                fprintf(stderr, "  WARNING: GPU physical view failed (error %lu);"
                        " the renderer keeps the identity view\n", GetLastError());
            }
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        /* A view of the CONTIGUOUS window, not of RAM.
         *
         * On hardware all three -- physical P, 0x80000000+P and 0xF0000000+P
         * -- are one and the same memory. Here they cannot be: the XBE image
         * is loaded at its own VA in the RAM mapping, so aliasing the
         * contiguous window onto RAM would drop a title's pinned physical
         * pools on top of its own code (Halo pins 3.4 MB at 0x61000, which is
         * inside its image). The contiguous window therefore has separate
         * storage, and the question becomes which of the two the tiled
         * aperture should be a view of.
         *
         * It is the contiguous one. A tiled address is a GPU surface address
         * by construction, and GPU surfaces come from
         * MmAllocateContiguousMemory -- so the pairing that has to hold is
         * tiled to contiguous. Against RAM instead, Half-Life 2's loader wrote
         * every decoded video frame through 0xF1C63000 while D3D sampled the
         * texture at 0x81C63000, and the sampler read zeros: 1.8 billion black
         * pixels rasterised, perfectly, from an empty texture.
         */
        if (tiled_size > XBOX_CONTIG_SIZE)
            tiled_size = XBOX_CONTIG_SIZE;
        g_tiled_view = g_contig_mapping
            ? MapViewOfFileEx(
                g_contig_mapping,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                tiled_size,
                (LPVOID)tiled_native)
            : NULL;
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_contig =
                    (volatile uint32_t *)((uintptr_t)(XBOX_CONTIG_BASE + 0x1000)
                                          + g_memory_offset);
                uint32_t saved = *via_contig;

                *via_tiled = 0xA5C30F17u;
                if (*via_contig != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " the contiguous window (wrote A5C30F17, read"
                            " %08X) -- the GPU will sample empty textures\n",
                            *via_contig);
                else
                    fprintf(stderr, "  Tiled aperture alias verified"
                            " (tiled 0x%08X == contiguous 0x%08X)\n",
                            XBOX_TILED_BASE, XBOX_CONTIG_BASE);
                *via_contig = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases the contiguous window)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

/* Reload the XBE image (header + sections) to guest memory the way a real Xbox
 * reboot reloads the title from disc: clears the game's dirty .data/.bss so a
 * reboot-to-self starts clean. The heap, kernel data, patched thunk table and
 * the persisted launch-data page are left untouched -- .rdata (which holds the
 * kernel thunk table the bridge rewrote) is deliberately skipped, and it is
 * read-only so it is never dirty anyway. */
void xbox_MemoryReloadSections(const void *xbe_data, size_t xbe_size)
{
    const uint8_t *xbe = (const uint8_t *)xbe_data;
    if (!g_memory_base || !xbe) return;
#define XVA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    {
        DWORD header_size = (xbe_size >= 0x10C) ? *(const DWORD *)(xbe + 0x0108) : 0;
        if (header_size == 0 || header_size > 0x10000) header_size = 0x1000;
        if (header_size > xbe_size) header_size = (DWORD)xbe_size;
        memcpy(XVA(XBOX_BASE_ADDRESS), xbe, header_size);
    }

    {
        DWORD base_addr        = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections     = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va  = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        DWORD si;
        if (num_sections > 64) num_sections = 64;
        for (si = 0; si < num_sections; si++) {
            const uint8_t *sh;
            DWORD sec_va, sec_vsize, sec_raw_off, sec_raw_size, copy_size, name_off;
            DWORD sec_name_va;
            const char *sec_name = "?";
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;
            sh          = xbe + sect_headers_off + si * SECTHDR_SIZE;
            sec_va      = *(const DWORD *)(sh + SECTHDR_VA);
            sec_vsize   = *(const DWORD *)(sh + SECTHDR_VSIZE);
            sec_raw_off = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            sec_raw_size= *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            sec_name_va = *(const DWORD *)(sh + 0x14);
            name_off    = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;
            /* Skip .rdata: it carries the kernel thunk table the bridge patched. */
            if (!strcmp(sec_name, ".rdata"))
                continue;
            copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;
            memset(XVA(sec_va), 0, sec_vsize);
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size)
                memcpy(XVA(sec_va), xbe + sec_raw_off, copy_size);
        }
    }
#undef XVA
    fprintf(stderr, "  [REBOOT] reloaded XBE image (clean .data/.bss, thunks kept)\n");
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;
static int      g_heap_rebased = 0;   /* one-time relocate above the loaded image */

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;                    /* image has no TLS; nothing to copy */

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    /* The TIB itself, copied so stack bounds and the fields the title filled
     * in are inherited, then the two that must not be. */
    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    *(uint32_t *)TIB_VA(block)      = thread_data;   /* slot 0           */
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total; /* fs:[4], see above*/

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
}

/* Bump allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base; XBOX_GPU_INSTANCE_DEFAULT is carved off the top by
 * the GPU-instance bridge, so the two do not meet until the window is full.
 *
 * Freed blocks ARE reclaimed now. MmFreeContiguousMemory used to route to
 * xbox_HeapFree, which never matched a window address, so every contiguous
 * free was lost and the arena only grew. A title that churns contiguous
 * memory while a level streams in -- Blinx re-allocates per-frame surfaces on
 * the warp screen -- then walks the bump to the 64 MB ceiling, and the next
 * >=64 KB texture buffer (a 256x256 DXT5 is exactly 0x10000) fails with the
 * general heap still mostly free. Same flat free-list as xbox_HeapAlloc: bump
 * order is address order, so coalescing is a neighbour check. */
static uint32_t g_contig_next = XBOX_CONTIG_BASE;

#define XBOX_CONTIG_MAX_BLOCKS 8192
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_contig_blocks[XBOX_CONTIG_MAX_BLOCKS];
static int g_contig_block_count = 0;

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;
    int i;

    /* One-time: start the arena ABOVE the loaded image. The contiguous window
     * is the physical-memory mirror (page P visible at XBOX_CONTIG_BASE+P), and
     * the Xbox 26-bit bus wraps it onto low RAM -- so bumping from P=0 hands out
     * pages that alias the title's own image: page zero, .text, .data. A GPU
     * instance block or pushbuffer written there scribbles on the guest's code
     * and globals, which surfaces later as heap/list corruption and a crash
     * (observed: MmAllocateContiguousMemoryEx returning phys 0 and phys 0x1C000,
     * the latter inside .text). Real hardware allocates contiguous memory from
     * free physical pages above the resident image; mirror that by skipping past
     * g_xbox_image_hi. */
    if (g_contig_next == XBOX_CONTIG_BASE && g_xbox_image_hi) {
        g_contig_next = XBOX_CONTIG_BASE + ((g_xbox_image_hi + 0xFFFFu) & ~0xFFFFu);
        fprintf(stderr, "  [CIMG] image bump: g_xbox_image_hi=0x%08X -> g_contig_next=0x%08X (image eats %u bytes of window)\n",
                (unsigned)g_xbox_image_hi, (unsigned)g_contig_next, (unsigned)(g_contig_next - XBOX_CONTIG_BASE)); fflush(stderr);
    }

    if (alignment < 4096) alignment = 4096;

    /* Reuse a freed block first. Without this the arena only ever grows: a
     * title that frees and re-allocates contiguous surfaces exhausts the 64 MB
     * window even though little is live at once. Whole-block reuse (no split),
     * matching xbox_HeapAlloc. */
    for (i = 0; i < g_contig_block_count; i++) {
        if (!g_contig_blocks[i].free || g_contig_blocks[i].size < size)
            continue;
        if (g_contig_blocks[i].addr & (alignment - 1))
            continue;   /* wrong alignment for this request */
        g_contig_blocks[i].free = 0;
        result = g_contig_blocks[i].addr;
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    result = (g_contig_next + alignment - 1) & ~(alignment - 1);

    /* Leave the top of the window for GPU instance memory. */
    if ((uint64_t)result + size >
            (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                - XBOX_GPU_INSTANCE_DEFAULT) {
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, %u of %u used, %d blocks)\n",
                size, g_contig_next - XBOX_CONTIG_BASE,
                (unsigned)XBOX_CONTIG_SIZE, g_contig_block_count);
        fflush(stderr);
        return 0;
    }

    g_contig_next = result + size;
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    { static int an; if (an++ < 60) { fprintf(stderr, "  [CALLOC] #%d size=%u (0x%X) align=%u -> total=%u/%u\n",
        an, (unsigned)size, (unsigned)size, (unsigned)alignment,
        (unsigned)(g_contig_next - XBOX_CONTIG_BASE), (unsigned)XBOX_CONTIG_SIZE); fflush(stderr); } }

    if (g_contig_block_count < XBOX_CONTIG_MAX_BLOCKS) {
        g_contig_blocks[g_contig_block_count].addr = result;
        g_contig_blocks[g_contig_block_count].size = size;
        g_contig_blocks[g_contig_block_count].free = 0;
        g_contig_block_count++;
    }

    return result;
}

/* Reclaim a contiguous block. MmFreeContiguousMemory routes here (it used to
 * call xbox_HeapFree, which never matched a window address, so the arena
 * leaked). Marks the block free and coalesces with bump-order neighbours so a
 * later large request fits after churn. Interior/foreign addresses are ignored
 * -- a free of something this arena never handed out is a no-op, as before. */
void xbox_ContiguousFree(uint32_t xbox_va)
{
    int i;

    if (!xbox_va)
        return;
    for (i = 0; i < g_contig_block_count; i++) {
        if (g_contig_blocks[i].addr != xbox_va || g_contig_blocks[i].free)
            continue;
        g_contig_blocks[i].free = 1;
        if (i + 1 < g_contig_block_count && g_contig_blocks[i + 1].free &&
            g_contig_blocks[i].addr + g_contig_blocks[i].size == g_contig_blocks[i + 1].addr) {
            g_contig_blocks[i].size += g_contig_blocks[i + 1].size;
            g_contig_blocks[i + 1].size = 0;
            g_contig_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_contig_blocks[i - 1].free &&
            g_contig_blocks[i - 1].addr + g_contig_blocks[i - 1].size == g_contig_blocks[i].addr) {
            g_contig_blocks[i - 1].size += g_contig_blocks[i].size;
            g_contig_blocks[i].size = 0;
            g_contig_blocks[i].addr = 0;
        }
        return;
    }
}

/* How much of the window has been handed out.
 *
 * Lets a caller holding a physical address decide whether it names contiguous
 * memory this runtime allocated. The pushbuffer executor needs exactly that:
 * a surface offset is physical, and only the window makes it addressable. */
uint32_t xbox_ContiguousAllocatedBytes(void)
{
    return g_contig_next - XBOX_CONTIG_BASE;
}

/* ── Texture-staging window ───────────────────────────────────────────────
 *
 * A second contiguous-style arena, host-backed and off the physical mirror,
 * for D3DX texture surfaces (see XBOX_STAGING_BASE). Same shape as
 * xbox_ContiguousAlloc -- address-ordered bump with a whole-block free-list and
 * neighbour coalescing -- but over its own window, with no image-bump skip
 * (nothing is resident here) and no GPU-instance reserve at the top. The bridge
 * routes raw-align-0x80 requests here and raw-align-16384 requests to the
 * mirror; a free is routed back by address via xbox_StagingContains. */
static uint32_t g_staging_next = XBOX_STAGING_BASE;

#define XBOX_STAGING_MAX_BLOCKS 8192
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_staging_blocks[XBOX_STAGING_MAX_BLOCKS];
static int g_staging_block_count = 0;

uint32_t xbox_StagingAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;
    int i;

    /* Window never came up -- tell the caller to fall back to the mirror. */
    if (!g_staging_memory || !size)
        return 0;

    if (alignment < 16) alignment = 16;

    /* Reuse a freed block first, so a title that frees and re-allocates texture
     * surfaces (level transitions Release the old set) doesn't walk the window
     * to its ceiling. Whole-block reuse, matching xbox_ContiguousAlloc. */
    for (i = 0; i < g_staging_block_count; i++) {
        if (!g_staging_blocks[i].free || g_staging_blocks[i].size < size)
            continue;
        if (g_staging_blocks[i].addr & (alignment - 1))
            continue;   /* wrong alignment for this request */
        g_staging_blocks[i].free = 0;
        result = g_staging_blocks[i].addr;
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    result = (g_staging_next + alignment - 1) & ~(alignment - 1);

    if ((uint64_t)result + size > (uint64_t)XBOX_STAGING_BASE + XBOX_STAGING_SIZE) {
        fprintf(stderr, "  [STAGE] window exhausted (%u requested, %u of %u used, %d blocks)\n",
                size, g_staging_next - XBOX_STAGING_BASE,
                (unsigned)XBOX_STAGING_SIZE, g_staging_block_count);
        fflush(stderr);
        return 0;   /* caller falls back to the mirror */
    }

    g_staging_next = result + size;
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    { static int an; if (an++ < 60) { fprintf(stderr, "  [STAGE] #%d size=%u (0x%X) align=%u -> VA 0x%08X, total=%u/%u\n",
        an, (unsigned)size, (unsigned)size, (unsigned)alignment, (unsigned)result,
        (unsigned)(g_staging_next - XBOX_STAGING_BASE), (unsigned)XBOX_STAGING_SIZE); fflush(stderr); } }

    if (g_staging_block_count < XBOX_STAGING_MAX_BLOCKS) {
        g_staging_blocks[g_staging_block_count].addr = result;
        g_staging_blocks[g_staging_block_count].size = size;
        g_staging_blocks[g_staging_block_count].free = 0;
        g_staging_block_count++;
    }

    return result;
}

/* Is this guest VA inside the staging window? The bridge uses it to route a
 * MmFreeContiguousMemory to the right arena. */
int xbox_StagingContains(uint32_t xbox_va)
{
    return xbox_va >= XBOX_STAGING_BASE
        && xbox_va <  XBOX_STAGING_BASE + XBOX_STAGING_SIZE;
}

/* Reclaim a staging block. Mirrors xbox_ContiguousFree: mark free, coalesce
 * with bump-order neighbours. A foreign/interior address is a no-op. */
void xbox_StagingFree(uint32_t xbox_va)
{
    int i;

    if (!xbox_va)
        return;
    for (i = 0; i < g_staging_block_count; i++) {
        if (g_staging_blocks[i].addr != xbox_va || g_staging_blocks[i].free)
            continue;
        g_staging_blocks[i].free = 1;
        if (i + 1 < g_staging_block_count && g_staging_blocks[i + 1].free &&
            g_staging_blocks[i].addr + g_staging_blocks[i].size == g_staging_blocks[i + 1].addr) {
            g_staging_blocks[i].size += g_staging_blocks[i + 1].size;
            g_staging_blocks[i + 1].size = 0;
            g_staging_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_staging_blocks[i - 1].free &&
            g_staging_blocks[i - 1].addr + g_staging_blocks[i - 1].size == g_staging_blocks[i].addr) {
            g_staging_blocks[i - 1].size += g_staging_blocks[i].size;
            g_staging_blocks[i].size = 0;
            g_staging_blocks[i].addr = 0;
        }
        return;
    }
}


uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    /* One-time: relocate the bump heap base ABOVE the loaded image. The static
     * XBOX_HEAP_BASE (0x00F80000) sits INSIDE a large image (Blinx's non-PRELOAD
     * MAP/MDL sections reach ~0x031DC780), so the growing bump heap would stomp
     * loaded sections (it clobbered MAP11's map-1-1 manifest -> the scene-9
     * loading hang). image_hi is set during section load, before any HeapAlloc.
     * Paired with the 128 MB map auto-size above so the heap has room. */
    /* Wait for image_hi (set during section load) before rebasing; only then
     * latch g_heap_rebased, so an early alloc during init can't skip it. */
    if (!g_heap_rebased && g_xbox_image_hi) {
        g_heap_rebased = 1;
        uint32_t above = (g_xbox_image_hi + 0xFFFFu) & ~0xFFFFu; /* 64 KB align */
        /* ...and above the contiguous arena too. xbox_ContiguousAlloc hands out
         * physical pages from the same image_hi upward (the 64 MB contiguous
         * window mirrors low RAM), so a heap based at image_hi sat on top of the
         * GPU's pushbuffers and framebuffers: DMA_PUT 0x031E1000 and the scan-out
         * 0x03264000 were inside the heap. The game's CRT heap free list then
         * got corrupted and sub_000F5EE6 (free-list insert) spun forever at
         * scene 9. With the 128 MB map the physical layout is:
         * [0, image_hi) image | [image_hi, 64 MB) contiguous | [64 MB, map) heap. */
        if (XBOX_HEAP_TOP > XBOX_CONTIG_SIZE + (16u << 20) && above < XBOX_CONTIG_SIZE)
            above = XBOX_CONTIG_SIZE;
        /* ...and above the stack area when it was moved into the heap region. */
        if (g_xbox_stack_base != XBOX_STACK_BASE_DEFAULT
            && above < g_xbox_stack_base + XBOX_STACK_SIZE)
            above = g_xbox_stack_base + XBOX_STACK_SIZE;
        if (g_heap_next < above) {
            fprintf(stderr, "  [HEAP] rebase base 0x%08X -> 0x%08X (above image_hi=0x%08X, top=0x%08X)\n",
                    g_heap_next, above, g_xbox_image_hi, (uint32_t)XBOX_HEAP_TOP);
            g_heap_next = above;
        }
    }

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    for (int i = 0; i < g_heap_block_count; i++) {
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
            continue;
        }
        if (g_heap_blocks[i].addr & (alignment - 1)) {
            continue;   /* wrong alignment for this request */
        }
        g_heap_blocks[i].free = 0;
        result = g_heap_blocks[i].addr;
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;

    if (!xbox_va)
        return 0;
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size)
            return g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
    }
    return 0;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. */
        if (i + 1 < g_heap_block_count && g_heap_blocks[i + 1].free &&
            g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[i + 1].addr) {
            g_heap_blocks[i].size += g_heap_blocks[i + 1].size;
            g_heap_blocks[i + 1].size = 0;
            g_heap_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_heap_blocks[i - 1].free &&
            g_heap_blocks[i - 1].addr + g_heap_blocks[i - 1].size == g_heap_blocks[i].addr) {
            g_heap_blocks[i - 1].size += g_heap_blocks[i].size;
            g_heap_blocks[i].size = 0;
            g_heap_blocks[i].addr = 0;
        }
        return;
    }
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
