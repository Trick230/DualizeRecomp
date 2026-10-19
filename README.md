# DualizeRecomp

A static recompilation of **NiGHTS: Journey of Dreams** (Wii) to native PC code.

The game's `main.dol` is translated from PowerPC to C by
[DolRecomp](https://github.com/ExpansionPak/DolRecomp), compiled into a native
module, and run on [ModernGekko](https://github.com/ExpansionPak/ModernGekko),
a runtime built on a Dolphin-derived core for video, audio and HLE.

> [!IMPORTANT]
> This repository contains **no game code, assets, disc images or keys**, and
> it does not download any. You must provide your own dump of a disc you legally
> own. Recompiled output is derived from the original game, so it is never
> committed either.

## Status

Work in progress.

| | |
|---|---|
| Supported version | NiGHTS: Journey of Dreams, USA (`R7EE8P`) |
| Recompilation | `main.dol` recompiles and builds into a native module |
| Tested platform | Windows x86_64, MSYS2 GCC 16.2, C backend |
| Other platforms | Linux and macOS should work (both tools support them) but are untested |

Other regions (PAL `R7EP8P`, JP `R7EJ8P`) have not been tested.

### Verifying your dump

The supported USA `main.dol` has this SHA-256:

```
e63c17343cf854fffc35155546536ae57bd7018dc3979575ac7c981262144c20
```

After extracting (see below), you can check it with
`sha256sum extracted/nights/sys/main.dol`.

## Requirements

- CMake, Ninja, pkg-config, Git and Python
- A C11/C++23 toolchain:
  - **Windows:** MSYS2 (MinGW GCC or Clang, tested) or Visual Studio 2022 with
    the "Desktop development with C++" workload
  - **Linux:** GCC or Clang, plus the Dolphin NoGUI dev packages:
    ```
    sudo apt-get install -y ninja-build build-essential pkg-config cmake \
      libevdev-dev libudev-dev libgtk-3-dev libsystemd-dev \
      libbluetooth-dev libasound2-dev libpulse-dev libgl1-mesa-dev \
      libxrandr-dev libxi-dev
    ```
  - **macOS:** Xcode command line tools and `brew install cmake ninja pkg-config`
- A 64-bit host
- Your own NiGHTS: Journey of Dreams (USA) disc image

Run `make check` to verify your environment. If the submodules are missing,
it offers to fetch them (`make check FETCH=1` does it without asking).

## Getting the source

```
git clone --recurse-submodules https://github.com/Trick230/DualizeRecomp.git
cd DualizeRecomp
```

> [!NOTE]
> `lib/ModernGekko` vendors a large part of Dolphin's dependency tree, so the
> first submodule sync downloads a few hundred MB.

## Building and running

1. Place your disc image at `iso/nights.iso`. The `iso/` folder is gitignored.
2. Build everything, extract the disc, recompile and launch:
   ```
   make run ISO=iso/nights.iso
   ```
   On the first run this builds DolRecomp and ModernGekko, downloads
   [Wiimms ISO Tools](https://wit.wiimm.de/) into `extern/wit` (needed for
   Wii extraction), extracts the disc into `extracted/nights/`, recompiles
   `main.dol` and compiles the native module.
3. On later runs, use the extracted copy:
   ```
   make run GAME=nights
   ```

Compiled modules are cached by DOL hash and toolchain under `build/modules/`,
so later runs skip recompilation unless something changed.

### Useful targets

| Target | Description |
|---|---|
| `make check` | Validate build tools, compilers, architecture and sources |
| `make tools` | Build DolRecomp and ModernGekko only |
| `make extract ISO=...` | Extract the disc into `extracted/nights/` |
| `make recompile GAME=nights` | Recompile and build the module without launching |
| `make run GAME=nights` | Recompile if needed and launch the game |
| `make llvm GAME=nights` | Build the module with the optimized LLVM backend |
| `make llvm-run GAME=nights` | Same, then launch |
| `make clean-extracted` | Remove the extracted disc and compiled modules |
| `make clean-tools` | Remove the DolRecomp/ModernGekko build trees |
| `make clean` | Remove all of the above |

### Variables

| Variable | Default | Description |
|---|---|---|
| `ISO` | *(none)* | Disc image path. Only needed the first time. |
| `GAME` | *(none)* | Extracted game slug (`nights`). |
| `BACKEND` | `c` | `c` (portable) or `llvm` (optimized native objects). |
| `LLVM_DIR` | *(CMake discovery)* | Path to LLVM 19/20's CMake package, if not found automatically. |
| `TOOLCHAIN` | `auto` | Module compiler: `auto`, `clang`, `gcc` or `msvc`. `auto` follows the compiler used to build ModernGekko. |
| `CMAKE_BUILD_TYPE` | `Release` | Build type for both tools. |
| `JOBS` | CPU count | Parallel build jobs. |
| `RUN_ARGS` | *(empty)* | Extra flags for the runner, e.g. `--headless` or `--graphics Vulkan`. |

Example debug build with Vulkan:

```
CMAKE_BUILD_TYPE=Debug make run GAME=nights RUN_ARGS="--graphics Vulkan"
```

## Controls

ModernGekko has no controller configuration UI. Bindings come from
Dolphin-format `Config/WiimoteNew.ini` (Wii Remote + Nunchuk / Classic
Controller) and `Config/GCPadNew.ini` (GameCube controller) in ModernGekko's
user directory. Nothing creates these files for you, so input does nothing
until you add one. A config exported from Dolphin works.

## Project layout

```
lib/DolRecomp/      submodule: PowerPC DOL -> C / LLVM static recompiler
lib/ModernGekko/    submodule: runtime the recompiled module runs on
scripts/            build environment checks
Makefile            the whole pipeline
iso/                your disc image (gitignored)
extracted/          extracted disc (gitignored)
build/              tool builds and compiled modules (gitignored)
extern/, database/  tools and title data downloaded on first run (gitignored)
```

## Credits

- [ModernGekko-Template](https://github.com/ExpansionPak/ModernGekko-Template),
  the pipeline this project is based on
- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) and
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko)
- The [Dolphin](https://dolphin-emu.org/) team
- [Wiimms ISO Tools](https://wit.wiimm.de/) and [GameTDB](https://www.gametdb.com/)

## Legal

This is an unofficial fan project. It is not affiliated with or endorsed by
SEGA or Nintendo. NiGHTS and NiGHTS: Journey of Dreams are trademarks of SEGA.
Wii is a trademark of Nintendo.

DolRecomp and ModernGekko are distributed under their own licenses (see
`lib/DolRecomp/LICENSE` and `lib/ModernGekko/LICENSE`; ModernGekko is
GPL-3.0-or-later because of its Dolphin-derived runtime).
