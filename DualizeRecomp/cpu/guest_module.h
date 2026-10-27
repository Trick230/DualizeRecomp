#pragma once

// The recompiled game code (DualizeRecompLib), linked into the executable.

#include "core/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GuestRange
{
    u32 start;  // inclusive
    u32 end;    // exclusive
} GuestRange;

typedef struct GuestModule
{
    // Runs recompiled code from address until it leaves its chunk or the
    // slice ends; ctx->pc then holds the next pc. Leaves pc alone with no
    // cycles charged if no chunk has an entry for address.
    void (*dispatch)(CPUState* ctx, u32 address);

    // Sorted, non-overlapping. The chunks tile the code ranges exactly, and
    // each has the FNV-1a 64 hash of the guest text it was built from: the
    // runtime checks guest RAM against it before running a chunk and again
    // after any icache invalidation that touches it.
    const GuestRange* code_ranges;
    u32 num_code_ranges;
    const GuestRange* chunk_ranges;
    u32 num_chunk_ranges;
    const u64* chunk_hashes;
} GuestModule;

extern const GuestModule g_guest_module;

// Which chunks generated code may call directly (one flag per chunk, NULL
// for none), and the idle-loop pc that must keep returning to the runtime.
void guest_set_direct_calls(const unsigned char* chunk_ok, u32 idle_pc);
// The same per line of MEM1, for calls whose target is only known at run time.
void guest_set_direct_lines(const unsigned char* lines, u32 size);
#define GUEST_DIRECT_LINE_SHIFT 7u

#ifdef __cplusplus
}
#endif
