#!/usr/bin/env bash
# Profile-guided build: an instrumented build, uncapped training runs over
# gameplay savestates, the merged profile, the optimised build, and a
# comparison against build/release (scripts/check/perf_suite.py).
# Usage: bash scripts/pgo.sh [scenes]   (two full builds: about an hour)
# Needs compiler-rt (pacman -S mingw-w64-ucrt-x86_64-compiler-rt) and the
# game to exit cleanly when stopped (the profile is written at exit).
set -eu
SCENES="${1:-forest,gate,bluetrees,trees,dark1}"
PY="${PYTHON:-python}"
RAW=build/pgo/raw

cmake --preset pgo-generate >/dev/null
bash scripts/build.sh pgo-generate
rm -rf "$RAW"
"$PY" scripts/check/perf_suite.py --no-history --rounds 1 --secs 40 --scenes "$SCENES" \
  train=build/pgo-generate/DualizeRecomp/DualizeRecomp.exe
ls "$RAW"/*.profraw >/dev/null 2>&1 || { echo "no profile in $RAW: did the game exit cleanly?"; exit 1; }
llvm-profdata merge -o build/pgo/merged.profdata "$RAW"/*.profraw
echo "profile: $(du -h build/pgo/merged.profdata | cut -f1) from $(ls "$RAW"/*.profraw | wc -l) runs"

cmake --preset pgo >/dev/null
bash scripts/build.sh pgo
"$PY" scripts/check/perf_suite.py --rounds 2 --scenes forest,gate \
  release=build/release/DualizeRecomp/DualizeRecomp.exe pgo=build/pgo/DualizeRecomp/DualizeRecomp.exe
