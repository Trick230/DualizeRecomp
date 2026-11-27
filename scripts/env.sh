# Puts the tools from scripts/setup.sh (.tools/) first on PATH: Clang, CMake,
# Ninja, ccache and Python. scripts/build.sh sources it; for a shell of your
# own: source scripts/env.sh
_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
for _d in "$_root/.tools/python/Scripts" "$_root/.tools/python/bin" "$_root/.tools/python" \
          "$_root/.tools/ccache" "$_root/.tools/ninja" "$_root/.tools/cmake/bin" "$_root/.tools/llvm-mingw/bin"; do
    [ -d "$_d" ] && PATH="$_d:$PATH"
done
export PATH
unset _root _d
