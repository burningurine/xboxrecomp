/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page inaccessible, catch the access (a vectored handler on
 * Windows, SIGSEGV on an x86-64 Linux/Android host such as the emulator),
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what compiled device and recompiled guest code
 * emits against registers: moves both ways, the immediate forms, the zero- and
 * sign-extending loads, the ALU operations with a memory operand (either
 * direction, and with an immediate) that read-modify-write and poll loops are
 * built from, and the SSE moves a struct copy becomes. Anything outside that
 * set returns 0 rather than guessing: stepping over an instruction that was
 * not understood corrupts the guest silently, which is far worse than a fault
 * naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>
#include <string.h>

/* The saved thread context the decoder reads and completes: its instruction
 * pointer, flags, general-purpose registers (in ModRM order: rax rcx rdx rbx
 * rsp rbp rsi rdi r8-r15) and XMM registers. An includer that defines
 * MMIO_RIP first supplies all of it (mmio_ctx_t, MMIO_RIP, MMIO_EFLAGS,
 * mmio_ctx_reg, mmio_ctx_xmm) -- tests/mmio_decode does, to run the decoder
 * as a pure function on any host. */
#if defined(MMIO_RIP)
#define MMIO_DECODE_X64 1

#elif defined(_WIN32)
#include <windows.h>
#define MMIO_DECODE_X64 1
typedef CONTEXT mmio_ctx_t;
#define MMIO_RIP(c)    ((c)->Rip)
#define MMIO_EFLAGS(c) ((c)->EFlags)

static inline uint64_t *mmio_ctx_reg(mmio_ctx_t *c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   default: return (uint64_t *)&c->R15;
    }
}

static inline uint8_t *mmio_ctx_xmm(mmio_ctx_t *c, int reg)
{
    return (uint8_t *)&c->FltSave.XmmRegisters[reg & 0xF];
}

#elif defined(__x86_64__) && (defined(__linux__) || defined(__ANDROID__))
#include <ucontext.h>
#define MMIO_DECODE_X64 1
typedef mcontext_t mmio_ctx_t;
#define MMIO_RIP(c)    ((c)->gregs[REG_RIP])
#define MMIO_EFLAGS(c) ((c)->gregs[REG_EFL])

static inline uint64_t *mmio_ctx_reg(mmio_ctx_t *c, int reg)
{
    static const int slot[16] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    };
    return (uint64_t *)&c->gregs[slot[reg & 0xF]];
}

/* The FP/SSE state the kernel saved in the signal frame; sigreturn restores
 * it from there, so a load completed into it reaches the register. */
static inline uint8_t *mmio_ctx_xmm(mmio_ctx_t *c, int reg)
{
    return c->fpregs ? (uint8_t *)&c->fpregs->_xmm[reg & 0xF] : NULL;
}
#endif

#if defined(MMIO_DECODE_X64)

/* Service one register access. dev is passed straight back to the callbacks.
 * (Named apart from mmio_trap.h's mmio_read_fn, which takes an unsigned size,
 * so one file can use both.) */
