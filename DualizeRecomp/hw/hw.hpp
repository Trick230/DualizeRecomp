#pragma once

// The Hollywood/Flipper blocks NiGHTS drives over MMIO, as Dolphin models
// them: the processor interface (interrupt routing), audio interface (the
// sample counter), memory interface, command processor and pixel engine
// registers, and the Starlet IPC/GPIO block. Each device works on its
// natural register width; Machine splits or widens other accesses. The video
// clock and the expansion bus have no registers: SDK natives drive them.

#include "cpu/cpu.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace dualize
{
class Machine;

enum PiCause : uint32_t
{
  PI_INT_PI = 0x1,
  PI_INT_RSW = 0x2,
  PI_INT_DI = 0x4,
  PI_INT_SI = 0x8,
  PI_INT_EXI = 0x10,
  PI_INT_AI = 0x20,
  PI_INT_DSP = 0x40,
  PI_INT_MEM = 0x80,
  PI_INT_VI = 0x100,
  PI_INT_PE_TOKEN = 0x200,
  PI_INT_PE_FINISH = 0x400,
  PI_INT_CP = 0x800,
  PI_INT_DEBUG = 0x1000,
  PI_INT_HSP = 0x2000,
  PI_INT_WII_IPC = 0x4000,
  PI_INT_RST_BUTTON = 0x10000,
};

class ProcessorInterface
{
public:
  explicit ProcessorInterface(Machine& machine) : m_machine(machine) {}
  void SetInterrupt(uint32_t cause, bool set);
  uint32_t Read32(uint32_t offset) const;
  void Write32(uint32_t offset, uint32_t value);
  void DoState(StateIO& io);

private:
  void Update();

  Machine& m_machine;
  uint32_t m_cause = PI_INT_RST_BUTTON | PI_INT_VI;
  uint32_t m_mask = 0;
  uint32_t m_fifo_base = 0, m_fifo_end = 0, m_fifo_write = 0;
  uint32_t m_reset_code = 0;
};

// The video clock behind the SDK's VI natives (sdk/rvl_vi.cpp): the beam
// position, field ends and the four display interrupts, timed by what the
// SDK's retrace handler last flushed from its register shadows. The guest
// sees none of it as registers.
struct VideoTiming
{
  struct Interrupt
  {
    uint32_t vct = 0, hct = 0;
    bool enabled = false;
  };
  uint32_t equ = 0, acv = 0, hlw = 0;
  uint32_t prb[2]{}, psb[2]{};  // odd, even field
  bool clock54 = false;         // the 54 MHz clock of progressive scan
  bool non_interlaced = false;
  std::array<Interrupt, 4> interrupts{};
};

class VideoInterface
{
public:
  explicit VideoInterface(Machine& machine);
  const VideoTiming& Timing() const { return m_timing; }
  void Configure(const VideoTiming& timing);
  // The beam's line and position in it, as the SDK reads them.
  uint32_t Vct();
  uint32_t Hct();
  // The display interrupts that fired since the last call (bit i for
  // interrupt i); taking them acknowledges them.
  uint32_t TakeInterrupts();
  // Video clock ticks per half line (the scheduler's event period).
  uint32_t TicksPerHalfLine() const;
  uint64_t Fields() const { return m_fields; }
  void DoState(StateIO& io);
  // A state from before the VI natives kept the VI register file; this reads
  // it into the timing it set.
  void DoLegacyState(StateIO& io);

private:
  uint32_t HalfLinesPerOddField() const
  {
    return 3 * m_timing.equ + m_timing.prb[0] + 2 * m_timing.acv + m_timing.psb[0];
  }
  uint32_t HalfLinesPerEvenField() const
  {
    return 3 * m_timing.equ + m_timing.prb[1] + 2 * m_timing.acv + m_timing.psb[1];
  }
  uint32_t TotalHalfLines() const { return HalfLinesPerOddField() + HalfLinesPerEvenField(); }

  void Preset();
  void UpdateParameters();
  void UpdateInterrupt();
  bool InterruptAt(const VideoTiming::Interrupt& interrupt, uint32_t half_line) const;
  bool HalfLineHasEffect(uint32_t half_line) const;
  uint32_t QuietHalfLinesAhead() const;
  void ApplyQuietHalfLine(uint64_t ticks);
  void CatchUpQuietHalfLines(uint64_t ticks);
  void Update(uint64_t ticks);
  void DoClockState(StateIO& io);
  static void Event(void* user, uint64_t, int64_t late);

  Machine& m_machine;
  int m_event = -1;
  VideoTiming m_timing;
  uint32_t m_pending = 0;
  uint32_t m_half_line = 0;
  uint32_t m_next_throttle = 0;
  uint64_t m_line_start_ticks = 0;
  uint32_t m_odd_first = 0, m_odd_last = 0, m_even_first = 0, m_even_last = 0;
  // Half lines known to change nothing, applied together at the next event.
  uint32_t m_quiet = 0;
  uint64_t m_last_update = 0;
  uint32_t m_batch_ticks = 0;
  uint64_t m_fields = 0;
};

// One GameCube pad as the PAD natives (sdk/rvl_pad.cpp) read it from the
// host, in the controller's report units (sticks centred on 0x80).
struct GcPadInput
{
  uint16_t buttons;
  uint8_t stick_x, stick_y, c_x, c_y, trigger_l, trigger_r, analog_a, analog_b;
};

// The expansion bus behind the SDK's EXI natives (sdk/rvl_exi.cpp): each
// channel's lock, waiting unlock callbacks and selection, as EXILock and
// EXISelect keep them, and the one device NiGHTS talks to, the IPL chip on
// channel 0 device 1 (the RTC, the SRAM and the debug UART).
class ExpansionBus
{
public:
  struct Channel
  {
    bool locked = false;
    bool selected = false;
    uint32_t device = 0;
    uint32_t waiting = 0;
    std::array<uint32_t, 3> callbacks{};  // EXICallbacks waiting for the unlock
  };
  std::array<Channel, 3> channels;

  explicit ExpansionBus(Machine& machine);
  void LoadSram(const char* path);
  // Starts a command when the selection reaches the IPL.
  void Select(int channel);
  // Shifts bytes through the selected device: a read stores what comes
  // back, a write sends data (both for a read-write).
  void Transfer(int channel, uint8_t* data, uint32_t length, bool read, bool write);
  void DoState(StateIO& io);
  // A state from before the SDK natives kept the SI and EXI register files;
  // this reads past them and keeps the IPL's part.
  void DoLegacyState(StateIO& io);

private:
  void IplTransfer(uint8_t& byte);

  Machine& m_machine;
  std::array<uint8_t, 0x44> m_sram{};
  uint32_t m_command = 0;
  uint32_t m_command_bytes = 0;
  uint32_t m_cursor = 0;
  uint8_t m_rtc_flags = 0;
};

class AudioInterface
{
public:
  explicit AudioInterface(Machine& machine);
  uint32_t Read32(uint32_t offset);
  void Write32(uint32_t offset, uint32_t value);
  uint32_t DmaDivisor() const { return m_aid_divisor; }
  void DoState(StateIO& io);

private:
  bool Playing() const { return m_control & 1; }
  void SetAisRate(bool k48);
  void SetAidRate(bool k32);
  int64_t Period() const;
  void IncreaseSampleCount(uint32_t amount);
  void UpdateInterrupts();
  void Reschedule();
  static void Event(void* user, uint64_t, int64_t late);

  Machine& m_machine;
  int m_event = -1;
  uint32_t m_control = 0, m_volume = 0, m_sample_counter = 0, m_interrupt_timing = 0;
  uint64_t m_last_cpu_time = 0;
  uint32_t m_ais_divisor = 0, m_aid_divisor = 0;
  uint64_t m_cycles_per_sample = 0;
};

// Pixel engine interrupts and token, set by the render thread's draw-done and
// token markers (queued, applied on the CPU thread), plus the command
// processor's and memory interface's plain register files.
class GraphicsRegisters
{
public:
  explicit GraphicsRegisters(Machine& machine) : m_machine(machine) {}
  uint16_t ReadCp(uint32_t offset) const;
  void WriteCp(uint32_t offset, uint16_t value);
  uint16_t ReadPe(uint32_t offset) const;
  void WritePe(uint32_t offset, uint16_t value);
  uint16_t ReadMi(uint32_t offset) const { return m_mi[(offset >> 1) & 0x3F]; }
  void WriteMi(uint32_t offset, uint16_t value) { m_mi[(offset >> 1) & 0x3F] = value; }

  // Render thread.
  void PostDrawDone();
  void PostToken(uint16_t token, bool interrupt);
  // CPU thread: applies what the render thread posted.
  void Poll();
  void DoState(StateIO& io);

private:
  void UpdateInterrupts();

  Machine& m_machine;
  std::array<uint16_t, 0x40> m_cp{};
  std::array<uint16_t, 0x40> m_mi{};
  std::array<uint16_t, 5> m_pe_conf{};  // ZCONF..ALPHAREAD
  uint16_t m_pe_control = 0;
  uint16_t m_token = 0;
  bool m_signal_token = false, m_signal_finish = false;

  // Posted by the render thread: bit 0 finish, bit 1 token, bit 2 token
  // interrupt; the token value beside it.
  std::atomic<uint32_t> m_posted{0};
  std::atomic<uint32_t> m_posted_token{0};
};

// The Hollywood registers the game touches besides IPC: GPIO (disc slot,
// sensor bar), AHBPROT and the reset lines. IPC goes through the SDK
// natives in sdk/rvl_ipc.cpp, so the mailbox registers read as zero and
// ignore writes.
class Hollywood
{
public:
  explicit Hollywood(Machine& machine);
  uint32_t Read32(uint32_t offset) const;
  void Write32(uint32_t offset, uint32_t value);
  void DoState(StateIO& io);
  // A reply that a state saved before SDK-level IPC had left in the mailbox
  // for the PPC (taken once, after loading).
  bool TakeMailboxReply(uint32_t* address);

private:
  uint32_t m_gpio_dir = 0xFFDF3F, m_gpio_out = 0;
  uint32_t m_resets = 0xFFFFFFFF;
  uint32_t m_mailbox_reply = 0;
};
}  // namespace dualize
