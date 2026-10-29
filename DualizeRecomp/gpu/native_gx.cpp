#include "gpu/native_gx.h"

#include "profiler.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/texture.hpp>
#include <gx/recomp.hpp>

#include <SDL3/SDL_mutex.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <string>
#include <vector>

namespace
{
using aurora::gx::recomp::StreamEvent;

// One frame of NiGHTS is ~1-2 MB of FIFO; the ring holds several so the CPU
// thread only waits when the renderer is genuinely behind.
constexpr size_t RING_SIZE = size_t{16} << 20;
constexpr size_t RING_MASK = RING_SIZE - 1;

struct Ring
{
  std::unique_ptr<uint8_t[]> data{new uint8_t[RING_SIZE]};
  alignas(64) std::atomic<uint64_t> write{0};
  alignas(64) std::atomic<uint64_t> read{0};
  // Set by the consumer before it sleeps, so the producer only pays for a
  // wake-up when one is needed.
  alignas(64) std::atomic<bool> consumer_waiting{false};
  // Ring position up to which the render thread has processed everything
  // (only an incomplete trailing packet may remain), for native_gx_drain.
  alignas(64) std::atomic<uint64_t> settled{0};
  // Set by native_gx_drain before it sleeps on `drained`, which the render
  // thread then signals when settled moves.
  std::atomic<bool> drain_waiting{false};
  // Set by native_gx_load_state: the next bytes start a fresh stream, so the
  // render thread drops the incomplete packet it kept.
  std::atomic<bool> restart{false};
  // SDL's semaphore is native on every platform; winpthreads' timed condvar
  // wait was seen sleeping far past its timeout.
  SDL_Semaphore* wake = nullptr;
  SDL_Semaphore* drained = nullptr;
};

NativeGxHost s_host;
NativeGxConfig s_config;
// Owned copies: the caller's strings need not outlive native_gx_start.
std::string s_title;
std::string s_cache_path;
std::string s_texture_path;
Ring s_ring;
// Received bytes not yet processed: after a batch, the head of an incomplete
// packet. Render thread only, except while drained (native_gx_save_state).
std::vector<uint8_t> s_work;
std::thread s_thread;
std::atomic<bool> s_stop{false};

// CPU-thread time spent waiting on the render thread: a full ring, or a drain
// before idling. Written by the CPU thread, collected by Stats::Report.
std::atomic<int64_t> s_cpu_wait_ns{0};

void AddCpuWait(std::chrono::steady_clock::time_point start)
{
  s_cpu_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start).count(),
                          std::memory_order_relaxed);
}

// DUALIZE_GX_STATS=1 prints one line per second of render-thread counters.
struct Stats
{
  bool enabled = false;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  uint64_t bytes = 0;
  uint32_t frames = 0;
  uint32_t draw_dones = 0;
  uint32_t tokens = 0;
  uint32_t draws = 0;
  uint32_t dropped = 0;  // draws that did not fit Aurora's fixed per-frame buffers
  uint32_t stalled_begins = 0;
  uint32_t peak_uniforms = 0;  // largest per-frame use of Aurora's fixed buffer budgets
  uint32_t peak_vertices = 0;
  uint32_t peak_storage = 0;
  // Render-thread time split: decoding the FIFO into Aurora draws, starting
  // frames (waiting for a free frame slot), ending them (handing the frame to
  // Aurora's submit worker) and idling for guest data.
  std::chrono::steady_clock::duration decode{}, begin_frame{}, end_frame{}, idle{};
  // Intervals between frames handed to Aurora; the previous frame's time
  // carries over so the first interval of each second is counted too.
  std::chrono::steady_clock::time_point last_frame{};
  std::chrono::steady_clock::duration frame_sum{}, frame_max{};
  double frame_sq_ms = 0.0;
  uint32_t intervals = 0;

