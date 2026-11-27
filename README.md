<div align="center">

# DualizeRecomp

**NiGHTS: Journey of Dreams (Wii) recompiled to native PC code, with no emulator.**

![Platform](https://img.shields.io/badge/platform-Windows%20x86__64-0078d4)
![Game](https://img.shields.io/badge/game-R7EE8P%20(USA)-7b3fe4)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-lightgrey)

</div>

```console
$ python scripts/check/perf_suite.py --rounds 2 --scenes forest,gate,dark1
      head    dark1: speed 771%  fps 232  frame 4.305 ms (sd 1.38)  gpu 0.98 ms  max frame 47.35 ms  (2 runs)
      head   forest: speed 557%  fps 174  frame 5.808 ms (sd 1.53)  gpu 1.25 ms  max frame 118.47 ms  (2 runs)
      head     gate: speed 569%  fps 170  frame 5.895 ms (sd 1.19)  gpu 1.11 ms  max frame 30.24 ms  (2 runs)
```

<sub>Three gameplay scenes from savestates, run uncapped (no frame cap, vsync or interpolation)
for 25 s each in two interleaved rounds; "speed" is the game against the Wii's 30 FPS.</sub>

At build time, the game's `main.dol` and all of its RSO modules are translated from PowerPC to
C and compiled into one executable. That executable also holds a runtime written for this game:
its OS, IOS, audio DSP and input are served at the SDK boundary, and a GX renderer runs on
D3D12 or Vulkan. The game still ticks at its original 30 Hz, and the frames in between are
interpolated at the display's refresh rate.

> [!IMPORTANT]
> This repository contains **no game code, assets, disc images or keys**, and it downloads
> none. Bring your own dump of a disc you legally own. The recompiled output is derived from
> the game, so it is never committed either.

## Highlights

- **Every game instruction is native code.** There is no JIT. From cold boot through play,
  stage loads included, the interpreter runs nothing; it stays only as a safety net.
- **High refresh rate.** The game logic runs at 30 Hz and is shown at 60, 144 or 240 Hz, with
  interpolated matrices, camera, lights and projected textures.
- **Modern host stack.** The renderer is Aurora on its own thin layer over D3D12 and Vulkan,
  without Dawn or WebGPU. Audio and gamepads go through SDL3.
- **Image quality.** MSAA 8×, rendering above the window's resolution, 16× anisotropic
  filtering and optional ambient occlusion, all set from an in-game Graphics Settings page.
- **HD textures.** Packs named like Dolphin's texture dumps (`tex1_...`) replace the game's
  own.

## Benchmarks

| Gameplay scene (uncapped, MSAA 8×) | Speed vs the Wii | Game FPS | GPU per frame |
|---|---|---|---|
| `forest` | 557% | 174 | 1.25 ms |
| `gate` | 569% | 170 | 1.11 ms |
| `dark1` | 771% | 232 | 0.98 ms |
| Interpolated present at 240 Hz | | | ~0.5 ms of CPU |

Measured with `scripts/check/perf_suite.py` on an i5-9600KF and an RTX 4060 (Windows 11, Clang
22): two interleaved rounds of 25 s per scene. The guest CPU thread sets the rate. At the
game's 30 FPS it idles most of the time.

## Installation

You need Windows 10/11 with Git for Windows (its Git Bash runs the scripts), an x86-64-v3 CPU
(AVX2) to run the build, and your own NiGHTS: Journey of Dreams (USA) disc image.

```sh
git clone --recurse-submodules https://github.com/Trick230/DualizeRecomp.git
cd DualizeRecomp
bash scripts/setup.sh                                       # Clang, CMake, Ninja, ccache, Python into .tools/
bash scripts/build.sh release                               # the tools only, no game yet
build/release/tools/DolRecomp/dolrecomp --setup             # Wiimms ISO Tools into extern/wit
build/release/tools/DolRecomp/dolrecomp extract iso/nights.iso extracted/nights
bash scripts/build.sh release                               # the game: ~30 min the first time
```

`setup.sh` fetches pinned, checksummed tools into `.tools/` and installs nothing on the system;
the folder can be moved. The configure step downloads SDL3, DXC, the Vulkan headers and the GPU
memory allocators. ThinLTO and ccache keep their work, so a rebuild only re-optimizes what
changed. Linux builds the recompiler only (the renderer is Windows-only for now). Presets, CPU
targets and how to check your dump: **[docs/GUIDE.md](docs/GUIDE.md#building)**.

## Usage

```sh
build/release/DualizeRecomp/DualizeRecomp --game extracted/nights
```

In the game's Options screen, the aspect ratio row becomes **Graphics Settings**:
anti-aliasing, render scale, frame interpolation, vsync and ambient occlusion, saved to
`graphics.ini` in the user directory. These flags override them for one run:

| Flag | Effect |
|---|---|
| `--game <dir>` | The extracted disc (required) |
| `--user <dir>` | NAND and saves (default `%LOCALAPPDATA%\DualizeRecomp`) |
| `--no-interpolation` | Show the game's own 30 FPS |
| `--msaa <1\|2\|4\|8>` | Multisample anti-aliasing (default 8) |
| `--render-scale <x>` | Render at x times the window (0.25 to 4) |
| `--ao` | Ambient occlusion over the 3D world |
| `--no-vsync` | Present without vsync, paced by a timer |

Every flag, the environment switches, the controls and texture packs are in
**[docs/GUIDE.md](docs/GUIDE.md#running)**.

## How it works

1. **Recompile.** [DolRecomp](https://github.com/Trick230/DolRecomp/tree/dualize) finds the
   game's functions and turns each into a C function, with direct calls between them and the
   cycle count kept for the event scheduler.
2. **Replace the SDK.** Hot SDK functions (threads, mutexes, matrices, memcpy, parts of nw4r
   g3d) are host code, matched to the game's code by signature.
3. **Serve the hardware at the SDK.** IOS, the DSP, the Wii Remotes, pads and EXI answer the
   SDK's calls directly; no hardware registers are modelled.
4. **Render.** The GX command stream goes to a render thread running
   [Aurora](https://github.com/Trick230/aurora/tree/dualize) on D3D12 or Vulkan.
5. **Interpolate.** Each frame's draws are recorded and replayed at every refresh, blended from
   the frame before using the game's own cameras.

The details are in [docs/GUIDE.md](docs/GUIDE.md#how-it-works), the phases and their
measurements in [docs/ROADMAP.md](docs/ROADMAP.md), and how the port got here in
[docs/HISTORY.md](docs/HISTORY.md).

## Development

```sh
bash scripts/build.sh release              # build, printing only errors
bash scripts/build.sh dev                  # no ThinLTO, guest code at -O1: faster builds
. scripts/env.sh                           # .tools/ first on PATH, for the Python scripts below
python scripts/check/regress.py            # replays input movies, compares guest RAM hashes
python scripts/check/perf_suite.py         # uncapped scenes from savestates, interleaved runs
scripts/sync.sh setup                      # once per clone: submodules follow checkouts and pulls
```

| Path | Contents |
|---|---|
| `DualizeRecomp/` | The runtime and the executable (cpu, kernel, hw, apu, hid, sdk) |
| `DualizeRecompLib/` | The game's config, the natives' signatures, the generated sources |
| `tools/DolRecomp/`, `thirdparty/aurora/` | Our forks (branch `dualize`), pinned as submodules |
| `scripts/`, `tests/` | Build, checks, automation, profiler; unit tests |

## Limitations

- Only the USA version (`R7EE8P`) is supported.
- FMVs play at their own 30 FPS and are not interpolated.
- Linux (Vulkan) is untested; macOS has no backend.
- Overlays that hook the swap chain (RivaTuner, Steam's frame pacer) can hang the renderer at
  startup.

## Acknowledgements

[DolRecomp](https://github.com/ExpansionPak/DolRecomp) and
[ModernGekko](https://github.com/ExpansionPak/ModernGekko), where this project started;
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) and XenonRecomp, whose
structure it follows; [Aurora](https://github.com/encounter/aurora) and
[Dawn](https://dawn.googlesource.com/dawn), which it ran on until phase 9; the [DirectX Shader
Compiler](https://github.com/microsoft/DirectXShaderCompiler),
[D3D12MA](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator),
[VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[volk](https://github.com/zeux/volk) and [SDL3](https://libsdl.org/); the
[Dolphin](https://dolphin-emu.org/) team, whose hardware research the runtime follows; and
[Wiimms ISO Tools](https://wit.wiimm.de/).

## Legal

An unofficial fan project, not affiliated with or endorsed by SEGA or Nintendo. NiGHTS and
NiGHTS: Journey of Dreams are trademarks of SEGA; Wii is a trademark of Nintendo. The runtime
is GPL-3.0-or-later; DolRecomp and Aurora are under their own licenses
(`tools/DolRecomp/LICENSE`, `thirdparty/aurora/LICENSE`).
