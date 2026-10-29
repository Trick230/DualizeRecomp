// Host implementations of the Revolution SDK's paired-single matrix routines
// (see rvl_os.c for the contract every native follows).
//
// Each one replays the guest's instruction sequence through the same inline
// paired-single helpers the recompiled code calls, so results, FPSCR and the
// registers left behind are identical; what it saves is the per-instruction
// FPU-available and exception checks and the callee-saved spills. It declines
// unless the FPU is enabled with its exceptions masked, GQR0 is the unquantized
// float format and every operand lies in fast guest memory, so no helper can
// fault part way through.

#include "core/cpu.h"

#include "ppc_natives.h"

#define MTX_SIZE 48u  // Mtx: 3 rows of 4 f32
#define VEC_SIZE 12u  // Vec: 3 f32

// From the natives list: MTXConcat's constant row (0, 0, 0, 1) as a {0, 1}
// pair in .sdata2 (MTX_UNIT_PAIR), and the .sdata2 constants 0.0f, 1.0f, 0.5f
// and 3.0f as offsets off r2 (SDA2_ZERO, SDA2_ONE, SDA2_HALF, SDA2_THREE).

// The FPU is on with its exceptions masked, so no instruction can trap.
static inline bool float_pairs_ready(CPUState* ctx) {
    return (ctx->msr & PPC_MSR_FP) && !(ctx->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)) &&
           (ctx->hid2 & PPC_HID2_LSQE) && (ctx->gqr[0] & 0x00070007u) == 0;
}

static inline bool fast_bytes(CPUState* ctx, u32 address, u32 size, bool written) {
    return written ? ppc_fast_write_ptr(ctx, address, size) != NULL
                   : ppc_fast_ptr(ctx->fast_read, address, size) != NULL;
}

static inline bool fast_matrix(CPUState* ctx, u32 address, bool written) {
    return fast_bytes(ctx, address, MTX_SIZE, written);
}

static inline bool fast_vec(CPUState* ctx, u32 address, bool written) {
    return fast_bytes(ctx, address, VEC_SIZE, written);
}

// The .sdata2 words from r2 + first to r2 + last inclusive.
static inline bool fast_sda2(CPUState* ctx, s32 first, s32 last) {
    return fast_bytes(ctx, ctx->gpr[2] + (u32)first, (u32)(last - first) + 4u, false);
}

// psq_l/psq_st through GQR0: a pair (W=0), or ps0 alone (W=1).
#define LOAD(f, base, offset) ppc_psq_load_inline(ctx, f, (base) + (offset), false, 0, false, 0)
#define STORE(f, base, offset) ppc_psq_store_inline(ctx, f, (base) + (offset), false, 0, false, 0)
#define LOAD1(f, base, offset) ppc_psq_load_inline(ctx, f, (base) + (offset), true, 0, false, 0)
#define STORE1(f, base, offset) ppc_psq_store_inline(ctx, f, (base) + (offset), true, 0, false, 0)

// lfs: both slots take the widened single.
static inline void load_single(CPUState* ctx, u8 f, u32 address) {
    const f64 value = f64_value(convert_to_double(mem_read32_at(ctx, 0, address)));
    ctx->fpr[f] = value;
    ctx->ps1[f] = value;
}

// stfs
static inline void store_single(CPUState* ctx, u8 f, u32 address) {
    mem_write32_at(ctx, 0, address, convert_to_single(f64_bits(ctx->fpr[f])));
}

// ps_merge: the operands are read before d is written.
static inline void merge(CPUState* ctx, u8 d, f64 ps0, f64 ps1) {
    ctx->fpr[d] = ps0;
    ctx->ps1[d] = ps1;
}

// One slot of ps_neg.
static inline f64 negate_single(f64 value) {
    return f64_value(convert_to_double(convert_to_single(f64_bits(value)) ^ 0x80000000u));
}

