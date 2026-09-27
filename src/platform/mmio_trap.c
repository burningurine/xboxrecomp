/*
 * mmio_trap.c - device-register emulation via page protection + AArch64
 * load/store decoding. See mmio_trap.h.
 *
 * The fault is synchronous: it is raised by the recompiled guest's own
 * load/store, so the handler runs at a well-defined point of guest code and
 * may call the device callbacks (which take the device's own locks) -- the
 * guest thread never holds those locks itself.
 */
#include "mmio_trap.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/mman.h>
#include <ucontext.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif
#include <unistd.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define MAX_TRAPS 8
static struct {
    uint32_t base, size;
    mmio_read_fn read;
    mmio_write_fn write;
    void *opaque;
} s_traps[MAX_TRAPS];
static int s_ntraps;
static volatile uint64_t s_reads, s_writes, s_unhandled;

int mmio_trap_register(uint32_t guest_base, uint32_t size,
                       mmio_read_fn read, mmio_write_fn write, void *opaque)
{
    uintptr_t host = (uintptr_t)xbox_GetMemoryOffset() + guest_base;
    if (s_ntraps >= MAX_TRAPS || (guest_base & 0xFFF) || (size & 0xFFF))
        return -1;
    s_traps[s_ntraps].base = guest_base;
    s_traps[s_ntraps].size = size;
    s_traps[s_ntraps].read = read;
    s_traps[s_ntraps].write = write;
    s_traps[s_ntraps].opaque = opaque;
    __atomic_store_n(&s_ntraps, s_ntraps + 1, __ATOMIC_RELEASE);
    if (mprotect((void *)host, size, PROT_NONE) != 0) {
        fprintf(stderr, "  [MMIO] mprotect 0x%08X+0x%X failed\n", guest_base, size);
        s_ntraps--;
        return -1;
    }
    fprintf(stderr, "  [MMIO] trapping guest 0x%08X..0x%08X\n", guest_base, guest_base + size);
    return 0;
}

void mmio_trap_stats(uint64_t *reads, uint64_t *writes, uint64_t *unhandled)
{
    if (reads) *reads = s_reads;
    if (writes) *writes = s_writes;
    if (unhandled) *unhandled = s_unhandled;
}

static int find_trap(uint64_t gva)
{
    int n = __atomic_load_n(&s_ntraps, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (gva >= s_traps[i].base && gva < (uint64_t)s_traps[i].base + s_traps[i].size)
            return i;
    return -1;
}

#if defined(__aarch64__)

static uint64_t sext(uint64_t v, unsigned bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    return (v ^ m) - m;
}

static uint64_t xreg(mcontext_t *mc, unsigned r, int sp_ctx)
{
    if (r == 31) return sp_ctx ? mc->sp : 0;     /* SP as base, XZR as data */
    return mc->regs[r];
}

static void set_xreg(mcontext_t *mc, unsigned r, uint64_t v, int sp_ctx)
{
    if (r == 31) { if (sp_ctx) mc->sp = v; return; }
    mc->regs[r] = v;
}

/* SIMD/FP register file lives in the signal frame's __reserved area. */
static struct fpsimd_context *fpsimd(mcontext_t *mc)
{
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)mc->__reserved;
    while (h->magic) {
        if (h->magic == FPSIMD_MAGIC) return (struct fpsimd_context *)h;
        if (!h->size) break;
        h = (struct _aarch64_ctx *)((char *)h + h->size);
    }
    return NULL;
}

static uint64_t dev_read(int t, uint64_t gva, unsigned size)
{
    s_reads++;
    return s_traps[t].read ? s_traps[t].read(s_traps[t].opaque, (uint32_t)(gva - s_traps[t].base), size) : 0;
}

static void dev_write(int t, uint64_t gva, uint64_t v, unsigned size)
{
    s_writes++;
    if (s_traps[t].write)
        s_traps[t].write(s_traps[t].opaque, (uint32_t)(gva - s_traps[t].base), v, size);
}

