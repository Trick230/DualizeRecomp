// Glue between the runtime and the code DolRecomp generated for the game.

#include "ppc_recomp.h"

#include "cpu/guest_module.h"

#include "ppc_tables.inc"

_Static_assert(sizeof(s_functions) / sizeof(s_functions[0]) ==
                   sizeof(s_function_ranges) / sizeof(s_function_ranges[0]),
               "one range per function");

const GuestModule g_guest_module = {
    s_code_ranges,
    sizeof(s_code_ranges) / sizeof(s_code_ranges[0]),
    s_function_ranges,
    s_functions,
    s_function_hashes,
    sizeof(s_functions) / sizeof(s_functions[0]),
};

void guest_set_direct_calls(const unsigned char* function_ok, u32 idle_pc)
{
    dolrecomp_idle_pc = idle_pc;
    dolrecomp_direct_ok = function_ok;
}
