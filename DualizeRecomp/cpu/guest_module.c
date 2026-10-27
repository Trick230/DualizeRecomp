// Glue between the runtime and the code DolRecomp generated for the game.

#include "ppc_recomp.h"

#include "cpu/guest_module.h"

_Static_assert(DOLRECOMP_DIRECT_LINE_SHIFT == GUEST_DIRECT_LINE_SHIFT, "direct line tables differ");

void dolrecomp_indirect_dispatch(CPUState* ctx, u32 address)
{
    (void)dolrecomp_call(ctx, address);
}

static void guest_dispatch(CPUState* ctx, u32 address)
{
    (void)dolrecomp_call(ctx, address);
}

#include "ppc_tables.inc"

const GuestModule g_guest_module = {
    guest_dispatch,
    s_code_ranges,
    sizeof(s_code_ranges) / sizeof(s_code_ranges[0]),
    s_chunk_ranges,
    sizeof(s_chunk_ranges) / sizeof(s_chunk_ranges[0]),
    s_chunk_hashes,
};

void guest_set_direct_calls(const unsigned char* chunk_ok, u32 idle_pc)
{
    dolrecomp_idle_pc = idle_pc;
    dolrecomp_direct_ok = chunk_ok;
}

void guest_set_direct_lines(const unsigned char* lines, u32 size)
{
    dolrecomp_direct_lines = lines;
    dolrecomp_direct_lines_size = lines ? size : 0u;
}
