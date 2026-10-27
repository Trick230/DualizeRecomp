// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_CPU_H
#define DOLRECOMP_CPU_H

#include "types.h"
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif
// more msvc shit fuck msvc
#if defined(_MSC_VER)
#define GXRUNTIME_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define GXRUNTIME_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define GXRUNTIME_ALWAYS_INLINE inline
#endif

// Minimal CPU support ABI for generated code and CPU tests.
//
// ABI status:
// - Fields up through `ram_size` intentionally mirror DolRecomp's current
//   generated-code CPUState contract.
// - GXRuntime extends that contract only at the tail with `external_pointer`,
//   used as a fast path for host-backed external memory such as locked cache.
// - Do not insert fields into the mirrored prefix without coordinating a
//   generated-code ABI bump with DolRecomp.
// - ABI v2 adds `downcount` at the tail: a guest-cycle charge accumulator.
//   Generated code subtracts each basic block's Gekko cycle cost (mirroring
//   Dolphin's PPCTables costs) at block entry; the embedding environment
//   consumes and resets it (Dolphin chassis: per-dispatch flush into
//   ppc_state.downcount). Hosts that do not meter guest time may ignore it
//   (s64: it cannot wrap in any realistic session).
// - ABI v4 adds `cycle_budget` directly after `downcount`, mirroring the
//   field DolRecomp added to its generated-code CPUState in f0a86be. The
//   prefix is shared with generated code, so the field must sit at the same
//   offset on both sides; hosts that do not meter guest time may ignore it.
// - ABI v5 appends the `fast_read`/`fast_write` RAM slice tables, which the
//   host fills with ppc_build_fast_memory() once `ram`/`exram` are set.
// - ABI v6 appends `cache_range`: one dcbf/dcbst/dcbi over `lines`
//   consecutive 32-byte lines, for natives of the DC*Range SDK loops. NULL
//   means the host has none and callers fall back to cache_control per line.
//   It also appends the host's write-gather pipe (`gather_pipe_ptr`, the
//   host's own write pointer, and `gather_pipe_base`): stores that leave the
//   pipe short of a 32-byte burst append to it directly, and only the store
//   that completes a burst reaches external_write. NULL keeps every gather
//   pipe store on external_write.
// - ABI v7 appends `flat_base`: the guest's 4 GiB effective address space,
//   mapped 1:1 by the host (see GXRUNTIME_FLAT_MEMORY below). Modules built
//   flat need it; others ignore it.
#define GXRUNTIME_CPU_ABI_VERSION 7u
#define GXRUNTIME_CPU_ABI_DOLRECOMP_PREFIX 1u
#define GXRUNTIME_CPU_ABI_EXTERNAL_POINTER_EXTENSION 1u

#define GC_MAIN_RAM_SIZE    (24 * 1024 * 1024)
#define GC_RAM_BASE         0x80000000u
#define GC_RAM_UNCACHED     0xC0000000u

#define PPC_EXC_PROGRAM        0x00000001u
#define PPC_EXC_DSI            0x00000002u
#define PPC_EXC_ALIGNMENT      0x00000004u
#define PPC_EXC_SYSTEM_CALL    0x00000008u
#define PPC_EXC_MACHINE_CHECK  0x00000010u
#define PPC_EXC_FP_UNAVAILABLE 0x00000020u

#define PPC_PROGRAM_FP        0x00100000u
#define PPC_PROGRAM_ILLEGAL   0x00080000u
#define PPC_PROGRAM_PRIV      0x00040000u
#define PPC_PROGRAM_TRAP      0x00020000u

#define PPC_DSI_EAR_DISABLED  0x00100000u

#define PPC_VECTOR_MACHINE_CHECK  0x00200u
#define PPC_VECTOR_DSI            0x00300u
#define PPC_VECTOR_ALIGNMENT      0x00600u
#define PPC_VECTOR_PROGRAM        0x00700u
#define PPC_VECTOR_FP_UNAVAILABLE 0x00800u
#define PPC_VECTOR_SYSTEM_CALL    0x00C00u

#define PPC_HID2_LSQE   0x80000000u
#define PPC_HID2_PSE    0x20000000u
#define PPC_HID2_LCE    0x10000000u
#define PPC_HID2_DCHERR 0x00800000u
#define PPC_HID2_DCHEE  0x00080000u

#define PPC_BIT(n) (1u << (31u - (n)))
#define PPC_MSR_RFI_MASK 0x87C0FFFFu
#define PPC_MSR_POW PPC_BIT(13)
#define PPC_MSR_ILE PPC_BIT(15)
#define PPC_MSR_EE  PPC_BIT(16)
#define PPC_MSR_PR  PPC_BIT(17)
#define PPC_MSR_FP  PPC_BIT(18)
#define PPC_MSR_ME  PPC_BIT(19)
#define PPC_MSR_FE0 PPC_BIT(20)
#define PPC_MSR_SE  PPC_BIT(21)
#define PPC_MSR_BE  PPC_BIT(22)
#define PPC_MSR_FE1 PPC_BIT(23)
#define PPC_MSR_IP  PPC_BIT(25)
#define PPC_MSR_IR  PPC_BIT(26)
#define PPC_MSR_DR  PPC_BIT(27)
#define PPC_MSR_PM  PPC_BIT(29)
#define PPC_MSR_RI  PPC_BIT(30)
#define PPC_MSR_LE  PPC_BIT(31)

#define PPC_EAR_ENABLE 0x80000000u

// LLVM-generated function entries ask the host whether a mod intercepts
// their range (DolRecomp runtime protocol).
#define PPC_HOST_CALL_NATIVE_REGION_QUERY 0xFFFFFFFCu
#define PPC_NATIVE_REGION_QUERY_PENDING 0xFEu
#define PPC_NATIVE_REGION_QUERY_HANDLED 0xFFu
#define PPC_SRR1_MACHINE_CHECK_DCBZL PPC_BIT(10)

typedef struct CPUState CPUState;

