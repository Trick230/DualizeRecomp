# DualizeRecomp

A static recompilation of **NiGHTS: Journey of Dreams** (Wii) to native PC code.

The game's `main.dol` is translated from PowerPC to C by
[DolRecomp](https://github.com/ExpansionPak/DolRecomp) and compiled into a
native module. The module runs on
[ModernGekko](https://github.com/ExpansionPak/ModernGekko), a runtime built on
a Dolphin-derived core. Every guest instruction runs as native code, with no
interpreter or JIT fallback. The long-term goal is a fully native port that
runs at high, interpolated frame rates, with Dolphin reduced to nothing (see
[Roadmap](#roadmap)).

> [!IMPORTANT]
> This repository contains **no game code, assets, disc images or keys**, and
> it does not download any. You must provide your own dump of a disc you legally
> own. Recompiled output is derived from the original game, so it is never
> committed either.

## Status

Work in progress, but playable from boot through the intro, Story Select and
gameplay.

| | |
|---|---|
| Supported version | NiGHTS: Journey of Dreams, USA (`R7EE8P`) |
| Gameplay | Full speed: 30 FPS, the game's native rate |
| Renderers | Dolphin's Vulkan/OpenGL backends (default), or the experimental [native renderer](#native-renderer) |
| Startup | ~6 s to the first frame (the asset hash is cached after the first boot) |
| Input | Xbox-layout XInput pad mapped to an emulated Classic Controller (see [Controls](#controls)) |
| Tested on | Windows 11 x86_64, MSYS2 UCRT64 (Clang for the game module, `-O3`), i5-9600KF + RTX 4060 |
| Untested | Linux and macOS (both tools support them), PAL `R7EP8P` and JP `R7EJ8P` |

Known limitations:

- The intro FMV runs at ~19 FPS. It is CPU-bound in the game's video decoder.
- Without a frame cap, gameplay reaches ~62 FPS (it was ~32 before phase 3).
  The bottleneck is still the emulated CPU thread: the game's own OS layer
  (thread switching, message queues) and Dolphin's timing and hardware
  emulation. The GPU and the native renderer's thread are mostly idle. The
  rest of phase 3 goes after this.

## Quick start

```
git clone --recurse-submodules https://github.com/Trick230/DualizeRecomp.git
cd DualizeRecomp
make check                                   # verify your toolchain
make run ISO=iso/nights.iso                  # first run: build, extract, recompile, play
make run GAME=nights                         # later runs
make run GAME=nights RUN_ARGS="--graphics Native"   # with the native renderer
```

The sections below cover each step.

### Requirements

- CMake, Ninja, pkg-config, Git and Python 3
- A C11/C++23 toolchain:
  - **Windows:** MSYS2 UCRT64 (tested). Install `mingw-w64-ucrt-x86_64-clang`
    and `mingw-w64-ucrt-x86_64-lld` for the fastest module build, and
    `mingw-w64-ucrt-x86_64-abseil-cpp` for the native renderer. Visual Studio
    2022 with the "Desktop development with C++" workload also works.
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

`make check` validates all of this. If the submodules are missing, it offers to
fetch them (`make check FETCH=1` does it without asking).

> [!NOTE]
> `lib/ModernGekko` vendors a large part of Dolphin's dependency tree, so the
> first submodule sync downloads a few hundred MB.

### First run

1. Place your disc image at `iso/nights.iso` (the `iso/` folder is gitignored).
2. Run `make run ISO=iso/nights.iso`. The first time, this:
   - builds DolRecomp and ModernGekko, applying the local [patches](#patches);
   - downloads [Wiimms ISO Tools](https://wit.wiimm.de/) into `extern/wit`;
   - extracts the disc into `extracted/nights/`;
   - recompiles `main.dol` and compiles the native module;
   - installs the per-game settings and controller profile, then launches the game.
3. On later runs, use `make run GAME=nights`.

Compiled modules are cached by DOL hash and toolchain under `build/modules/`,
so later runs skip recompilation unless something changed.

To check your dump, compare `sha256sum extracted/nights/sys/main.dol` with:

```
e63c17343cf854fffc35155546536ae57bd7018dc3979575ac7c981262144c20
```

### Make targets

| Target | Description |
|---|---|
| `make check` | Validate build tools, compilers, architecture and sources |
| `make tools` | Build DolRecomp and ModernGekko only |
| `make extract ISO=...` | Extract the disc into `extracted/nights/` |
| `make recompile GAME=nights` | Recompile and build the module without launching |
| `make run GAME=nights` | Recompile if needed and launch the game |
| `make llvm` / `make llvm-run` | Same as `recompile` / `run`, with the LLVM backend |
| `make symbols GAME=nights` | Identify the SDK functions in `main.dol` (writes `build/symbols/nights.map`) |
| `make clean-extracted` | Remove the extracted disc and compiled modules |
| `make clean-tools` | Remove the DolRecomp/ModernGekko build trees |
| `make clean` | Remove all of the above |

### Variables

| Variable | Default | Description |
|---|---|---|
| `ISO` | *(none)* | Disc image path. Only needed the first time. |
| `GAME` | *(none)* | Extracted game slug (`nights`). |
| `BACKEND` | `c` | `c` (portable) or `llvm` (native objects). |
| `LLVM_DIR` | *(CMake discovery)* | Path to the CMake package of LLVM 19 or newer (22 tested), if it isn't found automatically. |
| `TOOLCHAIN` | `clang` if installed, else `auto` | Module compiler: `auto`, `clang`, `gcc` or `msvc`. Clang builds are the fastest. `auto` follows the compiler that built ModernGekko. |
| `OPT_LEVEL` | `3` | Module optimization level (0-3). |
| `USER_DIR` | ModernGekko's user directory | Where `make run` installs `config/`. |
| `CMAKE_BUILD_TYPE` | `Release` | Build type for both tools. |
| `JOBS` | CPU count | Parallel build jobs. |
| `RUN_ARGS` | *(empty)* | Extra runner flags, e.g. `--graphics Native`, `--headless` or `--load-state file.sav`. |

## Native renderer

`RUN_ARGS="--graphics Native"` replaces Dolphin's emulated GPU with
[Aurora](https://github.com/encounter/aurora). Aurora is a GX implementation
on WebGPU through Dawn, using D3D12, Vulkan or Metal.

```
 game module (native PowerPC→C)          render thread (libmoderngekko_native_gx)
 ┌──────────────────────────────┐        ┌──────────────────────────────────────┐
 │ GX SDK writes to the gather  │  SPSC  │ decode the retail GX FIFO            │
 │ pipe ──► GPFifo burst sink   ├──ring─►│ ──► Aurora (pipelines, TEV shaders)  │
 │ OS / IOS / audio: Dolphin    │◄───────┤ ──► Dawn ──► D3D12 / Vulkan / Metal  │
 └──────────────────────────────┘ draw-  └──────────────────────────────────────┘
                                  done / token markers
```

The game's GX command stream comes straight from the gather pipe and is drawn
on a dedicated thread, in its own window. Dolphin still runs the OS, IOS and
audio side. This renderer is the foundation for interpolated high frame
rates.

- **Speed:** gameplay holds 30 FPS while the render thread is busy only ~15%
  of the time. Draws built from unchanged GX state reuse the previous
  pipeline, bind groups and uniforms, and consecutive draws merge into one, so
  a dense level goes from ~45k to ~1k GPU draws per frame.
- **Savestates:** the FIFO decoder's registers and any incomplete packet are
  saved with the state.
- **Stats:** `NATIVE_GX_STATS=1` prints a line once per second with:
  - frame, draw and FIFO counters;
  - per-frame buffer peaks;
  - the render thread's decode / begin-frame / end-frame / idle time split.

> [!WARNING]
> Overlays that hook the swap chain, such as RivaTuner Statistics Server or
> Steam's frame pacer, can hang or crash the native renderer at startup. Close
> them or exclude the game if that happens.

## Controls

NiGHTS supports the Classic Controller. `make run` installs
`config/Config/WiimoteNew.ini`, which sets up Wii Remote 1 with a Classic
Controller attached, driven by the first XInput pad (any Xbox controller, or
an 8BitDo pad in X-input mode). Buttons follow the physical Xbox layout:

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
Dolphin-format file, so one exported from Dolphin also works.

## Roadmap

The goal is a fully native port at high, interpolated frame rates. Dolphin is
replaced piece by piece rather than extended.

| Phase | | Status |
|---|---|---|
| 1. SDK symbol map | Identify the Nintendo SDK functions in `main.dol` (no map ships with the game) using Dolphin's signature database | Done: 1,369 strong + 253 range matches |
| 2. Native GPU | Draw the game's GX stream with Aurora instead of Dolphin's video core | Done (FIFO level): the native renderer |
| 3. Native OS/SDK | Threads, alarms, interrupts, video interface, disc/file system, Wii Remote and audio as native code, then drop the Dolphin core | In progress: CPU-side groundwork and first native SDK functions (32 → 62 FPS uncapped) |
| 4. Interpolation | Render between game ticks for 60–240 FPS gameplay | Planned |

## Patches

`make` applies these to the submodules automatically.

**ModernGekko**

- **`0001` Cached asset hash.** The startup asset hash computes each file's
  relative path once instead of inside the sort comparator. Without this,
  startup on Windows hangs for well over 5 minutes on NiGHTS' 12k files. The
  result is cached next to `files/`, keyed by every file's path, size and
  modification time.
- **`0002` No mod hooks without mods.** The mod host-call hooks are only wired
  into the recompiler core when mods are loaded. Otherwise every dispatched
  block queried the mod manager, which cost ~6% of the CPU thread.
- **`0003` Native renderer host.** A separate shared library (`src/native_gx`)
  keeps Aurora's dependency tree apart from Dolphin's.
  - It receives the FIFO through a lock-free ring, woken through an SDL
    semaphore.
  - It reports draw-done and token markers back to Dolphin's pixel engine.
  - Guest addresses resolve together with the bytes left in their memory
    region, so indexed vertex arrays and matrix loads see their whole array.
  - SDL input stays off: the emulator reads the controllers itself, and
    window events are pumped at most every 16 ms while idle.
- **`0004` CPU ABI 5 and a stricter module cache key.** Mirrors the new
  RAM slice tables in ModernGekko's `CPUState`. The module cache key also
  hashes the module template and GXRuntime's CPU sources, so editing them no
  longer reuses a stale module.
- **`0005` RSO recompilation.** Much of NiGHTS' code ships in RSO modules
  (`files/rso`) that the game loads into the heap at runtime, so it used to
  run on Dolphin's JIT, with a full state sync every time it called into the
  DOL and back (~300k times a second). `moderngekko-port --rso-layout`
  links each module ahead of time at the address the game's loader uses
  (`config/rso/R7EE8P.layout`) and hands its code to the recompiler as extra
  DOL text. The chunk hash check keeps it native only while RAM holds exactly
  those bytes, so a module that loads elsewhere just stays on the JIT. This
  took uncapped gameplay from ~48 to ~57 FPS.
- **`0006` Host-CPU modules.** Modules are compiled for the machine that
  builds them (`-march=native` on x86-64), so the inline FP paths use a
  hardware `fma`.
- **`0007` Native SDK functions.** `moderngekko-port --natives <list>` passes
  the list to DolRecomp and links the host implementations in
  `src/module_natives` into the module. `make` uses
  `config/natives/<disc id>.natives` when it exists. NiGHTS replaces the
  Revolution SDK's mutex lock/unlock, interrupt enable/disable/restore and
  current-thread lookup. Each native handles the common case (a free or
  recursively owned mutex, an unlock that wakes nobody) and declines the rest
  to the guest code. Uncapped gameplay went from ~57 to ~62 FPS.
- **`0008` Guest calls from automation.** The automation directory gains
  `command=call address=<fn> args=<r3>,<r4>,... [thread=<OSThread>]
  [async=1]`, which runs a guest function and writes its r3/f1. A plain call
  runs on the interpreter with the CPU paused and puts every register back; an
  async one diverts the guest thread into a small trampoline at 0x80001900 and
  lets the game run, so the callee can block or wait on hardware.
  `scripts/automation.py` drives the directory from Python.
- **`0009` Native scheduler and memory routines.** The thread switch itself
  goes native: `OSSaveContext`, `OSLoadContext` and `SelectThread`'s switching
  path (it declines when nothing switches, on the idle wait, and under a
  non-default switch callback) and `OSSleepThread`, plus `OSWakeupThread`
  when the woken threads don't outrank the caller, `OSTryLockMutex`,
  `PSMTXCopy`/`PSMTXConcat` (the guest's paired-single sequence replayed
  through the same helpers, minus the per-instruction checks), and the
  runtime's byte-wise `memcpy`/`__fill_mem`. Uncapped gameplay went from ~60
  to ~65 FPS. The port also points the module link at a ThinLTO cache in
  `build/modules/<disc id>/thinlto-cache`.
- **`0010` Classic Controller automation.** `pad`/`pad_frames` take
  `classic_*` controls (the game's profile uses a Classic Controller), so
  scripts can play: `Game.pad(frames, classic_ly=1.0)`.

**ModernGekko's Dolphin fork**

- **`0001` Fast paired singles.** Quantized `psq_st` uses exact power-of-two
  multiplies instead of libm `ldexp`, and `ps_add` gets an inline path for
  finite inputs. Results are bit-identical to the accurate versions. This took
  the intro FMV from ~12 to ~19 FPS.
- **`0002` Native GX sink and Aurora retail fixes.**
  - The gather-pipe burst sink, plus MinGW build fixes for Aurora.
  - Retail FIFO fixes:
    - 24-bit texture, copy and TLUT addresses (Wii MEM2);
    - viewport depth clamped to [0, 1] and placed from the stream's scissor
      offset;
    - a 1e-7 near-plane margin for the game's 2D and FMV quads;
    - a disabled TEV texture order reads white, as on hardware;
    - mip-filtered textures size their buffer by level count, not by highest
      LOD index. This fixed black hair, outfits and eyes on characters.
  - Speed:
    - Retail textures are cached by address and content hash.
    - Vertex arrays are uploaded once per frame per address until the game
      invalidates the vertex cache.
    - Identical uniform blocks are shared within a frame.
    - Indexed draws size their arrays by scanning only the indexed attributes.
- **`0003` Direct cross-chunk calls.** Generated code calls into another
  verified chunk directly instead of returning to the chassis for every
  cross-chunk `bl`/`blr`. The chassis publishes which chunks qualify (verified,
  not REL-relocated, no mod hooks, lockstep off) and keeps that table in step
  with the SMC guard. Dispatches per run dropped from 536M to 145M.
- **`0004` One-load guest RAM access.** `CPUState` carries a host pointer for
  each 8 MiB guest slice that lies wholly in MEM1/MEM2, so a load or store is
  one table lookup instead of ~25 instructions of range, reservation and
  journal checks. MMIO, journaled stores and stores under a live `lwarx`
  reservation take an out-of-line path with the old semantics.
- **`0005` Data-only cache flushes.** With the data cache off, `dcbst`/`dcbf`/
  `dcbi` only invalidate an icache line, so lines that hold no compiled code
  skip it.
- **`0006` Inline FP paths and host ISA.** The CPU runtime side of DolRecomp's
  `0002`: always-inline FPU and paired-single fast paths for finite operands,
  and memory accessors that take the instruction address.
- **`0007` Precomputed code lines.** That code-line check reads a bitmap of
  DOL chunk lines plus the loaded REL sections, instead of resolving chunks
  twice per flushed line (whose miss path rescanned RAM for REL headers).
- **`0008` Extra module sources.** The module template links
  `MODULE_EXTRA_SOURCES`, which carries the native SDK functions.
- **`0009` Module ThinLTO with lld.** MSYS2's Clang has no LLVMgold plugin, so
  the IPO check always failed and modules linked without cross-unit inlining.
  The template now links with `ld.lld`, runs 4 ThinLTO jobs (one per core ran a
  16 GiB machine out of memory) and takes an optional
  `RECOMPCORE_MODULE_LTO_CACHE`.
- **`0010` Cheaper data-cache flushes.** With the data cache off, a flushed
  line only syncs the MSR when it holds code and needs an invalidation.

**DolRecomp, as vendored in ModernGekko's Dolphin fork**

- **`0001` Direct calls, inline cache ops, LLVM 22.** The C emitter's side of
  direct cross-chunk calls; `dcbf`/`dcbst`/`dcbi` emitted inline instead of
  ending the block in a fallback; and the LLVM backend builds against
  LLVM 21/22. (The LLVM backend runs this game ~20% slower than the C one, so
  `BACKEND=c` stays the default.)
- **`0002` pc only on slow paths, inline FP.** Memory accesses pass the
  instruction address instead of storing `ctx->pc` before each one, and
  arithmetic FPU/paired-single instructions get inline paths for finite
  operands.
- **`0003` Native guest functions.** `--natives <list>` names guest functions
  with host implementations (`bool name(CPUState*)`). A `bl` to one calls it
  and resumes after the call if it accepts; its entry checks it for tail and
  indirect calls. Every entry carries the FNV-1a hash of the guest bytes it
  replaces (`scripts/natives_hash.py`), so a different SDK build is rejected
  at recompile time.
- **`0004` Jump natives.** An entry marked `jump` ends somewhere other than
  its return address (a context load ending in `rfi`): it sets `ctx->pc` and
  its callers return to the dispatcher. Native call sites also hand the
  chassis's idle-loop pc back to it, as direct calls do.

**Per-game settings (`config/GameSettings/R7EE8P.ini`)**

`make run` copies these to ModernGekko's user directory (override with
`USER_DIR=`).

- Idle-loop skipping for the game's `for (;;) OSWakeupThread(...)` idle
  thread, which otherwise holds the game at ~10 FPS.
- Dual core (`CPUThread`), which this Dolphin fork leaves off on desktop. It
  took gameplay from ~24 to a steady 30 FPS.

## Project layout

```
lib/DolRecomp/      submodule: PowerPC DOL -> C / LLVM static recompiler
lib/ModernGekko/    submodule: runtime the recompiled module runs on
patches/            local fixes applied to the submodules
config/             per-game runtime settings, RSO load layouts, native SDK lists, controller profile
scripts/            build environment checks, SDK symbol finder, natives hashes,
                    automation client
Makefile            the whole pipeline
iso/                your disc image (gitignored)
extracted/          extracted disc (gitignored)
build/              tool builds, compiled modules, symbol maps (gitignored)
extern/, database/  tools and title data downloaded on first run (gitignored)
```

## Credits

- [ModernGekko-Template](https://github.com/ExpansionPak/ModernGekko-Template),
  the pipeline this project is based on
- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) and
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko)
- [Aurora](https://github.com/encounter/aurora) and Google's
  [Dawn](https://dawn.googlesource.com/dawn)
- The [Dolphin](https://dolphin-emu.org/) team
- [Wiimms ISO Tools](https://wit.wiimm.de/) and [GameTDB](https://www.gametdb.com/)

## Legal

This is an unofficial fan project. It is not affiliated with or endorsed by
SEGA or Nintendo. NiGHTS and NiGHTS: Journey of Dreams are trademarks of SEGA.
Wii is a trademark of Nintendo.

DolRecomp and ModernGekko are distributed under their own licenses (see
`lib/DolRecomp/LICENSE` and `lib/ModernGekko/LICENSE`). ModernGekko is
GPL-3.0-or-later because of its Dolphin-derived runtime.
