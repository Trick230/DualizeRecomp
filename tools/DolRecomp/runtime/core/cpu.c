// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

/* --- Chassis lockstep journal (debug-only, NOT part of the CPU ABI) ---------
 * Optional callback fired just before every committed RAM write. The
 * dolphin-chassis differential ("lockstep") harness installs it to snapshot
 * pre-images, so it can restore a pre-block memory view before re-running a
 * block on Dolphin's interpreter (correct read-modify-write comparison).
 * NULL and zero-cost unless installed; the chassis resolves the setter by name
 * via dlsym, so its absence simply disables lockstep. `offset` is the RAM byte
 * offset (into cpu->ram) about to be written, `size` the width in bytes. */
PPCMemWriteJournal g_mem_write_journal = NULL;
void* g_mem_write_journal_user = NULL;
#if defined(_MSC_VER)
__declspec(dllexport) void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user) {
#else
__attribute__((visibility("default"))) void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user) {
#endif
    g_mem_write_journal = fn;
    g_mem_write_journal_user = user;
}

u64 ppc_mem_read_slow(CPUState* cpu, u32 addr, u8 size) {
    const u8* ptr = get_ram_ptr(cpu, addr, size, NULL);
    if (ptr == NULL)
        return cpu->external_read ? cpu->external_read(cpu, addr, size) : 0;
    switch (size) {
    case 1: return *ptr;
    case 2: return read_be16(ptr);
    case 4: return read_be32(ptr);
    default: return read_be64(ptr);
    }
}

// A store to the write-gather pipe page that leaves the pipe short of a full
// 32-byte burst only appends to the host's buffer.
static bool gather_pipe_append(CPUState* cpu, u32 addr, u64 value, u8 size) {
    if ((addr & 0xFFFFF000u) != 0xCC008000u || cpu->gather_pipe_ptr == NULL)
        return false;
    u8* ptr = *cpu->gather_pipe_ptr;
    if ((size_t)(ptr - cpu->gather_pipe_base) + size >= 32u)
        return false;
    switch (size) {
    case 1: *ptr = (u8)value; break;
    case 2: write_be16(ptr, (u16)value); break;
    case 4: write_be32(ptr, (u32)value); break;
    default: write_be64(ptr, value); break;
    }
    *cpu->gather_pipe_ptr = ptr + size;
    return true;
}

void ppc_mem_write_slow(CPUState* cpu, u32 addr, u64 value, u8 size) {
    if (gather_pipe_append(cpu, addr, value, size))
        return;
    u32 offset;
    u8* ptr = get_ram_ptr(cpu, addr, size, &offset);
    if (ptr == NULL) {
        if (cpu->external_write)
            cpu->external_write(cpu, addr, value, size);
        return;
    }
    clear_matching_reservation(cpu, addr);
    if (g_mem_write_journal && offset != (u32)-1)
        g_mem_write_journal(offset, size, g_mem_write_journal_user);
    switch (size) {
    case 1: *ptr = (u8)value; break;
    case 2: write_be16(ptr, (u16)value); break;
    case 4: write_be32(ptr, (u32)value); break;
    default: write_be64(ptr, value); break;
    }
}

u64 ppc_mem_read_slow_at(CPUState* cpu, u32 cia, u32 addr, u8 size) {
    cpu->pc = cia;
    return ppc_mem_read_slow(cpu, addr, size);
}

void ppc_mem_write_slow_at(CPUState* cpu, u32 cia, u32 addr, u64 value, u8 size) {
    cpu->pc = cia;
    ppc_mem_write_slow(cpu, addr, value, size);
}

bool cpu_init(CPUState* cpu) {
    memset(cpu, 0, sizeof(*cpu));

    cpu->ram_size = GC_MAIN_RAM_SIZE;
    cpu->ram = (u8*)calloc(1, cpu->ram_size);
    if (!cpu->ram) {
        fprintf(stderr, "error: failed to allocate %u bytes for RAM\n", cpu->ram_size);
        return false;
    }

    ppc_build_fast_memory(cpu, g_mem_write_journal == NULL);
    return true;
}

void cpu_free(CPUState* cpu) {
    if (cpu->ram) {
        free(cpu->ram);
        cpu->ram = NULL;
    }
}

