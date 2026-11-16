// Host implementations of the Revolution SDK's GXSetFog and GXSetFogRangeAdj
// (see rvl_os.c for the contract every native follows).
//
// They put the same BPs in the GX stream as the guest's code, with its single
// precision math (each operation rounded once, no fused multiply-add) and its
// constants read from .sdata2, and charge the guest's cycles for the path it
// takes. Volatile registers are left as they were.

#include <math.h>
#include <string.h>

#include "core/cpu.h"

#include "ppc_natives.h"
#include "sdk/guest_memory.h"
#include "sdk/rvl_gx_fog.h"

#pragma clang fp contract(off)

#define GATHER_PIPE 0xCC008000u
#define GX_DATA lwz(ctx->gpr[2] + (u32)GX_DATA_SDA2)
#define GX_DATA_BP_SENT_NOT 0x002u  // u16: zero once a BP was written

#define lhz(a) mem_read16_at(ctx, 0, (a))
#define lwz(a) mem_read32_at(ctx, 0, (a))
#define sth(a, v) mem_write16_at(ctx, 0, (a), (u16)(v))
#define stw(a, v) mem_write32_at(ctx, 0, (a), (u32)(v))

// The guest's cycles: GXSetFog's entry, its ortho and perspective paths to
// the scale loops (degenerate when far == near or end == start), the loops'
// entries, checks and bodies, the B conversion, __cvt_fp2unsigned by range,
// the BP writes; GXSetFogRangeAdj's table and BP. An fdivs is 17 cycles, an
// mtspr 2.
#define FOG_ENTRY_CYCLES 15
#define FOG_FAR_NEAR_CYCLES 6
#define FOG_END_START_CYCLES 8
#define FOG_ORTHO_CYCLES 30
#define FOG_PERSP_CYCLES 59
#define FOG_LOOPS_CYCLES 8
#define FOG_SHIFT_CYCLES 41
#define FOG_CVT_NEGATIVE_CYCLES 12
#define FOG_CVT_HUGE_CYCLES 14
#define FOG_CVT_CYCLES 21
#define FOG_CVT_HIGH_CYCLES 23
#define FOG_BP_CYCLES 47
#define FOG_RANGE_TABLE_CYCLES 57
#define FOG_RANGE_CYCLES 16

static u32 bits_f32(float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    return bits;
}

static float f32_bits(u32 bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static f64 f64_bits_at(CPUState* ctx, u32 address) {
    const u64 bits = ((u64)lwz(address) << 32) | lwz(address + 4u);
    f64 d;
    memcpy(&d, &bits, 8);
    return d;
}

static bool single(f64 d) { return isfinite(d) && (f64)(float)d == d; }

// The FPU is on, rounds to nearest (the host's flush to zero follows NI) and
// traps nothing.
static bool fog_fpu_ok(CPUState* ctx) {
    return (ctx->msr & PPC_MSR_FP) && !(ctx->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)) && (ctx->fpscr & 3u) == 0;
}

