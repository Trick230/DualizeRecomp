<div align="center">

# DualizeRecomp

**NiGHTS: Journey of Dreams (Wii), statically recompiled to native PC code**

Native CPU · native Wii runtime · WebGPU renderer · interpolated up to your refresh rate

![Platform](https://img.shields.io/badge/platform-Windows%20x86__64-0078d4)
![Game](https://img.shields.io/badge/game-R7EE8P%20(USA)-7b3fe4)
![Status](https://img.shields.io/badge/status-playable-2ea44f)
![License](https://img.shields.io/badge/license-GPL--3.0--or--later-lightgrey)

</div>

The game's `main.dol` and its RSO modules are translated from PowerPC to C at
build time by our fork of [DolRecomp](https://github.com/Trick230/DolRecomp/tree/dualize)
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
- **5× headroom.** ~162 FPS uncapped in gameplay, up from ~32 at the start
  of the native port.
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
| **Version** | NiGHTS: Journey of Dreams, USA (`R7EE8P`) only |
| **Playable** | Cold boot, intro, Story Select and gameplay |
| **Frame rate** | Game at 30 Hz, steady pacing (33.4 ms, σ ≈ 0.2 ms); shown at the display's refresh rate |
| **Video** | Native renderer, 16:9 or 4:3 from the console's setting, letterboxed when the window doesn't match |
| **Audio** | Native DSP/AX, bit-identical to Dolphin's HLE, out through SDL3 |
| **Input** | SDL3 gamepads as Classic Controllers (Xbox, PlayStation and Switch layouts) |
| **Tested on** | Windows 11, MSYS2 UCRT64, Clang 22, i5-9600KF + RTX 4060, 240 Hz display |
| **Untested** | Linux and macOS |

**Known issues**

- Interpolation: some trees close by can briefly jitter as you walk up to
  them. Being worked on.
- The intro FMV runs at ~19 FPS, bound by the game's own video decoder. FMVs
  aren't interpolated.
- The stage RSO modules aren't recompiled yet and run on the interpreter.

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
| `--unthrottled` | Run the game as fast as it goes (turns interpolation off) |
| `--window <w>x<h>` | Initial window size |
| `--load-state <file>` | Start from a savestate |
| `--automation-dir <dir>` | Drive the game from command files: `save_state`, `load_state`, `pad`, `pad_frames`, `clear_pad`, `stop` (client: `scripts/automation.py`) |

<details>
<summary><b>Environment switches</b></summary>

| Variable | Effect |
|---|---|
| `DUALIZE_GX_STATS=1` | Log the renderer once per second: FPS, draws, FIFO, render-thread time split, frame interval mean/σ/max, interpolated presents and their cost |
| `DUALIZE_PROFILE=<file>` | Sample the CPU and render threads' call stacks every millisecond and write them at exit; `scripts/profile.py <file> <exe> [--thread gx\|cpu] [--callers <name>]` reports per-function times (Windows) |
| `DUALIZE_IOS_TRACE=1` | Log each IOS request (open path, ioctl number, sizes), its reply and when the PPC receives it |
| `DUALIZE_PAD_TARGET=gc` | Gamepads drive GameCube pads instead |
| `DUALIZE_PAD_TARGET=none` | Gamepads drive nothing, only automation does (deterministic tests) |
| `DUALIZE_WIIMOTE_SPEAKER=0` | Silence the Wii Remote speaker |
| `DUALIZE_AUDIO_STATS=1` | Log the audio queue once per second |
| `DUALIZE_DUMP_TEXTURES=1` | Write each texture no HD pack replaces to `Cache/NativeGX/texture_dumps` in the user folder |
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
 disc ─► dolrecomp (R7EE8P.toml) ─► ppc_recomp.*.c ─► clang -O3 + ThinLTO ─┐
                                                                            ▼
 ┌─ DualizeRecomp ──────────────────────────────────────────────────────────────────┐
 │  CPU thread                                    recompiled game                  │
 │   ├ native SDK functions (OS scheduler, mutexes, PSMTX, memcpy, cache, MWCC)    │
 │   ├ flat guest memory: one load or store per access                             │
 │   ├ scheduler · VI · PI · SI · EXI · AI · DSP · GPIO                            │
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
[`DualizeRecomp/sdk`](DualizeRecomp/sdk), each pinned to a hash of the guest
bytes it replaces ([`R7EE8P.natives`](DualizeRecompLib/config/R7EE8P.natives)).
The game's RSO modules are linked inside the recompiler at the addresses the
game loads them to, so they recompile with the DOL.

### Runtime

| Part | What it does |
|---|---|
| `cpu/` | The recompiled code wherever it covers the pc; an interpreter with the same semantics elsewhere. Timebase, decrementer, exceptions (interrupts and FP-unavailable save the OS context natively and start in the OS handler; the OS's sync system call runs in host code), locked L1 cache and its DMA |
| `kernel/` | Guest memory: the 32-bit space as one host reservation, MEM1, MEM2 and the locked cache as views of one section. The event scheduler (one tick per CPU cycle; the idle loop leaves for it). IOS, entered from the SDK's IPC client (no mailbox): fd table, `/dev/di`, `/dev/fs` (NAND in the user directory), `/dev/es`, `/dev/stm`, `/dev/net/kd`, the remotes' pairing in SYSCONF, and the Wii Remote model (reports, EEPROM, speaker, Classic Controller) |
| `hw/` | PI, VI, SI, EXI, AI, DSP, CP/PE registers, Hollywood GPIO |
| `apu/` | The AX DSP microcode in host code, mixed out through SDL3 |
| `gpu/` | The GX FIFO into Aurora, frame pacing and interpolation |
| `hid/` | SDL3 gamepads as Classic Controllers or GameCube pads |
| `sdk/` | Native replacements for SDK functions, including the IPC client's transport and reply handler (`rvl_ipc.cpp`) and WUD, the Wii Remotes' side of the SDK's Bluetooth layer (`rvl_wud.cpp`): WPAD's reports go straight to the remote model, the Bluetooth stack never starts |
| `boot.cpp` | The system menu's disc launch, then the apploader |

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
| Cache-range and MWCC helper natives, gather-pipe fast path | 96 |
| Sparse entry switches | 101 |
| Native runtime with flat guest memory | 113 |
| One C function per guest function | 122 |
| GX decoder: leaner merge path for the ~16K small draws a frame | 143 |
| GX decoder: vertex array uploads grow geometrically | 158 |
| **Built for x86-64-v3 (AVX2, FMA, MOVBE) instead of the build machine** | **162** |

The guest CPU thread sets the uncapped rate again: GX decode takes ~4.2 ms a
frame and the render thread idles a third of the time, with the CPU waiting
on it ~3 ms a second. At 30 FPS that leaves the CPU thread idle about 70% of
the time. How each
step was done is in [docs/HISTORY.md](docs/HISTORY.md).

## Roadmap

| Phase | Goal | Status |
|---|---|---|
| 1. SDK symbol map | Name the SDK functions in `main.dol` from Dolphin's signature database | ✅ 1,369 strong + 253 range matches |
| 2. Native GPU | Draw the GX stream with Aurora | ✅ Native renderer |
| 3. Native OS/SDK | Natives, codegen, native DSP/IOS/input, a runtime with no emulator | ✅ · ⏳ stage RSO modules |
| 4. Interpolation | Render between game ticks at the display's rate | 🟡 Running at 240 Hz; tree and shadow wobble left |
| 5. Own runtime | One CMake project with the recompiler, Aurora and our runtime | ✅ |
| 6. Per-function recompiler | XenonRecomp-style functions: registers as locals, direct calls, hooks by symbol | 🟡 Functions, direct calls, live cycle counter, safepoints; registers as locals left |
| 7. HLE runtime | OS threads, alarms, IOS and WPAD at the SDK boundary instead of the hardware | ✅ Native interrupt entry, guest calls from the host, IOS and WUD at the SDK, dispatch cache |

What phase 7 did, in order:

| Step | What changes | Status |
|---|---|---|
| Native interrupt entry | External and decrementer interrupts and FP-unavailable save the OSContext in host code and start in the recompiled OS handler; the system call (a memory sync) runs in host code. No interpreter in play | ✅ |
| Guest calls from the host | Run a guest function to its return from host code, inside an interrupt handler (scheduler off, so no thread switch) | ✅ |
| IOS at the SDK | `ios_Ipc2` hands requests straight to the IOS devices; the IPC interrupt handler delivers replies as `IpcReplyHandler` does (callback or wakeup). No mailbox, doorbell or acks | ✅ |
| Alarms | `OSSetAlarm`, `OSSetPeriodicAlarm` and `OSCancelAlarm` as scheduler events instead of the decrementer | ⏭️ Skipped: the whole decrementer path is 0.8% of the CPU thread in play |
| Interrupt causes | VI, PE, DSP/AI and SI call their handlers from the OS table directly | ⏭️ Skipped: `__OSDispatchInterrupt` is 4.1% inclusive, nearly all of it the handlers' own work |
| WUD at the SDK | WPAD and KPAD stay the game's code; WUD's device side is the host's: output reports go to the remote model, input reports to WPAD's receive callback, connections to its connection callback. No Bluetooth stack, HCI, L2CAP or IOS USB driver | ✅ |
| Dispatch cache | A direct-mapped pc → function and code cache ahead of the function table, for the run loop and indirect calls | ✅ Dispatcher from 3.8% to 2.5% of the CPU thread |
| Threads as fibers | Each guest thread on its own host stack, so a thread switch or interrupt resumes without unwinding through the dispatcher | ⏭️ Skipped: ~1K switches and ~3K interrupts a second leave at most ~2.5% to win, and host stacks can't go in savestates |

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
tools/DolRecomp/      submodule, our fork: the PowerPC → C recompiler and the guest CPU core
thirdparty/aurora/    submodule, our fork: GX on WebGPU (Dawn), SDL3
DualizeRecompLib/     the game's code: recompiler config, natives list, generated sources
DualizeRecomp/        the runtime and the executable (cpu, kernel, hw, apu, gpu, hid, sdk)
scripts/              fork sync, SDK symbol finder, natives hashes, automation client, window capture
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
