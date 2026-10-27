#ifndef DOLRECOMP_APP_NATIVES_H
#define DOLRECOMP_APP_NATIVES_H

#include "analysis/code_section.h"
#include "backend/emitter.h"

// --natives file: one guest function per line,
//   <address> <size> <fnv1a64 of its code bytes> <host symbol> [jump]
// all numbers hex, '#' starts a comment; `jump` marks a native that sets the
// pc itself (see DolRecompNative). A host implementation hard-codes the
// structure layout of the exact SDK build it was written against, so every
// entry is pinned to the guest bytes it replaces and a mismatch is an error.
typedef struct {
    DolRecompNative* entries;  // sorted by address, names owned by the list
    u32* sizes;
    u64* hashes;
    u32 count;
} DolRecompNativeList;

int natives_load(DolRecompNativeList* list, const char* path);
int natives_verify(const DolRecompNativeList* list, const LoadedCodeSection* sections,
                   u32 section_count);
void natives_free(DolRecompNativeList* list);

#endif