// fnmsubs d, a, c, b
static inline void fnmsubs(CPUState* ctx, u8 d, u8 a, u8 c, u8 b) {
    f64 result;
    if (ppc_fma_inline(ctx, ctx->fpr[a], ctx->fpr[c], ctx->fpr[b], true, true, true, &result))
        ctx->fpr[d] = ctx->ps1[d] = result;
}

static inline void frsqrte(CPUState* ctx, u8 d, u8 b) {
    f64 result;
    if (ppc_frsqrte(ctx, ctx->fpr[b], &result))
        ctx->fpr[d] = result;
}

// void PSMTXCopy(const Mtx src, Mtx dst)
bool rvl_PSMTXCopy(CPUState* ctx) {
    const u32 src = ctx->gpr[3], dst = ctx->gpr[4];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, src, false) || !fast_matrix(ctx, dst, true))
        return false;
    for (u8 i = 0; i < 6; ++i) {
        LOAD(i, src, 8u * i);
        STORE(i, dst, 8u * i);
    }
    ctx->downcount -= 13;
    return true;
}

// void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
bool rvl_PSMTXConcat(CPUState* ctx) {
    const u32 a = ctx->gpr[3], b = ctx->gpr[4], ab = ctx->gpr[5];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, a, false) || !fast_matrix(ctx, b, false) ||
        !fast_matrix(ctx, ab, true) || !ppc_fast_ptr(ctx->fast_read, MTX_UNIT_PAIR, 8))
        return false;

    // f14, f15 and f31 are spilled and reloaded with lfd, which restores ps0
    // only: their ps1 keeps the last value computed into it.
    const f64 f14 = ctx->fpr[14], f15 = ctx->fpr[15], f31 = ctx->fpr[31];
    ctx->gpr[6] = MTX_UNIT_PAIR;
    LOAD(0, a, 0);
    LOAD(6, b, 0);
    LOAD(7, b, 8);
    LOAD(8, b, 16);
    ppc_ps_muls_inline(ctx, 12, 6, 0, false);
    LOAD(2, a, 16);
    ppc_ps_muls_inline(ctx, 13, 7, 0, false);
    LOAD(31, MTX_UNIT_PAIR, 0);
    ppc_ps_muls_inline(ctx, 14, 6, 2, false);
    LOAD(9, b, 24);
    ppc_ps_muls_inline(ctx, 15, 7, 2, false);
    LOAD(1, a, 8);
    ppc_ps_madds_inline(ctx, 12, 8, 0, 12, true);
    LOAD(3, a, 24);
    ppc_ps_madds_inline(ctx, 14, 8, 2, 14, true);
    LOAD(10, b, 32);
    ppc_ps_madds_inline(ctx, 13, 9, 0, 13, true);
    LOAD(11, b, 40);
    ppc_ps_madds_inline(ctx, 15, 9, 2, 15, true);
    LOAD(4, a, 32);
    LOAD(5, a, 40);
    ppc_ps_madds_inline(ctx, 12, 10, 1, 12, false);
    ppc_ps_madds_inline(ctx, 13, 11, 1, 13, false);
    ppc_ps_madds_inline(ctx, 14, 10, 3, 14, false);
    ppc_ps_madds_inline(ctx, 15, 11, 3, 15, false);
    STORE(12, ab, 0);
    ppc_ps_muls_inline(ctx, 2, 6, 4, false);
    ppc_ps_madds_inline(ctx, 13, 31, 1, 13, true);
    ppc_ps_muls_inline(ctx, 0, 7, 4, false);
    STORE(14, ab, 16);
    ppc_ps_madds_inline(ctx, 15, 31, 3, 15, true);
    STORE(13, ab, 8);
    ppc_ps_madds_inline(ctx, 2, 8, 4, 2, true);
    ppc_ps_madds_inline(ctx, 0, 9, 4, 0, true);
    ppc_ps_madds_inline(ctx, 2, 10, 5, 2, false);
    STORE(15, ab, 24);
    ppc_ps_madds_inline(ctx, 0, 11, 5, 0, false);
    STORE(2, ab, 32);
    ppc_ps_madds_inline(ctx, 0, 31, 5, 0, true);
    STORE(0, ab, 40);
    ctx->fpr[14] = f14;
    ctx->fpr[15] = f15;
    ctx->fpr[31] = f31;
    ctx->downcount -= 51;
    return true;
}