int mmio_trap_handle(void *fault_addr, void *ucontext)
{
    ucontext_t *uc = (ucontext_t *)ucontext;
    mcontext_t *mc = &uc->uc_mcontext;
    uintptr_t mem = (uintptr_t)xbox_GetMemoryOffset();
    uint64_t gva = (uint64_t)((uintptr_t)fault_addr - mem);
    int t;

    if (!s_ntraps || (uintptr_t)fault_addr < mem || gva > 0xFFFFFFFFull)
        return 0;
    if ((t = find_trap(gva)) < 0)
        return 0;

    uint32_t insn = *(const uint32_t *)(uintptr_t)mc->pc;
    unsigned rt = insn & 31, rn = (insn >> 5) & 31;

    if (((insn >> 27) & 7) == 7 && !((insn >> 26) & 1)) {
        /* Load/store register, general-purpose. */
        unsigned size = insn >> 30, opc = (insn >> 22) & 3;
        unsigned bytes = 1u << size;
        int writeback = 0;
        int64_t wb_off = 0;
        if (((insn >> 24) & 3) == 1) {
            /* unsigned immediate: EA from fault address */
        } else if (((insn >> 24) & 3) == 0) {
            unsigned mode = (insn >> 10) & 3;
            if ((insn >> 21) & 1) {
                if (mode != 2) goto unhandled;         /* register offset only */
            } else if (mode == 1 || mode == 3) {        /* post / pre index */
                writeback = 1;
                wb_off = (int64_t)sext((insn >> 12) & 0x1FF, 9);
            } else if (mode == 2) {
                goto unhandled;                        /* LDTR/STTR */
            }
        } else {
            goto unhandled;
        }
        if (opc == 0) {                                /* STR */
            uint64_t v = xreg(mc, rt, 0);
            dev_write(t, gva, bytes == 8 ? v : (v & ((1ull << (8 * bytes)) - 1)), bytes);
        } else if (opc == 1) {                         /* LDR zero-extend */
            set_xreg(mc, rt, dev_read(t, gva, bytes), 0);
        } else if (size != 3) {                        /* LDRS */
            uint64_t v = sext(dev_read(t, gva, bytes), 8 * bytes);
            if (opc == 3) v &= 0xFFFFFFFFull;          /* to W register */
            set_xreg(mc, rt, v, 0);
        } else {
            goto unhandled;                            /* PRFM */
        }
        if (writeback)
            set_xreg(mc, rn, xreg(mc, rn, 1) + (uint64_t)wb_off, 1);
        mc->pc += 4;
        return 1;
    }

    if (((insn >> 27) & 7) == 7 && ((insn >> 26) & 1)
            && (((insn >> 24) & 3) == 1 || (((insn >> 24) & 3) == 0 && !((insn >> 21) & 1)
                                             && ((insn >> 10) & 3) == 0))) {
        /* LDR/STR of a SIMD&FP register (unsigned-immediate or unscaled offset). */
        struct fpsimd_context *fp = fpsimd(mc);
        unsigned size = insn >> 30, opc = (insn >> 22) & 3;
        unsigned bytes = (opc >= 2) ? 16 : (1u << size);     /* opc 1x with size 00 = Q */
        int load = opc & 1;
        if (!fp || (opc >= 2 && size != 0)) goto unhandled;
        uint8_t *reg = (uint8_t *)&fp->vregs[rt];
        for (unsigned o = 0; o < bytes; o += (bytes >= 8 ? 8 : bytes)) {
            unsigned w = bytes >= 8 ? 8 : bytes;
            if (load) { uint64_t v = dev_read(t, gva + o, w); memcpy(reg + o, &v, w); }
            else      { uint64_t v = 0; memcpy(&v, reg + o, w); dev_write(t, gva + o, v, w); }
        }
        if (load && bytes < 16) memset(reg + bytes, 0, 16 - bytes);
        mc->pc += 4;
        return 1;
    }

    if (((insn >> 27) & 7) == 5 && ((insn >> 26) & 1)) {
        /* LDP/STP of SIMD&FP registers (S/D/Q). */
        struct fpsimd_context *fp = fpsimd(mc);
        unsigned opc = insn >> 30, type = (insn >> 23) & 7, load = (insn >> 22) & 1;
        unsigned rt2 = (insn >> 10) & 31;
        unsigned scale = 4u << opc;                          /* 00 S, 01 D, 10 Q */
        int64_t off = (int64_t)sext((insn >> 15) & 0x7F, 7) * scale;
        uint64_t base = xreg(mc, rn, 1), ea;
        if (!fp || opc == 3) goto unhandled;
        if (type == 1) ea = base;
        else if (type == 0 || type == 2 || type == 3) ea = base + (uint64_t)off;
        else goto unhandled;
        uint64_t g = ea - mem;
        if (find_trap(g) != t || find_trap(g + 2 * scale - 1) != t) goto unhandled;
        unsigned regs[2] = { rt, rt2 };
        for (int k = 0; k < 2; k++) {
            uint8_t *reg = (uint8_t *)&fp->vregs[regs[k]];
            for (unsigned o = 0; o < scale; o += (scale >= 8 ? 8 : scale)) {
                unsigned w = scale >= 8 ? 8 : scale;
                uint64_t ga = g + (uint64_t)k * scale + o;
                if (load) { uint64_t v = dev_read(t, ga, w); memcpy(reg + o, &v, w); }
                else      { uint64_t v = 0; memcpy(&v, reg + o, w); dev_write(t, ga, v, w); }
            }
            if (load && scale < 16) memset(reg + scale, 0, 16 - scale);
        }
        if (type == 1 || type == 3)
            set_xreg(mc, rn, base + (uint64_t)off, 1);
        mc->pc += 4;
        return 1;
    }

    if (((insn >> 27) & 7) == 5 && !((insn >> 26) & 1)) {
        /* Load/store pair, general-purpose (LDP/STP/LDPSW). */
        unsigned opc = insn >> 30, type = (insn >> 23) & 7, load = (insn >> 22) & 1;
        unsigned rt2 = (insn >> 10) & 31;
        unsigned scale = (opc == 2) ? 8 : 4;
        int64_t off = (int64_t)sext((insn >> 15) & 0x7F, 7) * scale;
        uint64_t base = xreg(mc, rn, 1);
        uint64_t ea;
        if (opc == 3 || (opc == 1 && !load)) goto unhandled;
        if (type == 1) ea = base;                      /* post-index */
        else if (type == 2 || type == 0 || type == 3) ea = base + (uint64_t)off;
        else goto unhandled;
        uint64_t g0 = ea - mem, g1 = g0 + scale;
        if (find_trap(g0) != t || find_trap(g1) != t) goto unhandled;
        if (load) {
            uint64_t a = dev_read(t, g0, scale), b = dev_read(t, g1, scale);
            if (opc == 1) { a = sext(a, 32); b = sext(b, 32); }
            set_xreg(mc, rt, a, 0);
            set_xreg(mc, rt2, b, 0);
        } else {
            uint64_t a = xreg(mc, rt, 0), b = xreg(mc, rt2, 0);
            if (scale == 4) { a &= 0xFFFFFFFFull; b &= 0xFFFFFFFFull; }
            dev_write(t, g0, a, scale);
            dev_write(t, g1, b, scale);
        }
        if (type == 1 || type == 3)
            set_xreg(mc, rn, base + (uint64_t)off, 1);
        mc->pc += 4;
        return 1;
    }

unhandled:
    s_unhandled++;
    if (s_unhandled <= 8)
        fprintf(stderr, "  [MMIO] unhandled instruction 0x%08X at guest 0x%08X\n",
                insn, (uint32_t)gva);
    return 0;
}

#else   /* not aarch64 */
int mmio_trap_handle(void *fault_addr, void *ucontext) { (void)fault_addr; (void)ucontext; return 0; }
#endif

#else   /* not POSIX: the Windows build uses its VEH hooks */
int mmio_trap_register(uint32_t guest_base, uint32_t size,
                       mmio_read_fn read, mmio_write_fn write, void *opaque)
{ (void)guest_base; (void)size; (void)read; (void)write; (void)opaque; return -1; }
int mmio_trap_handle(void *fault_addr, void *ucontext) { (void)fault_addr; (void)ucontext; return 0; }
void mmio_trap_stats(uint64_t *reads, uint64_t *writes, uint64_t *unhandled)
{ if (reads) *reads = 0; if (writes) *writes = 0; if (unhandled) *unhandled = 0; }
#endif
