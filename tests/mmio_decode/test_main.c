/*
 * mmio_decode - does the trapped-MMIO instruction decoder service what the
 * device code and the recompiled guest emit, and refuse what it does not
 * understand.
 *
 * This is testable without a device or a fault because the decoder only ever
 * reads instruction bytes at the context's RIP and edits the context. The
 * test supplies its own plain register file (mmio_decode.h takes one when
 * MMIO_RIP is defined first), points RIP at hand-encoded instructions, and
 * the whole thing is a pure function that runs on any host. The Windows and
 * x86-64 Linux/Android builds differ only in where those registers live.
 *
 * The cases that matter are the ones where being wrong is silent: an
 * instruction length that leaves RIP mid-instruction, a 32-bit read that does
 * not clear the high half of the destination, flags that send a poll loop the
 * wrong way, and an unrecognised opcode reported as handled -- which steps
 * over an instruction nobody decoded and corrupts the guest with no message.
 *
 * Build: any C compiler, e.g. cl /I ..\..\src\platform test_main.c, or
 * run_ndk.sh here, which needs only the Android NDK.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint64_t rip, eflags;
    uint64_t gpr[16];            /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
    uint8_t  xmm[16][16];
} test_ctx;

typedef test_ctx mmio_ctx_t;
#define MMIO_RIP(c)    ((c)->rip)
#define MMIO_EFLAGS(c) ((c)->eflags)
static inline uint64_t *mmio_ctx_reg(mmio_ctx_t *c, int reg) { return &c->gpr[reg & 15]; }
static inline uint8_t *mmio_ctx_xmm(mmio_ctx_t *c, int reg) { return c->xmm[reg & 15]; }

#include "mmio_decode.h"

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9 };
#define ZF 0x40
#define SF 0x80
#define CF 0x01
#define OF 0x800

static int failures;
#define CHECK(name, cond) \
    do { if (cond) {} else { printf("FAIL: %s\n", name); failures++; } } while (0)
#define CHECK_U64(name, got, want) \
    do { uint64_t g_=(uint64_t)(got), w_=(uint64_t)(want); \
         if (g_ == w_) {} else { \
            printf("FAIL: %s (got 0x%llX, want 0x%llX)\n", name, \
                   (unsigned long long)g_, (unsigned long long)w_); \
            failures++; } } while (0)

/* A device that records what it was asked for and answers reads from a
 * little register file of its own. */
static struct {
    uint32_t off; uint64_t val; int size; int reads, writes;
    uint8_t regs[0x200];
} dev;

static uint64_t t_read(void *d, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)d; dev.off = off; dev.size = size; dev.reads++;
    memcpy(&v, dev.regs + off, (size_t)size);
    return v;
}
static void t_write(void *d, uint32_t off, uint64_t v, int size)
{
    (void)d; dev.off = off; dev.val = v; dev.size = size; dev.writes++;
    memcpy(dev.regs + off, &v, (size_t)size);
}

static void dev_set(uint32_t off, uint64_t v, int size)
{
    memcpy(dev.regs + off, &v, (size_t)size);
}

/* Run one instruction against device offset 0x54. Returns what mmio_emulate
 * returned; RIP is expected to land exactly past the `len`-byte encoding. */
static int run_at(const uint8_t *code, size_t len, test_ctx *ctx, uint32_t off)
{
    int r;
    dev.off = 0; dev.val = 0; dev.size = 0; dev.reads = dev.writes = 0;
    ctx->rip = (uint64_t)(uintptr_t)code;
    r = mmio_emulate(ctx, off, NULL, t_read, t_write);
    if (r) {
        uint64_t advanced = ctx->rip - (uint64_t)(uintptr_t)code;
        if (advanced != len) {
            printf("FAIL: RIP advanced %llu, instruction %02X %02X %02X is %llu bytes\n",
                   (unsigned long long)advanced, code[0], code[1], code[2],
                   (unsigned long long)len);
            failures++;
        }
    } else if (ctx->rip != (uint64_t)(uintptr_t)code) {
        printf("FAIL: refused instruction moved RIP\n");
        failures++;
    }
    return r;
}
#define RUN(code, ctx) run_at(code, sizeof code, ctx, 0x54)

