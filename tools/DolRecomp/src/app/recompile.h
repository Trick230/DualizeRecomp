#ifndef DOLRECOMP_APP_RECOMPILE_H
#define DOLRECOMP_APP_RECOMPILE_H

#include "app/config.h"

// Recompiles the game a config describes into out_dir:
//   ppc_recomp.h          declarations and the dispatcher
//   chunks/*.c            the code, one file per chunk
//   ppc_recomp.<n>.c      config.shards files that include the chunks, so the
//                         build knows its sources before the recompiler runs
//   ppc_tables.inc        code and chunk ranges plus each chunk's text hash
int recompile(const RecompConfig* config, const char* game_dir, const char* out_dir);

#endif