typedef u64 (*PPCExternalRead)(CPUState* cpu, u32 ea, u8 size);
typedef void (*PPCExternalWrite)(CPUState* cpu, u32 ea, u64 value, u8 size);
typedef u32 (*PPCExternalRead32)(CPUState* cpu, u32 ea, u8 rid);
typedef void (*PPCExternalWrite32)(CPUState* cpu, u32 ea, u32 value, u8 rid);
typedef void* (*PPCExternalPointer)(CPUState* cpu, u32 ea, u32 size);
typedef void (*PPCInstructionFallback)(CPUState* cpu, u32 raw, u32 cia);
typedef bool (*PPCHostCall)(CPUState* cpu, u32 address);
typedef u32 (*PPCSPRRead)(CPUState* cpu, u16 spr, u32 cia);
typedef void (*PPCSPRWrite)(CPUState* cpu, u16 spr, u32 value, u32 cia);
typedef void (*PPCCacheControl)(CPUState* cpu, u8 operation, u32 ea, u32 cia);
typedef void (*PPCCacheRange)(CPUState* cpu, u8 operation, u32 ea, u32 lines);

enum {
    PPC_CACHE_DCBST,
    PPC_CACHE_DCBF,
    PPC_CACHE_DCBI,
    PPC_CACHE_ICBI,
};

struct CPUState {
    u32 gpr[32];
    f64 fpr[32];
    f64 ps1[32];
    u32 pc;
    u32 lr;
    u32 ctr;
    u32 cr;
    u32 xer;
    u32 fpscr;
    u32 msr;
    u32 srr0;
    u32 srr1;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u64 timebase;
    u32 sr[16];
    u32 gqr[8];
    u32 exception;
    u32 program_exception;
    u32 tlb_last_vps;
    u32 tlb_last_index;
    u32 tlb_invalidate_count;
    u32 external_addr;
    u32 external_value;
    u8 external_rid;
    u8 external_read_count;
    u8 external_write_count;
    u32 reserve_addr;
    bool reserve_valid;
    u32 locked_cache_tag[512];
    bool locked_cache_valid[512];
    PPCExternalRead external_read;
    PPCExternalWrite external_write;
    PPCExternalRead32 external_read32;
    PPCExternalWrite32 external_write32;
    PPCInstructionFallback instruction_fallback;
    PPCHostCall host_call;
    void* external_user_data;

    u8* ram;
    u32 ram_size;
    PPCExternalPointer external_pointer;
    s64 downcount;
    s64 cycle_budget;
    u8* exram;
    u32 exram_size;
    PPCSPRRead spr_read;
    PPCSPRWrite spr_write;
    PPCCacheControl cache_control;

    // Host pointer to each 8 MiB guest slice (indexed by ea >> 23) that lies
    // wholly inside MEM1 or MEM2, cached or uncached mirror. NULL slices
    // (MMIO, partial or absent RAM, and every write slice while stores must
    // be journaled) take the out-of-line path. Empty until the host calls
    // ppc_build_fast_memory().
    u8* fast_read[1u << 9];
    u8* fast_write[1u << 9];
    PPCCacheRange cache_range;
    u8** gather_pipe_ptr;
    u8* gather_pipe_base;
    u8* flat_base;
};

#define PPC_FAST_SLICE_SHIFT 23u
#define PPC_FAST_SLICE_SIZE (1u << PPC_FAST_SLICE_SHIFT)

static inline void ppc_map_fast_slices(CPUState* cpu, u32 ea, u8* host, u32 size, bool writes) {
    for (u32 offset = 0; host && size >= PPC_FAST_SLICE_SIZE && offset <= size - PPC_FAST_SLICE_SIZE;
         offset += PPC_FAST_SLICE_SIZE) {
        for (u32 mirror = 0; mirror < 2u; ++mirror) {
            const u32 slice = ((ea | (mirror ? 0x40000000u : 0u)) + offset) >> PPC_FAST_SLICE_SHIFT;
            cpu->fast_read[slice] = host + offset;
            cpu->fast_write[slice] = writes ? host + offset : NULL;
        }
    }
}

// Fill the slice tables from `ram`/`exram`. Pass writes=false while every
// store has to reach g_mem_write_journal (lockstep verification).
static inline void ppc_build_fast_memory(CPUState* cpu, bool writes) {
    memset(cpu->fast_read, 0, sizeof(cpu->fast_read));
    memset(cpu->fast_write, 0, sizeof(cpu->fast_write));
    ppc_map_fast_slices(cpu, 0x80000000u, cpu->ram, cpu->ram_size, writes);
    ppc_map_fast_slices(cpu, 0x90000000u, cpu->exram, cpu->exram_size, writes);
}

#include <stdio.h>

typedef void (*PPCMemWriteJournal)(u32 offset, u32 size, void* user);
extern PPCMemWriteJournal g_mem_write_journal;
extern void* g_mem_write_journal_user;

static GXRUNTIME_ALWAYS_INLINE u8* get_ram_ptr(CPUState* cpu, u32 addr, u32 size, u32* out_offset) {
    u32 masked_addr = addr & ~0x40000000u;
    
    // Check MEM2 (EXRAM) first as it is much more common in Wii titles
    if (cpu->exram) {
        u32 offset = masked_addr - 0x90000000u;
        if (offset <= cpu->exram_size - size) {
            if (out_offset) *out_offset = (u32)-1;
            return cpu->exram + offset;
        }
    }
    
    // Check MEM1
    u32 offset = masked_addr - 0x80000000u;
    if (offset <= cpu->ram_size - size) {
        if (out_offset) *out_offset = offset;
        return cpu->ram + offset;
    }
    
    return NULL;
}

static GXRUNTIME_ALWAYS_INLINE void clear_matching_reservation(CPUState* cpu, u32 addr) {
    u32 reserve_addr = cpu->reserve_addr & ~0x40000000u;
    u32 store_addr = addr & ~0x40000000u;
    if (cpu->reserve_valid && ((reserve_addr ^ store_addr) & ~31u) == 0)
        cpu->reserve_valid = false;
}

// Out-of-line RAM/MMIO access for everything the slice tables do not cover:
// MMIO, partial slices, slice-crossing accesses, journaled stores and stores
// that may break a lwarx reservation.
u64 ppc_mem_read_slow(CPUState* cpu, u32 addr, u8 size);
void ppc_mem_write_slow(CPUState* cpu, u32 addr, u64 value, u8 size);