// void PSMTXIdentity(Mtx m)
bool rvl_PSMTXIdentity(CPUState* ctx) {
    const u32 m = ctx->gpr[3];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, m, true) ||
        !fast_sda2(ctx, SDA2_ONE, SDA2_ZERO))
        return false;
    load_single(ctx, 0, ctx->gpr[2] + (u32)SDA2_ZERO);
    load_single(ctx, 1, ctx->gpr[2] + (u32)SDA2_ONE);
    STORE(0, m, 8);
    merge(ctx, 2, ctx->ps1[1], ctx->fpr[0]);
    merge(ctx, 1, ctx->fpr[0], ctx->ps1[1]);
    STORE(0, m, 24);
    STORE(0, m, 32);
    STORE(1, m, 16);
    STORE(2, m, 0);
    STORE(2, m, 40);
    ctx->downcount -= 11;
    return true;
}

// void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst)
bool rvl_PSMTXMultVec(CPUState* ctx) {
    const u32 m = ctx->gpr[3], src = ctx->gpr[4], dst = ctx->gpr[5];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, m, false) || !fast_vec(ctx, src, false) ||
        !fast_vec(ctx, dst, true))
        return false;
    LOAD(0, src, 0);
    LOAD(2, m, 0);
    LOAD1(1, src, 8);
    ppc_ps_mul_inline(ctx, 4, 2, 0);
    LOAD(3, m, 8);
    ppc_ps_madd_inline(ctx, 5, 3, 1, 4, false, false);
    LOAD(8, m, 16);
    ppc_ps_sum_inline(ctx, 6, 5, 6, 5, false);
    LOAD(9, m, 24);
    ppc_ps_mul_inline(ctx, 10, 8, 0);
    STORE1(6, dst, 0);
    ppc_ps_madd_inline(ctx, 11, 9, 1, 10, false, false);
    LOAD(2, m, 32);
    ppc_ps_sum_inline(ctx, 12, 11, 12, 11, false);
    LOAD(3, m, 40);
    ppc_ps_mul_inline(ctx, 4, 2, 0);
    STORE1(12, dst, 4);
    ppc_ps_madd_inline(ctx, 5, 3, 1, 4, false, false);
    ppc_ps_sum_inline(ctx, 6, 5, 6, 5, false);
    STORE1(6, dst, 8);
    ctx->downcount -= 21;
    return true;
}

// void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst)
bool rvl_PSMTXMultVecSR(CPUState* ctx) {
    const u32 m = ctx->gpr[3], src = ctx->gpr[4], dst = ctx->gpr[5];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, m, false) || !fast_vec(ctx, src, false) ||
        !fast_vec(ctx, dst, true))
        return false;
    LOAD(0, m, 0);
    LOAD(6, src, 0);
    LOAD(2, m, 16);
    ppc_ps_mul_inline(ctx, 8, 0, 6);
    LOAD(4, m, 32);
    ppc_ps_mul_inline(ctx, 10, 2, 6);
    LOAD1(7, src, 8);
    ppc_ps_mul_inline(ctx, 12, 4, 6);
    LOAD(3, m, 24);
    ppc_ps_sum_inline(ctx, 8, 8, 8, 8, false);
    LOAD(5, m, 40);
    ppc_ps_sum_inline(ctx, 10, 10, 10, 10, false);
    LOAD(1, m, 8);
    ppc_ps_sum_inline(ctx, 12, 12, 12, 12, false);
    ppc_ps_madd_inline(ctx, 9, 1, 7, 8, false, false);
    STORE1(9, dst, 0);
    ppc_ps_madd_inline(ctx, 11, 3, 7, 10, false, false);
    STORE1(11, dst, 4);
    ppc_ps_madd_inline(ctx, 13, 5, 7, 12, false, false);
    STORE1(13, dst, 8);
    ctx->downcount -= 21;
    return true;
}

