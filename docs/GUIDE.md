# Guide

Everything the [README](../README.md) leaves out: building in detail, every flag and
environment switch, the controls, and how the port works inside.

- [Building](#building)
- [Running](#running)
- [How it works](#how-it-works)
- [Performance](#performance)
- [Development](#development)

## Building

### Requirements

- CMake 3.25+, Ninja, Git; the configure step downloads SDL3, DXC, the
  Vulkan headers, volk and the D3D12 and Vulkan memory allocators
- Clang with lld (ThinLTO links the game's code); on Windows, MSYS2 UCRT64
  with `mingw-w64-ucrt-x86_64-clang`, `-lld`, `-cmake` and `-ninja`
- A 64-bit host and your own NiGHTS: Journey of Dreams (USA) disc image
- An x86-64-v3 CPU (AVX2, FMA: Intel Haswell or AMD Zen and later, ~96% of
  Steam's survey) to run the build. `-DDUALIZE_CPU_TARGET=x86-64-v2` reaches
  older CPUs, with the guest's fused multiply-adds done in software; `native`
  tunes for the build machine only

### Steps

```sh
git clone --recurse-submodules https://github.com/Trick230/DualizeRecomp.git
cd DualizeRecomp
cmake --preset release            # no game yet: configures the tools only
cmake --build build/release
build/release/tools/DolRecomp/dolrecomp --setup                    # Wiimms ISO Tools into extern/wit
build/release/tools/DolRecomp/dolrecomp extract iso/nights.iso extracted/nights
cmake --preset release            # now with the game
cmake --build build/release       # ~30 min the first time
build/release/DualizeRecomp/DualizeRecomp --game extracted/nights
```

The build runs DolRecomp on `extracted/nights` (set `DUALIZE_GAME_DIR` to use
another directory) with [`DualizeRecompLib/config/R7EE8P.toml`](../DualizeRecompLib/config/R7EE8P.toml)
and compiles what it writes. ThinLTO caches its work in `build/release`, so a
rebuild only re-optimizes what changed.

To check your dump, `sha256sum extracted/nights/sys/main.dol` should print
`e63c17343cf854fffc35155546536ae57bd7018dc3979575ac7c981262144c20`.

### Presets

| Preset | What it builds |
|---|---|
| `release` | The default: guest code at `-O3` with ThinLTO |
| `dev` | No ThinLTO and guest code at `-O1`, for faster iteration |
| `relwithdebinfo`, `debug` | With debug info; `debug` unoptimised |
| `pgo-generate`, `pgo` | Profile-guided build, driven by `scripts/pgo.sh` (two full builds; needs compiler-rt) |

`DUALIZE_LTO_JOBS` bounds the parallel ThinLTO backend jobs at link time
(each holds a few large chunks; one per core exhausts 16 GB of RAM).
`DUALIZE_CPU_TARGET` picks the CPU level (`x86-64-v3` by default) and
`DUALIZE_BUILD_TESTS=ON` adds the unit tests in `tests/`.

## Running

In the game's Options screen, the aspect ratio check row becomes
**Graphics Settings** and opens the runtime's own settings, drawn with
the screen's own art read from the disc: anti-aliasing, render scale,
frame interpolation, vsync and ambient occlusion. They are saved to
`graphics.ini` in the user directory and take effect at the next start;
the flags below override them for one run.

| Flag | Effect |
|---|---|
| `--game <dir>` | The extracted disc (required) |
| `--user <dir>` | User directory: NAND and saves (default `%LOCALAPPDATA%\DualizeRecomp`, `~/.local/share/DualizeRecomp`) |
| `--no-interpolation` | Show the game's own 30 FPS |
| `--ao` | Ambient occlusion (GTAO over the 3D world, off by default) |
| `--no-vsync` | Present without vsync, paced by a timer |
| `--hidden` | Never show the window; everything still renders (scripted runs, which `scripts/automation.py` starts this way) |
| `--present-rate <hz>` | With `--no-vsync`: the present rate (default: the display's refresh rate) |
| `--msaa <1\|2\|4\|8>` | Multisample anti-aliasing (default 8), capped at the GPU's most |
| `--render-scale <x>` | Render at x times the window (0.25–4, default 1), reduced by an area filter |
| `--unthrottled` | Run the game as fast as it goes (turns interpolation off) |
| `--window <w>x<h>` | Initial window size |
| `--idle-pc <hex>` | The idle thread's loop the CPU skips to the next event from (default: the game config's `IDLE_PC`, NiGHTS' `8036623C`) |
| `--load-state <file>` | Start from a savestate |
| `--automation-dir <dir>` | Drive the game from command files: `save_state`, `load_state`, `pad`, `pad_frames`, `clear_pad`, `stop` (client: `scripts/automation.py`) |
| `--record-input <file>` | Record what the game reads from its controllers, read by read (format in `DualizeRecomp/hid/input_movie.hpp`) |
| `--log-entries <file>` | Add to the file each point recompiled code was entered at without an entry there, stepping on the interpreter from it. Name the file in the config's `main.entries` and the next recompilation gives those points entries |
| `--replay-input <file>` | Play a recorded movie back and stop at its end. The game runs on its own clock (the GPU's signals reach it only at points of that clock), so the same start and the same movie make the same run at any speed |
| `--hash-every <n>` | Log a hash of guest RAM every n fields, to compare runs |

### Environment switches

| Variable | Effect |
|---|---|
| `AURORA_BACKEND=vulkan` | Render with Vulkan instead of D3D12 |
| `RHI_VALIDATION=1` | Run under the API's validation layer |
| `DUALIZE_GX_STATS=1` | Log the renderer once per second: FPS, draws, FIFO, render-thread time split, frame interval mean/σ/max, interpolated presents and their cost |
| `AURORA_FRAME_DUMP=<dir>` | Write each frame's hash to `hashes.txt` there and every 30th frame's pixels, to compare renderers by numbers (`scripts/frame_compare.py`) |
| `DUALIZE_PROFILE=<file>` | Sample the CPU and render threads' call stacks every millisecond and write them at exit; `scripts/profile.py <file> <exe> [--thread gx\|cpu] [--callers <name>]` reports per-function times (Windows) |
| `DUALIZE_IOS_TRACE=1` | Log each IOS request (open path, ioctl number, sizes), its reply and when the PPC receives it |
| `DUALIZE_PAD_TARGET=classic` | Gamepads drive the Classic Controller instead of the GameCube pad |
| `DUALIZE_PAD_TARGET=none` | Gamepads drive nothing, only automation does (deterministic tests) |
| `DUALIZE_WIIMOTE_SPEAKER=0` | Silence the Wii Remote speaker |
| `DUALIZE_AUDIO_STATS=1` | Log the audio queue once per second |
| `DUALIZE_DUMP_TEXTURES=1` | Write each texture no HD pack replaces to `Cache/NativeGX/texture_dumps` in the user folder |
| `AURORA_INTERP_NOBLEND=1` | Debug: replay frames without blending, to compare |
| `AURORA_INTERP_CHECK=<dir>` | Debug: write PPM pairs that must match (older frame at t = 1, newer frame at t = 0) |

Aurora's other switches (the ubershader, synchronous pipelines, HLSL dumps)
are in [its README](https://github.com/Trick230/aurora/tree/dualize#configuration).


### Controls

Each gamepad drives a GameCube pad, read through SDL3 at 1 kHz, with
hotplug and rumble; the game shows the GameCube's button prompts once it is
played, and its sticks and triggers have the full 8-bit range (the Classic
Controller's are 5 and 6 bits). Buttons map by position, so PlayStation and Switch pads
work the same way (Xbox names below).

| Gamepad | GameCube pad | Classic Controller (`DUALIZE_PAD_TARGET=classic`) |
|---|---|---|
| A / B / X / Y | A / X / B / Y | b / a / y / x |
| Left / right stick | Control stick / C stick | Left / right stick |
| LT / RT | L / R (analog) | L / R |
| LB / RB | — / Z | ZL / ZR |
| D-pad | D-pad | D-pad |
| Back / Start / Guide | — / START/PAUSE / — | − / + / Home |

> [!WARNING]
> Overlays that hook the swap chain (RivaTuner Statistics Server, Steam's frame
> pacer) can hang or crash the renderer at startup. Close them or exclude the
> game.

## How it works

```
 disc ─► dolrecomp (R7EE8P.toml) ─► ppc_recomp.*.c ─► clang -O3 + ThinLTO ─┐
                                                                            ▼
 ┌─ DualizeRecomp ──────────────────────────────────────────────────────────────────┐
 │  CPU thread                                    recompiled game                  │
 │   ├ native SDK functions (OS scheduler, mutexes, PSMTX, memcpy, cache, MWCC)    │
 │   ├ flat guest memory: one load or store per access                             │
 │   ├ scheduler · VI · PI · AI · DSP · GPIO                                       │
 │   └ IOS: /dev/di · fs · es · stm · net/kd · WUD: Wii Remotes at the SDK         │
 │       │ gather pipe                         │ DSP DMA          ▲ pads            │
 │       ▼ SPSC ring                           ▼                  │                 │
 │  render thread                         SDL3 audio        SDL3 gamepads (1 kHz) │
 │   GX FIFO decode ─► Aurora (TEV shaders) ─► frame record                        │
 │                                               │                                 │
 │  present pacer (refresh rate)                 ▼                                 │
 │   replay newest frame, blended toward it ─► RHI ─► D3D12 / Vulkan               │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

### Recompiler

DolRecomp finds the game's functions (from calls, symbols, code pointers in
data, and MWCC's switch tables) and turns each into a C function. Calls
between them are C calls, virtual calls and jump tables enter their target
directly, and each function keeps a switch only for the points it can be
resumed at. Generated code counts its cycles down against the runtime's event
scheduler and leaves only when the next event is due. Our fork also inlines
the common FPU and paired-single paths and loads the guest memory base once
per function. Hot SDK functions (thread switching,
mutexes, interrupts, matrix math, memcpy, cache ranges, MWCC's register
save/restore helpers) are replaced by host code in
[`DualizeRecomp/sdk`](../DualizeRecomp/sdk). The recompiler finds them by
signature: their code with the linker's bits masked (`mwcc.sigs`, `rvl.sigs`
and `sdk.sigs` in [`DualizeRecompLib/config`](../DualizeRecompLib/config)), so
each replaces only the SDK build it was written for. DolRecomp is only the
recompiler and what its output compiles with (CPU state, the instructions'
semantics); everything that runs the game is this repository's.
The RSO modules the game always loads (`global`, `play`, `select`, `title`)
are linked inside the recompiler at the addresses the game loads them to, so
they recompile with the DOL. The stage modules load one at a time into the
memory `title` used, at an address that follows from their size; each is
linked there as an overlay, its functions next to the ones it overlaps, and
the runtime runs whichever matches guest RAM. That address is only a
prediction. The CPU reads where each stage module was linked from the RSO
library's list of modules and runs its code there. For another game the
config needs no addresses at all: a resident module given none is compiled
the same way, to run wherever the game links it (calls and addresses into it
add its shift); REL modules, resident or overlays, are found by id in the
OS's module list; and modules are read from inside RARC and U8 archives and
Yaz0 files. NiGHTS keeps its resident modules' addresses: without them it
runs as fast (forest 613% either way) and interprets nothing, but where a
module is compiled moves a few function boundaries, so the guest clock's
event points, and the recorded replays' RAM hashes, would differ.

### Runtime

| Part | What it does |
|---|---|
| `cpu/` | The CPU: the recompiled code wherever it covers the pc and an interpreter with the same semantics elsewhere; timebase, decrementer, exceptions (interrupts and FP-unavailable save the OS context natively and start in the OS handler; the OS's sync system call runs in host code), guest calls, guest threads on fibers, locked L1 cache and its DMA. `machine.cpp` is its `CpuHost`, `state.hpp` the savestate stream |
| `kernel/` | Guest memory (the 32-bit space as one host reservation, MEM1, MEM2 and the locked cache as views of one section) and the event scheduler (one tick per CPU cycle; the idle loop leaves for it). IOS, entered from the SDK's IPC client (no mailbox): fd table, `/dev/di`, `/dev/fs` (NAND in the user directory), `/dev/es`, `/dev/stm`, `/dev/net/kd`, a fresh NAND's system directories and factory SYSCONF (16:9), the remotes' pairing in SYSCONF, and the Wii Remote model (reports, EEPROM, speaker, Classic Controller) |
| `hw/` | PI, AI, DSP, CP/PE registers, Hollywood GPIO; the video clock behind the VI natives and the expansion bus behind the EXI natives (the IPL chip's RTC and SRAM) |
| `apu/` | The AX DSP microcode in host code; each command list is mixed on a thread of its own, out through SDL3 |
| Aurora `lib/gx/fifo.cpp` | The GX FIFO's render thread (`aurora/fifo.h`): the ring the gather pipe fills, decoding into Aurora, draw-done and token markers back to the CPU, frame pacing and interpolation. `machine.cpp` is its host |
| `hid/` | SDL3 gamepads as GameCube pads or Classic Controllers; input movies (record and replay) |
| `sdk/` | Native replacements for SDK functions, including the OS's alarm queue (`rvl_alarm.cpp`) and interrupt dispatch (`rvl_interrupt.cpp`), the IPC client's transport and reply handler (`rvl_ipc.cpp`), the GameCube pads (`rvl_pad.cpp`: `PADRead` takes the host pad, no SI), the EXI driver (`rvl_exi.cpp`: transfers finish in the call, no EXI registers) and WUD, the Wii Remotes' side of the SDK's Bluetooth layer (`rvl_wud.cpp`): WPAD's reports go straight to the remote model, the Bluetooth stack never starts, and the VI's retrace handler and beam (`rvl_vi.cpp`). The OS's context, interrupt and mutex functions, cache ranges, matrices and the Metrowerks runtime are C (`rvl_os.c`, `rvl_cache.c`, `rvl_mtx.c`, `mwcc_runtime.c`, `msl_string.c`), compiled with the generated code so they inline into it, placed by `define`s their signatures read off the code |
| `boot.cpp` | The system menu's disc launch, then the apploader's work in host code (DOL, FST, bi2, low-memory globals) |

### Frame interpolation

The game keeps ticking at 30 Hz. The renderer records each frame's draws and
uniform stream, holds the newest one back, and replays it at every refresh
with its matrices blended from the frame before:

1. **Match.** Each draw is paired with its counterpart in the previous frame
   by the address its matrices were loaded from (each model instance has its
   own) and a hash of its state; instances drawn from a shared buffer fall
   back to the one nearest in space. A skinned draw is placed by the bones
   its vertices actually use (read from their matrix indices), and its
   instances are told apart by where those bones were loaded from.
2. **Camera.** The game says which camera each pass of models was built
   with: a hook in nw4r g3d's `ScnRoot::CalcView` (`sdk/nw4r_g3d.c`) writes
   the view matrix into the GX stream ahead of the pass's draws, as a host
   packet the decoder reads. The camera move between the two frames is then
   exact, and applied as a screw motion, so an orbiting camera follows the
   arc instead of cutting the chord. A draw projected differently from the
   first of its pass was built with a camera of its own and isn't given it. Estimating it from the draws instead
   was wrong in ~30% of the moving frames, which shook the whole scene.
3. **Classify.** A draw built with a known camera is blended in the world,
   wherever its object goes. Any other draw is judged by what explains its
   move best, against a camera move estimated from the draws: standing in
   the world, following the camera, a billboard facing it, or a backdrop
   turning around the eye. Lights and projected-texture matrices (shadows,
   shading) get the same treatment.
4. **Blend.** Position, normal and texture matrices, lights and post-transform
   matrices are blended at the present time *t*, scale preserved. Texture
   scrolls that wrap, and draws with no plausible counterpart, snap. When
   most of the scene jumps, it's a camera cut and the frame is shown as is.

A present costs ~0.5 ms of CPU, and 96–98% of draws blend in gameplay; the
forest presents 810–860 FPS uncapped on an RTX 4060.
Holding a frame adds one game frame (~33 ms) of latency. A check mode
(`AURORA_INTERP_CHECK`) renders the endpoints of each interval and compares
them pixel by pixel with the frames the game produced.

## Performance

Uncapped gameplay (`--unthrottled --no-vsync --no-interpolation`) in a
forest stage runs at about 557% of the Wii's speed, about 174 game frames a
second (2026-11-27, `scripts/check/perf_suite.py`), on an i5-9600KF + RTX 4060 (MSAA 8×). The guest CPU
thread sets that rate; at the game's 30 FPS it idles most of the time, and
240 Hz interpolation costs ~0.5 ms of CPU a present. How it got there, step
by step, is in [docs/HISTORY.md](HISTORY.md#performance-by-step).

## Development

DolRecomp and Aurora are our forks, checked out as submodules on their
`dualize` branch; this repository pins a commit of each. Once per clone:

```sh
scripts/sync.sh setup     # submodules follow checkouts and pulls; push sends fork commits first
```

A change to a fork is a commit in it, then a commit here that moves the pin
(`git add tools/DolRecomp`). `scripts/sync.sh push` pushes both forks and then
this repository, `status` shows where each fork stands, and `upstream` lists
what DolRecomp and Aurora upstream have that the forks don't.

```
tools/DolRecomp/      submodule, our fork: the PowerPC → C recompiler and the CPU core its output compiles with
thirdparty/aurora/    submodule, our fork: GX on D3D12 and Vulkan, SDL3, the GX FIFO's render thread
DualizeRecompLib/     the game's code: recompiler config, natives' signatures, generated sources
DualizeRecomp/        the runtime and the executable (cpu, kernel, hw, apu, hid, sdk)
tests/                the runtime's unit tests (-DDUALIZE_BUILD_TESTS=ON)
scripts/              fork sync, pipeline seed, automation client, profiler, window capture
docs/                 the history of the port and the full roadmap
iso/ extracted/ build/ extern/   your disc, its extraction, builds, downloads (gitignored)
```
