#!/usr/bin/env bash
# Fetch the build toolchain into .tools/: pinned versions, checked by sha256,
# nothing installed on the system, and the folder can be moved or copied.
#   Windows (Git Bash): Clang and lld (llvm-mingw, UCRT), CMake, Ninja, ccache.
#   Linux: CMake, Ninja, ccache; Clang, lld and SDL3's system headers come from
#   the distribution's packages.
#   Both: Python (a relocatable CPython build) with the scripts' packages
#   (scripts/requirements.txt), and with --re Ghidra, its GameCube/Wii loader
#   and the JDK it runs on.
# scripts/build.sh puts .tools/ first on PATH by itself; for a shell of your
# own: source scripts/env.sh
# Usage: bash scripts/setup.sh [--re]
set -euo pipefail
RE=""
[ "${1:-}" = --re ] && RE=1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/.tools"
DL="$T/dl"
mkdir -p "$DL"

# fetch NAME URL SHA256: download (or reuse) the archive, check it, and
# unpack it into .tools/NAME, without its top-level directory.
fetch() {
    local name=$1 url=$2 sum=$3 file="$DL/${2##*/}" tmp="$T/$1.tmp"
    if [ -f "$T/$name/.sha256" ] && [ "$(cat "$T/$name/.sha256")" = "$sum" ]; then
        echo "$name: up to date"
        return
    fi
    if ! { [ -f "$file" ] && echo "$sum  $file" | sha256sum -c --status -; }; then
        echo "$name: downloading ${url##*/}"
        curl -fSL --retry 3 -o "$file" "$url"
        echo "$sum  $file" | sha256sum -c --status - || { echo "$name: checksum mismatch, $file"; exit 1; }
    fi
    rm -rf "$tmp"
    mkdir -p "$tmp"
    case "$file" in
        *.zip) unzip -q "$file" -d "$tmp" ;;
        *) tar -xf "$file" -C "$tmp" ;;
    esac
    local entries=("$tmp"/*)
    rm -rf "$T/$name"
    if [ ${#entries[@]} -eq 1 ] && [ -d "${entries[0]}" ]; then
        mv "${entries[0]}" "$T/$name"
        rmdir "$tmp"
    else
        mv "$tmp" "$T/$name"
    fi
    echo "$sum" > "$T/$name/.sha256"
    echo "$name: ready"
}

GH=https://github.com
PBS="$GH/astral-sh/python-build-standalone/releases/download/20261124/cpython-3.11.18%2B20261124"
GHIDRA="$GH/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1_build/ghidra_12.1_PUBLIC_20260513.zip"
GHIDRA_SUM=aa5cbcbbf48f41ca185fce900e19592f1ade4cd5994eb6e0ede468dac8a6f302
JDK="$GH/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jdk_x64"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        fetch python "$PBS-x86_64-pc-windows-msvc-install_only.tar.gz"             3e4a35e3c238e5090d36bf95c041d9052bc82d6e298cb912ba7f70f6c66cf81a
        PY="$T/python/python.exe"
        if [ -n "$RE" ]; then
            fetch jdk "${JDK}_windows_hotspot_21.0.13_9.zip"                 a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9
        fi
        fetch llvm-mingw "$GH/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip" \
            4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412
        fetch cmake "$GH/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-windows-x86_64.zip" \
            bfe6eadb245dc8213e0d3feea316b520cb2aae92f5667ffd76059ba6b05c430c
        fetch ninja "$GH/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip" \
            07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65
        fetch ccache "$GH/ccache/ccache/releases/download/v4.14.3/ccache-4.14.3-windows-x86_64.zip" \
            c845c3d65be81954c75659145a522a793a27586bf940c5dcfe18af33fa1a67dd
        ;;
    Linux)
        [ "$(uname -m)" = x86_64 ] || { echo "only x86-64 Linux is supported"; exit 1; }
        fetch python "$PBS-x86_64-unknown-linux-gnu-install_only.tar.gz"             ef0764e884b5e123692281b91bf388f8ebfd0afb41d6a212bb4f2779e51abf0e
        PY="$T/python/bin/python3"
        [ -e "$T/python/bin/python" ] || ln -s python3 "$T/python/bin/python"
        if [ -n "$RE" ]; then
            fetch jdk "${JDK}_linux_hotspot_21.0.13_9.tar.gz"                 d7f5d5cf865a2902b70ff549b97586a7d3f234e016f041ab447ea740c7d66d1c
        fi
        fetch cmake "$GH/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-linux-x86_64.tar.gz" \
            83b71181fb62b4fe8f26067a9321448572e7a43bd04ff6c7f54a24c33698f16d
        fetch ninja "$GH/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip" \
            5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
        fetch ccache "$GH/ccache/ccache/releases/download/v4.14.3/ccache-4.14.3-linux-x86_64-glibc.tar.xz" \
            c94a5688546883eee4c87764b34fa2eb18a4879421a51589c37538aed6312dfc
        if ! command -v clang >/dev/null || ! command -v ld.lld >/dev/null; then
            echo "Clang and lld are missing. On Debian or Ubuntu:"
            echo "  sudo apt install clang lld libasound2-dev libpulse-dev libx11-dev libxext-dev \\"
            echo "    libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libwayland-dev libxkbcommon-dev libudev-dev"
        fi
        ;;
    *) echo "unsupported system: $(uname -s)"; exit 1 ;;
esac
if [ -n "$RE" ]; then
    fetch ghidra "$GHIDRA" "$GHIDRA_SUM"
    # Ghidra's GameCube/Wii loader and the Gekko/Broadway language.
    fetch gcloader "$GH/Cuyler36/Ghidra-GameCube-Loader/releases/download/1.3.1/GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip"         9892f28fc1e7f19bb3fcec7cfb4ad2ab7504503b571d93de8b142a6f7bec8ed0
    EXT="$T/ghidra/Ghidra/Extensions/GameCubeLoader"
    cmp -s "$T/gcloader/.sha256" "$EXT/.sha256" || { rm -rf "$EXT"; cp -r "$T/gcloader" "$EXT"; }
fi

# The scripts' packages, at the versions in requirements.txt, installed into
# that Python itself (no virtual environment: those keep absolute paths).
REQ="$ROOT/scripts/requirements.txt"
STAMP="$T/python/.requirements"
if ! cmp -s "$REQ" "$STAMP"; then
    echo "python: installing scripts/requirements.txt"
    "$PY" -m pip install -q --disable-pip-version-check --no-warn-script-location -r "$REQ"
    cp "$REQ" "$STAMP"
fi
echo "tools ready in .tools/; build with: bash scripts/build.sh release"
