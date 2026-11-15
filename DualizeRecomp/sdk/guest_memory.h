// Guest memory as the C natives see it.
//
// They build flat, as the generated code (GXRUNTIME_FLAT_MEMORY in
// DualizeRecompLib/CMakeLists.txt): the host maps the guest's effective
// address space 1:1 at flat_base, so any range outside the hardware window
// [0xC8000000, 0xD0000000) is plain host memory, the locked cache at
// 0xE0000000 included (g3d keeps its matrix work there).

#pragma once

#include "core/cpu.h"

#if !defined(GXRUNTIME_FLAT_MEMORY) || !GXRUNTIME_FLAT_MEMORY
#error "the natives build with flat guest memory"
#endif

// Host view of `size` guest bytes from `address`, or NULL when the range
// reaches the hardware window or the top of the address space.
static inline u8* guest_span(CPUState* ctx, u32 address, u32 size) {
    const u64 end = (u64)address + size;
    if (end > 0x100000000ull || (end > 0xC8000000u && address < 0xD0000000u))
        return NULL;
    return ctx->flat_base + address;
}