static GXRUNTIME_ALWAYS_INLINE u8* ppc_fast_ptr(u8* const* table, u32 addr, u32 size) {
    u8* slice = table[addr >> PPC_FAST_SLICE_SHIFT];
    const u32 offset = addr & (PPC_FAST_SLICE_SIZE - 1u);
    return slice && offset <= PPC_FAST_SLICE_SIZE - size ? slice + offset : NULL;
}

static GXRUNTIME_ALWAYS_INLINE u8* ppc_fast_write_ptr(CPUState* cpu, u32 addr, u32 size) {
    return cpu->reserve_valid ? NULL : ppc_fast_ptr(cpu->fast_write, addr, size);
}

static GXRUNTIME_ALWAYS_INLINE u64 mem_read64(CPUState* cpu, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 8);
    return ptr ? read_be64(ptr) : ppc_mem_read_slow(cpu, addr, 8);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write64(CPUState* cpu, u32 addr, u64 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 8);
    if (ptr)
        write_be64(ptr, value);
    else
        ppc_mem_write_slow(cpu, addr, value, 8);
}

static GXRUNTIME_ALWAYS_INLINE u32 mem_read32(CPUState* cpu, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 4);
    return ptr ? read_be32(ptr) : (u32)ppc_mem_read_slow(cpu, addr, 4);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write32(CPUState* cpu, u32 addr, u32 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 4);
    if (ptr)
        write_be32(ptr, value);
    else
        ppc_mem_write_slow(cpu, addr, value, 4);
}

static GXRUNTIME_ALWAYS_INLINE u16 mem_read16(CPUState* cpu, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 2);
    return ptr ? read_be16(ptr) : (u16)ppc_mem_read_slow(cpu, addr, 2);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write16(CPUState* cpu, u32 addr, u16 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 2);
    if (ptr)
        write_be16(ptr, value);
    else
        ppc_mem_write_slow(cpu, addr, value, 2);
}

static GXRUNTIME_ALWAYS_INLINE u8 mem_read8(CPUState* cpu, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 1);
    return ptr ? *ptr : (u8)ppc_mem_read_slow(cpu, addr, 1);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write8(CPUState* cpu, u32 addr, u8 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 1);
    if (ptr)
        *ptr = value;
    else
        ppc_mem_write_slow(cpu, addr, value, 1);
}

// The same accesses for generated code, which passes the address of the
// instruction (cia) instead of storing ctx->pc ahead of every load and store:
// only the slow path can reach anything that reads the pc (MMIO handlers,
// diagnostics), so only the slow path pays for setting it.
u64 ppc_mem_read_slow_at(CPUState* cpu, u32 cia, u32 addr, u8 size);
void ppc_mem_write_slow_at(CPUState* cpu, u32 cia, u32 addr, u64 value, u8 size);

#if defined(GXRUNTIME_FLAT_MEMORY) && GXRUNTIME_FLAT_MEMORY
// Generated code built flat. The host maps the effective address space 1:1
// at cpu->flat_base: MEM1 and MEM2 at both BAT mirrors and the locked L1
// window, with nothing behind anything else. An access is then one
// byte-swapping load or store; only the hardware window [0xC8000000,
// 0xD0000000) (EFB, MMIO, the gather pipe) goes out of line. Stores leave
// lwarx reservations alone: the SDK's lwarx/stwcx. pairs share a block,
// with no store in between.
#if defined(__GNUC__) || defined(__clang__)
#define PPC_FLAT_IO(addr) __builtin_expect((u32)((addr) - 0xC8000000u) < 0x08000000u, 0)
#else
#define PPC_FLAT_IO(addr) ((u32)((addr) - 0xC8000000u) < 0x08000000u)
#endif
// The base comes from PPC_FLAT_BASE(cpu): generated chunks point it at a
// local loaded once per function, which no store can alias.
#ifndef PPC_FLAT_BASE
#define PPC_FLAT_BASE(cpu) ((cpu)->flat_base)
#endif
static GXRUNTIME_ALWAYS_INLINE u64 ppc_flat_read64(const u8* base, CPUState* cpu, u32 cia, u32 addr) {
    return PPC_FLAT_IO(addr) ? ppc_mem_read_slow_at(cpu, cia, addr, 8) : read_be64(base + addr);
}
static GXRUNTIME_ALWAYS_INLINE u32 ppc_flat_read32(const u8* base, CPUState* cpu, u32 cia, u32 addr) {
    return PPC_FLAT_IO(addr) ? (u32)ppc_mem_read_slow_at(cpu, cia, addr, 4) : read_be32(base + addr);
}
static GXRUNTIME_ALWAYS_INLINE u16 ppc_flat_read16(const u8* base, CPUState* cpu, u32 cia, u32 addr) {
    return PPC_FLAT_IO(addr) ? (u16)ppc_mem_read_slow_at(cpu, cia, addr, 2) : read_be16(base + addr);
}
static GXRUNTIME_ALWAYS_INLINE u8 ppc_flat_read8(const u8* base, CPUState* cpu, u32 cia, u32 addr) {
    return PPC_FLAT_IO(addr) ? (u8)ppc_mem_read_slow_at(cpu, cia, addr, 1) : base[addr];
}
static GXRUNTIME_ALWAYS_INLINE void ppc_flat_write64(u8* base, CPUState* cpu, u32 cia, u32 addr, u64 value) {
    if (PPC_FLAT_IO(addr))
        ppc_mem_write_slow_at(cpu, cia, addr, value, 8);
    else
        write_be64(base + addr, value);
}
static GXRUNTIME_ALWAYS_INLINE void ppc_flat_write32(u8* base, CPUState* cpu, u32 cia, u32 addr, u32 value) {
    if (PPC_FLAT_IO(addr))
        ppc_mem_write_slow_at(cpu, cia, addr, value, 4);
    else
        write_be32(base + addr, value);
}
static GXRUNTIME_ALWAYS_INLINE void ppc_flat_write16(u8* base, CPUState* cpu, u32 cia, u32 addr, u16 value) {
    if (PPC_FLAT_IO(addr))
        ppc_mem_write_slow_at(cpu, cia, addr, value, 2);
    else
        write_be16(base + addr, value);
}
static GXRUNTIME_ALWAYS_INLINE void ppc_flat_write8(u8* base, CPUState* cpu, u32 cia, u32 addr, u8 value) {
    if (PPC_FLAT_IO(addr))
        ppc_mem_write_slow_at(cpu, cia, addr, value, 1);
    else
        base[addr] = value;
}
#define mem_read64_at(cpu, cia, addr) ppc_flat_read64(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr))
#define mem_read32_at(cpu, cia, addr) ppc_flat_read32(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr))
#define mem_read16_at(cpu, cia, addr) ppc_flat_read16(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr))
#define mem_read8_at(cpu, cia, addr) ppc_flat_read8(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr))
#define mem_write64_at(cpu, cia, addr, v) ppc_flat_write64(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr), (v))
#define mem_write32_at(cpu, cia, addr, v) ppc_flat_write32(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr), (v))
#define mem_write16_at(cpu, cia, addr, v) ppc_flat_write16(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr), (v))
#define mem_write8_at(cpu, cia, addr, v) ppc_flat_write8(PPC_FLAT_BASE(cpu), (cpu), (cia), (addr), (v))
#else
static GXRUNTIME_ALWAYS_INLINE u64 mem_read64_at(CPUState* cpu, u32 cia, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 8);
    return ptr ? read_be64(ptr) : ppc_mem_read_slow_at(cpu, cia, addr, 8);
}