  void Frame()
  {
    const auto now = std::chrono::steady_clock::now();
    if (last_frame != std::chrono::steady_clock::time_point{})
    {
      const auto interval = now - last_frame;
      const double ms = std::chrono::duration<double, std::milli>(interval).count();
      frame_sum += interval;
      frame_max = std::max(frame_max, interval);
      frame_sq_ms += ms * ms;
      ++intervals;
    }
    last_frame = now;
  }

  void Report()
  {
    const auto now = std::chrono::steady_clock::now();
    if (!enabled || now - last < std::chrono::seconds(1))
      return;
    const double mean =
        intervals ? std::chrono::duration<double, std::milli>(frame_sum).count() / intervals : 0.0;
    const double sd = intervals ? std::sqrt(std::max(0.0, frame_sq_ms / intervals - mean * mean)) : 0.0;
    // Interpolation totals since the last report.
    static AuroraStats seen{};
    const AuroraStats* totals = aurora_get_stats();
    const uint32_t presents = totals ? totals->interpPresents - seen.interpPresents : 0;
    const uint32_t interp_draws = totals ? totals->interpDraws - seen.interpDraws : 0;
    const uint32_t blended = totals ? totals->interpBlendedDraws - seen.interpBlendedDraws : 0;
    const uint32_t replay_us = totals ? totals->interpMicros - seen.interpMicros : 0;
    const uint32_t starved = totals ? totals->interpStarved - seen.interpStarved : 0;
    const uint32_t late = totals ? totals->interpLatePresents - seen.interpLatePresents : 0;
    if (totals)
      seen = *totals;
    std::fprintf(stderr,
                 "[native-gx] %u fps, %u draws (%u dropped), %.2f MB fifo, %u draw-done, %u tokens, "
                 "%u stalled frame starts, peak %.1f MB uniforms / %.1f MB vertices / %.1f MB arrays per frame, "
                 "ms decode %lld / begin frame %lld / end frame %lld / idle %lld, cpu waits %lld ms, "
                 "frame ms %.2f sd %.2f max %.2f, %u interpolated presents (%.0f%% of draws blended, %.2f ms each, "
                 "%u starved, %u late)\n",
                 frames, draws, dropped, bytes / 1048576.0, draw_dones, tokens, stalled_begins,
                 peak_uniforms / 1048576.0, peak_vertices / 1048576.0, peak_storage / 1048576.0,
                 Ms(decode), Ms(begin_frame), Ms(end_frame), Ms(idle),
                 static_cast<long long>(s_cpu_wait_ns.exchange(0, std::memory_order_relaxed) / 1000000), mean, sd,
                 std::chrono::duration<double, std::milli>(frame_max).count(), presents,
                 interp_draws ? 100.0 * blended / interp_draws : 0.0,
                 presents ? replay_us / 1000.0 / presents : 0.0, starved, late);
    std::fflush(stderr);
    *this = Stats{.enabled = true, .last_frame = last_frame};
    last = now;
  }

  static long long Ms(std::chrono::steady_clock::duration d)
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  }
};
Stats s_stats;

void Log(AuroraLogLevel level, const char* module, const char* message, unsigned int len)
{
  if (level < LOG_WARNING)
    return;
  std::fprintf(stderr, "[native-gx] %s: %.*s\n", module, static_cast<int>(len), message);
  if (level == LOG_FATAL)
    std::abort();
}

bool Resolve(void*, uint32_t address, uint32_t size, aurora::gx::recomp::AddressSpace,
             aurora::gx::recomp::ResourceKind, const void** data, uint32_t* available)
{
  return s_host.resolve(s_host.user, address, size, data, available);
}

// Window events are polled once per frame, and at this interval while the
// guest sends nothing (loading, a paused game), so the window stays responsive.
// Polling drains the Windows message queue, which is not free.
constexpr auto IDLE_PUMP_INTERVAL = std::chrono::milliseconds(16);
std::chrono::steady_clock::time_point s_last_pump;

