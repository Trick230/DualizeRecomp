#include "app/natives.h"
#include "backend/emitter.h"
#include "frontend/decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return 1; } } while (0)

static int write_text(const char* path, const char* text) {
    FILE* file = fopen(path, "w");
    if (!file)
        return 0;
    fputs(text, file);
    return fclose(file) == 0;
}

static char* read_text(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        text = NULL;
    }
    if (text)
        text[size] = '\0';
    fclose(file);
    return text;
}

int main(void) {
    // blr; li r3,0. FNV-1a 64 of the first four bytes is B6F2E0C4BC8017BB.
    static const u8 code[] = {0x4E, 0x80, 0x00, 0x20, 0x38, 0x60, 0x00, 0x00};
    LoadedCodeSection section = {0};
    section.data = code;
    section.address = 0x80001000u;
    section.size = sizeof(code);

    const char* path = "test_natives.list";
    DolRecompNativeList list;

    CHECK(write_text(path, "# comment\n"
                           "80001000 4 B6F2E0C4BC8017BB host_leaf\n"
                           "80001004 4 0 host_other  # trailing comment\n"));
    CHECK(natives_load(&list, path));
    CHECK(list.count == 2);
    CHECK(list.entries[0].address == 0x80001000u);
    CHECK(strcmp(list.entries[1].name, "host_other") == 0);
    CHECK(!natives_verify(&list, &section, 1));  // second hash is wrong
    natives_free(&list);
    CHECK(write_text(path, "80001000 4 B6F2E0C4BC8017BB host_leaf\n"));
    CHECK(natives_load(&list, path));
    CHECK(natives_verify(&list, &section, 1));
    natives_free(&list);

    CHECK(write_text(path, "80001000 4 0 a jump\n80001004 4 0 b\n"));
    CHECK(natives_load(&list, path));
    CHECK(list.entries[0].jump && !list.entries[1].jump);
    natives_free(&list);
    CHECK(write_text(path, "80001000 4 0 a leap\n"));
    CHECK(!natives_load(&list, path));  // unknown flag

    CHECK(write_text(path, "80001004 4 0 b\n80001000 4 0 a\n"));
    CHECK(!natives_load(&list, path));  // unsorted
    CHECK(write_text(path, "80001000 4 0 not-a-symbol\n"));
    CHECK(!natives_load(&list, path));
    CHECK(write_text(path, "80002000 4 0 outside\n"));
    CHECK(natives_load(&list, path));
    CHECK(!natives_verify(&list, &section, 1));
    natives_free(&list);

    // A bl to a native resumes after the call when the native accepts, and a
    // native function entry checks it before running its guest body.
    static const DolRecompNative natives[] = {{0x80001008u, "host_leaf", false}};
    PPCInst insts[3];
    const u32 words[3] = {0x48000009u /* bl +8 */, 0x60000000u, 0x4E800020u};
    for (u32 i = 0; i < 3; ++i)
        insts[i] = ppc_decode(words[i], 0x80001000u + i * 4u);
    const char* out_path = "test_natives.c.out";
    FILE* out = fopen(out_path, "wb");
    CHECK(out);
    emit_set_natives(natives, 1);
    CHECK(emit_function(out, insts, 3, 0x80001000u));
    emit_set_natives(NULL, 0);
    fclose(out);
    char* text = read_text(out_path);
    CHECK(text);
    CHECK(strstr(text, "if (host_leaf(ctx)) {\n"
                       "                    if (dolrecomp_idle_pc != 0x80001004u)\n"
                       "                        goto label_80001004;\n"
                       "                    ctx->pc = 0x80001004u;\n"
                       "                    return;"));
    CHECK(strstr(text, "goto native_declined_80001008;"));
    CHECK(strstr(text, "label_80001008:\n    {\n        bool host_leaf(CPUState* ctx);"));
    free(text);

    // A jump native's callers leave to the dispatcher at the pc it set.
    static const DolRecompNative jumps[] = {{0x80001008u, "host_jump", true}};
    out = fopen(out_path, "wb");
    CHECK(out);
    emit_set_natives(jumps, 1);
    CHECK(emit_function(out, insts, 3, 0x80001000u));
    emit_set_natives(NULL, 0);
    fclose(out);
    text = read_text(out_path);
    CHECK(text);
    CHECK(strstr(text, "if (host_jump(ctx))\n                    return;"));
    CHECK(strstr(text, "if (host_jump(ctx)) {\n            return;"));
    free(text);
    remove(out_path);
    remove(path);
    return 0;
}
