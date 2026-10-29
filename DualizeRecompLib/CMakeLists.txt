# The game's code: DolRecomp runs at build time on the extracted disc and
# writes a fixed set of sources, which compile with the guest-side glue into
# one static library.

set(GAME_ID R7EE8P)
set(CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/${GAME_ID}.toml")
set(PPC_DIR "${CMAKE_CURRENT_BINARY_DIR}/ppc")
set(SHARDS 64)  # main.shards in the config

set(PPC_SOURCES)
math(EXPR LAST_SHARD "${SHARDS} - 1")
foreach(i RANGE ${LAST_SHARD})
    list(APPEND PPC_SOURCES "${PPC_DIR}/ppc_recomp.${i}.c")
endforeach()

set(GAME_INPUTS
    "${DUALIZE_GAME_DIR}/sys/main.dol"
    "${DUALIZE_GAME_DIR}/files/rso/NightsWii.sel"
    "${DUALIZE_GAME_DIR}/files/rso/global.rso"
    "${DUALIZE_GAME_DIR}/files/rso/play.rso"
    "${DUALIZE_GAME_DIR}/files/rso/select.rso"
    "${DUALIZE_GAME_DIR}/files/rso/title.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage1000.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage1010.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage1100.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage1110.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage1200.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage1210.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage2000.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage2010.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage2100.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage2110.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage2200.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage2210.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage3000.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage3010.rso"
    "${DUALIZE_GAME_DIR}/files/rso/stage4000.rso"
    "${DUALIZE_GAME_DIR}/files/rso/myDream.rso"
    "${DUALIZE_GAME_DIR}/files/rso/staffRoll.rso"
    "${DUALIZE_GAME_DIR}/files/rso/loading2P.rso")
foreach(input IN LISTS GAME_INPUTS)
    if(NOT EXISTS "${input}")
        message(FATAL_ERROR "${input} is missing: set DUALIZE_GAME_DIR to the extracted disc")
    endif()
endforeach()

add_custom_command(
    OUTPUT ${PPC_SOURCES} "${PPC_DIR}/ppc_recomp.h" "${PPC_DIR}/ppc_tables.inc"
        "${PPC_DIR}/ppc_natives.h"
    COMMAND $<TARGET_FILE:dolrecomp> "${CONFIG}" "${DUALIZE_GAME_DIR}" "${PPC_DIR}"
    DEPENDS dolrecomp "${CONFIG}" "${CMAKE_CURRENT_SOURCE_DIR}/config/mwcc.sigs"
        "${CMAKE_CURRENT_SOURCE_DIR}/config/rvl.sigs" "${CMAKE_CURRENT_SOURCE_DIR}/config/sdk.sigs"
        ${GAME_INPUTS}
    COMMENT "Recompiling ${GAME_ID}"
    USES_TERMINAL)

set(RUNTIME_DIR "${CMAKE_SOURCE_DIR}/DualizeRecomp")
# The natives written in C compile here, with the game's flags, so they inline
# across the generated code as if they were the game's own.
add_library(DualizeRecompLib STATIC
    ${PPC_SOURCES}
    "${RUNTIME_DIR}/sdk/msl_string.c"
    "${RUNTIME_DIR}/sdk/mwcc_runtime.c"
    "${RUNTIME_DIR}/sdk/nw4r_g3d.c"
    "${RUNTIME_DIR}/sdk/rvl_cache.c"
    "${RUNTIME_DIR}/sdk/rvl_mtx.c"
    "${RUNTIME_DIR}/sdk/rvl_os.c")
target_include_directories(DualizeRecompLib PRIVATE "${PPC_DIR}" "${RUNTIME_DIR}")
target_link_libraries(DualizeRecompLib PUBLIC dr_core)
# DolRecomp's glue to the generated tables, compiled here too.
target_link_libraries(DualizeRecompLib PRIVATE dr_guest)
# Guest memory through CPUState::flat_base, one host load or store per access.
target_compile_definitions(DualizeRecompLib PRIVATE GXRUNTIME_FLAT_MEMORY=1)

# Every guest operation stays separately rounded, so results are bit-exact;
# line tables let a sampling profiler map host code back to guest code.
target_compile_options(DualizeRecompLib PRIVATE
    -O3 -ffp-contract=off -fno-fast-math -gline-tables-only)
# The CPU target comes from DUALIZE_CPU_TARGET at the top: the inline FP fast
# paths need a hardware fma(), which x86-64-v2 only reaches through libm.

# ThinLTO across the chunks: cross-chunk calls inline. The cache makes a
# rebuild re-optimize only what changed; each backend job holds a few large
# chunks, so a bounded number run (one per core exhausted 16 GiB).
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(TARGET DualizeRecompLib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(DUALIZE_LTO_JOBS 4 CACHE STRING "Parallel ThinLTO backend jobs at link time")
    target_link_options(DualizeRecompLib INTERFACE
        -fuse-ld=lld
        "LINKER:--thinlto-jobs=${DUALIZE_LTO_JOBS}"
        "LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache"
        "LINKER:--thinlto-cache-policy=cache_size_bytes=4g")
endif()