void cpu_reset(CPUState* cpu) {
    u8* ram = cpu->ram;
    u32 ram_size = cpu->ram_size;
    PPCExternalRead external_read = cpu->external_read;
    PPCExternalWrite external_write = cpu->external_write;
    PPCExternalRead32 external_read32 = cpu->external_read32;
    PPCExternalWrite32 external_write32 = cpu->external_write32;
    PPCInstructionFallback instruction_fallback = cpu->instruction_fallback;
    PPCHostCall host_call = cpu->host_call;
    PPCExternalPointer external_pointer = cpu->external_pointer;
    void* external_user_data = cpu->external_user_data;
    u8* exram = cpu->exram;
    u32 exram_size = cpu->exram_size;
    PPCSPRRead spr_read = cpu->spr_read;
    PPCSPRWrite spr_write = cpu->spr_write;
    PPCCacheControl cache_control = cpu->cache_control;
    PPCCacheRange cache_range = cpu->cache_range;
    u8** gather_pipe_ptr = cpu->gather_pipe_ptr;
    u8* gather_pipe_base = cpu->gather_pipe_base;
    u8* flat_base = cpu->flat_base;

    memset(cpu, 0, sizeof(*cpu));
    cpu->ram = ram;
    cpu->ram_size = ram_size;
    cpu->external_read = external_read;
    cpu->external_write = external_write;
    cpu->external_read32 = external_read32;
    cpu->external_write32 = external_write32;
    cpu->instruction_fallback = instruction_fallback;
    cpu->host_call = host_call;
    cpu->external_pointer = external_pointer;
    cpu->external_user_data = external_user_data;
    cpu->exram = exram;
    cpu->exram_size = exram_size;
    cpu->spr_read = spr_read;
    cpu->spr_write = spr_write;
    cpu->cache_control = cache_control;
    cpu->cache_range = cache_range;
    cpu->gather_pipe_ptr = gather_pipe_ptr;
    cpu->gather_pipe_base = gather_pipe_base;
    cpu->flat_base = flat_base;

    ppc_build_fast_memory(cpu, g_mem_write_journal == NULL);

    if (cpu->ram)
        memset(cpu->ram, 0, cpu->ram_size);
}

bool ppc_add_overflowed(u32 a, u32 b, u32 result) {
    return (((a ^ result) & (b ^ result)) >> 31) != 0;
}

void ppc_set_xer_ov(CPUState* cpu, bool ov) {
    cpu->xer = (cpu->xer & ~0x40000000u) | (ov ? 0x40000000u : 0u);
    if (ov)
        cpu->xer |= 0x80000000u;
}

bool g_ppc_lazy_fp_enabled = true;

void ppc_lazy_fp_set_enabled(bool enabled) {
    g_ppc_lazy_fp_enabled = enabled;
}

/* Slow path only. The test lives in ppc_fp_available_inline() in the header so
   generated code does not pay a call per FPU instruction to learn that MSR[FP]
   is set, which it almost always is. */
/* Depth of the generated code's native call chain. Cross-chunk direct calls
   turn guest recursion into host recursion, and the chunk headers declare this
   extern so all ~180 chunk translation units share one counter -- as a static
   in the header each would get its own and the guard would bound nothing.
   It lives here because the C backend compiles only chunks/*.c, so generated.c
   is not linked and cannot hold the definition. */
unsigned dolrecomp_call_depth = 0;

/* Cross-chunk direct calls: per-chunk "may enter directly" flags and the
   idle-loop pc, both owned by the host. NULL keeps every call on the host. */
const unsigned char* dolrecomp_direct_ok = NULL;
u32 dolrecomp_idle_pc = 0;
/* The same flags per 2^DOLRECOMP_DIRECT_LINE_SHIFT-byte line of MEM1, for calls
   whose target is known only at run time; a size of 0 keeps them on the host. */
const unsigned char* dolrecomp_direct_lines = NULL;
u32 dolrecomp_direct_lines_size = 0;

bool ppc_fp_raise_unavailable(CPUState* cpu, u32 cia) {
    ppc_take_exception(cpu, PPC_EXC_FP_UNAVAILABLE, PPC_VECTOR_FP_UNAVAILABLE, cia, 0);
    return false;
}

bool ppc_fp_available(CPUState* cpu, u32 cia) {
    return ppc_fp_available_inline(cpu, cia);
}

