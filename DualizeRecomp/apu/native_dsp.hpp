#pragma once

// Native DSP: the Wii DSP interface (mailboxes, control, ARAM and audio DMA)
// and the DSP itself, run as high-level emulation of its ucodes (boot ROM,
// init, AX for Wii). Independent of Dolphin; the host supplies guest memory,
// the interrupt line, timed events and the audio sink.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dualize::apu
{
enum DspInterrupt : uint32_t
{
  DSP_INT_DSP = 0x80,
  DSP_INT_ARAM = 0x20,
  DSP_INT_AID = 0x08,
};

struct DspHost
{
  void* user = nullptr;
  // MEM1 and MEM2; both stay mapped for the DSP's lifetime.
  uint8_t* ram = nullptr;
  uint32_t ram_size = 0;
  uint8_t* exram = nullptr;
  uint32_t exram_size = 0;
  // The DSP line into the processor interface.
  void (*set_interrupt)(void* user, bool asserted) = nullptr;
  // Calls DspCore::RaiseInterrupt(type) after cycles CPU ticks.
  void (*schedule_interrupt)(void* user, int64_t cycles, uint32_t type) = nullptr;
  // Calls DspCore::CompleteAramDma() after cycles CPU ticks.
  void (*schedule_aram_done)(void* user, int64_t cycles) = nullptr;
  uint64_t (*timebase)(void* user) = nullptr;
  // Makes the CPU leave its slice within cycles ticks to look at events again.
  void (*force_exception_check)(void* user, int64_t cycles) = nullptr;
  // count stereo frames of big-endian s16 (right, left) taken by the audio DMA.
  void (*push_samples)(void* user, const int16_t* samples, uint32_t count) = nullptr;
};

// Savestate transfer: moves size bytes between data and the state stream.
struct DspStateIO
{
  void* user;
  void (*bytes)(void* user, void* data, uint32_t size);
  bool reading;
};

class DspCore
{
public:
  // coefficient_paths: candidate dsp_coef.bin files, the first valid one wins.
  DspCore(const DspHost& host, std::vector<std::string> coefficient_paths);
  ~DspCore();
  DspCore(const DspCore&) = delete;
  DspCore& operator=(const DspCore&) = delete;

  // 16-bit registers, offset within the interface block (0x5000-0x503E).
  uint16_t Read(uint32_t offset);
  void Write(uint32_t offset, uint16_t value);

  void RaiseInterrupt(uint32_t type);
  void CompleteAramDma();
  // The DSP's time slice (once per millisecond of guest time).
  void Update();
  // One 32-byte audio DMA step (8 stereo frames).
  void UpdateAudioDma();

  // Same layout as Dolphin's DSP and DSP HLE state, so either loads the other.
  void DoState(const DspStateIO& io);

  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};
}  // namespace dualize::apu
