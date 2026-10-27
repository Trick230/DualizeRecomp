#include "app/natives.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u64 fnv1a64(const u8* data, u32 size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static int valid_symbol(const char* name) {
    if (!isalpha((unsigned char)name[0]) && name[0] != '_')
        return 0;
    for (const char* c = name; *c; ++c) {
        if (!isalnum((unsigned char)*c) && *c != '_')
            return 0;
    }
    return 1;
}

int natives_load(DolRecompNativeList* list, const char* path) {
    memset(list, 0, sizeof(*list));
    FILE* file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "error: cannot open natives file %s\n", path);
        return 0;
    }

    u32 capacity = 0;
    char line[512];
    u32 line_number = 0;
    int ok = 1;
    while (ok && fgets(line, sizeof(line), file)) {
        ++line_number;
        char* comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        unsigned address = 0, size = 0;
        unsigned long long hash = 0;
        char name[256];
        char flag[16];
        int fields =
            sscanf(line, "%x %x %llx %255s %15s", &address, &size, &hash, name, flag);
        if (fields <= 0)
            continue;
        if (fields < 4 || size == 0 || (address & 3u) != 0 || (size & 3u) != 0 ||
            !valid_symbol(name) || (fields == 5 && strcmp(flag, "jump") != 0)) {
            fprintf(stderr,
                    "error: %s:%u: expected '<address> <size> <hash> <symbol> [jump]'\n",
                    path, line_number);
            ok = 0;
            break;
        }
        if (list->count && list->entries[list->count - 1].address >= address) {
            fprintf(stderr, "error: %s:%u: entries must be sorted by address\n", path,
                    line_number);
            ok = 0;
            break;
        }
        if (list->count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            DolRecompNative* entries = realloc(list->entries, capacity * sizeof(*entries));
            u32* sizes = realloc(list->sizes, capacity * sizeof(*sizes));
            u64* hashes = realloc(list->hashes, capacity * sizeof(*hashes));
            if (entries)
                list->entries = entries;
            if (sizes)
                list->sizes = sizes;
            if (hashes)
                list->hashes = hashes;
            if (!entries || !sizes || !hashes) {
                fprintf(stderr, "error: out of memory reading %s\n", path);
                ok = 0;
                break;
            }
        }
        char* owned = malloc(strlen(name) + 1);
        if (!owned) {
            fprintf(stderr, "error: out of memory reading %s\n", path);
            ok = 0;
            break;
        }
        strcpy(owned, name);
        list->entries[list->count].address = address;
        list->entries[list->count].name = owned;
        list->entries[list->count].jump = fields == 5;
        list->sizes[list->count] = size;
        list->hashes[list->count] = hash;
        ++list->count;
    }
    fclose(file);
    if (!ok)
        natives_free(list);
    return ok;
}

int natives_verify(const DolRecompNativeList* list, const LoadedCodeSection* sections,
                   u32 section_count) {
    for (u32 i = 0; i < list->count; ++i) {
        const u32 address = list->entries[i].address;
        const u32 size = list->sizes[i];
        const LoadedCodeSection* section = NULL;
        for (u32 s = 0; s < section_count && !section; ++s) {
            if (address >= sections[s].address &&
                (u64)address + size <= (u64)sections[s].address + sections[s].size)
                section = &sections[s];
        }
        if (!section) {
            fprintf(stderr, "error: native %s at %08X is outside the code sections\n",
                    list->entries[i].name, address);
            return 0;
        }
        const u64 hash = fnv1a64(section->data + (address - section->address), size);
        if (hash != list->hashes[i]) {
            fprintf(stderr,
                    "error: native %s at %08X expects code hash %016llX, found %016llX "
                    "(different SDK build)\n",
                    list->entries[i].name, address, (unsigned long long)list->hashes[i],
                    (unsigned long long)hash);
            return 0;
        }
    }
    return 1;
}

void natives_free(DolRecompNativeList* list) {
    for (u32 i = 0; i < list->count; ++i)
        free((char*)list->entries[i].name);
    free(list->entries);
    free(list->sizes);
    free(list->hashes);
    memset(list, 0, sizeof(*list));
}
