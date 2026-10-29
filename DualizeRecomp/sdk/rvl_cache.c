// Host implementations of the Revolution SDK cache range functions.
//
// Each SDK function runs one dcbf/dcbst/dcbi/icbi per 32-byte line in a
// counted loop, and the syncing data-cache variants then take a system call
// whose handler at 0x80000C00 (copied there by OSInitSystemCall) pulses
// HID0[ABE] and returns. The natives hand the whole range to the host's
// cache_range hook in one call and reproduce the loop's registers, the system
// call's SRR0/SRR1 and the handler's r9/r10 and HID0 writes, so only the time
// spent differs.

#include "core/cpu.h"

#include "ppc_natives.h"

#define SPR_HID0 1008u
#define HID0_ABE 0x00000008u

// The system call vector, as OSInitSystemCall copies it from the SDK.
#define SYSTEM_CALL_VECTOR 0x80000C00u
static const u32 k_system_call_handler[] = {
    0x7D30FAA6u,  // mfhid0  r9
    0x612A0008u,  // ori     r10, r9, 8
    0x7D50FBA6u,  // mthid0  r10
    0x4C00012Cu,  // isync
    0x7C0004ACu,  // sync
    0x7D30FBA6u,  // mthid0  r9
    0x4C000064u,  // rfi
};
#define SYSTEM_CALL_HANDLER_WORDS (sizeof(k_system_call_handler) / sizeof(k_system_call_handler[0]))

// From the natives list: the address of DCFlushRange's and DCStoreRange's sc
// (DC_FLUSH_RANGE_SC, DC_STORE_RANGE_SC), the system call's SRR0 minus 4.

// Cycles the recompiled code charges: the size test, the line count, each
// loop iteration and the final blr; the system call block and its handler.
#define CYCLES_TEST 2
#define CYCLES_SETUP 6
#define CYCLES_PER_LINE 7
#define CYCLES_RETURN 1
#define CYCLES_SYSTEM_CALL 2
#define CYCLES_HANDLER 7
#define CYCLES_SYNC 2  // ICInvalidateRange's sync; isync

static inline void set_cr0_unsigned_compare_zero(CPUState* ctx, u32 value) {
    const u32 bits = (value != 0 ? 0x4u : 0x2u) | ((ctx->xer >> 31) & 1u);
    ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (bits << 28);
}

static bool system_call_handler_is_stock(CPUState* ctx) {
    const u8* code =
        ppc_fast_ptr(ctx->fast_read, SYSTEM_CALL_VECTOR, (u32)sizeof(k_system_call_handler));
    if (!code)
        return false;
    for (u32 i = 0; i < SYSTEM_CALL_HANDLER_WORDS; ++i) {
        if (read_be32(code + i * 4u) != k_system_call_handler[i])
            return false;
    }
    return true;
}

// The system call's exception entry and return leave MSR as it was as long as
// no bit outside the rfi mask is set; the vector is used as it stands only
// while the guest still has the SDK handler there.
static bool system_call_ready(CPUState* ctx) {
    return (ctx->msr & ~PPC_MSR_RFI_MASK) == 0 && (ctx->msr & PPC_MSR_IP) == 0 &&
           ctx->spr_read && ctx->spr_write && system_call_handler_is_stock(ctx);
}

static void system_call(CPUState* ctx, u32 sc_address) {
    ctx->srr0 = sc_address + 4u;
    ctx->srr1 = ctx->msr & PPC_MSR_RFI_MASK;
    const u32 hid0 = ctx->spr_read(ctx, SPR_HID0, SYSTEM_CALL_VECTOR);
    ctx->gpr[9] = hid0;
    ctx->gpr[10] = hid0 | HID0_ABE;
    ctx->spr_write(ctx, SPR_HID0, ctx->gpr[10], SYSTEM_CALL_VECTOR + 8u);
    ctx->spr_write(ctx, SPR_HID0, ctx->gpr[9], SYSTEM_CALL_VECTOR + 20u);
    ctx->downcount -= CYCLES_SYSTEM_CALL + CYCLES_HANDLER;
}

// The shared body: void DC*Range(void* addr, u32 nBytes). Returns false, with
// nothing touched, when the host has no range hook or the op would trap.
static bool range(CPUState* ctx, u8 operation, bool syncs, u32 sc_address) {
    if (!ctx->cache_range || (operation == PPC_CACHE_DCBI && (ctx->msr & PPC_MSR_PR)))
        return false;
    const u32 start = ctx->gpr[3];
    const u32 bytes = ctx->gpr[4];
    // A count that wraps to 0 lines makes the guest loop run 2^32 times.
    const u32 lines = (bytes + (start & 31u) + 31u) >> 5;
    if (bytes != 0 && (lines == 0 || (syncs && !system_call_ready(ctx))))
        return false;

    set_cr0_unsigned_compare_zero(ctx, bytes);
    ctx->downcount -= CYCLES_TEST;
    if (bytes == 0)
        return true;

    ctx->gpr[5] = start & 31u;
    ctx->gpr[4] = lines;
    ctx->ctr = 0;
    ctx->cache_range(ctx, operation, start, lines);
    ctx->gpr[3] = start + lines * 32u;
    ctx->downcount -= CYCLES_SETUP + (s64)CYCLES_PER_LINE * lines + CYCLES_RETURN;
    if (syncs)
        system_call(ctx, sc_address);
    return true;
}

// ICInvalidateRange(void* addr, u32 nBytes): an icbi per line, then sync and
// isync. When the lines held code the guest has run, the native declines:
// the guest loop's icbi then leaves every caller to the runtime, so none
// resumes over code that changed (the host has already retired it).
bool rvl_ICInvalidateRange(CPUState* ctx) {
    if (!ctx->cache_range)
        return false;
    const u32 start = ctx->gpr[3];
    const u32 bytes = ctx->gpr[4];
    const u32 lines = (bytes + (start & 31u) + 31u) >> 5;
    if (bytes != 0 && (lines == 0 || ctx->cache_range(ctx, PPC_CACHE_ICBI, start, lines)))
        return false;

    set_cr0_unsigned_compare_zero(ctx, bytes);
    ctx->downcount -= CYCLES_TEST;
    if (bytes == 0)
        return true;
    ctx->gpr[5] = start & 31u;
    ctx->gpr[4] = lines;
    ctx->ctr = 0;
    ctx->gpr[3] = start + lines * 32u;
    ctx->downcount -= CYCLES_SETUP + (s64)CYCLES_PER_LINE * lines + CYCLES_SYNC + CYCLES_RETURN;
    return true;
}

bool rvl_DCInvalidateRange(CPUState* ctx) {
    return range(ctx, PPC_CACHE_DCBI, false, 0);
}

bool rvl_DCFlushRange(CPUState* ctx) {
    return range(ctx, PPC_CACHE_DCBF, true, DC_FLUSH_RANGE_SC);
}

bool rvl_DCStoreRange(CPUState* ctx) {
    return range(ctx, PPC_CACHE_DCBST, true, DC_STORE_RANGE_SC);
}

bool rvl_DCFlushRangeNoSync(CPUState* ctx) {
    return range(ctx, PPC_CACHE_DCBF, false, 0);
}

bool rvl_DCStoreRangeNoSync(CPUState* ctx) {
    return range(ctx, PPC_CACHE_DCBST, false, 0);
}
