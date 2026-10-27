// Host implementations of the Metrowerks PPC EABI register save/restore
// helpers (Runtime.PPCEABI.H: _savegpr_N, _restgpr_N, _savefpr_N, _restfpr_N).
//
// Function prologues and epilogues call them with r11 pointing just past the
// save area; entry N stores or loads registers N..31 below r11 and returns.
// Each entry point is its own native, pinned to the bytes from that entry to
// the blr. They decline, before touching anything, unless the whole save area
// lies in fast guest memory (and, for the fpr forms, the FPU is on).

#include "core/cpu.h"

// One cycle per instruction, as the recompiled code charges: the loads or
// stores from the entry on and the blr.
static inline void charge(CPUState* ctx, u32 count) {
    ctx->downcount -= (s64)count + 1;
}

static bool save_gprs(CPUState* ctx, u32 first) {
    const u32 count = 32u - first;
    u8* area = ppc_fast_write_ptr(ctx, ctx->gpr[11] - 4u * count, 4u * count);
    if (!area)
        return false;
    for (u32 i = 0; i < count; ++i)
        write_be32(area + 4u * i, ctx->gpr[first + i]);
    charge(ctx, count);
    return true;
}

static bool restore_gprs(CPUState* ctx, u32 first) {
    const u32 count = 32u - first;
    const u8* area = ppc_fast_ptr(ctx->fast_read, ctx->gpr[11] - 4u * count, 4u * count);
    if (!area)
        return false;
    for (u32 i = 0; i < count; ++i)
        ctx->gpr[first + i] = read_be32(area + 4u * i);
    charge(ctx, count);
    return true;
}

// stfd stores ps0's double bits; lfd replaces ps0 and leaves ps1 alone.
static bool save_fprs(CPUState* ctx, u32 first) {
    const u32 count = 32u - first;
    u8* area = (ctx->msr & PPC_MSR_FP)
                   ? ppc_fast_write_ptr(ctx, ctx->gpr[11] - 8u * count, 8u * count)
                   : NULL;
    if (!area)
        return false;
    for (u32 i = 0; i < count; ++i)
        write_be64(area + 8u * i, f64_bits(ctx->fpr[first + i]));
    charge(ctx, count);
    return true;
}

static bool restore_fprs(CPUState* ctx, u32 first) {
    const u32 count = 32u - first;
    const u8* area = (ctx->msr & PPC_MSR_FP)
                         ? ppc_fast_ptr(ctx->fast_read, ctx->gpr[11] - 8u * count, 8u * count)
                         : NULL;
    if (!area)
        return false;
    for (u32 i = 0; i < count; ++i)
        ctx->fpr[first + i] = f64_value(read_be64(area + 8u * i));
    charge(ctx, count);
    return true;
}

#define MWCC_ENTRIES(kind, body)                                                     \
    bool mwcc_##kind##_14(CPUState* ctx) { return body(ctx, 14); }                   \
    bool mwcc_##kind##_15(CPUState* ctx) { return body(ctx, 15); }                   \
    bool mwcc_##kind##_16(CPUState* ctx) { return body(ctx, 16); }                   \
    bool mwcc_##kind##_17(CPUState* ctx) { return body(ctx, 17); }                   \
    bool mwcc_##kind##_18(CPUState* ctx) { return body(ctx, 18); }                   \
    bool mwcc_##kind##_19(CPUState* ctx) { return body(ctx, 19); }                   \
    bool mwcc_##kind##_20(CPUState* ctx) { return body(ctx, 20); }                   \
    bool mwcc_##kind##_21(CPUState* ctx) { return body(ctx, 21); }                   \
    bool mwcc_##kind##_22(CPUState* ctx) { return body(ctx, 22); }                   \
    bool mwcc_##kind##_23(CPUState* ctx) { return body(ctx, 23); }                   \
    bool mwcc_##kind##_24(CPUState* ctx) { return body(ctx, 24); }                   \
    bool mwcc_##kind##_25(CPUState* ctx) { return body(ctx, 25); }                   \
    bool mwcc_##kind##_26(CPUState* ctx) { return body(ctx, 26); }                   \
    bool mwcc_##kind##_27(CPUState* ctx) { return body(ctx, 27); }                   \
    bool mwcc_##kind##_28(CPUState* ctx) { return body(ctx, 28); }                   \
    bool mwcc_##kind##_29(CPUState* ctx) { return body(ctx, 29); }                   \
    bool mwcc_##kind##_30(CPUState* ctx) { return body(ctx, 30); }                   \
    bool mwcc_##kind##_31(CPUState* ctx) { return body(ctx, 31); }

MWCC_ENTRIES(savegpr, save_gprs)
MWCC_ENTRIES(restgpr, restore_gprs)
MWCC_ENTRIES(savefpr, save_fprs)
MWCC_ENTRIES(restfpr, restore_fprs)