// void PSMTXTrans(Mtx m, f32 x, f32 y, f32 z)
bool rvl_PSMTXTrans(CPUState* ctx) {
    const u32 m = ctx->gpr[3];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, m, true) ||
        !fast_sda2(ctx, SDA2_ONE, SDA2_ZERO))
        return false;
    load_single(ctx, 0, ctx->gpr[2] + (u32)SDA2_ZERO);
    load_single(ctx, 4, ctx->gpr[2] + (u32)SDA2_ONE);
    store_single(ctx, 1, m + 12);
    store_single(ctx, 2, m + 28);
    STORE(0, m, 4);
    STORE(0, m, 32);
    store_single(ctx, 0, m + 16);
    store_single(ctx, 4, m + 20);
    store_single(ctx, 0, m + 24);
    store_single(ctx, 4, m + 40);
    store_single(ctx, 3, m + 44);
    store_single(ctx, 4, m + 0);
    ctx->downcount -= 13;
    return true;
}

// void PSMTXTransApply(const Mtx src, Mtx dst, f32 x, f32 y, f32 z)
bool rvl_PSMTXTransApply(CPUState* ctx) {
    const u32 src = ctx->gpr[3], dst = ctx->gpr[4];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, src, false) || !fast_matrix(ctx, dst, true))
        return false;
    LOAD(4, src, 0);
    ppc_frsp_inline(ctx, 1, 1);
    LOAD(5, src, 8);
    ppc_frsp_inline(ctx, 2, 2);
    LOAD(7, src, 24);
    ppc_frsp_inline(ctx, 3, 3);
    LOAD(8, src, 40);
    STORE(4, dst, 0);
    ppc_ps_sum_inline(ctx, 5, 1, 5, 5, true);
    LOAD(6, src, 16);
    STORE(5, dst, 8);
    ppc_ps_sum_inline(ctx, 7, 2, 7, 7, true);
    LOAD(9, src, 32);
    STORE(6, dst, 16);
    ppc_ps_sum_inline(ctx, 8, 3, 8, 8, true);
    STORE(7, dst, 24);
    STORE(9, dst, 32);
    STORE(8, dst, 40);
    ctx->downcount -= 19;
    return true;
}

// void PSMTXScale(Mtx m, f32 x, f32 y, f32 z)
bool rvl_PSMTXScale(CPUState* ctx) {
    const u32 m = ctx->gpr[3];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, m, true) ||
        !fast_sda2(ctx, SDA2_ZERO, SDA2_ZERO))
        return false;
    load_single(ctx, 0, ctx->gpr[2] + (u32)SDA2_ZERO);
    store_single(ctx, 1, m + 0);
    STORE(0, m, 4);
    STORE(0, m, 12);
    store_single(ctx, 2, m + 20);
    STORE(0, m, 24);
    STORE(0, m, 32);
    store_single(ctx, 3, m + 40);
    store_single(ctx, 0, m + 44);
    ctx->downcount -= 10;
    return true;
}

// void PSMTXScaleApply(const Mtx src, Mtx dst, f32 x, f32 y, f32 z)
bool rvl_PSMTXScaleApply(CPUState* ctx) {
    const u32 src = ctx->gpr[3], dst = ctx->gpr[4];
    if (!float_pairs_ready(ctx) || !fast_matrix(ctx, src, false) || !fast_matrix(ctx, dst, true))
        return false;
    ppc_frsp_inline(ctx, 1, 1);
    LOAD(4, src, 0);
    ppc_frsp_inline(ctx, 2, 2);
    LOAD(5, src, 8);
    ppc_frsp_inline(ctx, 3, 3);
    ppc_ps_muls_inline(ctx, 4, 4, 1, false);
    LOAD(6, src, 16);
    ppc_ps_muls_inline(ctx, 5, 5, 1, false);
    LOAD(7, src, 24);
    ppc_ps_muls_inline(ctx, 6, 6, 2, false);
    LOAD(8, src, 32);
    STORE(4, dst, 0);
    ppc_ps_muls_inline(ctx, 7, 7, 2, false);
    LOAD(2, src, 40);
    STORE(5, dst, 8);
    ppc_ps_muls_inline(ctx, 8, 8, 3, false);
    STORE(6, dst, 16);
    ppc_ps_muls_inline(ctx, 2, 2, 3, false);
    STORE(7, dst, 24);
    STORE(8, dst, 32);
    STORE(2, dst, 40);
    ctx->downcount -= 22;
    return true;
}

