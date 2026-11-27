# Roadmap

Every phase, step by step, with what was measured on the way. The overview is in the [README](../README.md).

| Phase | Goal | Status |
|---|---|---|
| 1. SDK symbol map | Name the SDK functions in `main.dol` from Dolphin's signature database | ✅ 1,369 strong + 253 range matches |
| 2. Native GPU | Draw the GX stream with Aurora | ✅ Native renderer |
| 3. Native OS/SDK | Natives, codegen, native DSP/IOS/input, a runtime with no emulator, the stage RSO modules | ✅ |
| 4. Interpolation | Render between game ticks at the display's rate | ✅ 240 Hz; the game names its cameras, skinned draws pair by their bones |
| 5. Own runtime | One CMake project with the recompiler, Aurora and our runtime | ✅ |
| 6. Per-function recompiler | XenonRecomp-style functions: registers as locals, direct calls, hooks by symbol | ✅ Functions, direct calls, live cycle counter, safepoints, registers as locals where they pay (no speed gained in the forest, see below) |
| 7. HLE runtime | The OS, IOS, WPAD and every device the game drives at the SDK boundary instead of the hardware: no register models left | ✅ Native interrupt entry, guest calls from the host, every device at the SDK, dispatch cache; the steps below |
| 8. Image quality | Anti-aliasing, texture filtering and rendering above the display's resolution | ✅ MSAA 8× (default), render scale with an area filter, 16× anisotropic, GTAO ambient occlusion (off by default, `--ao`: on NiGHTS' layered models it either draws lines or leaves them bare, and the game's baked shading leaves it little to add; fades into the game's fog; 1.8 ms a frame GPU-bound at 3× scale and 8×, down from 4.2; 4 slices, since 2 left a grey wash on flat ground; full resolution up to a 1440-line frame buffer, 0.3 ms more at 1920×1080, since half resolution drew crawling dashes along creases; multi-bounce from the EFB's color, 0.16 ms at 1440 lines; with 8× and interpolation an RTX 4060 keeps 240 presents a second at 1920×1080, at 94%; resolving the EFB only for color copies took 1 ms a frame off at 3×, and replays that skip the copies nothing in their frame samples took the GPU from 94% to 85% there) |
| 9. Native renderer | Aurora on D3D12 and Vulkan directly instead of through Dawn (WebGPU), as UnleashedRecomp's plume does | ✅ Layer over both APIs done and tested against each other under their validation layers; the GX generator and the ubershader in HLSL compile offline for both (578 sources, 13 s); the game runs on both without Dawn, with interpolation, ImGui and texture replacement: in all six test scenes D3D12 matches Dawn's frames but for at most 0.1% of pixels off by more than 2 of 255, at the same speed, and both backends run clean under validation; Vulkan ran about 8% slower uncapped until its upload buffers moved from CPU-visible VRAM to system memory, as on D3D12; both now run the forest at about 600% uncapped. Dawn is out of the build. The seed's shaders and aurora's own (copies, palettes, present, ImGui) ship compiled for both backends (`aurora_shader_pack`, 786 shaders each, 14 s at build time), so a first start builds all pipelines, ubershaders included, in about 50 ms on D3D12 and 200 ms on Vulkan and never runs DXC. Machines keep what they compile in `Cache/NativeGX/shaders_<backend>.bin`. MSAA goes up to 8×, at the same speed as 4× (the game's thread is the limit uncapped; the render thread idles a quarter of the time, so `handle_draw` was left alone), and an optimized depth clear value measured no gain even GPU-bound (3× scale, 8×), so targets keep none. The matrix and light loads are GX at the API (below) |

What phase 7 did:

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
| GX registers at the SDK | The draw-done and token interrupt handlers are host code over the renderer's signals. GX's FIFO setup and the `GXPoke` family stay the game's code against windows that ignore writes and read idle: the renderer takes the bytes as the CPU writes them, so there is no FIFO to set up. No CP, PE or MI registers | ✅ |
| AI, DSP and ARAM at the SDK | AI DMA (`AIInitDMA`, `AIStartDMA`, `AIStopDMA`, `AIGetDMABytesLeft`), the DSP mailbox and both interrupt handlers go straight to the native DSP; AX stays the game's code over it. What runs once at boot (`__OSInitAudioSystem` with the game's only ARAM DMA, `AIInit`, `DSPInit`) keeps the interface block's registers: the game doesn't link the ARAM library | ✅ |
| SI and EXI at the SDK | `PADRead` builds each report from the host pad and runs the SDK's own `MakeStatus`; PAD's init, reset and rumble and the SI's init are host code. `EXILock`, `EXISelect`, `EXIImm`, `EXIDma` and the rest finish each transfer in the call on the IPL chip (RTC, SRAM, the UART behind OSReport). No serial or expansion interface registers or interrupts | ✅ |
| DVD and NAND at the SDK | `DVDLowRead` and `DVDLowClearCoverInterrupt` are served by the host's drive, and the IOS client calls (`IOS_Open`, `IOS_Read`, `IOS_Ioctl` and the rest, with their async forms) go straight to the host's file system when they name the NAND. The DVD queue, `NAND*` and `ISFS_*` stay the game's code. IOS requests remain for the rarer `DVDLow` commands and the other devices: boot and gate went from over 500 requests to under 50 | ✅ |

After phase 6, before the stage modules:

