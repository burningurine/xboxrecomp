/*
 * mmio_trap.h - device-register emulation for the recompiled guest on POSIX hosts.
 *
 * The recompiled code reaches guest memory with ordinary native loads and
 * stores (MEM32(a) = *(uint32_t *)(mem_base + a)), so a device's registers can
 * only be emulated by making their host pages inaccessible and completing the
 * faulting instruction in the SIGSEGV handler -- the POSIX counterpart of the
 * Windows VEH hooks (apu_mmio_hook.c / nv2a_mmio_hook.c).
 *
 * mmio_trap_handle() decodes the faulting AArch64 load/store (LDR/STR in all
 * sizes and addressing forms, sign-extending loads, LDP/STP of general
 * registers) or, on an x86-64 host such as the Android emulator, the faulting
 * x86-64 instruction (mmio_decode.h: moves, extending loads, ALU and compare
 * forms, SSE moves), performs it against the registered device, writes the
 * result register and advances the PC. Anything else is left unhandled (the
 * caller's normal crash path then reports it).
 */
#ifndef MMIO_TRAP_H
#define MMIO_TRAP_H

#include <stdint.h>

typedef uint64_t (*mmio_read_fn)(void *opaque, uint32_t offset, unsigned size);
typedef void     (*mmio_write_fn)(void *opaque, uint32_t offset, uint64_t value, unsigned size);

/* Trap [guest_base, guest_base + size) (page-aligned) and route accesses to the
 * callbacks with an offset relative to guest_base. Returns 0 on success. */
int mmio_trap_register(uint32_t guest_base, uint32_t size,
                       mmio_read_fn read, mmio_write_fn write, void *opaque);

/* From the SIGSEGV/SIGBUS handler: 1 if the fault was a trapped device access
 * and has been completed (return from the handler to resume), else 0. */
int mmio_trap_handle(void *fault_addr, void *ucontext);

/* Counters for diagnostics. */
void mmio_trap_stats(uint64_t *reads, uint64_t *writes, uint64_t *unhandled);

#endif /* MMIO_TRAP_H */
