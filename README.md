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
| Boot | Boots to the Sonic Team logo and the intro cutscene; all guest code runs natively (no interpreter/JIT fallback) |
| Startup | ~6 s to the first frame (asset hash is cached after the first boot) |
| Speed | Gameplay holds full speed (30 FPS, the game's native rate) with brief dips while areas load; the intro FMV runs at ~19 FPS (CPU bound in the video decoder) |
| Tested platform | Windows x86_64, MSYS2 Clang 22 (C backend, `-O3`, ThinLTO), Vulkan, i5-9600KF + RTX 4060 |
| Other platforms | Linux and macOS should work (both tools support them) but are untested |
| Input | Xbox-layout XInput pad mapped to an emulated Classic Controller (see [Controls](#controls)); tested with an 8BitDo Ultimate 2 |

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
  - **Windows:** MSYS2 UCRT64 (tested; install `mingw-w64-ucrt-x86_64-clang`
    and `mingw-w64-ucrt-x86_64-lld` for the fastest build) or Visual Studio 2022 with
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
| `TOOLCHAIN` | `clang` if installed, else `auto` | Module compiler: `auto`, `clang`, `gcc` or `msvc`. Clang builds use ThinLTO and are the fastest. `auto` follows the compiler used to build ModernGekko. |
| `OPT_LEVEL` | `3` | Module optimization level (0-3). |
| `USER_DIR` | ModernGekko's user directory | Where `make run` installs `config/`. |
| `CMAKE_BUILD_TYPE` | `Release` | Build type for both tools. |
| `JOBS` | CPU count | Parallel build jobs. |
| `RUN_ARGS` | *(empty)* | Extra flags for the runner, e.g. `--headless` or `--graphics Vulkan`. |

Example debug build with Vulkan:

```
CMAKE_BUILD_TYPE=Debug make run GAME=nights RUN_ARGS="--graphics Vulkan"
```

## Controls

NiGHTS supports the Classic Controller, so `make run` installs
`config/Config/WiimoteNew.ini`: Wii Remote 1 with a Classic Controller attached,
driven by the first XInput pad (any Xbox controller, or an 8BitDo pad in
X-input mode). Buttons follow the physical Xbox layout:

| Xbox | Classic Controller |
|---|---|
| A / B / X / Y | b / a / y / x |
| Left / right stick | Left / right stick |
| LT / RT | L / R |
| LB / RB | ZL / ZR |
| D-pad | D-pad |
| Back / Start / Guide | − / + / Home |

The profile is only installed when ModernGekko's user directory has no
`Config/WiimoteNew.ini` yet, so your own remaps are kept. It is a regular
Dolphin-format file, so one exported from Dolphin works too.

## Project-specific fixes

Both are applied automatically by `make`.

- **`patches/ModernGekko/0001`**: the startup asset hash computes each file's
  relative path once instead of inside the sort comparator (without it,
  startup on Windows hangs for well over 5 minutes on NiGHTS' 12k files), and
  caches the result next to `files/`, keyed by every file's path, size and
  modification time. Unchanged game files are not re-read on every boot.
- **`patches/ModernGekko/0002`**: only wires the mod host-call hooks into the
  recompiler core when mods are actually loaded. Otherwise every dispatched
  block queried the mod manager (~6% of the CPU thread in gameplay).
- **`patches/ModernGekko-dolphin/0001`**: faster paired-single paths in
  GXRuntime, the CPU runtime compiled into the game module. Quantized `psq_st`
  uses exact power-of-two multiplies instead of libm `ldexp` and is handled
  inline, and `ps_add` gets an inline path for finite inputs. Results are
  bit-identical to the interpreter-accurate versions; this took the intro FMV
  from ~12 to ~19 FPS.
- **`config/GameSettings/R7EE8P.ini`**: per-game Dolphin settings, copied to
  ModernGekko's user directory by `make run` (override with `USER_DIR=`). It
  enables idle-loop skipping for the game's `for (;;) OSWakeupThread(...)`
  idle thread, which otherwise holds the game at ~10 FPS, and turns on dual
  core (`CPUThread`), which this Dolphin fork leaves off on desktop. That
  moves GPU emulation (FIFO, vertex loading, texture hashing) to its own
  thread and took gameplay from ~24 to a steady 30 FPS.

## Project layout

```
lib/DolRecomp/      submodule: PowerPC DOL -> C / LLVM static recompiler
lib/ModernGekko/    submodule: runtime the recompiled module runs on
patches/            local fixes applied to the submodules
config/             per-game runtime settings
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
