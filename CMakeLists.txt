cmake_minimum_required(VERSION 3.25)

# DualizeRecomp: NiGHTS: Journey of Dreams (Wii) as a native PC program.
#
#   tools/DolRecomp    the PowerPC recompiler and the guest CPU runtime
#   thirdparty/aurora  the GX renderer (WebGPU through Dawn) and SDL3
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

add_subdirectory(thirdparty)
add_subdirectory(DualizeRecompLib)
add_subdirectory(DualizeRecomp)
