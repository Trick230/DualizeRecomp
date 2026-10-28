#pragma once

// Native GX: renders a guest's retail GX FIFO with Aurora (WebGPU) on its own
// render thread and window.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called on the render thread.
typedef struct NativeGxHost {
  void* user;
  // Guest address (virtual or physical) to host memory covering at least size
  // bytes; available receives the bytes left in that memory region.
  bool (*resolve)(void* user, uint32_t address, uint32_t size, const void** data,
                  uint32_t* available);
  // The GPU reached a GXSetDrawDone / GXSetDrawSync marker.
  void (*draw_done)(void* user);
  void (*token)(void* user, uint16_t token, bool interrupt);
  // The user closed the window.
  void (*quit)(void* user);
} NativeGxHost;

typedef struct NativeGxConfig {
  const char* title;
  const char* cache_path;  // existing directory for Aurora's pipeline cache and settings
  // HD texture packs: DDS/PNG files named like Dolphin's dumps (tex1_...).
  const char* texture_path;
  uint32_t width;
  uint32_t height;
  bool vsync;
  // Present replays between the guest's frames, blended between the last two
  // (see Aurora's frame_interp), at the refresh rate or present_rate (no
  // vsync; 0 = the refresh rate).
  bool interpolate;
  uint32_t present_rate;
  // The aspect the guest's frames are shown at; the window keeps it.
  float aspect;
} NativeGxConfig;

// Starts the render thread and waits until Aurora is up. False on failure.
bool native_gx_start(const NativeGxConfig* config, const NativeGxHost* host);
// Queues big-endian FIFO bytes (the guest's gather-pipe bursts). Producer
// thread only; blocks while the render thread is a full ring behind.
void native_gx_submit(const uint8_t* data, size_t size);
// Producer thread only: returns once every queued byte has been processed and
// its draw-done and token markers reported, so emulator state saved afterwards
// holds them. Gives up after a few seconds if the render thread is stalled.
void native_gx_drain(void);
// Producer thread only, after native_gx_drain: the decoder state (GX registers
// and any incomplete packet) as bytes for native_gx_load_state. The buffer
// stays valid until the next call.
const uint8_t* native_gx_save_state(size_t* size);
// Producer thread only: replaces the decoder state with one saved by
// native_gx_save_state; the stream submitted next continues from it.
void native_gx_load_state(const uint8_t* data, size_t size);
void native_gx_stop(void);

#ifdef __cplusplus
}
#endif