static GXRUNTIME_ALWAYS_INLINE u32 mem_read32_at(CPUState* cpu, u32 cia, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 4);
    return ptr ? read_be32(ptr) : (u32)ppc_mem_read_slow_at(cpu, cia, addr, 4);
}

static GXRUNTIME_ALWAYS_INLINE u16 mem_read16_at(CPUState* cpu, u32 cia, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 2);
    return ptr ? read_be16(ptr) : (u16)ppc_mem_read_slow_at(cpu, cia, addr, 2);
}

static GXRUNTIME_ALWAYS_INLINE u8 mem_read8_at(CPUState* cpu, u32 cia, u32 addr) {
    const u8* ptr = ppc_fast_ptr(cpu->fast_read, addr, 1);
    return ptr ? *ptr : (u8)ppc_mem_read_slow_at(cpu, cia, addr, 1);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write64_at(CPUState* cpu, u32 cia, u32 addr, u64 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 8);
    if (ptr)
        write_be64(ptr, value);
    else
        ppc_mem_write_slow_at(cpu, cia, addr, value, 8);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write32_at(CPUState* cpu, u32 cia, u32 addr, u32 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 4);
    if (ptr)
        write_be32(ptr, value);
    else
        ppc_mem_write_slow_at(cpu, cia, addr, value, 4);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write16_at(CPUState* cpu, u32 cia, u32 addr, u16 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 2);
    if (ptr)
        write_be16(ptr, value);
    else
        ppc_mem_write_slow_at(cpu, cia, addr, value, 2);
}

static GXRUNTIME_ALWAYS_INLINE void mem_write8_at(CPUState* cpu, u32 cia, u32 addr, u8 value) {
    u8* ptr = ppc_fast_write_ptr(cpu, addr, 1);
    if (ptr)
        *ptr = value;
    else
        ppc_mem_write_slow_at(cpu, cia, addr, value, 1);
}
#endif

#undef GXRUNTIME_ALWAYS_INLINE

bool cpu_init(CPUState* cpu);
void cpu_free(CPUState* cpu);
void cpu_reset(CPUState* cpu);

f64 ppc_approx_reciprocal(f64 value);
f64 ppc_approx_rsqrt(f64 value);
bool ppc_fres(CPUState* cpu, f64 value, f64* result);
bool ppc_frsqrte(CPUState* cpu, f64 value, f64* result);
void ppc_ps_res(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
void ppc_ps_rsqrte(CPUState* cpu, f64 a, f64 b, f64* result_a, f64* result_b);
bool ppc_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
             bool subtract, bool negative, f64* output);
bool ppc_fctiw(CPUState* cpu, f64 value, bool toward_zero, u64* result);

/* Instruction-shaped FP unit mirroring Dolphin's interpreter bit-exactly
 * (Interpreter_FloatingPoint/Paired + Interpreter_FPUtils NI_* semantics:
 * PPC NaN propagation, Force25Bit frC rounding, single-precision Fill of
 * both PS lanes, FPSCR FPRF/FI/FR/exception updates, VE/ZE write gating).
 * Generated code calls these; the register indices select CPUState lanes. */
