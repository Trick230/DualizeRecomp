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

// One guest function. It runs from ctx->pc, its start or a point it can resume
// at, until it returns to its caller or stops early (the slice ended, an
// exception, a call it could not make directly); ctx->pc then holds the next
// pc. It leaves the pc alone with no cycles charged if it has no entry there.
typedef void (*GuestCode)(CPUState* ctx);

typedef struct GuestModule
{
    // Sorted, non-overlapping.
    const GuestRange* code_ranges;
    u32 num_code_ranges;
    // The functions tile the code ranges exactly, sorted. Each has the FNV-1a
    // 64 hash of the guest text it was built from: the runtime checks guest
    // RAM against it before running the function and again after any icache
    // invalidation that touches it.
    const GuestRange* function_ranges;
    const GuestCode* functions;
    const u64* function_hashes;
    u32 num_functions;
} GuestModule;

extern const GuestModule g_guest_module;

// Which functions generated code may call directly (one flag per function,
// NULL for none), and the idle-loop pc that must keep returning to the runtime.
void guest_set_direct_calls(const unsigned char* function_ok, u32 idle_pc);

// The runtime's (cpu.cpp): the verified function holding target, or NULL.
GuestCode dolrecomp_lookup(CPUState* ctx, u32 target);
// The runtime's: the slice ran out at a loop about to go to pc. Runs the due
// events in place; nonzero if generated code must leave for the dispatcher.
int dolrecomp_safepoint(CPUState* ctx, u32 pc);

#ifdef __cplusplus
}
#endif
