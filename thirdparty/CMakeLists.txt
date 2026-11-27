# Aurora brings SDL3 with it; the runtime uses its SDL3 for audio and
# gamepads too, so there is one SDL in the process.
if(WIN32)
    # The prebuilt SDL3 is an MSVC build; as a DLL its C API links from MinGW
    # too.
    set(AURORA_SDL3_PROVIDER package CACHE STRING "" FORCE)
    set(AURORA_SDL3_LINKAGE shared CACHE STRING "" FORCE)
endif()
# Every other library Aurora uses (Abseil, fmt, zlib, libpng, SQLite) is built
# from its pinned source and linked statically, never taken from the system,
# so the executable is the same on every machine and ships no DLLs of them.
set(BUILD_SHARED_LIBS OFF)
set(AURORA_VENDORED_DEPS ON CACHE BOOL "" FORCE)
add_subdirectory(aurora EXCLUDE_FROM_ALL)
include(aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