// Polls window events; false once the user asked to quit.
bool PumpEvents()
{
  s_last_pump = std::chrono::steady_clock::now();
  for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
  {
    if (event->type == AURORA_EXIT)
      return false;
  }
  return true;
}

bool BeginFrame()
{
  const auto start = std::chrono::steady_clock::now();
  const bool begun = aurora_begin_frame();
  s_stats.begin_frame += std::chrono::steady_clock::now() - start;
  if (begun)
    return true;
  while (!aurora_begin_frame())
  {
    // Minimized or the surface is being rebuilt; the guest stalls with us.
    ++s_stats.stalled_begins;
    s_stats.Report();
    if (!PumpEvents() || s_stop.load(std::memory_order_relaxed))
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

// Waits for queued bytes and appends them to work. False on shutdown.
bool Receive(std::vector<uint8_t>& work)
{
  uint64_t read = s_ring.read.load(std::memory_order_relaxed);
  uint64_t write = s_ring.write.load(std::memory_order_acquire);
  while (write == read)
  {
    if (s_stop.load(std::memory_order_relaxed))
      return false;
    const auto start = std::chrono::steady_clock::now();
    const auto pump_due = s_last_pump + IDLE_PUMP_INTERVAL;
    if (start >= pump_due)
    {
      if (!PumpEvents())
      {
        s_host.quit(s_host.user);
        return false;
      }
    }
    else
    {
      s_ring.consumer_waiting.store(true, std::memory_order_seq_cst);
      write = s_ring.write.load(std::memory_order_seq_cst);
      if (write == read)
      {
        const auto timeout = std::chrono::ceil<std::chrono::milliseconds>(pump_due - start);
        SDL_WaitSemaphoreTimeout(s_ring.wake, static_cast<Sint32>(timeout.count()));
        s_stats.idle += std::chrono::steady_clock::now() - start;
      }
      s_ring.consumer_waiting.store(false, std::memory_order_relaxed);
    }
    s_stats.Report();
    write = s_ring.write.load(std::memory_order_acquire);
  }

  if (s_ring.restart.exchange(false, std::memory_order_acquire))
    work.clear();
  const size_t size = static_cast<size_t>(write - read);
  s_stats.bytes += size;
  const size_t offset = static_cast<size_t>(read & RING_MASK);
  const size_t first = std::min(size, RING_SIZE - offset);
  work.insert(work.end(), s_ring.data.get() + offset, s_ring.data.get() + offset + first);
  work.insert(work.end(), s_ring.data.get(), s_ring.data.get() + (size - first));
  s_ring.read.store(write, std::memory_order_release);
  return true;
}

void RenderThread(std::atomic<int>* started)
{
  AuroraConfig config{};
  config.appName = s_config.title;
  // Aurora otherwise derives these from the window title, which need not be
  // a valid directory name.
  config.userPath = s_config.cache_path;
  config.cachePath = s_config.cache_path;
  config.desiredBackend = BACKEND_AUTO;
  config.vsync = s_config.vsync;
  config.frameInterpolation = s_config.interpolate;
  config.presentRate = s_config.present_rate;
  config.displayAspect = s_config.aspect;
  config.msaa = s_config.msaa;
  config.renderScale = s_config.render_scale;
  // 16x on every mipmapped texture, not only where the game asks for it.
  config.maxTextureAnisotropy = 16;
  config.forceTextureAnisotropy = true;
  config.windowWidth = s_config.width;
  config.windowHeight = s_config.height;
  config.windowPosX = -1;
  config.windowPosY = -1;
  config.logCallback = Log;
  config.logLevel = LOG_WARNING;
  // Writes each texture no pack replaces to <cache>/texture_dumps, for making packs.
  config.allowTextureDumps = std::getenv("DUALIZE_DUMP_TEXTURES") != nullptr;
  const AuroraInfo info = aurora_initialize(0, nullptr, &config);
  if (!info.window)
  {
    started->store(-1, std::memory_order_release);
    started->notify_one();
    return;
  }
  aurora::gx::recomp::set_guest_address_resolver(Resolve, nullptr);
  aurora::texture::load_replacement_directory(s_texture_path);
  dualize::profiler::RegisterThisThread("gx");
  started->store(1, std::memory_order_release);
  started->notify_one();

  auto& work = s_work;
  bool running = BeginFrame();
  while (running && Receive(work))
  {
    s_stats.Report();
    size_t pos = 0;
    while (running && pos < work.size())
    {
      StreamEvent event;
      uint32_t value = 0;
      const auto decode_start = std::chrono::steady_clock::now();
      const uint32_t consumed = aurora::gx::recomp::process_stream(
          work.data() + pos, static_cast<uint32_t>(work.size() - pos), &event, &value);
      s_stats.decode += std::chrono::steady_clock::now() - decode_start;
      pos += consumed;
      switch (event)
      {
      case StreamEvent::None:
        break;
      case StreamEvent::DrawDone:
        ++s_stats.draw_dones;
        s_host.draw_done(s_host.user);
        break;
      case StreamEvent::Token:
      case StreamEvent::TokenInt:
        ++s_stats.tokens;
        s_host.token(s_host.user, static_cast<uint16_t>(value), event == StreamEvent::TokenInt);
        break;
      case StreamEvent::DisplayCopy:
        ++s_stats.frames;
        if (s_stats.enabled)
          s_stats.Frame();
        if (const AuroraStats* frame = aurora_get_stats())
        {
          s_stats.draws += frame->drawCallCount;
          s_stats.dropped += frame->droppedDrawCallCount;
          s_stats.peak_uniforms = std::max(s_stats.peak_uniforms, frame->lastUniformSize);
          s_stats.peak_vertices = std::max(s_stats.peak_vertices, frame->lastVertSize);
          s_stats.peak_storage = std::max(s_stats.peak_storage, frame->lastStorageSize);
        }
        {
          const auto end_start = std::chrono::steady_clock::now();
          aurora_end_frame();
          s_stats.end_frame += std::chrono::steady_clock::now() - end_start;
        }
        if (!PumpEvents())
        {
          s_host.quit(s_host.user);
          running = false;
        }
        else
        {
          running = BeginFrame();
        }
        break;
      }
      if (event == StreamEvent::None)
        break;  // the rest is an incomplete packet
    }
    work.erase(work.begin(), work.begin() + static_cast<std::ptrdiff_t>(pos));
    if (running)
    {
      s_ring.settled.store(s_ring.read.load(std::memory_order_relaxed), std::memory_order_seq_cst);
      if (s_ring.drain_waiting.exchange(false, std::memory_order_seq_cst))
        SDL_SignalSemaphore(s_ring.drained);
    }
  }

  // Nothing drains the ring from here on; never let the producer wait for it.
  s_stop.store(true);
  SDL_SignalSemaphore(s_ring.drained);
  aurora::gx::recomp::clear_guest_address_resolver();
  aurora_shutdown();
}
}  // namespace

extern "C" {
bool native_gx_start(const NativeGxConfig* config, const NativeGxHost* host)
{
  if (s_thread.joinable())
    return true;
  s_config = *config;
  s_title = config->title ? config->title : "";
  s_cache_path = config->cache_path ? config->cache_path : "";
  s_config.title = s_title.c_str();
  s_config.cache_path = s_cache_path.c_str();
  s_texture_path = config->texture_path ? config->texture_path : "";
  s_config.texture_path = s_texture_path.c_str();
  s_host = *host;
  s_stop.store(false);
  s_stats.enabled = std::getenv("DUALIZE_GX_STATS") != nullptr;
  s_ring.wake = SDL_CreateSemaphore(0);
  s_ring.drained = SDL_CreateSemaphore(0);
  if (!s_ring.wake || !s_ring.drained)
    return false;
  std::atomic<int> started{0};
  s_thread = std::thread(RenderThread, &started);
  started.wait(0, std::memory_order_acquire);
  if (started.load(std::memory_order_acquire) < 0)
  {
    s_thread.join();
    SDL_DestroySemaphore(s_ring.wake);
    SDL_DestroySemaphore(s_ring.drained);
    s_ring.wake = s_ring.drained = nullptr;
    return false;
  }
  return true;
}

void native_gx_submit(const uint8_t* data, size_t size)
{
  const uint64_t write = s_ring.write.load(std::memory_order_relaxed);
  if (write + size - s_ring.read.load(std::memory_order_acquire) > RING_SIZE)
  {
    const auto start = std::chrono::steady_clock::now();
    while (write + size - s_ring.read.load(std::memory_order_acquire) > RING_SIZE)
    {
      if (s_stop.load(std::memory_order_relaxed))
        return;
      std::this_thread::yield();
    }
    AddCpuWait(start);
  }
  const size_t offset = static_cast<size_t>(write & RING_MASK);
  const size_t first = std::min(size, RING_SIZE - offset);
  std::memcpy(s_ring.data.get() + offset, data, first);
  std::memcpy(s_ring.data.get(), data + first, size - first);
  s_ring.write.store(write + size, std::memory_order_seq_cst);
  if (s_ring.consumer_waiting.exchange(false, std::memory_order_seq_cst))
    SDL_SignalSemaphore(s_ring.wake);
}

void native_gx_drain(void)
{
  const uint64_t target = s_ring.write.load(std::memory_order_relaxed);
  if (s_ring.consumer_waiting.exchange(false, std::memory_order_seq_cst))
    SDL_SignalSemaphore(s_ring.wake);
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds(5);
  while (s_ring.settled.load(std::memory_order_acquire) < target)
  {
    if (s_stop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() > deadline)
    {
      std::fprintf(stderr, "[native-gx] drain timed out; markers may be lost\n");
      return;
    }
    // Sleep until the render thread moves settled. A sleep_for would round
    // up to the OS timer tick, 15.6 ms on Windows: a whole frame.
    s_ring.drain_waiting.store(true, std::memory_order_seq_cst);
    if (s_ring.settled.load(std::memory_order_seq_cst) < target)
      SDL_WaitSemaphoreTimeout(s_ring.drained, 100);
    s_ring.drain_waiting.store(false, std::memory_order_relaxed);
  }
  AddCpuWait(start);
}

const uint8_t* native_gx_save_state(size_t* size)
{
  // Drained, so the render thread is waiting for data and leaves the decoder
  // and s_work alone. The incomplete packet goes last: the rest of it is still
  // in the guest's gather pipe, which the emulator saves.
  static std::vector<uint8_t> s_state;
  s_state = aurora::gx::recomp::register_stream();
  s_state.insert(s_state.end(), s_work.begin(), s_work.end());
  *size = s_state.size();
  return s_state.data();
}

void native_gx_load_state(const uint8_t* data, size_t size)
{
  native_gx_drain();
  s_ring.restart.store(true, std::memory_order_relaxed);
  if (size != 0)
    native_gx_submit(data, size);
  else if (s_ring.consumer_waiting.exchange(false, std::memory_order_seq_cst))
    SDL_SignalSemaphore(s_ring.wake);
}

void native_gx_stop(void)
{
  if (!s_thread.joinable())
    return;
  s_stop.store(true);
  SDL_SignalSemaphore(s_ring.wake);
  SDL_SignalSemaphore(s_ring.drained);
  s_thread.join();
  SDL_DestroySemaphore(s_ring.wake);
  SDL_DestroySemaphore(s_ring.drained);
  s_ring.wake = s_ring.drained = nullptr;
}
}
