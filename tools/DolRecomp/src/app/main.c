#include "app/config.h"
#include "app/recompile.h"
#include "app/setup.h"
#include "frontend/container/disc_extract.h"

#include <stdio.h>
#include <string.h>

static void usage(void) {
    printf("usage: dolrecomp <config.toml> <extracted game> <output dir>\n"
           "       dolrecomp extract <iso> <output dir>\n"
           "       dolrecomp --setup    download Wiimms ISO Tools for extract\n");
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "extract") == 0)
        return disc_extract_main(argc - 1, argv + 1);
    if (argc == 2 && strcmp(argv[1], "--setup") == 0)
        return run_setup() ? 0 : 1;
    if (argc != 4) {
        usage();
        return argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) ? 0 : 1;
    }

    RecompConfig config;
    if (!config_load(&config, argv[1]))
        return 1;
    return recompile(&config, argv[2], argv[3]) ? 0 : 1;
}
