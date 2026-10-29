<div align="center">

# DualizeRecomp

**NiGHTS: Journey of Dreams (Wii), statically recompiled to native PC code**

Native CPU · native Wii runtime · WebGPU renderer · interpolated up to your refresh rate

![Platform](https://img.shields.io/badge/platform-Windows%20x86__64-0078d4)
![Game](https://img.shields.io/badge/game-R7EE8P%20(USA)-7b3fe4)
![Status](https://img.shields.io/badge/status-playable-2ea44f)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-lightgrey)

</div>

The game's `main.dol` and the RSO modules it always loads are translated from
PowerPC to C at build time by our fork of [DolRecomp](https://github.com/Trick230/DolRecomp/tree/dualize)
and compiled into the executable, together with a runtime written for this
game: its own Wii hardware, IOS, DSP, audio and input, and a GX renderer on
our fork of [Aurora](https://github.com/Trick230/aurora/tree/dualize). No
emulator is involved. The game still ticks at its original 30 Hz; frames in
between are **interpolated** at the display's refresh rate.

> [!IMPORTANT]
> This repository contains **no game code, assets, disc images or keys**, and
> it downloads none. Bring your own dump of a disc you legally own. The
> recompiled output is derived from the game, so it is never committed either.

## Highlights

- **Every game instruction is native code.** No JIT. Interrupts and the
  exceptions the OS handles (FPU switch, system call) enter its handlers
  directly; in play the interpreter runs nothing.
- **One program.** The recompiled game, the runtime and the renderer build
  into one executable from one CMake project, the way
  [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) is laid out.
- **High refresh gameplay.** 30 Hz game logic, presented at 60, 144 or 240 Hz
  with interpolated matrices, camera, lights and projected textures.
- **5× headroom.** Uncapped gameplay runs at 550–600% of the Wii's speed
  (~165–180 game frames a second), up from ~32 FPS at the start of the
  native port.
- **Modern host stack.** Aurora on Dawn (D3D12 / Vulkan / Metal) for video,
  SDL3 for audio and gamepads. The fork keeps only what the game runs: its
  Dolphin SDK layer, input, card/DVD, rmlui, Tracy, the D3D11/OpenGL backends
  and every path the GX stream never reaches are gone (39k lines). ImGui stays
  for in-game menus and costs nothing while none is open.
- **HD textures.** DDS or PNG packs named like Dolphin's dumps (`tex1_...`)
  go in `Load/Textures` in the user folder; `DUALIZE_DUMP_TEXTURES=1` writes
  the textures no pack replaces, to make one.

## Status

| | |
|---|---|
| **Version** | NiGHTS: Journey of Dreams, USA (`R7EE8P`) |
| **Playable** | Cold boot, intro, Story Select and gameplay |
| **Frame rate** | Game at 30 Hz, steady pacing (33.4 ms, σ ≈ 0.4 ms); shown at the display's refresh rate |
| **Video** | Native renderer, 16:9 or 4:3 from the console's setting, letterboxed when the window doesn't match |
| **Audio** | Native DSP/AX, the same samples as Dolphin's HLE, mixed on its own thread and out through SDL3 |
| **Input** | SDL3 gamepads as GameCube pads, with their button prompts, or Classic Controllers (Xbox, PlayStation and Switch layouts) |
| **Tested on** | Windows 11, MSYS2 UCRT64, Clang 22, i5-9600KF + RTX 4060, 240 Hz display |
| **Untested** | Linux and macOS |

**Known issues**

- FMVs (Sofdec) play at their 30 FPS and aren't interpolated. The game copies
  each video frame when its decoder finishes, not on the retrace, so frames
  arrive unevenly (σ ~6 ms against ~0.3 ms in play); the present pacer
  absorbs that, short of about one late refresh every second or two.
- The stage RSO modules (`stage*`, `myDream`, `staffRoll`, `loading2P`)
  aren't recompiled yet; what code of theirs runs goes through the
  interpreter (none of it per frame in the forest stage measured).

## Building

### Requirements

- CMake 3.25+, Ninja, Git
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
another directory) with [`DualizeRecompLib/config/R7EE8P.toml`](DualizeRecompLib/config/R7EE8P.toml)
and compiles what it writes. ThinLTO caches its work in `build/release`, so a
rebuild only re-optimizes what changed.

To check your dump, `sha256sum extracted/nights/sys/main.dol` should print
`e63c17343cf854fffc35155546536ae57bd7018dc3979575ac7c981262144c20`.

## Running

| Flag | Effect |
|---|---|
| `--game <dir>` | The extracted disc (required) |
| `--user <dir>` | User directory: NAND and saves (default `%LOCALAPPDATA%\DualizeRecomp`, `~/.local/share/DualizeRecomp`) |
| `--no-interpolation` | Show the game's own 30 FPS |
| `--no-vsync` | Present without vsync, paced by a timer |
| `--present-rate <hz>` | With `--no-vsync`: the present rate (default: the display's refresh rate) |
| `--msaa <1\|4>` | Multisample anti-aliasing (default 4) |
| `--render-scale <x>` | Render at x times the window (0.25–4, default 1), reduced by an area filter |
| `--unthrottled` | Run the game as fast as it goes (turns interpolation off) |
| `--window <w>x<h>` | Initial window size |
| `--idle-pc <hex>` | The idle thread's loop the CPU skips to the next event from (default NiGHTS' `8036623C`) |
| `--load-state <file>` | Start from a savestate |
| `--automation-dir <dir>` | Drive the game from command files: `save_state`, `load_state`, `pad`, `pad_frames`, `clear_pad`, `stop` (client: `scripts/automation.py`) |

<details>
<summary><b>Environment switches</b></summary>

| Variable | Effect |
|---|---|
| `DUALIZE_GX_STATS=1` | Log the renderer once per second: FPS, draws, FIFO, render-thread time split, frame interval mean/σ/max, interpolated presents and their cost |
| `DUALIZE_PROFILE=<file>` | Sample the CPU and render threads' call stacks every millisecond and write them at exit; `scripts/profile.py <file> <exe> [--thread gx\|cpu] [--callers <name>]` reports per-function times (Windows) |
| `DUALIZE_IOS_TRACE=1` | Log each IOS request (open path, ioctl number, sizes), its reply and when the PPC receives it |
| `DUALIZE_PAD_TARGET=classic` | Gamepads drive the Classic Controller instead of the GameCube pad |
| `DUALIZE_PAD_TARGET=none` | Gamepads drive nothing, only automation does (deterministic tests) |
| `DUALIZE_WIIMOTE_SPEAKER=0` | Silence the Wii Remote speaker |
| `DUALIZE_AUDIO_STATS=1` | Log the audio queue once per second |
| `DUALIZE_DUMP_TEXTURES=1` | Write each texture no HD pack replaces to `Cache/NativeGX/texture_dumps` in the user folder |
| `AURORA_INTERP_NOBLEND=1` | Debug: replay frames without blending, to compare |
| `AURORA_INTERP_CHECK=<dir>` | Debug: write PPM pairs that must match (older frame at t = 1, newer frame at t = 0) |

</details>

### Controls

Each gamepad drives a GameCube pad, read through SDL3 at 1 kHz, with
hotplug and rumble; the game shows the GameCube's button prompts once it is
played, and its sticks and triggers have the full 8-bit range (the Classic
Controller's are 5 and 6 bits). Buttons map by position, so PlayStation and Switch pads
work the same way (Xbox names below).

| Gamepad | GameCube pad | Classic Controller (`DUALIZE_PAD_TARGET=classic`) |
|---|---|---|
| A · B · X · Y | A · X · B · Y | b · a · y · x |
| Left / right stick | Control stick / C stick | Left / right stick |
| LT · RT | L · R (analog) | L · R |
| LB · RB | — · Z | ZL · ZR |
| D-pad | D-pad | D-pad |
| Back · Start · Guide | — · START/PAUSE · — | − · + · Home |

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
 │   replay newest frame, blended toward it ─► Dawn ─► D3D12 / Vulkan / Metal      │
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
[`DualizeRecomp/sdk`](DualizeRecomp/sdk) and DolRecomp's `runtime/sdk`. The
recompiler finds them by signature: their code with the linker's bits masked
([`sdk.sigs`](DualizeRecompLib/config/sdk.sigs) and DolRecomp's built-in
sets), so each replaces only the SDK build it was written for.
The RSO modules the game always loads (`global`, `play`, `select`, `title`)
are linked inside the recompiler at the addresses the game loads them to, so
they recompile with the DOL.

### Runtime

| Part | What it does |
|---|---|
| DolRecomp `runtime/host` | The CPU layer, shared with any GameCube or Wii port: the recompiled code wherever it covers the pc and an interpreter with the same semantics elsewhere; timebase, decrementer, exceptions (interrupts and FP-unavailable save the OS context natively and start in the OS handler; the OS's sync system call runs in host code), guest calls, guest threads on fibers, locked L1 cache and its DMA. Guest memory (the 32-bit space as one host reservation, MEM1, MEM2 and the locked cache as views of one section), the event scheduler (one tick per CPU cycle; the idle loop leaves for it) and the savestate stream. `machine.cpp` is its `CpuHost` |
| `kernel/` | IOS, entered from the SDK's IPC client (no mailbox): fd table, `/dev/di`, `/dev/fs` (NAND in the user directory), `/dev/es`, `/dev/stm`, `/dev/net/kd`, a fresh NAND's system directories and factory SYSCONF (16:9), the remotes' pairing in SYSCONF, and the Wii Remote model (reports, EEPROM, speaker, Classic Controller) |
| `hw/` | PI, AI, DSP, CP/PE registers, Hollywood GPIO; the video clock behind the VI natives and the expansion bus behind the EXI natives (the IPL chip's RTC and SRAM) |
| `apu/` | The AX DSP microcode in host code; each command list is mixed on a thread of its own, out through SDL3 |
| Aurora `lib/gx/fifo.cpp` | The GX FIFO's render thread (`aurora/fifo.h`): the ring the gather pipe fills, decoding into Aurora, draw-done and token markers back to the CPU, frame pacing and interpolation. `machine.cpp` is its host |
| `hid/` | SDL3 gamepads as GameCube pads or Classic Controllers |
| `sdk/` | Native replacements for SDK functions, including the OS's alarm queue (`rvl_alarm.cpp`) and interrupt dispatch (`rvl_interrupt.cpp`), the IPC client's transport and reply handler (`rvl_ipc.cpp`), the GameCube pads (`rvl_pad.cpp`: `PADRead` takes the host pad, no SI), the EXI driver (`rvl_exi.cpp`: transfers finish in the call, no EXI registers) and WUD, the Wii Remotes' side of the SDK's Bluetooth layer (`rvl_wud.cpp`): WPAD's reports go straight to the remote model, the Bluetooth stack never starts, and the VI's retrace handler and beam (`rvl_vi.cpp`). The natives any game can list (OS context, interrupts and mutexes, cache, matrices, the Metrowerks runtime) live in DolRecomp's `runtime/sdk`, placed by `define`s their signatures read off the code |
| `boot.cpp` | The system menu's disc launch, then the apploader |

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
forest stage runs at 550–600% of the Wii's speed, about
165–180 game frames a second, on an i5-9600KF + RTX 4060. The guest CPU
thread sets that rate; at the game's 30 FPS it idles most of the time, and
240 Hz interpolation costs ~0.5 ms of CPU a present. How it got there, step
by step, is in [docs/HISTORY.md](docs/HISTORY.md#performance-by-step).

## Roadmap

| Phase | Goal | Status |
|---|---|---|
| 1. SDK symbol map | Name the SDK functions in `main.dol` from Dolphin's signature database | ✅ 1,369 strong + 253 range matches |
| 2. Native GPU | Draw the GX stream with Aurora | ✅ Native renderer |
| 3. Native OS/SDK | Natives, codegen, native DSP/IOS/input, a runtime with no emulator | ✅ · ⏳ stage RSO modules |
| 4. Interpolation | Render between game ticks at the display's rate | ✅ 240 Hz; the game names its cameras, skinned draws pair by their bones |
| 5. Own runtime | One CMake project with the recompiler, Aurora and our runtime | ✅ |
| 6. Per-function recompiler | XenonRecomp-style functions: registers as locals, direct calls, hooks by symbol | 🟡 Functions, direct calls, live cycle counter, safepoints; registers as locals left |
| 7. HLE runtime | The OS, IOS, WPAD and every device the game drives at the SDK boundary instead of the hardware: no register models left | 🟡 Native interrupt entry, guest calls from the host, IOS and WUD at the SDK, dispatch cache; the rest below |
| 8. Image quality | Anti-aliasing, texture filtering and rendering above the display's resolution | 🟡 MSAA 4×, render scale with an area filter, 16× anisotropic |
| 9. Native renderer | Aurora on D3D12 and Vulkan directly instead of through Dawn (WebGPU), as UnleashedRecomp's plume does | ⏳ After GX at the API |

What phase 7 did and what is left of it (🔸 steps are architecture only: measured in the forest, they win no speed, so phase 6 comes first):

| Step | What changes | Status |
|---|---|---|
| Native interrupt entry | External and decrementer interrupts and FP-unavailable save the OSContext in host code and start in the recompiled OS handler; the system call (a memory sync) runs in host code. No interpreter in play | ✅ |
| Guest calls from the host | Run a guest function to its return from host code, inside an interrupt handler (scheduler off, so no thread switch) | ✅ |
| IOS at the SDK | `ios_Ipc2` hands requests straight to the IOS devices; the IPC interrupt handler delivers replies as `IpcReplyHandler` does (callback or wakeup). No mailbox, doorbell or acks | ✅ |
| Alarms | `OSSetAlarm`, `OSSetPeriodicAlarm`, `OSCancelAlarm` and the decrementer handler keep the alarm queue in host code; the timer is a scheduler event | ✅ |
| Interrupt causes | `__OSDispatchInterrupt` takes the causes straight from the host's devices and picks the handler by the SDK's priorities | ✅ |
| WUD at the SDK | WPAD and KPAD stay the game's code; WUD's device side is the host's: output reports go to the remote model, input reports to WPAD's receive callback, connections to its connection callback. No Bluetooth stack, HCI, L2CAP or IOS USB driver | ✅ |
| Dispatch cache | A direct-mapped pc → function and code cache ahead of the function table, for the run loop and indirect calls | ✅ Dispatcher from 3.8% to 2.5% of the CPU thread |
| Threads as fibers | Each guest thread on its own host stack, so a thread switch or interrupt resumes without unwinding through the dispatcher | ✅ Interrupts run in place at the safepoint; each guest thread runs on its own Windows fiber, parked where it switched away and resumed there. Cold boot dispatches −22%, forest 350 VPS |
| VI at the SDK | The retrace handler flushes what `VIConfigure`, `VISetNextFrameBuffer`, `VISetBlack` and the rest of VI's API staged straight into the host's video clock, and the beam getters (`VIGetCurrentLine`, `VIGetNextField`, the scan mode and cable) and the VI's hardware init are host code. The staging API stays the game's code: it never touched the hardware. No video interface registers | ✅ |
| GX registers at the SDK | The token, finish and CP interrupts and GX's FIFO setup in host code; no CP/PE/MI register files | 🔸 Optional: the device models are host code already, all of them under 0.5% of the CPU thread in play |
| AI, DSP and ARAM at the SDK | AI DMA, the DSP mailbox and ARAM DMA at their SDK functions; AX stays the game's code over the native DSP | 🔸 Optional: the device models are host code already, all of them under 0.5% of the CPU thread in play |
| SI and EXI at the SDK | `PADRead` builds each report from the host pad and runs the SDK's own `MakeStatus`; PAD's init, reset and rumble and the SI's init are host code. `EXILock`, `EXISelect`, `EXIImm`, `EXIDma` and the rest finish each transfer in the call on the IPL chip (RTC, SRAM, the UART behind OSReport). No serial or expansion interface registers or interrupts | ✅ |
| DVD and NAND at the SDK | `DVD*` and `NAND*` served by host functions instead of IOS requests; IOS keeps only what the SDK can't skip | 🔸 Optional: the device models are host code already, all of them under 0.5% of the CPU thread in play |

After phase 6, before the stage modules:

| Step | What changes | Status |
|---|---|---|
| GX at the API | `GXBegin`, the vertex writes, matrix loads and state setters call Aurora directly; no FIFO encoding and decoding | 🔸 Optional: the game's GX functions are ~2–3% of the CPU thread, the GX thread waits on the CPU, and vertex writes (inline stores) and display lists stay FIFO streams anyway |
| AX mixing off the CPU thread | Each AX command list is mixed on a thread of its own while the game runs on; the DSP's mails arrive half a millisecond of DSP time later | ✅ Mixing from ~1.3% of the CPU thread to ~0.6% (the mailbox and the hand-off) |
| AX at the SDK | The host mixes AX's voices itself; no DSP mailbox or ucode | 🔸 Optional: what is left of the DSP on the CPU thread is ~0.6% |

Phase 8, image quality, in order (Aurora renders at the window's size and
scales EFB copies with it, so shadows, bloom and reflections follow the
render scale):

| Step | What changes | Status |
|---|---|---|
| MSAA 4× | Multisampled render targets, resolved for EFB copies and the present; depth copies read sample 0. On by default (`--msaa 1` turns it off). WebGPU allows 1 or 4 samples | ✅ Forest at 1080p: as fast as without (530–585 presents/s, GPU-bound) |
| Render scale | `--render-scale <x>` renders at x times the window (EFB copies, so shadows and bloom, follow) and reduces with an area (box) filter, so every rendered pixel counts; at 1× the frame goes straight to the present | ✅ Interpolation renders every present, so the cost scales with the present rate: forest at 1080p ×2 with MSAA ~120 presents/s, without ~270, ×1.5 ~210 |
| Alpha-to-coverage | Unblended alpha-test cutouts (the forest's ferns and leaves) become coverage under MSAA: the edge spreads over about a pixel's samples instead of a hard discard. Only where the EFB keeps no alpha, as in all of NiGHTS | ✅ No measurable cost (forest ~450 presents/s either way) |
| Anisotropic filtering | 16× on every mipmapped texture, not only where the game asks | ✅ |
| Color depth | 8 bits a channel with no dithering, where the console has 6-bit RGBA6 or dithered RGB8 | ✅ Aurora's behaviour |
| Texture LOD | Mip selection follows the render resolution, so higher render scales pick sharper mips with no bias; HD replacement textures keep their own bias | ✅ Aurora's behaviour |
| Pipeline precompile | Everything the pipeline cache knows compiles on a worker pool (cores minus two) before the game starts, so none of it pops in; the stats line counts draws skipped while compiling | ✅ Forest, 310 pipelines: 60 ms warm, 3.1 s with no driver cache (11.1 s on one thread) |
| Seed pipeline cache | `DualizeRecomp/resources/initial_pipeline_cache.db`, merged into the user's cache at each start; `scripts/pipeline_seed.py` adds a played cache to it. Seeded with MSAA on, it serves `--msaa 1` too | ✅ Seeded from the forest; a playthrough of every stage fills it |
| Ubershader | One shader that reads the TEV, texgen and lighting setup from uniforms, drawing anything whose own pipeline is still compiling, so a never-seen pipeline doesn't pop in either | ⏳ |

Phase 9, the native renderer, comes after GX at the API, when the whole
renderer is ours: Aurora's backend on D3D12 and Vulkan without Dawn, the TEV
shaders as HLSL/SPIR-V. It lifts WebGPU's limits (MSAA 8×) and drops a layer
between Aurora and the driver.

Next, in order: the stage RSO modules, booting without the interpreter, dropping the interpreter, then the
🔸 steps (AI/DSP/ARAM, GX registers, DVD/NAND, AX at the SDK), phase 6,
GX at the API and phase 9.

Dropped after measuring: checking MSR[FP] once per function instead of per
FPU instruction (under 1%; the OS switches the FPU lazily, so the check has to
stay, and every instruction is a resume point) and paired singles as SIMD
(the two lanes live in separate register arrays, and the helpers already
reproduce the Wii's rounding bit for bit). Cheaper guest time too: in the
forest uncapped (~610% speed) over half the scheduler's passes find no event
due, but ending slices only at events instead of every 20,000 cycles changes
nothing measurable, and keeping each function's block costs in a register,
charged to the downcount only at safepoints and returns, measured the same
(609% against 611%): the per-block charge is a store the CPU already
forwards.

## Working on the forks

DolRecomp and Aurora are our forks, checked out as submodules on their
`dualize` branch; this repository pins a commit of each. Once per clone:

```sh
scripts/sync.sh setup     # submodules follow checkouts and pulls; push sends fork commits first
```

A change to a fork is a commit in it, then a commit here that moves the pin
(`git add tools/DolRecomp`). `scripts/sync.sh push` pushes both forks and then
this repository, `status` shows where each fork stands, and `upstream` lists
what DolRecomp and Aurora upstream have that the forks don't.

## Repository layout

```
tools/DolRecomp/      submodule, our fork: the PowerPC → C recompiler, the guest CPU core and the host CPU layer
thirdparty/aurora/    submodule, our fork: GX on WebGPU (Dawn), SDL3, the GX FIFO's render thread
DualizeRecompLib/     the game's code: recompiler config, natives' signatures, generated sources
DualizeRecomp/        the runtime and the executable (kernel, hw, apu, hid, sdk)
scripts/              fork sync, pipeline seed, automation client, profiler, window capture
docs/                 the history of the port
iso/ extracted/ build/ extern/   your disc, its extraction, builds, downloads (gitignored)
```

## Credits

- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) and
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko), where this
  project started
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) and
  XenonRecomp, whose structure this follows
- [Aurora](https://github.com/encounter/aurora) and Google's
  [Dawn](https://dawn.googlesource.com/dawn)
- [SDL3](https://libsdl.org/)
- The [Dolphin](https://dolphin-emu.org/) team, whose hardware research the
  runtime follows
- [Wiimms ISO Tools](https://wit.wiimm.de/)

## Legal

An unofficial fan project, not affiliated with or endorsed by SEGA or
Nintendo. NiGHTS and NiGHTS: Journey of Dreams are trademarks of SEGA; Wii is
a trademark of Nintendo. The runtime is GPL-3.0-or-later; DolRecomp and
Aurora are under their own licenses (`tools/DolRecomp/LICENSE`,
`thirdparty/aurora/LICENSE`).
