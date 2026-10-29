#pragma once

// The whole Wii as NiGHTS uses it, without Dolphin: guest memory, the event
// scheduler, the Broadway CPU (recompiled code plus interpreter: DolRecomp's
// host runtime, which calls back through CpuHost), the
// Hollywood blocks, the DSP, IOS, and the host ends they feed (native GX,
// SDL audio and gamepads). One thread runs it; native GX renders on its own.

#include "apu/audio_out.hpp"
#include "apu/native_dsp.hpp"
#include "cpu/cpu.hpp"
#include "hw/hw.hpp"
#include "hid/gamepads.hpp"
#include "kernel/ios/disc.hpp"
#include "kernel/ios_kernel.hpp"
#include "sdk/wud.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace dualize
{
struct MachineConfig
{
  std::filesystem::path game;     // extracted disc directory
  std::filesystem::path user;     // NAND, SRAM, shader cache
  uint32_t idle_pc = 0;
  bool throttle = true;
  bool vsync = true;
  bool interpolate = true;    // only while throttled to the game's rate
  uint32_t present_rate = 0;  // without vsync; 0 = the display's refresh rate
  uint32_t width = 1280, height = 960;
  uint32_t msaa = 4;          // samples a pixel: 1 or 4 (WebGPU allows no other)
  float render_scale = 1.0f;  // frame buffer over window; above 1 reduced by an area filter
};

class Machine : public CpuHost
{
public:
  explicit Machine(MachineConfig config);
  ~Machine();
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  // Loads the disc, starts the host ends and boots the game.
  bool Start(std::string* error);
  // Runs on the calling thread until RequestStop.
  void Run();
  // Any thread.
  void RequestStop();

  // Savestates (CPU thread, between slices: see RequestSave/RequestLoad).
  bool SaveState(const std::filesystem::path& path, std::string* error);
  bool LoadState(const std::filesystem::path& path, std::string* error);
  // Any thread: performed by the CPU thread at its next event boundary.
  void RequestSave(std::filesystem::path path);
  void RequestLoad(std::filesystem::path path);

  // Automation input, merged with the physical gamepad: the Classic
  // Controller on Wii Remote `port` and the GameCube pad on `port`. Any thread;
  // null clears.
  struct AutomationPad
  {
    ios::WiimoteInput wii;  // buttons, classic_buttons, sticks, triggers
    bool gc = false;        // drives (and plugs in) the GameCube pad
    uint16_t gc_buttons = 0;
    float gc_main_x = 0, gc_main_y = 0, gc_c_x = 0, gc_c_y = 0;
    float gc_left = 0, gc_right = 0;
  };
  static constexpr int AUTOMATION_PORTS = 4;
  void SetAutomationPad(int port, const AutomationPad* pad);
  // VI fields so far (any thread).
  uint64_t FieldCount() const { return m_fields.load(std::memory_order_relaxed); }

  // --- device hooks ---
  uint32_t MmioRead(uint32_t physical, uint32_t size, uint32_t pc);
  void MmioWrite(uint32_t physical, uint32_t value, uint32_t size, uint32_t pc);
  void GatherPipeBursts(const uint8_t* data, size_t size);
  void AfterEvents();
  // AfterEvents' work that is safe with generated code on the stack: false
  // if a request (stop, save, load) waits for the CPU's run loop.
  bool AfterEventsInPlace();
  void BeforeIdle();
  void OnFieldEnd(uint64_t ticks);
  void Throttle(uint64_t ticks);
  // Seconds since the Unix epoch on the guest's clock.
  int64_t EmulatedTime() const;
  // The host's GameCube pad on a port; false while nothing drives it.
  bool ReadGcPad(int port, GcPadInput* input);
  void SetGcRumble(int port, bool on);

  const MachineConfig config;
  Memory memory;
  Scheduler scheduler;
  Cpu cpu;
  ProcessorInterface pi;
  VideoInterface vi;
  ExpansionBus exi;
  AudioInterface ai;
  GraphicsRegisters gfx;
  Hollywood hollywood;
  IosKernel ios;
  Wud wud;
  std::unique_ptr<apu::DspCore> dsp;
  std::shared_ptr<const ios::Disc> disc;

private:
  void StartDsp();
  void DoState(StateIO& io, uint32_t version);
  void ServiceRequests();
  static bool SpeakerEnabled();
  static void ReadWiimote(void* user, int index, ios::WiimoteInput* input);
  static void WiimoteSpeaker(void* user, int index, const int16_t* samples, uint32_t count,
                             uint32_t rate, uint8_t left_volume, uint8_t right_volume);

  int m_dsp_event = -1, m_dsp_interrupt_event = -1, m_aram_event = -1, m_audio_dma_event = -1;
  int m_dsp_mix_event = -1;
  static void DspEvent(void* user, uint64_t, int64_t late);
  static void DspInterruptEvent(void* user, uint64_t type, int64_t);
  static void AramEvent(void* user, uint64_t, int64_t);
  static void DspMixEvent(void* user, uint64_t, int64_t);
  static void AudioDmaEvent(void* user, uint64_t, int64_t late);

  apu::AudioOut m_audio;
  hid::Gamepads m_gamepads;
  bool m_gx_started = false;

  // Guest clock at boot, seconds since the Unix epoch in local time (the
  // console's RTC keeps local time).
  int64_t m_boot_time = 0;

  // Throttle: guest ticks and host time at the last resync.
  using Clock = std::chrono::steady_clock;
  Clock::time_point m_throttle_host{};
  uint64_t m_throttle_ticks = 0;

  // Speed report.
  Clock::time_point m_report_host{};
  uint64_t m_report_ticks = 0;
  uint64_t m_report_fields = 0;
  Cpu::Stats m_report_stats;
  uint64_t m_gx_bytes = 0, m_report_gx_bytes = 0;

  bool ReadAutomationPad(int port, AutomationPad* pad);
  std::array<AutomationPad, AUTOMATION_PORTS> m_automation_pads;
  std::array<std::atomic<bool>, AUTOMATION_PORTS> m_automation_active{};
  std::mutex m_automation_mutex;
  std::atomic<uint64_t> m_fields{0};

  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_requests{false};
  std::mutex m_request_mutex;
  std::filesystem::path m_save_path, m_load_path;
};

// The machine a CPU runs in, for natives that reach its hardware.
inline Machine& GetMachine(Cpu& cpu)
{
  return static_cast<Machine&>(cpu.Host());
}
}  // namespace dualize
