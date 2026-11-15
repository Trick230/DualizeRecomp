// Host implementations of nw4r::math routines (see rvl_os.c for the contract
// every native follows).
//
// Like rvl_mtx.c, each one replays the guest's instruction sequence, callees
// and stack traffic included, through the helpers the recompiled code uses, so
// memory, registers and FPSCR come out identical. Each declines unless the FPU
// is on with its exceptions masked, quantized loads and stores are enabled and
// every operand lies in plain guest memory, and when it would run out of
// cycles inside (the guest's returns and loops are safepoints).

#include "core/cpu.h"

#include "ppc_natives.h"
#include "sdk/guest_memory.h"

// From the natives list: SinCosFIdx's table and its constants 65536.0f and
// 0.0f, and MTX34RotXYZFIdx's 0.0f, all absolute.

static inline bool fpu_ready(CPUState* ctx) {
    return (ctx->msr & PPC_MSR_FP) && !(ctx->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)) &&
           (ctx->hid2 & PPC_HID2_LSQE) && (ctx->gqr[0] & 0x00070007u) == 0;
}

static inline bool plain(CPUState* ctx, u32 address, u32 size) {
    return guest_span(ctx, address, size) != NULL;
}

#define LOAD(f, base, offset) ppc_psq_load_inline(ctx, f, (base) + (offset), false, 0, false, 0)
#define STORE(f, base, offset) ppc_psq_store_inline(ctx, f, (base) + (offset), false, 0, false, 0)
#define STORE1(f, base, offset) ppc_psq_store_inline(ctx, f, (base) + (offset), true, 0, false, 0)

// What lis loads for an address whose low half goes with a signed offset.
static inline u32 high_half(u32 address) {
    return (address + 0x8000u) & 0xFFFF0000u;
}

static inline void lfs(CPUState* ctx, u8 f, u32 address) {
    const f64 value = f64_value(convert_to_double(mem_read32_at(ctx, 0, address)));
    ctx->fpr[f] = value;
    ctx->ps1[f] = value;
}

static inline void stfs(CPUState* ctx, u8 f, u32 address) {
    mem_write32_at(ctx, 0, address, convert_to_single(f64_bits(ctx->fpr[f])));
}

static inline void fneg(CPUState* ctx, u8 d, u8 b) {
    ctx->fpr[d] = f64_value(f64_bits(ctx->fpr[b]) ^ 0x8000000000000000ull);
}

// stfd fN then psq_st fN, and back with psq_l then lfd.
static inline void spill(CPUState* ctx, u8 f, u32 address) {
    mem_write64(ctx, address, f64_bits(ctx->fpr[f]));
    STORE(f, address, 8);
}

static inline void reload(CPUState* ctx, u8 f, u32 address) {
    LOAD(f, address, 8);
    ctx->fpr[f] = f64_value(mem_read64(ctx, address));
}

// SinCosFIdx's cycles when its range loop does not run.
#define SINCOS_CYCLES 75

// Whether SinCosFIdx(f1) can run natively with r1 = sp: its loop would not
// run and every byte it touches is plain.
static bool sincos_ok(CPUState* ctx, u32 sp, u32 sin_out, u32 cos_out) {
    const f64 index = f64_value(f64_bits(ctx->fpr[1]) & 0x7FFFFFFFFFFFFFFFull);
    const f64 range = f64_value(convert_to_double(mem_read32_at(ctx, 0, (u32)SINCOS_RANGE)));
    return index < range && plain(ctx, sp - 0x50u, 0x58u) && plain(ctx, sin_out, 4) &&
           plain(ctx, cos_out, 4);
}

