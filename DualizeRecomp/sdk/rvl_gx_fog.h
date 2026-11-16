// GXSetFog and GXSetFogRangeAdj for natives that make the calls themselves
// (rvl_gx_fog.c).
#ifndef DUALIZE_SDK_RVL_GX_FOG_H
#define DUALIZE_SDK_RVL_GX_FOG_H

#include "core/cpu.h"

// The five fog BPs GXSetFog(type, start, end, near, far, color) writes, and
// the guest's cycles for the call.
typedef struct {
    u32 bp[5];
    s64 cycles;
} GxFogRegs;

// Fills regs, or returns false when the host can't compute them exactly as
// the guest (the FPU state, an input that isn't a finite single, a scale
// past the range). Touches nothing.
bool rvl_gx_fog_regs(CPUState* ctx, u32 type, f64 start, f64 end, f64 near_z, f64 far_z, u32 color,
                     GxFogRegs* regs);
void rvl_gx_fog_emit(CPUState* ctx, const GxFogRegs* regs);

// GXSetFogRangeAdj(enable, center, table), table's 10 u16 in plain memory
// when enable. Returns the guest's cycles.
s64 rvl_gx_fog_range_adj(CPUState* ctx, u32 enable, u32 center, u32 table);

#endif
