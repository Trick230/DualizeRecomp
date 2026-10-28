# Aurora brings SDL3 and Dawn with it; the runtime uses its SDL3 for audio and
# gamepads too, so there is one SDL in the process.
if(WIN32)
    # Prebuilt Dawn and SDL3 are MSVC builds; as DLLs their C APIs link from
    # MinGW too.
    set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
    set(AURORA_DAWN_LINKAGE shared CACHE STRING "" FORCE)
    set(AURORA_SDL3_PROVIDER package CACHE STRING "" FORCE)
    set(AURORA_SDL3_LINKAGE shared CACHE STRING "" FORCE)
endif()
add_subdirectory(aurora EXCLUDE_FROM_ALL)
include(aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