void ppc_fadds(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsubs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmuls(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdivs(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fadd(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fsub(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmul(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_fdiv(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_fmadd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                  bool single, bool subtract, bool negative);
void ppc_frsp(CPUState* cpu, u8 d, u8 b);
void ppc_fres_op(CPUState* cpu, u8 d, u8 b);
void ppc_frsqrte_op(CPUState* cpu, u8 d, u8 b);
void ppc_fctiw_op(CPUState* cpu, u8 d, u8 b, bool toward_zero);
void ppc_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered);
void ppc_ps_add_op_slow(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_div_op(CPUState* cpu, u8 d, u8 a, u8 b);
void ppc_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                    bool subtract, bool negative);
void ppc_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void ppc_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c);
void ppc_ps_res_op(CPUState* cpu, u8 d, u8 b);
void ppc_ps_rsqrte_op(CPUState* cpu, u8 d, u8 b);
/* FP loads/stores with Dolphin's alignment exception + lfs/stfs ConvertTo*
 * bit repack. Return false when an exception was taken (callers skip the
 * update-form RA write-back). */
bool ppc_lfs_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
bool ppc_lfd_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
bool ppc_stfs_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
bool ppc_stfd_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
bool ppc_lwarx_op(CPUState* cpu, u8 d, u32 ea, u32 cia);
void ppc_stwcx_op(CPUState* cpu, u8 s, u32 ea, u32 cia);
/* stswi/stswx store-string: mirrors Dolphin's Helper_StoreString word-based
 * read-modify-write of the head/tail partial words. n = byte count, r =
 * first source GPR (wraps). */
void ppc_stsw(CPUState* cpu, u32 ea, u32 n, u8 r, u32 cia);
/* FPSCR control write (mtfsf/mtfsb/mtfsfi/mcrfs): recompute VX/FEX and re-arm
 * the host FPU rounding/flush mode from RN/NI (Dolphin FPSCRUpdated chain). */
void ppc_fpscr_control_updated(CPUState* cpu);
void ppc_mtfsb0_op(CPUState* cpu, u8 bit);
void ppc_mtfsb1_op(CPUState* cpu, u8 bit);
u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia);
void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia);
void ppc_lswx(CPUState* cpu, u8 rD, u8 rA, u8 rB, u32 cia);
void ppc_cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia);

bool ppc_add_overflowed(u32 a, u32 b, u32 result);
bool ppc_trap_condition(u8 to, u32 a, u32 b);
void ppc_set_xer_ov(CPUState* cpu, bool ov);
void ppc_take_exception(CPUState* cpu, u32 exception, u32 vector, u32 srr0, u32 srr1_info);
void ppc_program_exception(CPUState* cpu, u32 cause, u32 cia);

/* Gekko lazy FP: generated FPU instructions call this first. Returns true
 * when MSR[FP] is set; otherwise raises the FP-unavailable exception (srr0 =
 * cia so the instruction retries after the OS restores the FP context) and
 * returns false. ppc_lazy_fp_set_enabled(false) restores the historical
 * execute-regardless behavior for hosts that eagerly restore FP state
 * themselves (StrikersRecomp standalone; see recomp-codegen.md Lazy FPU). */
bool ppc_fp_available(CPUState* cpu, u32 cia);
void ppc_lazy_fp_set_enabled(bool enabled);

/* Same contract, inlined. The emitter puts this in front of every FPU
 * instruction, so on a float-heavy title it is one of the hottest things in the
 * module: a `sample` of a Mario Kart race showed the out-of-line call at ~7% of
 * self time inside StaticRecompCore::Run, second only to the hottest guest loop.
 * Nearly every call takes the fast path -- a running game has MSR[FP] set -- so
 * the work was the call itself, not the test. Only the raise stays out of line.
 *
 * ppc_fp_available() is deliberately kept as a real symbol: the LLVM backend
 * emits calls to it by name (llvm_runtime_lowering.cpp), so it cannot become
 * header-only. */
extern bool g_ppc_lazy_fp_enabled;
bool ppc_fp_raise_unavailable(CPUState* cpu, u32 cia);

static inline bool ppc_fp_available_inline(CPUState* cpu, u32 cia) {
    if (!g_ppc_lazy_fp_enabled || (cpu->msr & PPC_MSR_FP))
        return true;
    return ppc_fp_raise_unavailable(cpu, cia);
}
void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia);
bool ppc_host_call(CPUState* cpu, u32 address);
bool ppc_native_region_available(CPUState* cpu, u32 start, u32 end);
void ppc_system_call_exception(CPUState* cpu, u32 cia);
void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr);
void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia);
u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia);
void ppc_rfi(CPUState* cpu, u32 cia);
void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia);
bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);

#if defined(_MSC_VER)
#define GXRUNTIME_PS_INLINE static __forceinline
#define PPC_FP_UNLIKELY(x) (x)
#else
#define GXRUNTIME_PS_INLINE static inline __attribute__((always_inline))
#define PPC_FP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
/* The inline FP fast paths below take the out-of-line, bit-exact helper only
 * for NaN/infinite/denormal operands; keep that call off the hot layout. */
#define PPC_FP_SLOW(fast_ok) PPC_FP_UNLIKELY(!(fast_ok))

/* Inlined fast path for the unquantised case. A `sample` of a Mario Kart race
 * put psq_load_value and psq_store_value at ~9% of self time inside
 * StaticRecompCore::Run, on top of ppc_psq_load's own frame.
 *
 * Almost all of that is GQR type 0 -- plain IEEE singles, no quantisation and
 * no scale -- which reduces to two 32-bit accesses and a float conversion.
 * Quantised types (4..7), an invalid type, and the LSQE illegal-instruction
 * check all fall through to the out-of-line version, so behaviour is unchanged.
 *
 * The win is bigger than skipping a call: at every generated site w, gqr_index,
 * indexed and cia are literals, so inlining lets the compiler fold the LSQE
 * test and the w branch away per site, which it cannot do across a call. A
 * plain `inline` is not enough for that: inside a 16 KiB chunk function the
 * compiler's budget runs out and keeps them as calls, so they are forced. */
GXRUNTIME_PS_INLINE bool ppc_psq_load_inline(CPUState* cpu, u8 frD, u32 ea, bool w,
                                             u8 gqr_index, bool indexed, u32 cia) {
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if (((gqr >> 16) & 7u) == 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        cpu->fpr[frD] = f64_value(convert_to_double(mem_read32_at(cpu, cia, ea)));
        cpu->ps1[frD] = w ? 1.0 : f64_value(convert_to_double(mem_read32_at(cpu, cia, ea + 4u)));
        return true;
    }
    return ppc_psq_load(cpu, frD, ea, w, gqr_index, indexed, cia);
}