bool rvl_gx_fog_regs(CPUState* ctx, u32 type, f64 start, f64 end, f64 near_z, f64 far_z, u32 color,
                     GxFogRegs* regs) {
    const u32 sda2 = ctx->gpr[2];
    if (!fog_fpu_ok(ctx) || !single(start) || !single(end) || !single(near_z) || !single(far_z) ||
        !guest_span(ctx, sda2 + (u32)GX_FOG_ZERO, 4) || !guest_span(ctx, sda2 + (u32)GX_FOG_ONE, 4) ||
        !guest_span(ctx, sda2 + (u32)GX_FOG_HALF, 4) || !guest_span(ctx, sda2 + (u32)GX_FOG_TWO, 4) ||
        !guest_span(ctx, sda2 + (u32)GX_FOG_B_MAX, 8) || !guest_span(ctx, sda2 + (u32)GX_FOG_B_MIN, 8) ||
        !guest_span(ctx, sda2 + (u32)GX_FOG_B_MANTISSA, 4) || !guest_span(ctx, sda2 + (u32)GX_FOG_I2F, 8))
        return false;
    const float s = (float)start, e = (float)end, n = (float)near_z, f = (float)far_z;
    const float zero = f32_bits(lwz(sda2 + (u32)GX_FOG_ZERO));
    s64 cycles = FOG_ENTRY_CYCLES;
    const bool degenerate = f == n || e == s;
    const s64 degenerate_cycles = f == n ? FOG_FAR_NEAR_CYCLES : FOG_END_START_CYCLES;
    float a, c;
    u32 b_m = 0, b_s = 0;
    if ((type >> 3) & 1u) {
        // Orthographic: B's registers stay 0, their ids too.
        if (degenerate) {
            a = c = zero;
            cycles += degenerate_cycles;
        } else {
            const float scale = f32_bits(lwz(sda2 + (u32)GX_FOG_ONE)) / (e - s);
            a = scale * (f - n);
            c = scale * (s - n);
            cycles += FOG_ORTHO_CYCLES;
        }
    } else {
        float b;
        if (degenerate) {
            a = c = zero;
            b = f32_bits(lwz(sda2 + (u32)GX_FOG_HALF));
            cycles += degenerate_cycles;
        } else {
            const float fn = f - n, es = e - s;
            const float product = f * n;
            b = f / fn;
            const float denominator = fn * es;
            c = s / es;
            a = product / denominator;
            cycles += FOG_PERSP_CYCLES;
        }
        // B into [0.5, 1] by halving and doubling, the shift counted.
        const float half = f32_bits(lwz(sda2 + (u32)GX_FOG_HALF)), two = f32_bits(lwz(sda2 + (u32)GX_FOG_TWO));
        const f64 b_max = f64_bits_at(ctx, sda2 + (u32)GX_FOG_B_MAX), b_min = f64_bits_at(ctx, sda2 + (u32)GX_FOG_B_MIN);
        s32 shift = 0;
        cycles += FOG_LOOPS_CYCLES + 2;
        for (int guard = 0; (f64)b > b_max; ++guard) {
            if (guard > 300)
                return false;
            b = b * half;
            ++shift;
            cycles += 4;
        }
        for (int guard = 0;; ++guard) {
            if (guard > 300)
                return false;
            if (!(b > zero)) {
                cycles += 4;
                break;
            }
            if (!((f64)b < b_min)) {
                cycles += 6;
                break;
            }
            b = b * two;
            --shift;
            cycles += 8;
        }
        const u32 amount = (u32)(shift + 1) & 0x3Fu;
        const u32 power = amount < 32u ? 1u << amount : 0u;
        if (power == 0)
            return false;
        // The int to float through the magic double, then fsubs.
        const u64 magic_bits = 0x4330000000000000ull | (power ^ 0x80000000u);
        f64 magic;
        memcpy(&magic, &magic_bits, 8);
        const float divisor = (float)(magic - f64_bits_at(ctx, sda2 + (u32)GX_FOG_I2F));
        const f64 mantissa = (f64)(f32_bits(lwz(sda2 + (u32)GX_FOG_B_MANTISSA)) * b);
        a = a / divisor;
        cycles += FOG_SHIFT_CYCLES;
        // __cvt_fp2unsigned.
        u32 m;
        if (mantissa < 0.0) {
            m = 0;
            cycles += FOG_CVT_NEGATIVE_CYCLES;
        } else if (mantissa >= 4294967296.0) {
            m = 0xFFFFFFFFu;
            cycles += FOG_CVT_HUGE_CYCLES;
        } else if (mantissa < 2147483648.0) {
            m = (u32)(s32)mantissa;
            cycles += FOG_CVT_CYCLES;
        } else {
            m = (u32)(s32)(mantissa - 2147483648.0) + 0x80000000u;
            cycles += FOG_CVT_HIGH_CYCLES;
        }
        b_m = 0xEF000000u | (m & 0xFFFFFFu);
        b_s = 0xF0000000u | ((u32)(shift + 1) & 0x1Fu);
    }
    regs->bp[0] = 0xEE000000u | (bits_f32(a) >> 12);
    regs->bp[1] = b_m;
    regs->bp[2] = b_s;
    regs->bp[3] = 0xF1000000u | ((type & 7u) << 21) | (((type >> 3) & 1u) << 20) | (bits_f32(c) >> 12);
    regs->bp[4] = 0xF2000000u | (color >> 8);
    regs->cycles = cycles + FOG_BP_CYCLES;
    return true;
}

void rvl_gx_fog_emit(CPUState* ctx, const GxFogRegs* regs) {
    for (u32 i = 0; i < 5u; ++i) {
        mem_write8_at(ctx, 0, GATHER_PIPE, 0x61);
        stw(GATHER_PIPE, regs->bp[i]);
    }
    sth(GX_DATA + GX_DATA_BP_SENT_NOT, 0);
}

s64 rvl_gx_fog_range_adj(CPUState* ctx, u32 enable, u32 center, u32 table) {
    s64 cycles = FOG_RANGE_CYCLES;
    if (enable) {
        for (u32 i = 0; i < 5u; ++i) {
            const u32 lo = lhz(table + 4u * i), hi = lhz(table + 4u * i + 2u);
            mem_write8_at(ctx, 0, GATHER_PIPE, 0x61);
            stw(GATHER_PIPE, ((0xE9u + i) << 24) | ((hi << 12) & 0xFFF000u) | (lo & 0xFFFu));
        }
        cycles += FOG_RANGE_TABLE_CYCLES;
    }
    mem_write8_at(ctx, 0, GATHER_PIPE, 0x61);
    stw(GATHER_PIPE, 0xE8000000u | ((enable & 1u) << 10) | ((center + 0x156u) & 0x3FFu));
    sth(GX_DATA + GX_DATA_BP_SENT_NOT, 0);
    return cycles;
}

// void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz,
// GXColor color).
bool rvl_GXSetFog(CPUState* ctx) {
    GxFogRegs regs;
    const u32 color = ctx->gpr[4];
    if (!guest_span(ctx, color, 4) || !guest_span(ctx, ctx->gpr[1] + 4u, 4) ||
        !rvl_gx_fog_regs(ctx, ctx->gpr[3], ctx->fpr[1], ctx->fpr[2], ctx->fpr[3], ctx->fpr[4], lwz(color), &regs))
        return false;
    // The LR save into the caller's frame.
    stw(ctx->gpr[1] + 4u, ctx->lr);
    rvl_gx_fog_emit(ctx, &regs);
    ctx->downcount -= regs.cycles;
    return true;
}

// void GXSetFogRangeAdj(GXBool enable, u16 center, const GXFogAdjTable* table).
bool rvl_GXSetFogRangeAdj(CPUState* ctx) {
    const u32 enable = ctx->gpr[3], table = ctx->gpr[5];
    if (enable && !guest_span(ctx, table, 20))
        return false;
    ctx->downcount -= rvl_gx_fog_range_adj(ctx, enable, ctx->gpr[4], table);
    return true;
}
