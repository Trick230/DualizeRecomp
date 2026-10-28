#pragma once

// The Hollywood/Flipper blocks NiGHTS drives over MMIO, as Dolphin models
// them: the processor interface (interrupt routing), video interface (beam
// timing and display interrupts), serial interface (GameCube pads), expansion
// interface (the IPL's RTC and SRAM), audio interface (the sample counter),
// memory interface, command processor and pixel engine registers, and the
// Starlet IPC/GPIO block. Each device works on its natural register width;
// Machine splits or widens other accesses.

#include "state.hpp"

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

class VideoInterface
{
public:
  explicit VideoInterface(Machine& machine);
  uint16_t Read16(uint32_t offset);
  void Write16(uint32_t offset, uint16_t value);
  void DoState(StateIO& io);
  // Video clock ticks per half line (the scheduler's event period).
  uint32_t TicksPerHalfLine() const;
  uint64_t Fields() const { return m_fields; }

private:
  // Registers as the guest sees them, one u16 per halfword offset.
  uint16_t& Reg(uint32_t offset) { return m_regs[offset >> 1]; }
  uint16_t Reg(uint32_t offset) const { return m_regs[offset >> 1]; }
  uint32_t Equ() const { return Reg(0x00) & 0xF; }
  uint32_t Acv() const { return (Reg(0x00) >> 4) & 0x3FF; }
  uint32_t Hlw() const { return Reg(0x06) & 0x3FF; }
  uint32_t Prb(bool even) const { return Reg(even ? 0x12 : 0x0E) & 0x3FF; }
  uint32_t Psb(bool even) const { return Reg(even ? 0x10 : 0x0C) & 0x3FF; }
  uint32_t Interrupt(int i) const
  {
    return uint32_t{Reg(0x30 + 4 * i)} << 16 | Reg(0x32 + 4 * i);
  }
  uint32_t HalfLinesPerOddField() const { return 3 * Equ() + Prb(false) + 2 * Acv() + Psb(false); }
  uint32_t HalfLinesPerEvenField() const { return 3 * Equ() + Prb(true) + 2 * Acv() + Psb(true); }
  uint32_t TotalHalfLines() const { return HalfLinesPerOddField() + HalfLinesPerEvenField(); }

  void Preset();
  void UpdateParameters();
  void UpdateInterrupts();
  bool HalfLineHasEffect(uint32_t half_line) const;
  uint32_t QuietHalfLinesAhead() const;
  void ApplyQuietHalfLine(uint64_t ticks);
  void CatchUpQuietHalfLines(uint64_t ticks);
  void Update(uint64_t ticks);
  void BeginTimingChange();
  void EndTimingChange();
  static void Event(void* user, uint64_t, int64_t late);

  Machine& m_machine;
  int m_event = -1;
  std::array<uint16_t, 0x40> m_regs{};
  uint32_t m_unknown_aa = 0;
  uint32_t m_half_line = 0;
  uint32_t m_next_si_poll = 0;
  uint64_t m_line_start_ticks = 0;
  uint32_t m_odd_first = 0, m_odd_last = 0, m_even_first = 0, m_even_last = 0;
  // Half lines known to change nothing, applied together at the next event.
  uint32_t m_quiet = 0;
  uint64_t m_last_update = 0;
  uint32_t m_batch_ticks = 0;
  uint64_t m_fields = 0;
};

// GameCube controller ports. A pad is plugged in while the host input reports
// one on the port; NiGHTS itself reads the Wii Remote.
class SerialInterface
{
public:
  explicit SerialInterface(Machine& machine);
  uint32_t Read32(uint32_t offset);
  void Write32(uint32_t offset, uint32_t value);
  // The video interface's poll: latches every port's input.
  void Poll();
  uint32_t PollLines() const { return (m_poll >> 16) & 0x3FF; }
  void DoState(StateIO& io);

  // Host input for one port, already in GCPadStatus units; null = unplugged.
  struct PadInput
  {
    uint16_t buttons;
    uint8_t stick_x, stick_y, c_x, c_y, trigger_l, trigger_r, analog_a, analog_b;
  };
  using ReadPad = bool (*)(void* user, int port, PadInput* input);
  void SetInputSource(ReadPad read, void* user)
  {
    m_read_pad = read;
    m_read_user = user;
  }

private:
  struct Channel
  {
    uint32_t out = 0, in_hi = 0, in_lo = 0;
    uint8_t mode = 3;
    std::array<uint8_t, 10> origin{0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0};
  };
  bool ReadPort(int port, PadInput* input) const;
  int RunBuffer(int channel, uint8_t* buffer, int length);
  void UpdateInterrupts();

  Machine& m_machine;
  std::array<Channel, 4> m_channels;
  uint32_t m_poll = 492u << 16;
  uint32_t m_com_csr = 0;
  uint32_t m_status = 0;
  uint32_t m_exi_clock = 0;
  std::array<uint8_t, 128> m_buffer{};
  ReadPad m_read_pad = nullptr;
  void* m_read_user = nullptr;
};

class ExpansionInterface
{
public:
  explicit ExpansionInterface(Machine& machine);
  uint32_t Read32(uint32_t offset);
  void Write32(uint32_t offset, uint32_t value);
  void DoState(StateIO& io);
  void LoadSram(const char* path);

private:
  struct Channel
  {
    uint32_t status = 0, dma_address = 0, dma_length = 0, control = 0, data = 0;
  };
  // Only channel 0 chip select 2 (the IPL: RTC, SRAM, UART) answers.
  bool IplSelected(int channel) const
  {
    return channel == 0 && ((m_channels[0].status >> 7) & 7) == 2;
  }
  void IplTransfer(uint8_t& byte);
  void UpdateInterrupts();

  Machine& m_machine;
  std::array<Channel, 3> m_channels;
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