int main(void)
{
    test_ctx ctx;
    printf("mmio_decode: running\n");
    memset(&ctx, 0, sizeof ctx);
    memset(&dev, 0, sizeof dev);

    /* ---- moves --------------------------------------------------------- */

    /* mov [rax], ecx  -- 89 08 */
    { static const uint8_t c[] = { 0x89, 0x08 };
      ctx.gpr[RCX] = 0xDEADBEEF;
      CHECK("mov r/m,r handled", RUN(c, &ctx));
      CHECK_U64("mov r/m,r value", dev.val, 0xDEADBEEF);
      CHECK_U64("mov r/m,r size",  dev.size, 4);
      CHECK_U64("mov r/m,r offset", dev.off, 0x54); }

    /* mov [rax], cl   -- 88 08, byte sized */
    { static const uint8_t c[] = { 0x88, 0x08 };
      ctx.gpr[RCX] = 0xAA;
      CHECK("mov r/m8,r8 handled", RUN(c, &ctx));
      CHECK_U64("mov r/m8 size", dev.size, 1);
      CHECK_U64("mov r/m8 value", dev.val, 0xAA); }

    /* mov [rax], ch   -- 88 28: without REX, byte register 5 is CH, not BPL. */
    { static const uint8_t c[] = { 0x88, 0x28 };
      ctx.gpr[RCX] = 0x1234; ctx.gpr[RBP] = 0x99;
      CHECK("mov r/m8,ch handled", RUN(c, &ctx));
      CHECK_U64("no REX: reg 5 is CH", dev.val, 0x12); }

    /* mov [rax], bpl  -- 40 88 28: with any REX it is BPL. */
    { static const uint8_t c[] = { 0x40, 0x88, 0x28 };
      CHECK("mov r/m8,bpl handled", RUN(c, &ctx));
      CHECK_U64("REX: reg 5 is BPL", dev.val, 0x99); }

    /* mov ecx, [rax]  -- 8B 08. A 32-bit read must clear the high half; if it
     * does not, a register holding a stale 64-bit value reads back wrong. */
    { static const uint8_t c[] = { 0x8B, 0x08 };
      ctx.gpr[RCX] = 0xFFFFFFFFFFFFFFFFULL;
      dev_set(0x54, 0x12345678, 4);
      CHECK("mov r,r/m handled", RUN(c, &ctx));
      CHECK_U64("32-bit read clears high half", ctx.gpr[RCX], 0x12345678ULL); }

    /* mov ah, [rax]   -- 8A 20: merges into bits 8-15 of rax. */
    { static const uint8_t c[] = { 0x8A, 0x20 };
      ctx.gpr[RAX] = 0x1111111111111111ULL;
      dev_set(0x54, 0xAB, 1);
      CHECK("mov ah,r/m8 handled", RUN(c, &ctx));
      CHECK_U64("mov ah merges", ctx.gpr[RAX], 0x111111111111AB11ULL); }

    /* mov cx, [rax]   -- 66 8B 08: a 16-bit read merges. */
    { static const uint8_t c[] = { 0x66, 0x8B, 0x08 };
      ctx.gpr[RCX] = 0xFFFFFFFFFFFFFFFFULL;
      dev_set(0x54, 0x1234, 2);
      CHECK("mov r16,r/m16 handled", RUN(c, &ctx));
      CHECK_U64("16-bit read merges", ctx.gpr[RCX], 0xFFFFFFFFFFFF1234ULL); }

    /* mov r9, [rax]   -- 4C 8B 08: REX.R selects r8-r15, REX.W 64 bits. */
    { static const uint8_t c[] = { 0x4C, 0x8B, 0x08 };
      dev_set(0x54, 0x0123456789ABCDEFULL, 8);
      CHECK("mov r9,r/m64 handled", RUN(c, &ctx));
      CHECK_U64("REX.R + REX.W read", ctx.gpr[R9], 0x0123456789ABCDEFULL);
      CHECK_U64("REX.W read size", dev.size, 8); }

    /* mov dword [rax], imm32 -- C7 00 EF BE AD DE */
    { static const uint8_t c[] = { 0xC7, 0x00, 0xEF, 0xBE, 0xAD, 0xDE };
      CHECK("mov r/m,imm32 handled", RUN(c, &ctx));
      CHECK_U64("imm32 value", dev.val, 0xDEADBEEF); }

    /* mov word [rax], imm16 -- 66 C7 00 34 12: the immediate is 2 bytes. */
    { static const uint8_t c[] = { 0x66, 0xC7, 0x00, 0x34, 0x12 };
      CHECK("mov r/m16,imm16 handled", RUN(c, &ctx));
      CHECK_U64("imm16 value", dev.val, 0x1234);
      CHECK_U64("imm16 size", dev.size, 2); }

    /* mov qword [rax], -2 -- 48 C7 00 FE FF FF FF: sign-extended. */
    { static const uint8_t c[] = { 0x48, 0xC7, 0x00, 0xFE, 0xFF, 0xFF, 0xFF };
      CHECK("mov r/m64,imm32 handled", RUN(c, &ctx));
      CHECK_U64("imm32 sign-extends to 64", dev.val, 0xFFFFFFFFFFFFFFFEULL); }

    /* mov byte [rax+0x10], 0x7F -- C6 40 10 7F */
    { static const uint8_t c[] = { 0xC6, 0x40, 0x10, 0x7F };
      CHECK("mov r/m8,imm8 handled", RUN(c, &ctx));
      CHECK_U64("imm8 value", dev.val, 0x7F); }

    /* movzx eax, byte [rax] -- 0F B6 00 */
    { static const uint8_t c[] = { 0x0F, 0xB6, 0x00 };
      ctx.gpr[RAX] = 0xFFFFFFFFFFFFFFFFULL;
      dev_set(0x54, 0x91, 1);
      CHECK("movzx r32,r/m8 handled", RUN(c, &ctx));
      CHECK_U64("movzx zero-extends", ctx.gpr[RAX], 0x91ULL); }

    /* movsx ecx, word [rax] -- 0F BF 08: sign-extends into 32 bits, and the
     * 32-bit write clears the upper half. */
    { static const uint8_t c[] = { 0x0F, 0xBF, 0x08 };
      ctx.gpr[RCX] = 0xFFFFFFFFFFFFFFFFULL;
      dev_set(0x54, 0xBEEF, 2);
      CHECK("movsx r32,r/m16 handled", RUN(c, &ctx));
      CHECK_U64("movsx sign-extends to 32", ctx.gpr[RCX], 0xFFFFBEEFULL);
      CHECK_U64("movsx reads 2 bytes", dev.size, 2); }

    /* movsx rcx, byte [rax] -- 48 0F BE 08 */
    { static const uint8_t c[] = { 0x48, 0x0F, 0xBE, 0x08 };
      dev_set(0x54, 0xA5, 1);
      CHECK("movsx r64,r/m8 handled", RUN(c, &ctx));
      CHECK_U64("movsx sign-extends to 64", ctx.gpr[RCX], 0xFFFFFFFFFFFFFFA5ULL); }

    /* movsxd rdx, dword [rax] -- 48 63 10 */
    { static const uint8_t c[] = { 0x48, 0x63, 0x10 };
      dev_set(0x54, 0x80000000, 4);
      CHECK("movsxd handled", RUN(c, &ctx));
      CHECK_U64("movsxd sign-extends", ctx.gpr[RDX], 0xFFFFFFFF80000000ULL); }

    /* ---- flags: poll loops and compares -------------------------------- */

    /* test [rax], ecx -- 85 08. The whole point is the flags: a poll loop
     * spins on jz/jnz off this. */
    { static const uint8_t c[] = { 0x85, 0x08 };
      ctx.gpr[RCX] = 0x00000001; ctx.eflags = 0;
      dev_set(0x54, 1, 4);
      RUN(c, &ctx);
      CHECK("test bit set -> ZF clear", (ctx.eflags & ZF) == 0);
      dev_set(0x54, 2, 4);
      RUN(c, &ctx);
      CHECK("test bit clear -> ZF set", (ctx.eflags & ZF) != 0);
      CHECK("test writes nothing", dev.writes == 0); }

    /* cmp [rax], ecx -- 39 08. Equal sets ZF; below sets CF. */
    { static const uint8_t c[] = { 0x39, 0x08 };
      ctx.gpr[RCX] = 0x10; dev_set(0x54, 0x10, 4); ctx.eflags = 0;
      RUN(c, &ctx);
      CHECK("cmp equal -> ZF", (ctx.eflags & ZF) != 0);
      CHECK("cmp equal -> no CF", (ctx.eflags & CF) == 0);
      ctx.gpr[RCX] = 0x20;
      RUN(c, &ctx);
      CHECK("cmp below -> CF", (ctx.eflags & CF) != 0);
      CHECK("cmp writes nothing", dev.writes == 0); }

    /* cmp ecx, [rax] -- 3B 08: the other direction, ecx - [rax]. Signed:
     * 1 - (-1) = 2 is not less, so SF == OF. */
    { static const uint8_t c[] = { 0x3B, 0x08 };
      ctx.gpr[RCX] = 1; dev_set(0x54, 0xFFFFFFFF, 4); ctx.eflags = 0;
      CHECK("cmp r,r/m handled", RUN(c, &ctx));
      CHECK("1 > -1 signed: SF == OF", !!(ctx.eflags & SF) == !!(ctx.eflags & OF));
      CHECK("1 < 0xFFFFFFFF unsigned: CF", (ctx.eflags & CF) != 0); }

    /* cmp dword [rax], 0x7FFFFFFF vs 0x80000000 -- 81 38 FF FF FF 7F:
     * signed overflow, so jl must see SF != OF. */
    { static const uint8_t c[] = { 0x81, 0x38, 0xFF, 0xFF, 0xFF, 0x7F };
      dev_set(0x54, 0x80000000, 4); ctx.eflags = 0;
      CHECK("cmp r/m,imm32 handled", RUN(c, &ctx));
      CHECK("INT_MIN < INT_MAX signed: SF != OF",
            !!(ctx.eflags & SF) != !!(ctx.eflags & OF)); }

    /* cmp dword [rax+0x10], 0 -- 83 78 10 00 */
    { static const uint8_t c[] = { 0x83, 0x78, 0x10, 0x00 };
      dev_set(0x54, 0, 4); ctx.eflags = 0;
      CHECK("cmp r/m,imm8 handled", RUN(c, &ctx));
      CHECK("cmp 0,0 -> ZF", (ctx.eflags & ZF) != 0); }

    /* cmp word [rax], 0xBEEF -- 66 81 38 EF BE: a 16-bit immediate. */
    { static const uint8_t c[] = { 0x66, 0x81, 0x38, 0xEF, 0xBE };
      dev_set(0x54, 0xBEEF, 2); ctx.eflags = 0;
      CHECK("cmp r/m16,imm16 handled", RUN(c, &ctx));
      CHECK("16-bit compare equal -> ZF", (ctx.eflags & ZF) != 0);
      CHECK_U64("16-bit compare reads 2 bytes", dev.size, 2); }

    /* cmp byte [rax], 0xA5 -- 80 38 A5 */
    { static const uint8_t c[] = { 0x80, 0x38, 0xA5 };
      dev_set(0x54, 0xA5, 1); ctx.eflags = 0;
      CHECK("cmp r/m8,imm8 handled", RUN(c, &ctx));
      CHECK("byte compare equal -> ZF", (ctx.eflags & ZF) != 0); }

    /* test byte [rax], 1 -- F6 00 01, and test dword [rax], 0x100 --
     * F7 00 00 01 00 00. */
    { static const uint8_t c1[] = { 0xF6, 0x00, 0x01 };
      static const uint8_t c2[] = { 0xF7, 0x00, 0x00, 0x01, 0x00, 0x00 };
      dev_set(0x54, 0x100, 4); ctx.eflags = 0;
      CHECK("test r/m8,imm8 handled", RUN(c1, &ctx));
      CHECK("bit 0 clear -> ZF", (ctx.eflags & ZF) != 0);
      CHECK("test r/m32,imm32 handled", RUN(c2, &ctx));
      CHECK("bit 8 set -> no ZF", (ctx.eflags & ZF) == 0); }

    /* ---- read-modify-write --------------------------------------------- */

    /* or [rax], ecx -- 09 08, and and [rax], ecx -- 21 08. Both are
     * read-modify-write: a device that only sees the write loses the bits it
     * already had. */
    { static const uint8_t c[] = { 0x09, 0x08 };
      ctx.gpr[RCX] = 0x0F; dev_set(0x54, 0xF0, 4);
      RUN(c, &ctx);
      CHECK_U64("or merges existing bits", dev.val, 0xFF);
      CHECK("or reads before writing", dev.reads == 1 && dev.writes == 1); }

    { static const uint8_t c[] = { 0x21, 0x08 };
      ctx.gpr[RCX] = 0x0F; dev_set(0x54, 0xFF, 4);
      RUN(c, &ctx);
      CHECK_U64("and masks existing bits", dev.val, 0x0F); }

    /* or dword [rax], 0x80000000 -- 81 08 00 00 00 80 */
    { static const uint8_t c[] = { 0x81, 0x08, 0x00, 0x00, 0x00, 0x80 };
      dev_set(0x54, 0x1, 4);
      CHECK("or r/m,imm32 handled", RUN(c, &ctx));
      CHECK_U64("or imm32", dev.val, 0x80000001ULL); }

    /* and dword [rax], -2 -- 83 20 FE: imm8 sign-extends to 0xFFFFFFFE. */
    { static const uint8_t c[] = { 0x83, 0x20, 0xFE };
      dev_set(0x54, 0x3, 4);
      CHECK("and r/m,imm8 handled", RUN(c, &ctx));
      CHECK_U64("and imm8 sign-extends", dev.val, 0x2); }

    /* add dword [rax], 1 -- 83 00 01, sub dword [rax], 1 -- 83 28 01 */
    { static const uint8_t c1[] = { 0x83, 0x00, 0x01 };
      static const uint8_t c2[] = { 0x83, 0x28, 0x01 };
      dev_set(0x54, 0xFFFFFFFF, 4); ctx.eflags = 0;
      CHECK("add r/m,imm8 handled", RUN(c1, &ctx));
      CHECK_U64("add wraps", dev.val, 0);
      CHECK("add wrap -> ZF and CF", (ctx.eflags & (ZF | CF)) == (ZF | CF));
      CHECK("sub r/m,imm8 handled", RUN(c2, &ctx));
      CHECK_U64("sub borrows", dev.val, 0xFFFFFFFFULL);
      CHECK("sub borrow -> CF", (ctx.eflags & CF) != 0); }

    /* inc dword [rax+0x20010] -- FF 80 10 00 02 00: the runtime's own tick of
     * the APU sample counter, as clang emits it. INC and DEC leave CF. */
    { static const uint8_t c1[] = { 0xFF, 0x80, 0x10, 0x00, 0x02, 0x00 };
      static const uint8_t c2[] = { 0xFE, 0x08 };            /* dec byte [rax] */
      dev_set(0x54, 0x7FFFFFFF, 4); ctx.eflags = CF;
      CHECK("inc r/m32 handled", RUN(c1, &ctx));
      CHECK_U64("inc value", dev.val, 0x80000000ULL);
      CHECK("inc keeps CF", (ctx.eflags & CF) != 0);
      CHECK("inc overflow -> OF, SF", (ctx.eflags & (OF | SF)) == (OF | SF));
      dev_set(0x54, 0x01, 1); ctx.eflags = 0;
      CHECK("dec r/m8 handled", RUN(c2, &ctx));
      CHECK_U64("dec value", dev.val, 0);
      CHECK_U64("dec size", dev.size, 1);
      CHECK("dec to 0 -> ZF, no CF", (ctx.eflags & (ZF | CF)) == ZF); }

    /* xor [rax], ecx -- 31 08 */
    { static const uint8_t c[] = { 0x31, 0x08 };
      ctx.gpr[RCX] = 0xFF; dev_set(0x54, 0x0F, 4);
      CHECK("xor r/m,r handled", RUN(c, &ctx));
      CHECK_U64("xor", dev.val, 0xF0); }

    /* add ecx, [rax] -- 03 08: the register is the destination; the device
     * is only read. */
    { static const uint8_t c[] = { 0x03, 0x08 };
      ctx.gpr[RCX] = 0xFFFFFFFF00000010ULL; dev_set(0x54, 0x20, 4);
      CHECK("add r,r/m handled", RUN(c, &ctx));
      CHECK_U64("add r,r/m result, high half cleared", ctx.gpr[RCX], 0x30);
      CHECK("add r,r/m writes nothing", dev.writes == 0); }

    /* sub ecx, [rax] -- 2B 08: ecx - [rax]. */
    { static const uint8_t c[] = { 0x2B, 0x08 };
      ctx.gpr[RCX] = 0x30; dev_set(0x54, 0x10, 4);
      CHECK("sub r,r/m handled", RUN(c, &ctx));
      CHECK_U64("sub r,r/m order", ctx.gpr[RCX], 0x20); }

    /* ---- SSE moves (struct copies) ------------------------------------- */

    /* movups xmm0, [rax] -- 0F 10 00: two 8-byte reads in address order. */
    { static const uint8_t c[] = { 0x0F, 0x10, 0x00 };
      uint64_t lo, hi;
      dev_set(0x54, 0x1111111122222222ULL, 8);
      dev_set(0x5C, 0x3333333344444444ULL, 8);
      CHECK("movups load handled", RUN(c, &ctx));
      memcpy(&lo, ctx.xmm[0], 8); memcpy(&hi, ctx.xmm[0] + 8, 8);
      CHECK_U64("movups low", lo, 0x1111111122222222ULL);
      CHECK_U64("movups high", hi, 0x3333333344444444ULL);
      CHECK("movups load = 2 reads", dev.reads == 2); }

    /* movups [rax+0x10], xmm9 -- 44 0F 11 48 10 */
    { static const uint8_t c[] = { 0x44, 0x0F, 0x11, 0x48, 0x10 };
      uint64_t lo = 0xAAAAAAAABBBBBBBBULL, hi = 0xCCCCCCCCDDDDDDDDULL, got;
      memcpy(ctx.xmm[9], &lo, 8); memcpy(ctx.xmm[9] + 8, &hi, 8);
      CHECK("movups store handled", RUN(c, &ctx));
      CHECK("movups store = 2 writes", dev.writes == 2);
      memcpy(&got, dev.regs + 0x54, 8); CHECK_U64("movups store low", got, lo);
      memcpy(&got, dev.regs + 0x5C, 8); CHECK_U64("movups store high", got, hi); }

    /* movdqu xmm1, [rax] -- F3 0F 6F 08; movdqa [rax], xmm1 -- 66 0F 7F 08 */
    { static const uint8_t c1[] = { 0xF3, 0x0F, 0x6F, 0x08 };
      static const uint8_t c2[] = { 0x66, 0x0F, 0x7F, 0x08 };
      CHECK("movdqu load handled", RUN(c1, &ctx));
      CHECK("movdqa store handled", RUN(c2, &ctx)); }

    /* movq xmm2, [rax] -- F3 0F 7E 10: loads 8 bytes, zeroes the rest. */
    { static const uint8_t c[] = { 0xF3, 0x0F, 0x7E, 0x10 };
      uint64_t lo, hi;
      memset(ctx.xmm[2], 0xFF, 16);
      dev_set(0x54, 0x0102030405060708ULL, 8);
      CHECK("movq load handled", RUN(c, &ctx));
      memcpy(&lo, ctx.xmm[2], 8); memcpy(&hi, ctx.xmm[2] + 8, 8);
      CHECK_U64("movq low", lo, 0x0102030405060708ULL);
      CHECK_U64("movq zeroes high", hi, 0); }

    /* movd [rax], xmm3 -- 66 0F 7E 18: stores 4 bytes. */
    { static const uint8_t c[] = { 0x66, 0x0F, 0x7E, 0x18 };
      uint32_t v = 0xCAFEF00D;
      memcpy(ctx.xmm[3], &v, 4);
      CHECK("movd store handled", RUN(c, &ctx));
      CHECK_U64("movd store value", dev.val, 0xCAFEF00D);
      CHECK_U64("movd store size", dev.size, 4); }

    /* movss xmm4, [rax] -- F3 0F 10 20: 4 bytes, rest zeroed. */
    { static const uint8_t c[] = { 0xF3, 0x0F, 0x10, 0x20 };
      uint64_t lo;
      memset(ctx.xmm[4], 0xFF, 16);
      dev_set(0x54, 0x3F800000, 4);
      CHECK("movss load handled", RUN(c, &ctx));
      memcpy(&lo, ctx.xmm[4], 8);
      CHECK_U64("movss zeroes above 4 bytes", lo, 0x3F800000ULL);
      CHECK_U64("movss read size", dev.size, 4); }

    /* ---- instruction lengths ------------------------------------------- */

    /* 66 prefix selects 16-bit, REX.W selects 64-bit. */
    { static const uint8_t c[] = { 0x66, 0x89, 0x08 };
      ctx.gpr[RCX] = 0x1234;
      CHECK("66-prefixed handled", RUN(c, &ctx));
      CHECK_U64("66 prefix -> 2 bytes", dev.size, 2); }

    { static const uint8_t c[] = { 0x48, 0x89, 0x08 };
      CHECK("REX.W handled", RUN(c, &ctx));
      CHECK_U64("REX.W -> 8 bytes", dev.size, 8); }

    /* disp8 and disp32 forms have to add to the length, or RIP lands inside
     * the instruction and the next decode is garbage.
     * mov [rax+0x10], ecx -- 89 48 10        (mod=01, disp8)
     * mov [rax+0x1000], ecx -- 89 88 00 10 00 00  (mod=10, disp32) */
    { static const uint8_t c1[] = { 0x89, 0x48, 0x10 };
      CHECK("disp8 handled", RUN(c1, &ctx)); }
    { static const uint8_t c2[] = { 0x89, 0x88, 0x00, 0x10, 0x00, 0x00 };
      CHECK("disp32 handled", RUN(c2, &ctx)); }
    /* SIB: mov [rax+rbx*1], ecx -- 89 0C 18 */
    { static const uint8_t c3[] = { 0x89, 0x0C, 0x18 };
      CHECK("SIB handled", RUN(c3, &ctx)); }
    /* SIB + disp8: mov [rax+rbx*4+0x20], ecx -- 89 4C 98 20 */
    { static const uint8_t c4[] = { 0x89, 0x4C, 0x98, 0x20 };
      CHECK("SIB+disp8 handled", RUN(c4, &ctx)); }
    /* SIB with no base: mov [rbx*4+0x12345678], ecx -- 89 0C 9D 78 56 34 12
     * (mod=00, SIB base=101 means disp32 and no base register). */
    { static const uint8_t c5[] = { 0x89, 0x0C, 0x9D, 0x78, 0x56, 0x34, 0x12 };
      CHECK("SIB no-base disp32 handled", RUN(c5, &ctx)); }
    /* SIB + disp32 with an immediate after it: the immediate starts after
     * the displacement. mov dword [rax+rcx*1+0x100], 7 --
     * C7 84 08 00 01 00 00 07 00 00 00 */
    { static const uint8_t c6[] = { 0xC7, 0x84, 0x08, 0x00, 0x01, 0x00, 0x00,
                                    0x07, 0x00, 0x00, 0x00 };
      CHECK("SIB+disp32+imm32 handled", RUN(c6, &ctx));
      CHECK_U64("imm after disp32", dev.val, 7); }

    /* ---- refusals ------------------------------------------------------- */

    /* An unknown opcode reported as handled steps over an instruction nobody
     * decoded. neg dword [rax] (F7 /3), adc [rax], ecx (11 08: needs the
     * incoming carry), call [rax] (FF /2), xchg [rax], ecx (87 08),
     * MMX movq mm0, [rax] (0F 6F 00). */
    { static const uint8_t c1[] = { 0xF7, 0x18 };
      static const uint8_t c2[] = { 0x11, 0x08 };
      static const uint8_t c3[] = { 0xFF, 0x10 };
      static const uint8_t c4[] = { 0x87, 0x08 };
      static const uint8_t c5[] = { 0x0F, 0x6F, 0x00 };
      CHECK("neg refused",  RUN(c1, &ctx) == 0);
      CHECK("adc refused",  RUN(c2, &ctx) == 0);
      CHECK("call refused", RUN(c3, &ctx) == 0);
      CHECK("xchg refused", RUN(c4, &ctx) == 0);
      CHECK("MMX refused",  RUN(c5, &ctx) == 0);
      CHECK("refusals touch no device", dev.reads == 0 && dev.writes == 0); }

    if (failures == 0) {
        printf("mmio_decode: ALL PASS\n");
        return 0;
    }
    printf("mmio_decode: %d FAILURE(S)\n", failures);
    return 1;
}