GXRUNTIME_PS_INLINE bool ppc_psq_store_inline(CPUState* cpu, u8 frS, u32 ea, bool w,
                                              u8 gqr_index, bool indexed, u32 cia) {
    const u32 gqr = cpu->gqr[gqr_index & 7u];
    if ((gqr & 7u) == 0u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        mem_write32_at(cpu, cia, ea, convert_to_single_ftz(f64_bits(cpu->fpr[frS])));
        if (!w)
            mem_write32_at(cpu, cia, ea + 4u, convert_to_single_ftz(f64_bits(cpu->ps1[frS])));
        return true;
    }
    if ((gqr & 7u) >= 4u && (indexed || (cpu->hid2 & PPC_HID2_LSQE) != 0u)) {
        /* Quantized u8/u16/s8/s16 stores, exactly as psq_quantize_int in
         * cpu.c: round to f32, scale by 2^scale, clamp, truncate. Kept inline
         * because FMV/texture code issues them per pixel. */
        const u32 type = gqr & 7u;
        const s32 scale = (s32)((gqr >> 8) & 0x3Fu) - (s32)(((gqr >> 8) & 0x20u) << 1);
        union { u32 bits; f32 value; } mul = { (u32)(127 + scale) << 23 };
        const f32 lo = (type & 2u) ? ((type & 1u) ? -32768.0f : -128.0f) : 0.0f;
        const f32 hi = (type & 2u) ? ((type & 1u) ? 32767.0f : 127.0f)
                                   : ((type & 1u) ? 65535.0f : 255.0f);
        const f64 lanes[2] = {cpu->fpr[frS], cpu->ps1[frS]};
        for (int lane = 0; lane < (w ? 1 : 2); ++lane) {
            const f32 conv = (f32)lanes[lane] * mul.value;
            const s32 q = conv != conv ? 0 : conv <= lo ? (s32)lo : conv >= hi ? (s32)hi : (s32)conv;
            if (type & 1u)
                mem_write16_at(cpu, cia, ea + (u32)lane * 2u, (u16)q);
            else
                mem_write8_at(cpu, cia, ea + (u32)lane, (u8)q);
        }
        return true;
    }
    return ppc_psq_store(cpu, frS, ea, w, gqr_index, indexed, cia);
}

/* Fast paths for the arithmetic FPU instructions. The out-of-line versions
 * mirror Dolphin's interpreter bit for bit, and nearly all of their work is
 * NaN, infinity and denormal handling that game math never reaches. With every
 * input finite, and every frC multiplier normal or zero (where Force25Bit is
 * the plain rounding below), none of these instructions can produce a NaN or
 * raise an FPSCR exception, so each reduces to host arithmetic and the FPRF
 * update, computed exactly as the slow path computes them. Any other input
 * takes the slow path. */
#define PPC_F64_EXP_MASK 0x7FF0000000000000ull

GXRUNTIME_PS_INLINE bool ppc_fp_finite(f64 value) {
    return (f64_bits(value) & PPC_F64_EXP_MASK) != PPC_F64_EXP_MASK;
}

/* Finite and not a double denormal: a frC the fast Force25Bit can round. */
GXRUNTIME_PS_INLINE bool ppc_fp_mul_ok(f64 value) {
    const u64 bits = f64_bits(value);
    const u64 exponent = bits & PPC_F64_EXP_MASK;
    return exponent != PPC_F64_EXP_MASK && (exponent != 0 || (bits << 1) == 0);
}

GXRUNTIME_PS_INLINE f64 ppc_force25_fast(f64 value) {
    const u64 bits = f64_bits(value);
    return f64_value((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x8000000ull));
}

/* force_single: with FPSCR[NI], values below the f32 normal range flush to
 * signed zero. */
GXRUNTIME_PS_INLINE f32 ppc_force_single_fast(const CPUState* cpu, f64 value) {
    const u64 bits = f64_bits(value);
    if ((cpu->fpscr & 0x4u) && (bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull)
        return f32_value((u32)((bits & 0x8000000000000000ull) >> 32));
    return (f32)value;
}

GXRUNTIME_PS_INLINE u32 ppc_fprf_f32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, 4);
    const u32 sign = bits >> 31, exponent = bits & 0x7F800000u, fraction = bits & 0x007FFFFFu;
    return exponent == 0x7F800000u ? (fraction ? 0x11u : (sign ? 0x09u : 0x05u))
         : exponent == 0u ? (fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u))
         : (sign ? 0x08u : 0x04u);
}

GXRUNTIME_PS_INLINE u32 ppc_fprf_f64(f64 value) {
    const u64 bits = f64_bits(value);
    const u64 sign = bits >> 63, exponent = bits & PPC_F64_EXP_MASK;
    const u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    return exponent == PPC_F64_EXP_MASK ? (fraction ? 0x11u : (sign ? 0x09u : 0x05u))
         : exponent == 0 ? (fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u))
         : (sign ? 0x08u : 0x04u);
}

GXRUNTIME_PS_INLINE void ppc_set_fprf_fast(CPUState* cpu, u32 fprf) {
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | (fprf << 12);
}

GXRUNTIME_PS_INLINE void ppc_write_single_fast(CPUState* cpu, u8 d, f32 value) {
    cpu->fpr[d] = (f64)value;
    cpu->ps1[d] = (f64)value;
    ppc_set_fprf_fast(cpu, ppc_fprf_f32(value));
}

GXRUNTIME_PS_INLINE void ppc_write_pair_fast(CPUState* cpu, u8 d, f32 ps0, f32 ps1, f32 fprf_of) {
    cpu->fpr[d] = (f64)ps0;
    cpu->ps1[d] = (f64)ps1;
    ppc_set_fprf_fast(cpu, ppc_fprf_f32(fprf_of));
}

/* ni_madd_msub for singles: a * Force25Bit(c) + b with one host rounding,
 * corrected so that the later rounding to f32 does not round twice. */
GXRUNTIME_PS_INLINE f64 ppc_madd_single_fast(f64 a, f64 c, f64 b) {
    const f64 c_round = ppc_force25_fast(c);
    f64 result = fma(a, c_round, b);
    const u64 bits = f64_bits(result);
    if ((bits & 0x000000001FFFFFFFull) == 0x0000000010000000ull) {
        const f64 a_prime = b - result;
        const f64 b_prime = result + a_prime;
        const f64 error = fma(a, c_round, a_prime) + (b - b_prime);
        if (error != 0.0)
            result = f64_value((error > 0.0) == (result > 0.0) ? bits + 1 : bits - 1);
    }
    return result;
}

GXRUNTIME_PS_INLINE void ppc_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b]) &
          ppc_fp_finite(cpu->ps1[a]) & ppc_fp_finite(cpu->ps1[b]))) {
        ppc_ps_add_op_slow(cpu, d, a, b);
        return;
    }
    const f32 ps0 = ppc_force_single_fast(cpu, cpu->fpr[a] + cpu->fpr[b]);
    const f32 ps1 = ppc_force_single_fast(cpu, cpu->ps1[a] + cpu->ps1[b]);
    ppc_write_pair_fast(cpu, d, ps0, ps1, ps0);
}