void ppc_fallback_instruction(CPUState* cpu, u32 raw, u32 cia) {
    if (cpu->instruction_fallback) {
        cpu->instruction_fallback(cpu, raw, cia);
        return;
    }

    (void)raw;
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

bool ppc_host_call(CPUState* cpu, u32 address) {
    return cpu->host_call ? cpu->host_call(cpu, address) : false;
}

bool ppc_native_region_available(CPUState* cpu, u32 start, u32 end) {
    if (!cpu || !cpu->host_call)
        return true;

    u32 saved_addr = cpu->external_addr;
    u32 saved_value = cpu->external_value;
    u8 saved_rid = cpu->external_rid;
    cpu->external_addr = start;
    cpu->external_value = end;
    cpu->external_rid = PPC_NATIVE_REGION_QUERY_PENDING;
    bool blocked = cpu->host_call(cpu, PPC_HOST_CALL_NATIVE_REGION_QUERY);
    bool handled = cpu->external_rid == PPC_NATIVE_REGION_QUERY_HANDLED;
    cpu->external_addr = saved_addr;
    cpu->external_value = saved_value;
    cpu->external_rid = saved_rid;
    return handled && !blocked;
}

void ppc_system_call_exception(CPUState* cpu, u32 cia) {
    ppc_take_exception(cpu, PPC_EXC_SYSTEM_CALL, PPC_VECTOR_SYSTEM_CALL, cia + 4u, 0);
}

void ppc_dsi_exception(CPUState* cpu, u32 ea, u32 cia, u32 dsisr) {
    cpu->dar = ea;
    cpu->dsisr = dsisr;
    ppc_take_exception(cpu, PPC_EXC_DSI, PPC_VECTOR_DSI, cia, 0);
}

void ppc_alignment_exception(CPUState* cpu, u32 ea, u32 cia) {
    cpu->dar = ea;
    ppc_take_exception(cpu, PPC_EXC_ALIGNMENT, PPC_VECTOR_ALIGNMENT, cia, 0);
}

u32 ppc_mftb(CPUState* cpu, u16 tbr, u32 cia) {
    if (tbr == 268)
        return (u32)cpu->timebase;
    if (tbr == 269)
        return (u32)(cpu->timebase >> 32);

    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
}
static s32 gqr_scale(u32 value) {
    return sign_extend(value & 0x3Fu, 6);
}


static u32 psq_type_size(u8 type) {
    switch (type) {
    case 0: return 4;
    case 4:
    case 6: return 1;
    case 5:
    case 7: return 2;
    default: return 0;
    }
}

/* Quantized load/store semantics mirror Dolphin's interpreter (the chassis
 * lockstep oracle) exactly:
 *  - psq_l/psq_st (non-indexed) require only HID2.LSQE; the indexed forms
 *    are never gated. PSE is not checked, and no alignment exceptions are
 *    raised (unaligned accesses go straight to memory).
 *  - Invalid GQR types 1-3 load 0.0 into both lanes and store nothing.
 *  - Dequantization: f32(int) * f32 power-of-two, rounded once to f32.
 *  - Quantization: round the lane to f32 first, multiply by the f32
 *    power-of-two scale, clamp in f32, truncate. NaN quantizes to 0
 *    (matching SType(NaN-after-clamp) in release Dolphin on arm64). */
/* GQR scales are 6-bit signed (-32..31), so 2^scale is always a normal
 * float/double and can be built straight from its exponent bits. Multiplying
 * by it is exact and identical to ldexp, which libm implements out of line
 * and which dominated paired-single heavy code (e.g. FMV decoders). */
static f32 psq_pow2f(s32 exponent) {
    union { u32 bits; f32 value; } pow2 = { (u32)(127 + exponent) << 23 };
    return pow2.value;
}

static f64 psq_pow2(s32 exponent) {
    union { u64 bits; f64 value; } pow2 = { (u64)(1023 + exponent) << 52 };
    return pow2.value;
}

static f64 psq_dequant(f64 value, s32 scale) {
    if (scale == 0)
        return (f64)(f32)value;
    return (f64)(f32)(value * psq_pow2(-scale));
}

static f64 psq_load_value(CPUState* cpu, u32 ea, u8 type, s32 scale) {
    switch (type) {
    case 0:
        return f64_value(convert_to_double(mem_read32(cpu, ea)));
    case 4:
        return psq_dequant((f64)mem_read8(cpu, ea), scale);
    case 5:
        return psq_dequant((f64)mem_read16(cpu, ea), scale);
    case 6:
        return psq_dequant((f64)(s8)mem_read8(cpu, ea), scale);
    case 7:
        return psq_dequant((f64)(s16)mem_read16(cpu, ea), scale);
    default:
        return 0.0;
    }
}

static s64 psq_quantize_int(f64 value, s64 min_value, s64 max_value, s32 scale) {
    f32 conv = (f32)value * psq_pow2f(scale);
    if (isnan(conv))
        return 0;
    if (conv <= (f32)min_value)
        return min_value;
    if (conv >= (f32)max_value)
        return max_value;
    return (s64)conv;
}

static void psq_store_value(CPUState* cpu, u32 ea, u8 type, s32 scale, f64 value) {
    switch (type) {
    case 0:
        mem_write32(cpu, ea, convert_to_single_ftz(f64_bits(value)));
        break;
    case 4:
        mem_write8(cpu, ea, (u8)psq_quantize_int(value, 0, 255, scale));
        break;
    case 5:
        mem_write16(cpu, ea, (u16)psq_quantize_int(value, 0, 65535, scale));
        break;
    case 6:
        mem_write8(cpu, ea, (u8)(s8)psq_quantize_int(value, -128, 127, scale));
        break;
    case 7:
        mem_write16(cpu, ea, (u16)(s16)psq_quantize_int(value, -32768, 32767, scale));
        break;
    }
}

static bool psq_check_enabled(CPUState* cpu, bool indexed, u32 cia) {
    if (!indexed && (cpu->hid2 & PPC_HID2_LSQE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return false;
    }
    return true;
}

bool ppc_psq_load(CPUState* cpu, u8 frD, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    u32 gqr = cpu->gqr[gqr_index & 7u];
    s32 scale = gqr_scale(gqr >> 24);
    u8 type = (u8)((gqr >> 16) & 7u);
    u32 size = psq_type_size(type);
    if (size == 0) { /* invalid GQR type: both lanes read as 0.0 */
        cpu->fpr[frD] = 0.0;
        cpu->ps1[frD] = 0.0;
        return true;
    }

    cpu->fpr[frD] = psq_load_value(cpu, ea, type, scale);
    cpu->ps1[frD] = w ? 1.0 : psq_load_value(cpu, ea + size, type, scale);
    return true;
}

bool ppc_psq_store(CPUState* cpu, u8 frS, u32 ea, bool w, u8 gqr_index, bool indexed, u32 cia) {
    if (!psq_check_enabled(cpu, indexed, cia))
        return false;

    u32 gqr = cpu->gqr[gqr_index & 7u];
    s32 scale = gqr_scale(gqr >> 8);
    u8 type = (u8)(gqr & 7u);
    u32 size = psq_type_size(type);
    if (size == 0) /* invalid GQR type: nothing is stored */
        return true;

    psq_store_value(cpu, ea, type, scale, cpu->fpr[frS]);
    if (!w)
        psq_store_value(cpu, ea + size, type, scale, cpu->ps1[frS]);
    return true;
}

void ppc_rfi(CPUState* cpu, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->msr = (cpu->msr & ~PPC_MSR_RFI_MASK) | (cpu->srr1 & PPC_MSR_RFI_MASK);
    cpu->msr &= ~PPC_MSR_POW;
    cpu->pc = cpu->srr0 & ~3u;
}

void ppc_dcbz_l(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    if ((cpu->hid2 & PPC_HID2_LCE) == 0) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return;
    }

    u32 block = ea & ~31u;
    u32 slot = (block >> 5) & 511u;
    bool hit = cpu->locked_cache_valid[slot] && cpu->locked_cache_tag[slot] == block;
    bool first_hit_error = hit && (cpu->hid2 & PPC_HID2_DCHERR) == 0;

    if (hit) {
        cpu->hid2 |= PPC_HID2_DCHERR;
        if (first_hit_error && (cpu->hid2 & PPC_HID2_DCHEE) &&
            (cpu->msr & PPC_MSR_EE) && (cpu->msr & PPC_MSR_ME)) {
            ppc_take_exception(cpu, PPC_EXC_MACHINE_CHECK, PPC_VECTOR_MACHINE_CHECK,
                               cia, PPC_SRR1_MACHINE_CHECK_DCBZL);
        }
    } else {
        cpu->locked_cache_valid[slot] = true;
        cpu->locked_cache_tag[slot] = block;
    }

    for (u32 i = 0; i < 32; i += 4)
        mem_write32(cpu, block + i, 0);
}

u32 ppc_eciwx(CPUState* cpu, u32 ea, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return 0;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return 0;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_rid = rid;
    cpu->external_read_count++;
    if (cpu->external_read32)
        return cpu->external_read32(cpu, ea, rid);
    return 0;
}

void ppc_ecowx(CPUState* cpu, u32 ea, u32 value, u32 cia) {
    if ((cpu->ear & PPC_EAR_ENABLE) == 0) {
        ppc_dsi_exception(cpu, ea, cia, PPC_DSI_EAR_DISABLED);
        return;
    }

    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }

    u8 rid = (u8)(cpu->ear & 0xFu);
    cpu->external_addr = ea;
    cpu->external_value = value;
    cpu->external_rid = rid;
    cpu->external_write_count++;
    if (cpu->external_write32)
        cpu->external_write32(cpu, ea, value, rid);
}

void ppc_tlbie(CPUState* cpu, u32 ea, u32 cia) {
    if (cpu->msr & PPC_MSR_PR) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    cpu->tlb_last_vps = (ea >> 12) & 0xFFFFu;
    cpu->tlb_last_index = (ea >> 12) & 0x3Fu;
    cpu->tlb_invalidate_count++;
}

bool ppc_trap_condition(u8 to, u32 a, u32 b) {
    s32 sa = (s32)a;
    s32 sb = (s32)b;

    return ((sa < sb) && (to & 0x10u)) ||
           ((sa > sb) && (to & 0x08u)) ||
           ((sa == sb) && (to & 0x04u)) ||
           ((a < b) && (to & 0x02u)) ||
           ((a > b) && (to & 0x01u));
}