| Step | What changes | Status |
|---|---|---|
| GX at the API | `GXBegin`, the vertex writes, matrix loads and state setters call Aurora directly; no FIFO encoding and decoding | ✅ Done as far as it pays: `GXLoadPosMtxImm`, `GXLoadNrmMtxImm`, `GXLoadTexMtxImm` and `GXLoadLightObjImm` are natives that put one host packet with the call and its data in the stream (aurora's README, "GX calls from the host"); a check mode that let the game's own packet follow each one matched it word for word over ~240,000 calls in four scenes. Forest uncapped 624% → 634%. What is left of the game's GX library is 1.0% of the CPU thread (was 2.2%), spread over dozens of functions; the state setters would need the SDK's own state block kept exact for that, and vertex writes (the game's own stores) and display lists stay FIFO streams anyway, so they stay as they are |
| AX mixing off the CPU thread | Each AX command list is mixed on a thread of its own while the game runs on; the DSP's mails arrive half a millisecond of DSP time later | ✅ Mixing from ~1.3% of the CPU thread to ~0.6% (the mailbox and the hand-off) |
| AX at the SDK | `__DSP_boot_task` starts the host's mixer with no upload, `__AXOutNewFrame` hands it the command list, and `__DSPHandler` resumes the task at a mix's end, all with no mail to or from the guest in a frame. AX's voice and command list code stays the game's, and the mixer stays the ucode's model behind the DSP interrupt. Quitting or switching tasks still goes through the mailbox | ✅ |

Phase 8, image quality, in order (Aurora renders at the window's size and
scales EFB copies with it, so shadows, bloom and reflections follow the
render scale):

| Step | What changes | Status |
|---|---|---|
| MSAA | Multisampled render targets, resolved for EFB copies and the present; depth copies read sample 0. 8× by default, capped at what the GPU does; `--msaa 1` turns it off | ✅ Forest at 1080p uncapped: 4× and 8× as fast as each other (about 590% on D3D12) |
| Render scale | `--render-scale <x>` renders at x times the window (EFB copies, so shadows and bloom, follow) and reduces with an area (box) filter, so every rendered pixel counts; at 1× the frame goes straight to the present | ✅ Interpolation renders every present, so the cost scales with the present rate: forest at 1080p ×2 with MSAA ~120 presents/s, without ~270, ×1.5 ~210 |
| Alpha-to-coverage | Unblended alpha-test cutouts (the forest's ferns and leaves) become coverage under MSAA: the edge spreads over about a pixel's samples instead of a hard discard. Only where the EFB keeps no alpha, as in all of NiGHTS | ✅ No measurable cost (forest ~450 presents/s either way) |
| Anisotropic filtering | 16× on every mipmapped texture, not only where the game asks | ✅ |
| Color depth | 8 bits a channel with no dithering, where the console has 6-bit RGBA6 or dithered RGB8 | ✅ Aurora's behaviour |
| Texture LOD | Mip selection follows the render resolution, so higher render scales pick sharper mips with no bias; HD replacement textures keep their own bias | ✅ Aurora's behaviour |
| Pipeline precompile | Everything the pipeline cache knows compiles on a worker pool (cores minus two) before the game starts, so none of it pops in; the stats line counts draws skipped while compiling | ✅ Forest, 310 pipelines: 60 ms warm, 3.1 s with no driver cache (11.1 s on one thread) |
| Seed pipeline cache | `DualizeRecomp/resources/initial_pipeline_cache.db`, merged into the user's cache at each start, its shaders compiled at build time; `scripts/pipeline_seed.py` adds a played cache to it and drops rows of stale config versions. Seeded with MSAA on, it serves `--msaa 1` too | ✅ 680 pipelines from the test scenes and coverage replays; a playthrough of every stage fills it |
| Ubershader | One shader that reads the TEV, texgen and lighting setup from a per-draw table, drawing anything whose own pipeline is still compiling, so a never-seen shader doesn't pop in. Its pipelines vary only by fixed-function state (blend, depth, cull): they're derived from the cache at boot, and the frame waits for a never-seen combination of those, so no draw is skipped. `AURORA_FORCE_UBERSHADER=1` draws everything with it, `2` every other pipeline | ✅ Entering a stage with none of its shaders cached: 10708 draws by the ubershader, 0 skipped, worst frame 105 ms (one such wait) and otherwise 65 ms or less. Forced on every draw, with and without GX state randomized (texgens with emboss, indirect, TEV, lights, fog, alpha compare, lines and points; deliberate errors in those change the frames): the forest's frames are bit-identical, others differ by 1/255 in a few pixels at most. All of a frame on it is slow (forest 25 fps), which never happens in play. Unthrottled speed unchanged (625%) |

Phase 9, the native renderer: Aurora's backend on D3D12 and Vulkan without Dawn,
with the same features on both, and the TEV shaders as one HLSL source that DXC
compiles to DXIL and SPIR-V. Dawn stayed until the numbers matched. It lifts WebGPU's limits (MSAA 8×) and drops a layer
between Aurora and the driver.

Registers as locals, measured in the forest uncapped: the old code reached
~626% speed, every register in a local ~586% (each one loaded on entry and
stored on the way out, past what x86-64 can keep in registers), and `restrict`
on the context with or without locals ~624%. A register now gets a local only
where it is touched well more often than it has to move, and that wins no speed
either; guest RAM hashes match the old code's in the gate and the forest.

Next: an in-game graphics menu, a playthrough of every stage (bugs, and the
seed pipeline cache) and interpolated FMVs. The interpreter stays as the
safety net: in play nothing runs on it (the gate and the forest interpret
nothing), but code the tables don't cover, an entry point inside a function
and the system instructions the emitter leaves to it (`mtspr`, `mfspr`,
`icbi`) still step on it instead of stopping the game.

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
