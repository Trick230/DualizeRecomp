cmake_minimum_required(VERSION 3.25)

# DualizeRecomp: NiGHTS: Journey of Dreams (Wii) as a native PC program.
#
#   tools/DolRecomp    the PowerPC recompiler and what its output compiles with
#   thirdparty/aurora  the GX renderer (D3D12 and Vulkan) and SDL3
#   DualizeRecompLib   the game's code, recompiled at build time from the disc
#   DualizeRecomp      the runtime and the executable

project(DualizeRecomp-ALL C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(DUALIZE_GAME_DIR "${CMAKE_SOURCE_DIR}/extracted/nights" CACHE PATH
    "The game's extracted disc (dolrecomp extract <iso> <dir>)")

add_subdirectory(tools/DolRecomp)

# Without the game, only the tools build: dolrecomp extracts the disc.
if(NOT EXISTS "${DUALIZE_GAME_DIR}/sys/main.dol")
    message(WARNING "No game at ${DUALIZE_GAME_DIR}: building the tools only. "
        "Extract the disc with `dolrecomp extract <iso> <dir>` and configure again.")
    return()
endif()

# The CPU the game, the renderer and the runtime are built for. x86-64-v3
# (AVX2, FMA, BMI2, LZCNT, MOVBE; Haswell / Zen and later, ~96% of Steam's
# survey) gives the guest's fused multiply-adds a hardware fma() and its
# big-endian loads a movbe. `native` tunes for the build machine only.
set(DUALIZE_CPU_TARGET "x86-64-v3" CACHE STRING "-march for the game and runtime (x86-64-v3, x86-64-v2, native)")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    add_compile_options(-march=${DUALIZE_CPU_TARGET})
    # MSYS2's Abseil includes <bmi2intrin.h> by itself when BMI2 is on, which
    # Clang only accepts through <immintrin.h>.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include immintrin.h>")
endif()

# Profile-guided optimisation (scripts/pgo.sh): GENERATE builds an instrumented
# game that writes raw profiles to build/pgo/raw as it exits; USE optimises
# everything with the merged profile. Needs compiler-rt's profile runtime.
set(DUALIZE_PGO "OFF" CACHE STRING "Profile-guided optimisation: OFF, GENERATE or USE")
set(DUALIZE_PGO_PROFILE "${CMAKE_SOURCE_DIR}/build/pgo/merged.profdata" CACHE FILEPATH "Merged profile for DUALIZE_PGO=USE")
if(DUALIZE_PGO STREQUAL "GENERATE")
    add_compile_options(-fprofile-generate=${CMAKE_SOURCE_DIR}/build/pgo/raw)
    add_link_options(-fprofile-generate=${CMAKE_SOURCE_DIR}/build/pgo/raw)
elseif(DUALIZE_PGO STREQUAL "USE")
    # Code changed since the profile keeps its old counts or none: not an error.
    add_compile_options(-fprofile-use=${DUALIZE_PGO_PROFILE} -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
    add_link_options(-fprofile-use=${DUALIZE_PGO_PROFILE})
endif()

add_subdirectory(thirdparty)
add_subdirectory(DualizeRecompLib)
add_subdirectory(DualizeRecomp)

option(DUALIZE_BUILD_TESTS "Build the runtime's unit tests" OFF)
if(DUALIZE_BUILD_TESTS)
    enable_testing()
    # The CPU over a hand-made module: picking among overlapping groups.
    add_executable(test_overlay_dispatch tests/test_overlay_dispatch.cpp
        DualizeRecomp/cpu/cpu.cpp DualizeRecomp/cpu/interpreter.cpp
        DualizeRecomp/kernel/memory.cpp DualizeRecomp/kernel/scheduler.cpp)
    target_include_directories(test_overlay_dispatch PRIVATE DualizeRecomp)
    target_link_libraries(test_overlay_dispatch PRIVATE dr_core)
    add_test(NAME overlay_dispatch COMMAND test_overlay_dispatch)
endif()
