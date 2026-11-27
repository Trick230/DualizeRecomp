#!/usr/bin/env bash
# Fetch the build toolchain into .tools/: pinned versions, checked by sha256,
# nothing installed on the system, and the folder can be moved or copied.
#   Windows (Git Bash): Clang and lld (llvm-mingw, UCRT), CMake, Ninja, ccache.
#   Linux: CMake, Ninja, ccache; Clang, lld and SDL3's system headers come from
#   the distribution's packages.
# scripts/build.sh puts .tools/ first on PATH by itself.
# Usage: bash scripts/setup.sh
set -euo pipefail
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
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
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
        fetch cmake "$GH/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-linux-x86_64.tar.gz" \
            83b71181fb62b4fe8f26067a9321448572e7a43bd04ff6c7f54a24c33698f16d
        fetch ninja "$GH/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip" \
            5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
        fetch ccache "$GH/ccache/ccache/releases/download/v4.14.3/ccache-4.14.3-linux-x86_64-glibc.tar.xz" \
            c94a5688546883eee4c87764b34fa2eb18a4879421a51589c37538aed6312dfc
        if ! command -v clang >/dev/null || ! command -v ld.lld >/dev/null; then
            echo "Clang and lld are missing. On Debian or Ubuntu:"
            echo "  sudo apt install clang lld libasound2-dev libpulse-dev libx11-dev libxext-dev \\"
            echo "    libxrandr-dev libxcursor-dev libxi-dev libxss-dev libwayland-dev libxkbcommon-dev libudev-dev"
        fi
        ;;
    *) echo "unsupported system: $(uname -s)"; exit 1 ;;
esac
echo "toolchain ready in .tools/; build with: bash scripts/build.sh release"