// void PSVECAdd(const Vec* a, const Vec* b, Vec* ab)
bool rvl_PSVECAdd(CPUState* ctx) {
    const u32 a = ctx->gpr[3], b = ctx->gpr[4], ab = ctx->gpr[5];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, a, false) || !fast_vec(ctx, b, false) ||
        !fast_vec(ctx, ab, true))
        return false;
    LOAD(2, a, 0);
    LOAD(4, b, 0);
    ppc_ps_add_op(ctx, 6, 2, 4);
    STORE(6, ab, 0);
    LOAD1(3, a, 8);
    LOAD1(5, b, 8);
    ppc_ps_add_op(ctx, 7, 3, 5);
    STORE1(7, ab, 8);
    ctx->downcount -= 9;
    return true;
}

// void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b)
bool rvl_PSVECSubtract(CPUState* ctx) {
    const u32 a = ctx->gpr[3], b = ctx->gpr[4], a_b = ctx->gpr[5];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, a, false) || !fast_vec(ctx, b, false) ||
        !fast_vec(ctx, a_b, true))
        return false;
    LOAD(2, a, 0);
    LOAD(4, b, 0);
    ppc_ps_sub_inline(ctx, 6, 2, 4);
    STORE(6, a_b, 0);
    LOAD1(3, a, 8);
    LOAD1(5, b, 8);
    ppc_ps_sub_inline(ctx, 7, 3, 5);
    STORE1(7, a_b, 8);
    ctx->downcount -= 9;
    return true;
}

// void PSVECScale(const Vec* src, Vec* dst, f32 scale)
bool rvl_PSVECScale(CPUState* ctx) {
    const u32 src = ctx->gpr[3], dst = ctx->gpr[4];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, src, false) || !fast_vec(ctx, dst, true))
        return false;
    LOAD(0, src, 0);
    LOAD1(2, src, 8);
    ppc_ps_muls_inline(ctx, 0, 0, 1, false);
    ppc_ps_muls_inline(ctx, 1, 2, 1, false);
    STORE(0, dst, 0);
    STORE1(1, dst, 8);
    ctx->downcount -= 7;
    return true;
}

// void PSVECNormalize(const Vec* src, Vec* unit)
bool rvl_PSVECNormalize(CPUState* ctx) {
    const u32 src = ctx->gpr[3], unit = ctx->gpr[4];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, src, false) || !fast_vec(ctx, unit, true) ||
        !fast_sda2(ctx, SDA2_HALF, SDA2_THREE))
        return false;
    LOAD(2, src, 0);
    LOAD1(3, src, 8);
    ppc_ps_mul_inline(ctx, 5, 2, 2);
    load_single(ctx, 0, ctx->gpr[2] + (u32)SDA2_HALF);
    load_single(ctx, 1, ctx->gpr[2] + (u32)SDA2_THREE);
    ppc_ps_madd_inline(ctx, 4, 3, 3, 5, false, false);
    ppc_ps_sum_inline(ctx, 4, 4, 3, 5, false);
    frsqrte(ctx, 5, 4);
    ppc_fmuls_inline(ctx, 6, 5, 5);
    ppc_fmuls_inline(ctx, 0, 5, 0);
    fnmsubs(ctx, 6, 6, 4, 1);
    ppc_fmuls_inline(ctx, 5, 6, 0);
    ppc_ps_muls_inline(ctx, 2, 2, 5, false);
    ppc_ps_muls_inline(ctx, 3, 3, 5, false);
    STORE(2, unit, 0);
    STORE1(3, unit, 8);
    ctx->downcount -= 17;
    return true;
}