GXRUNTIME_PS_INLINE void ppc_ps_sub_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b]) &
          ppc_fp_finite(cpu->ps1[a]) & ppc_fp_finite(cpu->ps1[b]))) {
        ppc_ps_sub_op(cpu, d, a, b);
        return;
    }
    const f32 ps0 = ppc_force_single_fast(cpu, cpu->fpr[a] - cpu->fpr[b]);
    const f32 ps1 = ppc_force_single_fast(cpu, cpu->ps1[a] - cpu->ps1[b]);
    ppc_write_pair_fast(cpu, d, ps0, ps1, ps0);
}

GXRUNTIME_PS_INLINE void ppc_ps_mul_inline(CPUState* cpu, u8 d, u8 a, u8 c) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->ps1[a]) &
          ppc_fp_mul_ok(cpu->fpr[c]) & ppc_fp_mul_ok(cpu->ps1[c]))) {
        ppc_ps_mul_op(cpu, d, a, c);
        return;
    }
    const f32 ps0 = ppc_force_single_fast(cpu, cpu->fpr[a] * ppc_force25_fast(cpu->fpr[c]));
    const f32 ps1 = ppc_force_single_fast(cpu, cpu->ps1[a] * ppc_force25_fast(cpu->ps1[c]));
    ppc_write_pair_fast(cpu, d, ps0, ps1, ps0);
}

/* ps_muls0/ps_muls1: both lanes scaled by one lane of frC. */
GXRUNTIME_PS_INLINE void ppc_ps_muls_inline(CPUState* cpu, u8 d, u8 a, u8 c, bool lane1) {
    const f64 scale = lane1 ? cpu->ps1[c] : cpu->fpr[c];
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->ps1[a]) & ppc_fp_mul_ok(scale))) {
        if (lane1)
            ppc_ps_muls1(cpu, d, a, c);
        else
            ppc_ps_muls0(cpu, d, a, c);
        return;
    }
    const f64 c_round = ppc_force25_fast(scale);
    const f32 ps0 = ppc_force_single_fast(cpu, cpu->fpr[a] * c_round);
    const f32 ps1 = ppc_force_single_fast(cpu, cpu->ps1[a] * c_round);
    ppc_write_pair_fast(cpu, d, ps0, ps1, ps0);
}

GXRUNTIME_PS_INLINE void ppc_ps_madd_inline(CPUState* cpu, u8 d, u8 a, u8 c, u8 b,
                                            bool subtract, bool negative) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->ps1[a]) &
          ppc_fp_finite(cpu->fpr[b]) & ppc_fp_finite(cpu->ps1[b]) &
          ppc_fp_mul_ok(cpu->fpr[c]) & ppc_fp_mul_ok(cpu->ps1[c]))) {
        ppc_ps_madd_op(cpu, d, a, c, b, subtract, negative);
        return;
    }
    const f64 b0 = subtract ? -cpu->fpr[b] : cpu->fpr[b];
    const f64 b1 = subtract ? -cpu->ps1[b] : cpu->ps1[b];
    const f32 tmp0 = ppc_force_single_fast(cpu, ppc_madd_single_fast(cpu->fpr[a], cpu->fpr[c], b0));
    const f32 tmp1 = ppc_force_single_fast(cpu, ppc_madd_single_fast(cpu->ps1[a], cpu->ps1[c], b1));
    const f32 ps0 = negative ? -tmp0 : tmp0;
    const f32 ps1 = negative ? -tmp1 : tmp1;
    ppc_write_pair_fast(cpu, d, ps0, ps1, ps0);
}

/* ps_madds0/ps_madds1: both lanes multiplied by one lane of frC. */
GXRUNTIME_PS_INLINE void ppc_ps_madds_inline(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool lane1) {
    const f64 scale = lane1 ? cpu->ps1[c] : cpu->fpr[c];
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->ps1[a]) &
          ppc_fp_finite(cpu->fpr[b]) & ppc_fp_finite(cpu->ps1[b]) & ppc_fp_mul_ok(scale))) {
        if (lane1)
            ppc_ps_madds1(cpu, d, a, c, b);
        else
            ppc_ps_madds0(cpu, d, a, c, b);
        return;
    }
    const f32 ps0 = ppc_force_single_fast(cpu, ppc_madd_single_fast(cpu->fpr[a], scale, cpu->fpr[b]));
    const f32 ps1 = ppc_force_single_fast(cpu, ppc_madd_single_fast(cpu->ps1[a], scale, cpu->ps1[b]));
    ppc_write_pair_fast(cpu, d, ps0, ps1, ps0);
}

/* ps_sum0 puts frA.ps0 + frB.ps1 in lane 0 and copies frC.ps1 to lane 1;
 * ps_sum1 copies frC.ps0 to lane 0 and puts the sum in lane 1. FPRF follows
 * the sum. The copied lane goes through force_single on both paths. */
GXRUNTIME_PS_INLINE void ppc_ps_sum_inline(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool lane1) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->ps1[b]))) {
        if (lane1)
            ppc_ps_sum1(cpu, d, a, c, b);
        else
            ppc_ps_sum0(cpu, d, a, c, b);
        return;
    }
    const f32 sum = ppc_force_single_fast(cpu, cpu->fpr[a] + cpu->ps1[b]);
    const f32 copy = ppc_force_single_fast(cpu, lane1 ? cpu->fpr[c] : cpu->ps1[c]);
    if (lane1)
        ppc_write_pair_fast(cpu, d, copy, sum, sum);
    else
        ppc_write_pair_fast(cpu, d, sum, copy, sum);
}

GXRUNTIME_PS_INLINE void ppc_fadds_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b]))) {
        ppc_fadds(cpu, d, a, b);
        return;
    }
    ppc_write_single_fast(cpu, d, ppc_force_single_fast(cpu, cpu->fpr[a] + cpu->fpr[b]));
}

