#!/usr/bin/env bash
# Configure when needed, build, and print only the errors.
# Usage: bash scripts/build.sh [preset] [extra cmake --build args]
# The toolchain from scripts/setup.sh (.tools/) goes first on PATH when it is
# there. The build directory is configured again by itself when it has none,
# when it was moved or copied from elsewhere (its cache names the old place),
# and when the game appeared since a tools-only configure.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PRESET="${1:-release}"
[ $# -gt 0 ] && shift
BUILD_DIR="${BUILD_DIR:-build/${PRESET}}"

T="$ROOT/.tools"
for d in "$T/llvm-mingw/bin" "$T/cmake/bin" "$T/ninja" "$T/ccache"; do
    [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH

# The directory as CMake writes it (D:/... on Windows, where case and the
# drive letter's case don't matter).
here() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }
same_dir() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) [ "$(printf %s "$1" | tr A-Z a-z)" = "$(printf %s "$2" | tr A-Z a-z)" ] ;;
        *) [ "$1" = "$2" ] ;;
    esac
}

CACHE="$BUILD_DIR/CMakeCache.txt"
CONFIGURE=""
if [ ! -f "$CACHE" ] || [ ! -f "$BUILD_DIR/build.ninja" ]; then
    CONFIGURE=new
elif ! same_dir "$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$CACHE")" "$(here "$BUILD_DIR")"; then
    CONFIGURE=moved
elif ! grep -q '^DUALIZE_CPU_TARGET:' "$CACHE"; then
    GAME="$(sed -n 's/^DUALIZE_GAME_DIR:[A-Z]*=//p' "$CACHE")"
    case "$GAME" in /*|[A-Za-z]:*) ;; *) GAME="$ROOT/$GAME" ;; esac
    [ -f "$GAME/sys/main.dol" ] && CONFIGURE=game
fi
if [ -n "$CONFIGURE" ]; then
    echo "configuring $BUILD_DIR ($CONFIGURE)"
    ARGS=(--preset "$PRESET")
    [ "$BUILD_DIR" != "build/$PRESET" ] && ARGS+=(-B "$BUILD_DIR")
    # A moved tree's cache points at the old place: start it over (objects
    # come back from ccache).
    [ "$CONFIGURE" = moved ] && ARGS+=(--fresh)
    CLOG="$(mktemp)"
    if ! cmake "${ARGS[@]}" >"$CLOG" 2>&1; then
        echo "CONFIGURE FAILED. First errors:"
        grep -E -m 15 -A4 'CMake Error' "$CLOG"
        echo "--- full log: $CLOG"
        exit 1
    fi
    grep -E -A2 'CMake Warning' "$CLOG" | head -8
fi

LOG="$(mktemp)"
cmake --build "$BUILD_DIR" "$@" >"$LOG" 2>&1
STATUS=$?

if [ "$STATUS" -eq 0 ]; then
  echo "BUILD OK"
  echo "warnings: $(grep -c 'warning:' "$LOG")"
else
  echo "BUILD FAILED (exit $STATUS). First errors:"
  grep -E -m 15 -B1 -A3 'error:|fatal error:|undefined reference|ld.lld: error|FAILED:' "$LOG"
  echo "--- full log (grep it, don't cat it): $LOG"
fi
exit "$STATUS"