// void SinCosFIdx(f32* s, f32* c, f32 fidx), entered with lr = `ret`.
static void run_sincos(CPUState* ctx, u32 ret) {
    ctx->lr = ret;
    const u32 sp = ctx->gpr[1], frame = sp - 0x40u;
    mem_write32_at(ctx, 0, frame, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[0] = ctx->lr;
    mem_write32_at(ctx, 0, frame + 0x44u, ctx->gpr[0]);
    spill(ctx, 31, frame + 0x30u);
    spill(ctx, 30, frame + 0x20u);
    ctx->gpr[11] = frame + 0x20u;
    // _savegpr_29
    ctx->lr = (u32)SINCOS_BASE + 0x24u;
    mem_write32_at(ctx, 0, ctx->gpr[11] - 12u, ctx->gpr[29]);
    mem_write32_at(ctx, 0, ctx->gpr[11] - 8u, ctx->gpr[30]);
    mem_write32_at(ctx, 0, ctx->gpr[11] - 4u, ctx->gpr[31]);
    ctx->fpr[30] = ctx->fpr[1];
    ctx->gpr[29] = ctx->gpr[3];
    ctx->gpr[30] = ctx->gpr[4];
    // fabsf
    ctx->lr = (u32)SINCOS_BASE + 0x34u;
    ctx->fpr[1] = f64_value(f64_bits(ctx->fpr[1]) & 0x7FFFFFFFFFFFFFFFull);
    ctx->gpr[3] = high_half((u32)SINCOS_RANGE);
    ctx->fpr[31] = ctx->fpr[1];
    lfs(ctx, 0, (u32)SINCOS_RANGE);
    // fcmpo cr0, f31, f0; cror eq, gt, eq; the loop is known not to run.
    ppc_fcmp_inline(ctx, 0, ctx->fpr[31], ctx->fpr[0], true);
    {
        const u32 gt = (ctx->cr >> 30) & 1u, eq = (ctx->cr >> 29) & 1u;
        ctx->cr = (ctx->cr & ~0x20000000u) | ((gt | eq) ? 0x20000000u : 0u);
    }
    ctx->fpr[1] = ctx->fpr[31];
    // u16 conversion through GQR3
    ctx->lr = (u32)SINCOS_BASE + 0x5Cu;
    mem_write32_at(ctx, 0, frame - 0x10u, frame);
    ppc_psq_store_inline(ctx, 1, frame - 8u, true, 3, false, 0);
    ctx->gpr[3] = mem_read16_at(ctx, 0, frame - 8u);
    ctx->gpr[31] = ctx->gpr[3];
    ctx->gpr[3] &= 0xFFFFu;
    // and back to float through GQR3
    ctx->lr = (u32)SINCOS_BASE + 0x68u;
    mem_write32_at(ctx, 0, frame - 0x10u, frame);
    mem_write16_at(ctx, 0, frame - 8u, (u16)ctx->gpr[3]);
    ppc_psq_load_inline(ctx, 1, frame - 8u, true, 3, false, 0);
    const u32 table = (u32)SINCOS_TABLE;
    ctx->gpr[4] = table;
    ctx->gpr[0] = (ctx->gpr[31] << 4) & 0xFF0u;
    lfs(ctx, 0, (u32)SINCOS_ZERO);
    ctx->gpr[3] = high_half((u32)SINCOS_ZERO);
    ppc_fsubs_inline(ctx, 4, 31, 1);
    ctx->gpr[3] = table + ctx->gpr[0];
    ppc_fcmp_inline(ctx, 0, ctx->fpr[30], ctx->fpr[0], true);
    lfs(ctx, 3, ctx->gpr[3] + 8u);
    lfs(ctx, 1, ctx->gpr[3] + 12u);
    ppc_fmuls_inline(ctx, 3, 4, 3);
    lfs(ctx, 2, table + ctx->gpr[0]);
    ppc_fmuls_inline(ctx, 1, 4, 1);
    lfs(ctx, 0, ctx->gpr[3] + 4u);
    ppc_fadds_inline(ctx, 2, 2, 3);
    ppc_fadds_inline(ctx, 0, 0, 1);
    s32 cycles = SINCOS_CYCLES;
    if (ctx->cr & 0x80000000u) {
        fneg(ctx, 2, 2);
        ++cycles;
    }
    stfs(ctx, 2, ctx->gpr[29]);
    stfs(ctx, 0, ctx->gpr[30]);
    reload(ctx, 31, frame + 0x30u);
    reload(ctx, 30, frame + 0x20u);
    ctx->gpr[11] = frame + 0x20u;
    // _restgpr_29
    ctx->lr = (u32)SINCOS_BASE + 0xD0u;
    ctx->gpr[29] = mem_read32_at(ctx, 0, ctx->gpr[11] - 12u);
    ctx->gpr[30] = mem_read32_at(ctx, 0, ctx->gpr[11] - 8u);
    ctx->gpr[31] = mem_read32_at(ctx, 0, ctx->gpr[11] - 4u);
    ctx->gpr[0] = mem_read32_at(ctx, 0, frame + 0x44u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] = sp;
    ctx->downcount -= cycles;
}

// void SinCosFIdx(f32* s, f32* c, f32 fidx)
bool nw4r_SinCosFIdx(CPUState* ctx) {
    if (!fpu_ready(ctx) || ctx->downcount <= SINCOS_CYCLES + 1 ||
        !sincos_ok(ctx, ctx->gpr[1], ctx->gpr[3], ctx->gpr[4]))
        return false;
    const u32 lr = ctx->lr;
    run_sincos(ctx, lr);
    return true;
}

// MTX34* MTX34RotXYZFIdx(MTX34* out, f32 fx, f32 fy, f32 fz)
bool nw4r_MTX34RotXYZFIdx(CPUState* ctx) {
    const u32 sp = ctx->gpr[1], frame = sp - 0x50u, out = ctx->gpr[3];
    if (!fpu_ready(ctx) || ctx->downcount <= 0x55 + 3 * (SINCOS_CYCLES + 1) ||
        !plain(ctx, frame - 0x50u, 0xA8u) || !plain(ctx, out, 48))
        return false;
    // Every angle must keep SinCosFIdx's loop from running.
    const f64 range = f64_value(convert_to_double(mem_read32_at(ctx, 0, (u32)SINCOS_RANGE)));
    for (u8 f = 1; f <= 3; ++f)
        if (!(f64_value(f64_bits(ctx->fpr[f]) & 0x7FFFFFFFFFFFFFFFull) < range))
            return false;

    mem_write32_at(ctx, 0, frame, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[0] = ctx->lr;
    mem_write32_at(ctx, 0, frame + 0x54u, ctx->gpr[0]);
    spill(ctx, 31, frame + 0x40u);
    spill(ctx, 30, frame + 0x30u);
    ctx->fpr[30] = ctx->fpr[2];
    ctx->fpr[31] = ctx->fpr[3];
    ctx->gpr[4] = frame + 0x18u;
    mem_write32_at(ctx, 0, frame + 0x2Cu, ctx->gpr[31]);
    ctx->gpr[31] = out;
    ctx->gpr[3] = frame + 0x1Cu;
    run_sincos(ctx, (u32)ROTXYZ_BASE + 0x38u);
    ctx->fpr[1] = ctx->fpr[30];
    ctx->gpr[3] = frame + 0x14u;
    ctx->gpr[4] = frame + 0x10u;
    run_sincos(ctx, (u32)ROTXYZ_BASE + 0x48u);
    ctx->fpr[1] = ctx->fpr[31];
    ctx->gpr[3] = frame + 0x0Cu;
    ctx->gpr[4] = frame + 0x08u;
    run_sincos(ctx, (u32)ROTXYZ_BASE + 0x58u);

    const u32 o = out, f = frame;
    lfs(ctx, 1, f + 0x14u);
    ctx->gpr[3] = high_half((u32)ROTXYZ_ZERO);
    lfs(ctx, 0, (u32)ROTXYZ_ZERO);
    ctx->gpr[3] = o;
    fneg(ctx, 1, 1);
    stfs(ctx, 1, o + 0x20u);
    lfs(ctx, 2, f + 0x08u);
    lfs(ctx, 1, f + 0x10u);
    ppc_fmuls_inline(ctx, 1, 2, 1);
    stfs(ctx, 1, o + 0x00u);
    lfs(ctx, 2, f + 0x0Cu);
    lfs(ctx, 1, f + 0x10u);
    ppc_fmuls_inline(ctx, 1, 2, 1);
    stfs(ctx, 1, o + 0x10u);
    lfs(ctx, 2, f + 0x10u);
    lfs(ctx, 1, f + 0x1Cu);
    ppc_fmuls_inline(ctx, 1, 2, 1);
    stfs(ctx, 1, o + 0x24u);
    lfs(ctx, 2, f + 0x10u);
    lfs(ctx, 1, f + 0x18u);
    ppc_fmuls_inline(ctx, 1, 2, 1);
    stfs(ctx, 1, o + 0x28u);
    lfs(ctx, 2, f + 0x1Cu);
    lfs(ctx, 1, f + 0x08u);
    lfs(ctx, 4, f + 0x18u);
    ppc_fmuls_inline(ctx, 5, 2, 1);
    lfs(ctx, 3, f + 0x0Cu);
    lfs(ctx, 1, f + 0x14u);
    ppc_fmuls_inline(ctx, 2, 4, 3);
    ppc_fmuls_inline(ctx, 1, 5, 1);
    ppc_fsubs_inline(ctx, 1, 1, 2);
    stfs(ctx, 1, o + 0x04u);
    lfs(ctx, 1, f + 0x14u);
    ppc_fmuls_inline(ctx, 1, 2, 1);
    ppc_fsubs_inline(ctx, 1, 1, 5);
    stfs(ctx, 1, o + 0x18u);
    lfs(ctx, 2, f + 0x18u);
    lfs(ctx, 1, f + 0x08u);
    lfs(ctx, 4, f + 0x1Cu);
    ppc_fmuls_inline(ctx, 5, 2, 1);
    lfs(ctx, 3, f + 0x0Cu);
    lfs(ctx, 1, f + 0x14u);
    ppc_fmuls_inline(ctx, 2, 4, 3);
    ppc_fmuls_inline(ctx, 1, 5, 1);
    ppc_fadds_inline(ctx, 1, 2, 1);
    stfs(ctx, 1, o + 0x08u);
    lfs(ctx, 1, f + 0x14u);
    ppc_fmuls_inline(ctx, 1, 2, 1);
    stfs(ctx, 0, o + 0x0Cu);
    stfs(ctx, 0, o + 0x1Cu);
    ppc_fadds_inline(ctx, 1, 5, 1);
    stfs(ctx, 0, o + 0x2Cu);
    stfs(ctx, 1, o + 0x14u);
    reload(ctx, 31, frame + 0x40u);
    reload(ctx, 30, frame + 0x30u);
    ctx->gpr[31] = mem_read32_at(ctx, 0, frame + 0x2Cu);
    ctx->gpr[0] = mem_read32_at(ctx, 0, frame + 0x54u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] = sp;
    ctx->downcount -= 0x55;
    return true;
}

// void MTX34ToMTX33(MTX33* out, const MTX34* m)
bool nw4r_MTX34ToMTX33(CPUState* ctx) {
    const u32 out = ctx->gpr[3], m = ctx->gpr[4];
    if (!fpu_ready(ctx) || !plain(ctx, m, 48) || !plain(ctx, out, 36))
        return false;
    for (u8 i = 0; i < 6; ++i)
        LOAD(i, m, 8u * i);
    STORE(0, out, 0x00);
    STORE1(1, out, 0x08);
    STORE(2, out, 0x0C);
    STORE1(3, out, 0x14);
    STORE(4, out, 0x18);
    STORE1(5, out, 0x20);
    ctx->downcount -= 13;
    return true;
}

// void MTX44Copy(MTX44* out, const MTX44* m)
bool nw4r_MTX44Copy(CPUState* ctx) {
    const u32 out = ctx->gpr[3], m = ctx->gpr[4];
    if (!fpu_ready(ctx) || !plain(ctx, m, 64) || !plain(ctx, out, 64))
        return false;
    for (u8 i = 0; i < 8; ++i)
        LOAD(i, m, 8u * i);
    for (u8 i = 0; i < 8; ++i)
        STORE(i, out, 8u * i);
    ctx->downcount -= 17;
    return true;
}

// One row of AABB::Set: the row's x terms give lo/hi (in the frame at `lo`
// and `hi`, swapped through the game's swap routine when lo > hi), then the
// smaller of each y and z pair goes to lo and the larger to hi. a-d are the
// registers the guest keeps the y and z products in.
static s32 aabb_row(CPUState* ctx, u32 row, u32 lo, u32 hi, u8 a, u8 b, u8 c, u8 d, u32 swap_ret) {
    const u32 m = ctx->gpr[31] + 0x10u * row, box = ctx->gpr[30];
    s32 cycles = 29;
    lfs(ctx, 4, m + 0x00u);
    lfs(ctx, 1, box + 0x00u);
    lfs(ctx, 0, box + 0x0Cu);
    ppc_fmuls_inline(ctx, 3, 4, 1);
    lfs(ctx, 2, m + 0x0Cu);
    ppc_fmuls_inline(ctx, 1, 4, 0);
    lfs(ctx, 4, m + 0x04u);
    lfs(ctx, 0, box + 0x04u);
    ppc_fadds_inline(ctx, 6, 2, 3);
    ppc_fadds_inline(ctx, 5, 2, 1);
    lfs(ctx, 3, box + 0x10u);
    ppc_fmuls_inline(ctx, a, 4, 0);
    lfs(ctx, 2, m + 0x08u);
    lfs(ctx, 1, box + 0x08u);
    lfs(ctx, 0, box + 0x14u);
    ppc_fcmp_inline(ctx, 0, ctx->fpr[6], ctx->fpr[5], true);
    stfs(ctx, 6, lo);
    ppc_fmuls_inline(ctx, b, 4, 3);
    ppc_fmuls_inline(ctx, c, 2, 1);
    stfs(ctx, 5, hi);
    ppc_fmuls_inline(ctx, d, 2, 0);
    if (ctx->cr & 0x40000000u) {
        ctx->gpr[3] = lo;
        ctx->gpr[4] = hi;
        ctx->lr = swap_ret;
        lfs(ctx, 1, lo);
        lfs(ctx, 0, hi);
        stfs(ctx, 0, lo);
        stfs(ctx, 1, hi);
        cycles += 8;
    }
    ppc_fcmp_inline(ctx, 0, ctx->fpr[a], ctx->fpr[b], true);
    lfs(ctx, 0, lo);
    lfs(ctx, 1, hi);
    if (ctx->cr & 0x80000000u) {
        ppc_fadds_inline(ctx, 0, 0, a);
        ppc_fadds_inline(ctx, 1, 1, b);
        cycles += 5;
    } else {
        ppc_fadds_inline(ctx, 0, 0, b);
        ppc_fadds_inline(ctx, 1, 1, a);
        cycles += 4;
    }
    ppc_fcmp_inline(ctx, 0, ctx->fpr[c], ctx->fpr[d], true);
    ppc_frsp_inline(ctx, 0, 0);
    ppc_frsp_inline(ctx, 1, 1);
    if (ctx->cr & 0x80000000u) {
        ppc_fadds_inline(ctx, 0, 0, c);
        ppc_fadds_inline(ctx, 1, 1, d);
        cycles += 7;
    } else {
        ppc_fadds_inline(ctx, 0, 0, d);
        ppc_fadds_inline(ctx, 1, 1, c);
        cycles += 6;
    }
    stfs(ctx, 0, lo);
    stfs(ctx, 1, hi);
    return cycles;
}

// void AABB::Set(AABB* this, const AABB* box, const MTX34* m): the box
// transformed by m, as lo/hi per axis.
bool nw4r_AABBSet(CPUState* ctx) {
    const u32 sp = ctx->gpr[1], frame = sp - 0x70u;
    const u32 out = ctx->gpr[3], box = ctx->gpr[4], m = ctx->gpr[5];
    if (!fpu_ready(ctx) || ctx->downcount <= 17 + 3 * 49 + 25 || !plain(ctx, frame, 0x78u) ||
        !plain(ctx, out, 24) || !plain(ctx, box, 24) || !plain(ctx, m, 48))
        return false;
    const u32 base = (u32)AABB_SET_BASE;
    mem_write32_at(ctx, 0, frame, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[0] = ctx->lr;
    mem_write32_at(ctx, 0, frame + 0x74u, ctx->gpr[0]);
    spill(ctx, 31, frame + 0x60u);
    spill(ctx, 30, frame + 0x50u);
    spill(ctx, 29, frame + 0x40u);
    spill(ctx, 28, frame + 0x30u);
    ctx->gpr[11] = frame + 0x30u;
    ctx->lr = base + 0x34u;  // _savegpr_29
    mem_write32_at(ctx, 0, ctx->gpr[11] - 12u, ctx->gpr[29]);
    mem_write32_at(ctx, 0, ctx->gpr[11] - 8u, ctx->gpr[30]);
    mem_write32_at(ctx, 0, ctx->gpr[11] - 4u, ctx->gpr[31]);
    ctx->gpr[29] = out;
    ctx->gpr[30] = box;
    ctx->gpr[31] = m;
    s32 cycles = 17;
    cycles += aabb_row(ctx, 0, frame + 0x1Cu, frame + 0x10u, 28, 29, 30, 31, base + 0xA4u);
    cycles += aabb_row(ctx, 1, frame + 0x18u, frame + 0x0Cu, 31, 30, 29, 28, base + 0x170u);
    cycles += aabb_row(ctx, 2, frame + 0x14u, frame + 0x08u, 31, 30, 29, 28, base + 0x23Cu);
    static const u8 order[6] = {0x1C, 0x18, 0x14, 0x10, 0x0C, 0x08};
    for (u32 i = 0; i < 6; ++i) {
        lfs(ctx, 0, frame + order[i]);
        stfs(ctx, 0, out + 4u * i);
    }
    reload(ctx, 31, frame + 0x60u);
    reload(ctx, 30, frame + 0x50u);
    reload(ctx, 29, frame + 0x40u);
    reload(ctx, 28, frame + 0x30u);
    ctx->gpr[11] = frame + 0x30u;
    ctx->lr = base + 0x2FCu;  // _restgpr_29
    ctx->gpr[29] = mem_read32_at(ctx, 0, ctx->gpr[11] - 12u);
    ctx->gpr[30] = mem_read32_at(ctx, 0, ctx->gpr[11] - 8u);
    ctx->gpr[31] = mem_read32_at(ctx, 0, ctx->gpr[11] - 4u);
    ctx->gpr[0] = mem_read32_at(ctx, 0, frame + 0x74u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] = sp;
    ctx->downcount -= cycles + 25;
    return true;
}
