#include "app/config.h"

#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_string(char* out, size_t size, toml_datum_t value, const char* key) {
    if (!value.ok) {
        fprintf(stderr, "error: config needs %s\n", key);
        return 0;
    }
    const int fits = strlen(value.u.s) < size;
    if (fits)
        strcpy(out, value.u.s);
    else
        fprintf(stderr, "error: config %s is too long\n", key);
    free(value.u.s);
    return fits;
}

static int read_u32(toml_datum_t value, u32* out, const char* key) {
    if (!value.ok || value.u.i < 0 || value.u.i > 0xFFFFFFFFll) {
        fprintf(stderr, "error: config %s must be a 32-bit unsigned integer\n", key);
        return 0;
    }
    *out = (u32)value.u.i;
    return 1;
}

static int load_rso(RecompConfig* config, const toml_table_t* rso) {
    if (!copy_string(config->sel, sizeof(config->sel), toml_string_in(rso, "sel"), "rso.sel"))
        return 0;

    const toml_array_t* sections = toml_array_in(rso, "static_sections");
    const int section_count = sections ? toml_array_nelem(sections) : -1;
    if (section_count <= 0 || (u32)section_count > RECOMP_MAX_STATIC_SECTIONS) {
        fprintf(stderr, "error: config rso.static_sections needs 1..%u addresses\n",
                RECOMP_MAX_STATIC_SECTIONS);
        return 0;
    }
    for (int i = 0; i < section_count; i++) {
        if (!read_u32(toml_int_at(sections, i), &config->static_sections[i],
                      "rso.static_sections"))
            return 0;
    }
    config->static_section_count = (u32)section_count;

    const toml_array_t* modules = toml_array_in(rso, "modules");
    const int module_count = modules ? toml_array_nelem(modules) : -1;
    if (module_count <= 0 || (u32)module_count > RECOMP_MAX_RSO_MODULES) {
        fprintf(stderr, "error: config rso.modules needs 1..%u modules\n", RECOMP_MAX_RSO_MODULES);
        return 0;
    }
    for (int i = 0; i < module_count; i++) {
        const toml_table_t* entry = toml_table_at(modules, i);
        RecompRsoModule* module = &config->modules[i];
        if (!entry ||
            !copy_string(module->file, sizeof(module->file), toml_string_in(entry, "file"),
                         "rso.modules.file") ||
            !read_u32(toml_int_in(entry, "base"), &module->base, "rso.modules.base"))
            return 0;
        module->bss = 0;
        if (toml_key_exists(entry, "bss") &&
            !read_u32(toml_int_in(entry, "bss"), &module->bss, "rso.modules.bss"))
            return 0;
    }
    config->module_count = (u32)module_count;
    return 1;
}

int config_load(RecompConfig* config, const char* path) {
    memset(config, 0, sizeof(*config));
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "error: can't open config '%s'\n", path);
        return 0;
    }
    char error[256];
    toml_table_t* root = toml_parse_file(file, error, sizeof(error));
    fclose(file);
    if (!root) {
        fprintf(stderr, "error: %s: %s\n", path, error);
        return 0;
    }

    int ok = 0;
    const toml_table_t* main = toml_table_in(root, "main");
    if (!main) {
        fprintf(stderr, "error: config needs a [main] table\n");
        goto done;
    }
    if (!copy_string(config->dol, sizeof(config->dol), toml_string_in(main, "dol"), "main.dol"))
        goto done;

    if (toml_key_exists(main, "natives")) {
        // Relative to the config file.
        char natives[512];
        if (!copy_string(natives, sizeof(natives), toml_string_in(main, "natives"), "main.natives"))
            goto done;
        const char* slash = strrchr(path, '/');
        const char* backslash = strrchr(path, '\\');
        if (backslash > slash)
            slash = backslash;
        const int dir = slash ? (int)(slash - path + 1) : 0;
        if (snprintf(config->natives, sizeof(config->natives), "%.*s%s", dir, path, natives) >=
            (int)sizeof(config->natives)) {
            fprintf(stderr, "error: natives path is too long\n");
            goto done;
        }
    }

    config->shards = 64;
    if (toml_key_exists(main, "shards") &&
        (!read_u32(toml_int_in(main, "shards"), &config->shards, "main.shards") ||
         config->shards == 0)) {
        fprintf(stderr, "error: config main.shards must be at least 1\n");
        goto done;
    }

    const toml_table_t* rso = toml_table_in(root, "rso");
    ok = !rso || load_rso(config, rso);

done:
    toml_free(root);
    return ok;
}