GXRUNTIME_PS_INLINE void ppc_fsubs_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b]))) {
        ppc_fsubs(cpu, d, a, b);
        return;
    }
    ppc_write_single_fast(cpu, d, ppc_force_single_fast(cpu, cpu->fpr[a] - cpu->fpr[b]));
}

GXRUNTIME_PS_INLINE void ppc_fmuls_inline(CPUState* cpu, u8 d, u8 a, u8 c) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_mul_ok(cpu->fpr[c]))) {
        ppc_fmuls(cpu, d, a, c);
        return;
    }
    ppc_write_single_fast(
        cpu, d, ppc_force_single_fast(cpu, cpu->fpr[a] * ppc_force25_fast(cpu->fpr[c])));
    cpu->fpscr &= ~0x00060000u; /* FI | FR */
}

/* A finite quotient by a finite non-zero divisor cannot be NaN, and only
 * division by zero raises ZX, so this is plain division too. */
GXRUNTIME_PS_INLINE void ppc_fdivs_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b])) || cpu->fpr[b] == 0.0) {
        ppc_fdivs(cpu, d, a, b);
        return;
    }
    ppc_write_single_fast(cpu, d, ppc_force_single_fast(cpu, cpu->fpr[a] / cpu->fpr[b]));
}

GXRUNTIME_PS_INLINE void ppc_write_double_fast(CPUState* cpu, u8 d, f64 value) {
    cpu->fpr[d] = value;
    ppc_set_fprf_fast(cpu, ppc_fprf_f64(value));
}

GXRUNTIME_PS_INLINE void ppc_fadd_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b]))) {
        ppc_fadd(cpu, d, a, b);
        return;
    }
    ppc_write_double_fast(cpu, d, cpu->fpr[a] + cpu->fpr[b]);
}

GXRUNTIME_PS_INLINE void ppc_fsub_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b]))) {
        ppc_fsub(cpu, d, a, b);
        return;
    }
    ppc_write_double_fast(cpu, d, cpu->fpr[a] - cpu->fpr[b]);
}

GXRUNTIME_PS_INLINE void ppc_fmul_inline(CPUState* cpu, u8 d, u8 a, u8 c) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[c]))) {
        ppc_fmul(cpu, d, a, c);
        return;
    }
    ppc_write_double_fast(cpu, d, cpu->fpr[a] * cpu->fpr[c]);
    cpu->fpscr &= ~0x00060000u; /* FI | FR */
}

GXRUNTIME_PS_INLINE void ppc_fdiv_inline(CPUState* cpu, u8 d, u8 a, u8 b) {
    if (PPC_FP_SLOW(ppc_fp_finite(cpu->fpr[a]) & ppc_fp_finite(cpu->fpr[b])) || cpu->fpr[b] == 0.0) {
        ppc_fdiv(cpu, d, a, b);
        return;
    }
    ppc_write_double_fast(cpu, d, cpu->fpr[a] / cpu->fpr[b]);
}

/* fmadd family, same contract as ppc_fma: the result in *output, false when
 * an enabled invalid-operation exception suppresses the write. */
GXRUNTIME_PS_INLINE bool ppc_fma_inline(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
                                        bool subtract, bool negative, f64* output) {
    if (PPC_FP_SLOW(ppc_fp_finite(a) & ppc_fp_finite(b) & (single ? ppc_fp_mul_ok(c) : ppc_fp_finite(c))))
        return ppc_fma(cpu, a, c, b, single, subtract, negative, output);
    const f64 addend = subtract ? -b : b;
    f64 result;
    if (single) {
        result = (f64)(f32)ppc_madd_single_fast(a, c, addend);
        if (negative)
            result = -result;
        ppc_set_fprf_fast(cpu, ppc_fprf_f32((f32)result));
    } else {
        result = fma(a, c, addend);
        if (negative)
            result = -result;
        ppc_set_fprf_fast(cpu, ppc_fprf_f64(result));
    }
    *output = result;
    return true;
}

/* frsp of any non-NaN: round, then FI/FR/XX exactly as ppc_frsp, including the
 * FX/VX/FEX summary bits set_fp_exception maintains. */
GXRUNTIME_PS_INLINE void ppc_frsp_inline(CPUState* cpu, u8 d, u8 b) {
    const f64 value = cpu->fpr[b];
    if (PPC_FP_UNLIKELY(value != value)) {
        ppc_frsp(cpu, d, b);
        return;
    }
    const f32 rounded = ppc_force_single_fast(cpu, value);
    u32 fpscr = cpu->fpscr;
    if (value != (f64)rounded) {
        if ((fpscr & 0x02000000u) == 0)              /* XX newly set */
            fpscr |= 0x80000000u;                    /* FX */
        fpscr |= 0x02000000u | 0x00020000u;          /* XX, FI */
        fpscr = (fpscr & ~0x20000000u) | ((fpscr & 0x01F80700u) ? 0x20000000u : 0u); /* VX */
        fpscr = (fpscr & ~0x40000000u) |
                ((((fpscr >> 22) & fpscr & 0xF8u) != 0) ? 0x40000000u : 0u);       /* FEX */
    } else {
        fpscr &= ~0x00020000u;
    }
    fpscr = (fpscr & ~0x00040000u) | ((fabs((f64)rounded) > fabs(value)) ? 0x00040000u : 0u);
    cpu->fpscr = fpscr;
    ppc_write_single_fast(cpu, d, rounded);
}

GXRUNTIME_PS_INLINE void ppc_fcmp_inline(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) {
    if (PPC_FP_UNLIKELY(a != a || b != b)) {
        ppc_fcmp(cpu, crfd, a, b, ordered);
        return;
    }
    const u32 compare = a < b ? 0x8u : a > b ? 0x4u : 0x2u;
    const u32 shift = 4u * (7u - crfd);
    cpu->fpscr = (cpu->fpscr & ~(0xFu << 12)) | (compare << 12);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (compare << shift);
}
u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia);
void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia);
void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia);
void ppc_fpscr_updated(CPUState* cpu);
void ppc_memory_fence(void);

#ifdef __cplusplus
}
#endif

#endif /* DOLRECOMP_CPU_H */
