# History

DualizeRecomp started as patch series on ModernGekko, its Dolphin fork and
the DolRecomp it built. Those patches are now part of the source:

| Then | Now |
|---|---|
| `patches/ModernGekko/` (the native machine) | [`DualizeRecomp/`](../DualizeRecomp) |
| `patches/ModernGekko-dolphin/` (Aurora parts) | commits on [Trick230/aurora](https://github.com/Trick230/aurora/commits/dualize) |
| `patches/ModernGekko-dolphin-DolRecomp/` | commits on [Trick230/DolRecomp](https://github.com/Trick230/DolRecomp/commits/dualize) |

What each patch did, and what it bought, is kept below. The FPS figures are
uncapped gameplay in Will's first stage on the reference machine (i5-9600KF,
RTX 4060) at the time of each change. Parts that only concerned the
Dolphin-derived core are gone with it.

## Performance by step

Uncapped gameplay in Will's first stage, i5-9600KF + RTX 4060, as FPS while
the README still counted it that way.

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

At that point the guest CPU thread set the uncapped rate again: GX decode took
~4.2 ms a frame and the render thread idled a third of the time, with the CPU
waiting on it ~3 ms a second. Later changes are measured as the speed the
game runs at uncapped (see the README).

## ModernGekko

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
- **`0011` Display stays awake.** The runtime inhibits the screen saver while
  the game runs (honouring Dolphin's `DisableScreenSaver`); with the display
  asleep, presentation blocks and the game drops to a few frames per second.
- **`0012` PSMTX/PSVEC natives.** Fourteen more paired-single matrix and
  vector functions (MultVec, Trans, Scale, Normalize, Cross/DotProduct, ...).
- **`0013` CPU ABI 6, cache and MWCC runtime natives.** The `DC*Range` cache
  functions run as one host call per range; MWCC's `_savegpr`/`_restgpr`/
  `_savefpr`/`_restfpr` helpers, which almost every prologue and epilogue
  calls, run natively. With patch 0015 below: 82.7 → 95.7 FPS uncapped.
- **`0014` Native DSP.** The DSP interface, mail handler, ROM/INIT/AXWii
  ucodes and sample accelerator run in ModernGekko's own `DspCore`
  (`src/audio`). Steady-state audio is bit-identical to Dolphin's HLE, with
  the same savestate layout.
- **`0015` SDL3 audio output.** The DSP's DMA stream plays through an SDL3
  stream. A ±0.5% rate correction holds the queue near 40 ms, so clock drift
  never underruns or builds latency.
- **`0016` SDL3 gamepads.** `src/input/gamepads.cpp` polls pads on a 1 kHz
  thread into seqlock snapshots. `native_input_host.cpp` maps them and merges
  automation input on top.
- **`0017` Native GameCube pads.** The same gamepads drive SI pads when
  `MODERNGEKKO_PAD_TARGET=gc`.
- **`0018` Buffer-speed disc reads, unplugged idle GC pads.** A cold boot
  waited 7 emulated seconds on modelled seeks; now it waits 0.5 s. Disc time
  during loads is ~3% of the load: loading is CPU-bound in the game.
- **`0019` Wii Remote speaker, SDL ownership.** Speaker samples play on their
  own SDL stream on the same device. With native input, Dolphin's SDL
  backend is never created. It called `SDL_Quit` concurrently, which hung
  about 1 in 6 savestate loads.
- **`0020` Fast asset listing, frame interval stats.** The startup asset hash
  check lists `files/` with one `FindFirstFileExW` pass instead of two
  `std::filesystem` stats per file. That pass ran twice per launch on 12k
  files. The cached hash value is unchanged. Time to the first frame went
  from ~11 s to ~4.5 s. `NATIVE_GX_STATS` also reports frame intervals.
- **`0021` Native IOS devices.** `src/ios` holds IOS's `/dev/di` driver and
  drive, `/dev/stm` and `/dev/net/kd` as host code that doesn't depend on
  Dolphin. The host supplies guest memory, timed events and the IPC reply
  path. The disc driver queues commands like IOS does and completes reads
  block by block at the drive buffer's rate. It matches Dolphin's timing,
  results and savestate layout, so old states load and new ones load in
  Dolphin. Cold boot audio is bit-identical to Dolphin's devices for the
  whole 62 s that boot is deterministic, steady-state audio too, and
  gameplay is ~1% faster.
- **`0022` Native machine.** `moderngekko-native` (`src/native`) and
  `moderngekko-port run-native`; see [Native machine](../README.md#native-machine).
  `/dev/fs`, `/dev/es` and Bluetooth join `src/ios`. Along the way:
  - The locked-cache DMA takes physical addresses as well as effective ones.
    The SDK passes MEM2 matrix arrays as physical addresses, and dropping
    them left every indexed matrix zero, so the 3D scene was black.
  - When the module reaches the idle loop, the CPU leaves for the scheduler
    instead of re-entering native code. Re-entering ran a full
    wake/poll/sleep round of the game's worker thread for every interrupt,
    ~45% of the time in gameplay. Gameplay went from 54 to ~90 FPS
    uncapped.
  - The disc directory opens and seeks files with the platform's wide-path,
    64-bit calls.
  - CPU ABI 7 adds `flat_base`, the guest address space mapped 1:1 (see
    [Native machine](../README.md#native-machine)). Modules index it directly instead of
    going through the 64 KiB-slice table: 90 → ~113 FPS uncapped, together
    with the next fix.
  - The CPU thread waits for the render thread to drain on a semaphore the
    render thread signals, not a 1 ms sleep, which Windows rounds to its
    15.6 ms timer tick. With flat memory the sleep alone held the game at
    64 FPS.
- **`0023` Frame interpolation options, display aspect.** The native
  machine turns on Aurora's [frame interpolation](../README.md#frame-interpolation)
  while throttled, with `--no-interpolation` and `--present-rate <hz>`. It
  reads the console's aspect setting (SYSCONF `IPL.AR`) so the window keeps
  16:9 or 4:3. `NATIVE_GX_STATS` adds interpolated presents, the share of
  draws blended and the cost of each replay.

## ModernGekko's Dolphin fork

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
- **`0011` Idle until an interrupt, compact chunk lookup.** At the game's idle
  pc, `Run()` skips from CoreTiming event to CoreTiming event until an
  interrupt is actually taken. Before, it re-ran the idle thread's
  wake/poll/sleep round after each event, which was ~45% of all dispatches.
  The chunk lookup is one `s16` per 128-byte line. 68 → 81.6 FPS uncapped.
- **`0012` Direct indirect and tail calls.** One "may enter directly" byte per
  MEM1 line lets `bctrl`, jump tables and cross-chunk tail branches enter the
  target without going through `Run()`. Dispatches drop 62%.
- **`0013` Inline single widening.** The normal-number path of
  `convert_to_double` is inlined.
- **`0014` Locked cache without the MMU, builtin byte swaps.** MinGW's
  `_byteswap_*` were real calls. 83.5 → 88 FPS.
- **`0015` CPU ABI 6.** Locked-cache and gather-pipe accesses skip MMU
  translation. Short gather-pipe stores append straight to GPFifo, and a
  `cache_range` hook backs the `DC*Range` natives. The chassis steps a pc
  that has no entry case in the interpreter and reports these misses at
  shutdown.
- **`0016` VI batches quiet half-lines.** The VI event fires only at
  half-lines that do something. Beam reads and register writes catch up
  first, and states save in the old format.
- **`0017` Game-ID generation.** `Run()` re-checks the module match only when
  SConfig's game ID changes, instead of copying the string every slice.
- **`0018` Native DSP hooks.** `DSPManager::SetNative` forwards MMIO, events,
  slices and savestates to ModernGekko's `DspCore`.
- **`0019` SDL audio.** Turns SDL's audio subsystem on in the vendored SDL3.
- **`0020` Wii Remote native input.** `WiimoteEmu::SetNativeInput` replaces
  InputCommon for buttons, sticks and triggers. BTEmu stops polling
  ControllerInterface.
- **`0021` GameCube pad native input.** `Pad::SetNative`, the SI-side
  equivalent.
- **`0022` Speaker sink, SDL backend switch.** SpeakerLogic forwards decoded
  samples, and ControllerInterface's SDL backend can be left out.
- **`0023` Native IOS device routing.** `IOS::HLE::SetNativeDevices` lets the
  host add devices under the names of Dolphin's. The game's opens, fd
  restores and per-device savestates go to the native ones. Dolphin's stay in
  the map for the code that casts them. DVDInterface completions and console
  buttons reach the native `/dev/di` and `/dev/stm`, and a CoreTiming event
  carries the native devices' timers.
- **`0024` Flat guest memory.** GXRuntime's `mem_read*_at`/`mem_write*_at`
  index `CPUState::flat_base` when a module is built with
  `GXRUNTIME_FLAT_MEMORY` (the module template sets it), with one range check
  for the hardware registers. The Dolphin core hands modules its fastmem
  arena's logical base. Modules also carry line tables now, for profiling;
  code generation is unchanged.
- **`0025` Aurora frame interpolation, aspect, resizing.**
  - `lib/gfx/frame_interp.cpp`: each frame's draws and uniform stream are
    recorded; the newest frame is held back while the one before it is
    replayed at the display's refresh rate with blended matrices, lights and
    post-transform texture matrices. See
    [Frame interpolation](../README.md#frame-interpolation).
    Draws are paired with the older frame's in two passes: first every draw
    exactly where an older one is, then the rest with the nearest close
    instance of their model. In one pass a tree coming into view next to
    another instance of its model took that one's older draw and slid
    across from it.
  - The window keeps the console's aspect ratio and letterboxes when it
    can't (maximized, fullscreen).
  - A window resize is applied at the start of the next guest frame, and
    interpolation drops its held frames first. Resizing mid-frame used to
    crash and stall a third of the presents.
  - Viewports, scissors and EFB copies are snapped to whole target pixels.
    At non-integer scales the game's 256×128 bloom buffer ended mid-pixel,
    and the blur smeared the half-drawn column into a light stripe down the
    right edge.

## DolRecomp (vendored in the Dolphin fork)

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
- **`0005` Direct indirect and tail calls.** The emitter side of the Dolphin
  fork's `0012`.
- **`0006` Sparse entry switches.** A chunk's entry switch keeps cases only
  where code can be entered from outside: block leaders, natives, branch
  targets, symbols, code pointers in data and resume points. This lets the
  compiler optimize across the instructions in between. 96 → 101 FPS, and
  module builds go from 42 to 27 min. `DOLRECOMP_C_SPARSE_ENTRIES=0` restores
  the dense switch.
- **`0007` Flat base local, CR selects.** Each generated function loads
  `ctx->flat_base` once into a local, so memory accesses don't reload it
  after every store through `ctx`. Compares set a CR field with one select
  chain instead of three conditional ORs.
