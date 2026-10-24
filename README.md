<div align="center">

# DualizeRecomp

**NiGHTS: Journey of Dreams (Wii), statically recompiled to native PC code**

Native CPU · native Wii machine · WebGPU renderer · interpolated up to your refresh rate

![Platform](https://img.shields.io/badge/platform-Windows%20x86__64-0078d4)
![Game](https://img.shields.io/badge/game-R7EE8P%20(USA)-7b3fe4)
![Status](https://img.shields.io/badge/status-playable-2ea44f)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-lightgrey)

</div>

The game's `main.dol` and its RSO modules are translated from PowerPC to C by
[DolRecomp](https://github.com/ExpansionPak/DolRecomp) and compiled into one
native module. That module runs on
[ModernGekko](https://github.com/ExpansionPak/ModernGekko), either on its own
**native Wii machine** (no Dolphin code) or on a Dolphin-derived core kept
for reference. The game still ticks at its original 30 Hz; frames in between
are **interpolated** at the display's refresh rate.

> [!IMPORTANT]
> This repository contains **no game code, assets, disc images or keys**, and
> it downloads none. Bring your own dump of a disc you legally own. The
> recompiled output is derived from the game, so it is never committed either.

## Highlights

- **Every game instruction is native code.** No JIT, and an interpreter only
  for the exception stubs and the apploader.
- **No Dolphin at runtime.** `make native` runs a Wii built for this game:
  CPU, hardware, scheduler, IOS, DSP, GX, audio and input.
- **High refresh gameplay.** 30 Hz game logic, presented at 60, 144 or 240 Hz
  with interpolated matrices, camera, lights and projected textures.
- **3.5× headroom.** ~113 FPS uncapped in gameplay, up from ~32 at the start
  of the native port.
- **Modern host stack.** Aurora on Dawn (D3D12 / Vulkan / Metal) for video,
  SDL3 for audio and gamepads.

## Status

| | |
|---|---|
| **Version** | NiGHTS: Journey of Dreams, USA (`R7EE8P`) |
| **Playable** | Cold boot, intro, Story Select and gameplay |
| **Frame rate** | Game at 30 Hz, steady pacing (33.4 ms, σ ≈ 0.2 ms); shown at the display's refresh rate |
| **Video** | Native renderer, 16:9 or 4:3 from the console's setting, letterboxed when the window doesn't match |
| **Audio** | Native DSP/AX, bit-identical to Dolphin's HLE, out through SDL3 |
| **Input** | SDL3 gamepads as Classic Controllers (Xbox, PlayStation and Switch layouts) |
| **Startup** | ~4.5 s to the first frame from a savestate |
| **Tested on** | Windows 11, MSYS2 UCRT64, Clang `-O3`, i5-9600KF + RTX 4060, 240 Hz display |
| **Untested** | Linux and macOS (the tools support both), PAL `R7EP8P`, JP `R7EJ8P` |

**Known issues**

- Interpolation: some trees and the ground shadow can still waver while the
  camera turns. Being worked on.
- The intro FMV runs at ~19 FPS, bound by the game's own video decoder. FMVs
  aren't interpolated.
- The stage RSO modules still run on the Dolphin core's JIT; on the native
  machine they run on the interpreter.

## Quick start

```sh
git clone --recurse-submodules https://github.com/Trick230/DualizeRecomp.git
cd DualizeRecomp
make check                          # verify the toolchain
make native ISO=iso/nights.iso      # first run: build, extract, recompile, play
make native GAME=nights             # later runs
```

`make run` launches the same module on the Dolphin-derived core instead.

## Building

### Requirements

- CMake, Ninja, pkg-config, Git and Python 3
- A C11/C++23 toolchain
  - **Windows:** MSYS2 UCRT64 (tested), with `mingw-w64-ucrt-x86_64-clang`,
    `-lld` and `-abseil-cpp`. Visual Studio 2022 ("Desktop development with
    C++") also builds.
  - **Linux:** GCC or Clang, plus Dolphin's NoGUI packages:
    ```sh
    sudo apt-get install -y ninja-build build-essential pkg-config cmake \
      libevdev-dev libudev-dev libgtk-3-dev libsystemd-dev \
      libbluetooth-dev libasound2-dev libpulse-dev libgl1-mesa-dev \
      libxrandr-dev libxi-dev
    ```
  - **macOS:** Xcode command line tools, `brew install cmake ninja pkg-config`
- A 64-bit host and your own NiGHTS: Journey of Dreams (USA) disc image

`make check` validates all of this and offers to fetch missing submodules
(`make check FETCH=1` doesn't ask). The first sync downloads a few hundred MB:
ModernGekko vendors much of Dolphin's dependency tree.

### First run

1. Put the disc image at `iso/nights.iso` (gitignored).
2. Run `make native ISO=iso/nights.iso`. The first time, this:
   1. builds DolRecomp and ModernGekko and applies the local [patches](docs/PATCHES.md);
   2. downloads [Wiimms ISO Tools](https://wit.wiimm.de/) into `extern/wit`;
   3. extracts the disc into `extracted/nights/`;
   4. recompiles the game and compiles the module (about 30 min with ThinLTO);
   5. launches the game.
3. Later, `make native GAME=nights`. Modules are cached under `build/modules/`
   by DOL hash and toolchain, so nothing is rebuilt unless something changed.

To check your dump, `sha256sum extracted/nights/sys/main.dol` should print
`e63c17343cf854fffc35155546536ae57bd7018dc3979575ac7c981262144c20`.

### Make targets

| Target | Description |
|---|---|
| `make check` | Validate tools, compilers, architecture and sources |
| `make tools` | Build DolRecomp and ModernGekko only |
| `make extract ISO=…` | Extract the disc into `extracted/nights/` |
| `make recompile GAME=nights` | Recompile and build the module without launching |
| `make native GAME=nights` | Run on the [native machine](#native-machine) |
| `make run GAME=nights` | Run on the Dolphin-derived core |
| `make llvm` / `make llvm-run` | `recompile` / `run` with the LLVM backend |
| `make symbols GAME=nights` | Name the SDK functions in `main.dol` (`build/symbols/nights.map`) |
| `make clean-extracted` · `clean-tools` · `clean` | Remove the extracted disc and modules · the tool builds · both |

| Variable | Default | Description |
|---|---|---|
| `ISO` | | Disc image; only needed the first time |
| `GAME` | | Extracted game slug (`nights`) |
| `RUN_ARGS` | | Runner flags, see [Running](#running) |
| `BACKEND` | `c` | `c` or `llvm` (LLVM 19+; ~20% slower on this game) |
| `TOOLCHAIN` | `clang` if installed | Module compiler: `auto`, `clang`, `gcc` or `msvc` |
| `OPT_LEVEL` | `3` | Module optimization level |
| `LLVM_DIR` | CMake discovery | LLVM's CMake package, if not found |
| `USER_DIR` | ModernGekko's | Where `make run` installs `config/` |
| `CMAKE_BUILD_TYPE` · `JOBS` | `Release` · CPU count | Tool build type and parallelism |

## Running

Flags go in `RUN_ARGS="…"`.

| Native machine flag | Effect |
|---|---|
| `--no-interpolation` | Show the game's own 30 FPS |
| `--no-vsync` | Present without vsync, paced by a timer |
| `--present-rate <hz>` | With `--no-vsync`: the present rate (default: the display's refresh rate) |
| `--unthrottled` | Run the game as fast as it goes (turns interpolation off) |
| `--load-state <file>` | Start from a savestate |
| `--automation-dir <dir>` | Drive the game from command files: `save_state`, `load_state`, `pad`, `pad_frames`, `clear_pad`, `stop`, `call` (see `scripts/automation.py`) |
| `--user <dir>` | User directory (NAND, saves) |

On the Dolphin core, `--graphics Native` selects the native renderer
(without interpolation) and `--headless` runs without a window.

<details>
<summary><b>Environment switches</b></summary>

| Variable | Effect |
|---|---|
| `NATIVE_GX_STATS=1` | Log the renderer once per second: FPS, draws, FIFO, render-thread time split, frame interval mean/σ/max, interpolated presents and their cost |
| `MODERNGEKKO_PAD_TARGET=gc` | Gamepads drive GameCube pads instead |
| `MODERNGEKKO_PAD_TARGET=none` | Gamepads drive nothing, only automation does (deterministic tests) |
| `MODERNGEKKO_WIIMOTE_SPEAKER=0` | Silence the Wii Remote speaker |
| `MODERNGEKKO_REAL_DISC_TIMING=1` | Emulate disc seeks and rotation instead of buffer-speed reads |
| `MODERNGEKKO_AUDIO_STATS=1` | Log the audio queue once per second |
| `MODERNGEKKO_NATIVE_INPUT=0` · `_DSP=0` · `_IOS=0` | Dolphin core only: use Dolphin's input, DSP HLE or IOS devices instead of the native ones |
| `AURORA_INTERP_NOBLEND=1` | Debug: replay frames without blending, to compare |
| `AURORA_INTERP_CHECK=<dir>` | Debug: write PPM pairs that must match (older frame at t = 1, newer frame at t = 0) |

</details>

### Controls

Each gamepad drives a Wii Remote with a Classic Controller, read through SDL3
at 1 kHz, with hotplug and rumble. Buttons map by position, so PlayStation and
Switch pads work the same way (Xbox names below).

| Gamepad | Classic Controller |
|---|---|
| A · B · X · Y | b · a · y · x |
| Left / right stick | Left / right stick |
| LT · RT | L · R |
| LB · RB | ZL · ZR |
| D-pad | D-pad |
| Back · Start · Guide | − · + · Home |

> [!WARNING]
> Overlays that hook the swap chain (RivaTuner Statistics Server, Steam's frame
> pacer) can hang or crash the renderer at startup. Close them or exclude the
> game.

## How it works

```
 disc ─► DolRecomp ─► C ─► clang -O3 + ThinLTO ─► game module (.dll)
                                                        │
 ┌─ moderngekko-native ─────────────────────────────────┼──────────────────────────┐
 │                                                      ▼                          │
 │  CPU thread                                    recompiled game                  │
 │   ├ native SDK functions (OS scheduler, mutexes, PSMTX, memcpy, cache, MWCC)    │
 │   ├ flat guest memory: one load or store per access                             │
 │   ├ scheduler · VI · PI · SI · EXI · AI · DSP · Starlet IPC                     │
 │   └ IOS: /dev/di · fs · es · stm · net/kd · Bluetooth + Wii Remotes             │
 │       │ gather pipe                         │ DSP DMA          ▲ pads            │
 │       ▼ SPSC ring                           ▼                  │                 │
 │  render thread                         SDL3 audio        SDL3 gamepads (1 kHz) │
 │   GX FIFO decode ─► Aurora (TEV shaders) ─► frame record                        │
 │                                               │                                 │
 │  present pacer (refresh rate)                 ▼                                 │
 │   replay newest frame, blended toward it ─► Dawn ─► D3D12 / Vulkan / Metal      │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

### Recompiler

DolRecomp turns each chunk of PowerPC code into a C function with a switch
over its entry points. On top of upstream, this project's patches make the
generated code call other chunks, virtual calls and jump tables directly,
keep entry cases only where code can actually be entered, inline the common
FPU and paired-single paths, and load the guest memory base once per
function. Hot SDK functions (thread switching, mutexes, interrupts, matrix
math, memcpy, cache ranges, MWCC's register save/restore helpers) are
replaced by host code, each pinned to a hash of the guest bytes it replaces.
The game's RSO modules are linked ahead of time at the addresses the game
loads them to, so they recompile with the DOL.

### Native machine

`moderngekko-native` (ModernGekko's `src/native`) is a Wii built for this
game, with no Dolphin code.

| Part | What it does |
|---|---|
| CPU | The module wherever it covers the pc; an interpreter with the same semantics elsewhere. Timebase, decrementer, exceptions, locked L1 cache and its DMA |
| Memory | The guest's 32-bit space as one host reservation; MEM1, MEM2 and the locked cache are views of one section at their cached and uncached addresses. Only `0xC8000000`–`0xCFFFFFFF` leaves inline code |
| Hardware | PI, VI, SI, EXI, AI, DSP, CP/PE registers, Starlet IPC/GPIO, timed as Dolphin models them |
| Scheduler | One tick per CPU cycle; the CPU runs in slices up to the next event, and the idle loop leaves for the scheduler |
| IOS | IPC mailbox, fd table and replies; `/dev/di`, `/dev/fs` (NAND in a host directory), `/dev/es`, `/dev/stm`, `/dev/net/kd`, Bluetooth with emulated Wii Remotes |
| Boot | The system menu's disc launch, then the apploader |
| Host | Native GX, native DSP + SDL3 audio, SDL3 gamepads |

Savestates use the machine's own format; Dolphin-core states don't load.

### Native renderer

The game's GX command stream comes straight off the gather pipe and is drawn
on a render thread by [Aurora](https://github.com/encounter/aurora), a GX
implementation on WebGPU through Dawn. Retail textures are cached by content
hash, vertex arrays upload once per frame, identical uniform blocks are
shared, and draws with unchanged state reuse their pipeline and merge, so a
dense level goes from ~45k to ~1k GPU draws per frame.

### Frame interpolation

The game keeps ticking at 30 Hz. The renderer records each frame's draws and
uniform stream, holds the newest one back, and replays it at every refresh
with its matrices blended from the frame before:

1. **Match.** Each draw is paired with its counterpart in the previous frame
   by the address its matrices were loaded from (each model instance has its
   own) and a hash of its state; instances drawn from a shared buffer fall
   back to the one nearest in space.
2. **Camera.** The camera move between the two frames is the transform most
   uniquely matched draws agree on. It is applied as a screw motion, so an
   orbiting camera follows the arc instead of cutting the chord.
3. **Classify.** Each draw is judged by what explains its move best: standing
   in the world, following the camera, a billboard facing it, or a backdrop
   turning around the eye. Lights and projected-texture matrices (shadows,
   shading) get the same treatment.
4. **Blend.** Position, normal and texture matrices, lights and post-transform
   matrices are blended at the present time *t*, scale preserved. Texture
   scrolls that wrap, and draws with no plausible counterpart, snap. When
   most of the scene jumps, it's a camera cut and the frame is shown as is.

A replay costs ~1.4 ms on an RTX 4060, and 99% of draws blend in gameplay.
Holding a frame adds one game frame (~33 ms) of latency. A check mode
(`AURORA_INTERP_CHECK`) renders the endpoints of each interval and compares
them pixel by pixel with the frames the game produced.

## Performance

Uncapped gameplay in Will's first stage, i5-9600KF + RTX 4060.

| Step | FPS |
|---|---:|
| Native renderer, recompiled DOL (start of phase 3) | 32 |
| Direct cross-chunk calls, inline cache ops | 41 |
| One-load RAM access through slice tables | 48 |
| RSO modules recompiled | 57 |
| Native SDK: mutexes, interrupts, scheduler, memcpy | 65 |
| Idle until interrupt | 82 |
| Direct indirect calls, PSMTX natives, builtin byte swaps | 88 |
| CPU ABI 6: cache-range and MWCC helper natives, gather-pipe fast path | 96 |
| Sparse entry switches | 101 |
| **Native machine** with flat guest memory | **113** |

At 30 FPS that leaves the CPU thread idle about 70% of the time.

## Roadmap

| Phase | Goal | Status |
|---|---|---|
| 1. SDK symbol map | Name the SDK functions in `main.dol` from Dolphin's signature database | ✅ 1,369 strong + 253 range matches |
| 2. Native GPU | Draw the GX stream with Aurora | ✅ Native renderer |
| 3. Native OS/SDK | Natives, codegen, native DSP/IOS/input, then a machine with no Dolphin | ✅ Native machine · ⏳ stage RSO modules |
| 4. Interpolation | Render between game ticks at the display's rate | 🟡 Running at 240 Hz; tree and shadow wobble left |

## Repository layout

```
lib/DolRecomp/       submodule: PowerPC → C / LLVM static recompiler
lib/ModernGekko/     submodule: the runtime, native machine and renderer host
patches/             local changes to the submodules, applied by make (docs/PATCHES.md)
config/              game settings, RSO load layout, native SDK list, controller profile
scripts/             toolchain check, SDK symbol finder, natives hashes, automation client
docs/                patch notes
Makefile             the whole pipeline
iso/ extracted/ build/ extern/ database/   your disc, its extraction, builds, downloads (gitignored)
```

The changes to ModernGekko, its Dolphin fork, Aurora and DolRecomp live in
[`patches/`](patches) so the submodules stay at their upstream commits. What
each patch does, and what it bought, is in [docs/PATCHES.md](docs/PATCHES.md).

Per-game settings (`config/GameSettings/R7EE8P.ini`, installed by `make run`)
turn on idle-loop skipping for the game's `OSWakeupThread` idle thread and
Dolphin's dual-core mode.

## Credits

- [ModernGekko-Template](https://github.com/ExpansionPak/ModernGekko-Template),
  the pipeline this project started from
- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) and
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko)
- [Aurora](https://github.com/encounter/aurora) and Google's
  [Dawn](https://dawn.googlesource.com/dawn)
- [SDL3](https://libsdl.org/)
- The [Dolphin](https://dolphin-emu.org/) team
- [Wiimms ISO Tools](https://wit.wiimm.de/) and [GameTDB](https://www.gametdb.com/)

## Legal

An unofficial fan project, not affiliated with or endorsed by SEGA or
Nintendo. NiGHTS and NiGHTS: Journey of Dreams are trademarks of SEGA; Wii is
a trademark of Nintendo. DolRecomp and ModernGekko are under their own
licenses (`lib/DolRecomp/LICENSE`, `lib/ModernGekko/LICENSE`); ModernGekko is
GPL-3.0-or-later because of its Dolphin-derived runtime.