// f32 PSVECMag(const Vec* v)
bool rvl_PSVECMag(CPUState* ctx) {
    const u32 v = ctx->gpr[3];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, v, false) ||
        !fast_sda2(ctx, SDA2_HALF, SDA2_THREE))
        return false;
    LOAD(0, v, 0);
    load_single(ctx, 4, ctx->gpr[2] + (u32)SDA2_HALF);
    ppc_ps_mul_inline(ctx, 0, 0, 0);
    load_single(ctx, 1, v + 8);
    ppc_fsubs_inline(ctx, 2, 4, 4);
    ppc_ps_madd_inline(ctx, 1, 1, 1, 0, false, false);
    ppc_ps_sum_inline(ctx, 1, 1, 0, 0, false);
    ppc_fcmp_inline(ctx, 0, ctx->fpr[1], ctx->fpr[2], false);
    ctx->downcount -= 9;
    if (ctx->cr & 0x20000000u)  // beqlr: a zero vector has magnitude 0
        return true;
    frsqrte(ctx, 0, 1);
    load_single(ctx, 3, ctx->gpr[2] + (u32)SDA2_THREE);
    ppc_fmuls_inline(ctx, 2, 0, 0);
    ppc_fmuls_inline(ctx, 0, 0, 4);
    fnmsubs(ctx, 2, 2, 1, 3);
    ppc_fmuls_inline(ctx, 0, 2, 0);
    ppc_fmuls_inline(ctx, 1, 1, 0);
    ctx->downcount -= 8;
    return true;
}

// f32 PSVECDotProduct(const Vec* a, const Vec* b)
bool rvl_PSVECDotProduct(CPUState* ctx) {
    const u32 a = ctx->gpr[3], b = ctx->gpr[4];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, a, false) || !fast_vec(ctx, b, false))
        return false;
    LOAD(2, a, 4);
    LOAD(3, b, 4);
    ppc_ps_mul_inline(ctx, 2, 2, 3);
    LOAD(5, a, 0);
    LOAD(4, b, 0);
    ppc_ps_madd_inline(ctx, 3, 5, 4, 2, false, false);
    ppc_ps_sum_inline(ctx, 1, 3, 2, 2, false);
    ctx->downcount -= 8;
    return true;
}

// void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb)
bool rvl_PSVECCrossProduct(CPUState* ctx) {
    const u32 a = ctx->gpr[3], b = ctx->gpr[4], axb = ctx->gpr[5];
    if (!float_pairs_ready(ctx) || !fast_vec(ctx, a, false) || !fast_vec(ctx, b, false) ||
        !fast_vec(ctx, axb, true))
        return false;
    LOAD(1, b, 0);
    load_single(ctx, 2, a + 8);
    LOAD(0, a, 0);
    merge(ctx, 6, ctx->ps1[1], ctx->fpr[1]);
    load_single(ctx, 3, b + 8);
    ppc_ps_mul_inline(ctx, 4, 1, 2);
    ppc_ps_muls_inline(ctx, 7, 1, 0, false);
    ppc_ps_madd_inline(ctx, 5, 0, 3, 4, true, false);
    ppc_ps_madd_inline(ctx, 8, 0, 6, 7, true, false);
    merge(ctx, 9, ctx->ps1[5], ctx->ps1[5]);
    merge(ctx, 10, ctx->fpr[5], ctx->ps1[8]);
    STORE1(9, axb, 0);
    ctx->fpr[10] = negate_single(ctx->fpr[10]);
    ctx->ps1[10] = negate_single(ctx->ps1[10]);
    STORE(10, axb, 4);
    ctx->downcount -= 15;
    return true;
}
