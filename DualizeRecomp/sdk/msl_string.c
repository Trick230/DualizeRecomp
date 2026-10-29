// Host implementations of the Metrowerks runtime's memory routines (see
// rvl_os.c for the contract every native follows).
//
// Ranges that leave fast guest memory, or straddle two of its slices, decline
// to the guest code, which goes through the out-of-line memory path.

#include "core/cpu.h"

#include <string.h>

// Host view of `size` guest bytes, or NULL; ppc_fast_ptr's bound assumes
// accesses no larger than a slice.
static inline const u8* source(CPUState* ctx, u32 address, u32 size) {
    return size <= PPC_FAST_SLICE_SIZE ? ppc_fast_ptr(ctx->fast_read, address, size) : NULL;
}

static inline u8* destination(CPUState* ctx, u32 address, u32 size) {
    return size <= PPC_FAST_SLICE_SIZE ? ppc_fast_write_ptr(ctx, address, size) : NULL;
}

// void* memcpy(void* dst, const void* src, size_t n)
//
// The runtime's copy is byte-wise in whichever direction is safe for
// overlapping ranges, so it behaves as memmove.
bool msl_memcpy(CPUState* ctx) {
    const u32 size = ctx->gpr[5];
    if (size == 0) {
        ctx->downcount -= 4;
        return true;
    }
    u8* dst = destination(ctx, ctx->gpr[3], size);
    const u8* src = source(ctx, ctx->gpr[4], size);
    if (!dst || !src)
        return false;
    memmove(dst, src, size);
    ctx->downcount -= 4 + (s32)(size >> 1);
    return true;
}

// void __fill_mem(void* dst, int value, size_t n)
bool msl_fill_mem(CPUState* ctx) {
    const u32 size = ctx->gpr[5];
    if (size == 0) {
        ctx->downcount -= 4;
        return true;
    }
    u8* dst = destination(ctx, ctx->gpr[3], size);
    if (!dst)
        return false;
    memset(dst, (u8)ctx->gpr[4], size);
    ctx->downcount -= 8 + (s32)(size >> 3);
    return true;
}