typedef uint64_t (*mmio_dec_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_dec_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

/* Length of the ModRM byte and what follows it (SIB, displacement). */
static inline int mmio_modrm_len(const uint8_t *ip)
{
    uint8_t modrm = ip[0];
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    int len = 1;

    if (mod == 3) return 1;
    if (rm == 4) {
        len += 1;                                   /* SIB    */
        if (mod == 0 && (ip[1] & 7) == 5) len += 4; /* no base: disp32 */
    }
    if (mod == 0 && rm == 5) len += 4;              /* RIP-relative disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

static inline uint64_t mmio_mask(int size)
{
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}

static inline uint64_t mmio_sext(uint64_t v, int size)
{
    int sh = 64 - size * 8;
    return size >= 8 ? v : (uint64_t)((int64_t)(v << sh) >> sh);
}

static inline uint64_t mmio_imm(const uint8_t *p, int n)
{
    uint64_t v = 0;
    memcpy(&v, p, (size_t)n);
    return v;
}

/* A general-purpose register operand as x86 reads it: without a REX prefix,
 * byte registers 4-7 are AH CH DH BH. */
static inline uint64_t mmio_get_r(mmio_ctx_t *c, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*mmio_ctx_reg(c, reg - 4) >> 8) & 0xFF;
    return *mmio_ctx_reg(c, reg) & mmio_mask(size);
}

/* ... and written back as x86 does: 8- and 16-bit writes merge into the
 * register, a 32-bit write zeroes its upper half. */
static inline void mmio_set_r(mmio_ctx_t *c, int reg, int size, int has_rex, uint64_t v)
{
    uint64_t *r;
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        r = mmio_ctx_reg(c, reg - 4);
        *r = (*r & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    r = mmio_ctx_reg(c, reg);
    if (size == 1)      *r = (*r & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *r = (*r & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *r = v & 0xFFFFFFFFULL;
    else                *r = v;
}

/* The ALU operations, numbered as the 00-3F opcodes and the 80/81/83 group
 * number them (2 and 3, ADC and SBB, read the incoming carry and are not
 * handled), plus TEST. */
enum { MMIO_ADD = 0, MMIO_OR = 1, MMIO_AND = 4, MMIO_SUB = 5, MMIO_XOR = 6,
       MMIO_CMP = 7, MMIO_TEST = 8 };

/* a op b at the given width. Sets ZF, SF, CF and OF as x86 does, so a poll
 * loop or a signed compare after it branches correctly; PF and AF are left
 * alone (nothing emitted against a register reads them). */
static inline uint64_t mmio_alu(mmio_ctx_t *c, int op, uint64_t a, uint64_t b, int size)
{
    uint64_t m = mmio_mask(size), sign = 1ULL << (size * 8 - 1), r;
    int cf = 0, of = 0;

    a &= m; b &= m;
    switch (op) {
    case MMIO_ADD:
        r = (a + b) & m;
        cf = r < a;
        of = (~(a ^ b) & (a ^ r) & sign) != 0;
        break;
    case MMIO_SUB: case MMIO_CMP:
        r = (a - b) & m;
        cf = a < b;
        of = ((a ^ b) & (a ^ r) & sign) != 0;
        break;
    case MMIO_OR:  r = a | b; break;
    case MMIO_XOR: r = a ^ b; break;
    default:       r = a & b; break;          /* AND, TEST */
    }
    MMIO_EFLAGS(c) &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
    if (r == 0)   MMIO_EFLAGS(c) |= 0x0040;   /* ZF */
    if (r & sign) MMIO_EFLAGS(c) |= 0x0080;   /* SF */
    if (cf)       MMIO_EFLAGS(c) |= 0x0001;   /* CF */
    if (of)       MMIO_EFLAGS(c) |= 0x0800;   /* OF */
    return r;
}

/* 1 if the instruction at the context's RIP was serviced and RIP advanced
 * past it. off is the faulting address's offset into the device. */
static inline int mmio_emulate(mmio_ctx_t *ctx, uint32_t off, void *dev,
                               mmio_dec_read_fn rd, mmio_dec_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)(uintptr_t)MMIO_RIP(ctx);
    int prefix = 0, has66 = 0, has_f2 = 0, has_f3 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, mlen, reg, ilen;
    uint64_t v, imm;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2)              { has_f2 = 1; prefix++; }
        else if (b == 0xF3)              { has_f3 = 1; prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
        if (prefix > 4)
            return 0;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);

    op   = ip + prefix;
    size = has66 ? 2 : (rex_w ? 8 : 4);

    /* ALU with a register: 00-03 ADD, 08-0B OR, 20-23 AND, 28-2B SUB,
     * 30-33 XOR, 38-3B CMP. Bit 0 is the width (byte or full), bit 1 the
     * direction (the register is the destination). */
    if (op[0] < 0x40 && (op[0] & 7) < 4) {
        int alu = op[0] >> 3, to_reg = op[0] & 2;
        uint64_t m, r, res;
        if (alu == 2 || alu == 3)
            return 0;                                /* ADC, SBB */
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = mmio_get_r(ctx, reg, size, has_rex);
        res  = to_reg ? mmio_alu(ctx, alu, r, m, size) : mmio_alu(ctx, alu, m, r, size);
        if (alu != MMIO_CMP) {
            if (to_reg) mmio_set_r(ctx, reg, size, has_rex, res);
            else        wr(dev, off, res, size);
        }
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;
    }

    switch (op[0]) {
    case 0x80: case 0x81: case 0x83: {               /* ALU r/m, imm         */
        int alu = (op[1] >> 3) & 7;
        uint64_t res;
        if (alu == 2 || alu == 3)
            return 0;                                /* ADC, SBB */
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1);
        if (op[0] == 0x81 && size != 1) {
            ilen = size == 2 ? 2 : 4;
            imm  = mmio_sext(mmio_imm(op + 1 + mlen, ilen), ilen);
        } else {
            ilen = 1;
            imm  = mmio_sext(op[1 + mlen], 1);
        }
        res = mmio_alu(ctx, alu, rd(dev, off, size), imm, size);
        if (alu != MMIO_CMP)
            wr(dev, off, res, size);
        MMIO_RIP(ctx) += prefix + 1 + mlen + ilen;
        return 1;
    }

    case 0xF6: case 0xF7:                            /* TEST r/m, imm        */
        if (((op[1] >> 3) & 7) != 0)
            return 0;                                /* NOT NEG MUL DIV ... */
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1);
        ilen = size == 1 ? 1 : (size == 2 ? 2 : 4);
        imm  = mmio_sext(mmio_imm(op + 1 + mlen, ilen), ilen);
        mmio_alu(ctx, MMIO_TEST, rd(dev, off, size), imm, size);
        MMIO_RIP(ctx) += prefix + 1 + mlen + ilen;
        return 1;

    case 0xFE: case 0xFF: {                          /* INC / DEC r/m        */
        /* The runtime's own counter tick (*c += 1 on the APU sample counter)
         * compiles to this on x86-64. /2 and up are CALL, JMP and PUSH
         * through memory: refused. */
        int grp = (op[1] >> 3) & 7;
        uint64_t cf, res;
        if (grp > 1)
            return 0;
        if (op[0] == 0xFE) size = 1;
        mlen = mmio_modrm_len(op + 1);
        cf   = MMIO_EFLAGS(ctx) & 0x0001;            /* INC/DEC leave CF     */
        res  = mmio_alu(ctx, grp ? MMIO_SUB : MMIO_ADD, rd(dev, off, size), 1, size);
        MMIO_EFLAGS(ctx) = (MMIO_EFLAGS(ctx) & ~0x0001) | cf;
        wr(dev, off, res, size);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;
    }

    case 0x84: case 0x85:                            /* TEST r/m, r          */
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_alu(ctx, MMIO_TEST, rd(dev, off, size), mmio_get_r(ctx, reg, size, has_rex), size);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, mmio_get_r(ctx, reg, size, has_rex), size);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0x8A: case 0x8B:                            /* MOV r, r/m   (read)  */
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mmio_set_r(ctx, reg, size, has_rex, rd(dev, off, size));
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0x63:                                       /* MOVSXD r64, r/m32    */
        mlen = mmio_modrm_len(op + 1);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        v    = rd(dev, off, 4);
        mmio_set_r(ctx, reg, rex_w ? 8 : size, has_rex, rex_w ? mmio_sext(v, 4) : v);
        MMIO_RIP(ctx) += prefix + 1 + mlen;
        return 1;

    case 0xC6:                                       /* MOV r/m8, imm8       */
        mlen = mmio_modrm_len(op + 1);
        wr(dev, off, op[1 + mlen], 1);
        MMIO_RIP(ctx) += prefix + 1 + mlen + 1;
        return 1;

    case 0xC7:                                       /* MOV r/m, imm16/32    */
        mlen = mmio_modrm_len(op + 1);
        ilen = size == 2 ? 2 : 4;
        imm  = mmio_imm(op + 1 + mlen, ilen);
        if (size == 8) imm = mmio_sext(imm, 4);
        wr(dev, off, imm, size);
        MMIO_RIP(ctx) += prefix + 1 + mlen + ilen;
        return 1;

    case 0x0F:
        switch (op[1]) {
        case 0xB6: case 0xB7:                        /* MOVZX r, r/m8|16     */
        case 0xBE: case 0xBF: {                      /* MOVSX r, r/m8|16     */
            int s = (op[1] & 1) ? 2 : 1;
            mlen = mmio_modrm_len(op + 2);
            reg  = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            v    = rd(dev, off, s) & mmio_mask(s);
            if (op[1] >= 0xBE) v = mmio_sext(v, s);
            mmio_set_r(ctx, reg, size, has_rex, v);
            MMIO_RIP(ctx) += prefix + 2 + mlen;
            return 1;
        }

        /* SSE moves between an XMM register and memory, done as 8-byte (or
         * smaller) device accesses in address order. */
        case 0x10: case 0x11:                        /* MOVUPS/UPD/SS/SD     */
        case 0x28: case 0x29:                        /* MOVAPS/APD           */
        case 0x6F: case 0x7F:                        /* MOVDQA/DQU           */
        case 0x6E: case 0x7E:                        /* MOVD/MOVQ            */
        case 0xD6: {                                 /* MOVQ m64, xmm        */
            int load, bytes, o;
            uint8_t *x;
            switch (op[1]) {
            case 0x10: case 0x11:
                load = op[1] == 0x10;
                bytes = has_f3 ? 4 : (has_f2 ? 8 : 16);
                break;
            case 0x28: case 0x29:
                if (has_f2 || has_f3) return 0;
                load = op[1] == 0x28;
                bytes = 16;
                break;
            case 0x6F: case 0x7F:
                if (!has66 && !has_f3) return 0;     /* MMX register */
                load = op[1] == 0x6F;
                bytes = 16;
                break;
            case 0x6E:
                if (!has66) return 0;
                load = 1;
                bytes = rex_w ? 8 : 4;
                break;
            case 0x7E:
                if (has_f3)     { load = 1; bytes = 8; }
                else if (has66) { load = 0; bytes = rex_w ? 8 : 4; }
                else return 0;
                break;
            default:                                 /* 0xD6 */
                if (!has66) return 0;
                load = 0;
                bytes = 8;
                break;
            }
            mlen = mmio_modrm_len(op + 2);
            reg  = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            x    = mmio_ctx_xmm(ctx, reg);
            if (!x)
                return 0;
            for (o = 0; o < bytes; o += 8) {
                int w = bytes - o >= 8 ? 8 : bytes - o;
                if (load) {
                    v = rd(dev, off + (uint32_t)o, w);
                    memcpy(x + o, &v, (size_t)w);
                } else {
                    v = 0;
                    memcpy(&v, x + o, (size_t)w);
                    wr(dev, off + (uint32_t)o, v, w);
                }
            }
            if (load && bytes < 16)                  /* loads zero the rest */
                memset(x + bytes, 0, (size_t)(16 - bytes));
            MMIO_RIP(ctx) += prefix + 2 + mlen;
            return 1;
        }
        default:
            return 0;
        }

    default:
        return 0;
    }
}

#endif /* MMIO_DECODE_X64 */
#endif /* MMIO_DECODE_H */
