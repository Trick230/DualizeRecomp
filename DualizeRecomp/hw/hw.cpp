#include "hw/hw.hpp"

#include "machine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace dualize
{
// ---------------------------------------------------------------------------
// Processor interface

void ProcessorInterface::SetInterrupt(uint32_t cause, bool set)
{
  const uint32_t old = m_cause;
  m_cause = set ? m_cause | cause : m_cause & ~cause;
  if (m_cause != old)
    Update();
}

void ProcessorInterface::Update()
{
  m_machine.cpu.SetExternalInterrupt((m_cause & m_mask) != 0);
}

uint32_t ProcessorInterface::Read32(uint32_t offset) const
{
  switch (offset)
  {
  case 0x00:
    return m_cause;
  case 0x04:
    return m_mask;
  case 0x0C:
    return m_fifo_base;
  case 0x10:
    return m_fifo_end;
  case 0x14:
    return m_fifo_write;
  case 0x24:
    return m_reset_code;
  case 0x2C:
    return 0x246500B1;  // Flipper revision C
  default:
    return 0;
  }
}

void ProcessorInterface::Write32(uint32_t offset, uint32_t value)
{
  switch (offset)
  {
  case 0x00:
    m_cause &= ~value;
    Update();
    break;
  case 0x04:
    m_mask = value;
    Update();
    break;
  case 0x0C:
    m_fifo_base = value & 0xFFFFFFE0;
    break;
  case 0x10:
    m_fifo_end = value & 0xFFFFFFE0;
    break;
  case 0x14:
    m_fifo_write = value & 0xFFFFFFE0;
    break;
  case 0x18:
    // GXAbortFrame: drop what the CPU still holds of the next burst.
    if (value & 1)
      m_machine.cpu.ResetGatherPipe();
    break;
  case 0x24:
    m_reset_code = value;
    break;
  default:
    break;
  }
}

void ProcessorInterface::DoState(StateIO& io)
{
  io.Do(m_cause);
  io.Do(m_mask);
  io.Do(m_fifo_base);
  io.Do(m_fifo_end);
  io.Do(m_fifo_write);
  io.Do(m_reset_code);
  if (io.Reading())
    Update();
}

// ---------------------------------------------------------------------------
// Video interface

namespace
{
// Half lines from a field boundary to the first input poll of the field.
constexpr uint32_t SI_POLL_HALF_LINES = 15;
constexpr uint32_t CLOCK_FREQUENCIES[2] = {27000000, 54000000};
}  // namespace

VideoInterface::VideoInterface(Machine& machine) : m_machine(machine)
{
  m_event = m_machine.scheduler.Register(Event, this);
  Preset();
  m_machine.scheduler.Schedule(TicksPerHalfLine(), m_event);
}

void VideoInterface::Preset()
{
  m_regs = {};
  Reg(0x00) = 6;               // EQU 6, ACV 0
  Reg(0x02) = 1;               // ENB, NTSC
  Reg(0x04) = 105 | 71 << 8;   // HCE, HCS
  Reg(0x06) = 429;             // HLW
  const uint32_t h1 = 64 | 162 << 7 | 373u << 17;  // HSY, HBE640, HBS640
  Reg(0x08) = static_cast<uint16_t>(h1 >> 16);
  Reg(0x0A) = static_cast<uint16_t>(h1);
  Reg(0x0C) = 5;    // odd PSB
  Reg(0x0E) = 502;  // odd PRB
  Reg(0x10) = 4;    // even PSB
  Reg(0x12) = 503;  // even PRB
  const uint32_t burst_odd = 12 | 520 << 5 | 12 << 16 | 520u << 21;
  const uint32_t burst_even = 13 | 519 << 5 | 13 << 16 | 519u << 21;
  Reg(0x14) = static_cast<uint16_t>(burst_odd >> 16);
  Reg(0x16) = static_cast<uint16_t>(burst_odd);
  Reg(0x18) = static_cast<uint16_t>(burst_even >> 16);
  Reg(0x1A) = static_cast<uint16_t>(burst_even);
  Reg(0x30) = 263 | 1 << 12;  // pre-retrace: VCT 263, masked in
  Reg(0x32) = 430;
  Reg(0x34) = 1 | 1 << 12;  // post-retrace: VCT 1
  Reg(0x36) = 1;
  Reg(0x48) = 40 | 40 << 8;  // STD, WPL
  Reg(0x6C) = 1;             // 54 MHz clock (NTSC)
  Reg(0x6E) = 1;             // component cable (progressive scan offered)
  m_unknown_aa = 0;
  m_line_start_ticks = 0;
  m_half_line = 0;
  m_next_si_poll = SI_POLL_HALF_LINES;
  UpdateParameters();
}

uint32_t VideoInterface::TicksPerHalfLine() const
{
  return 2 * Scheduler::CPU_CLOCK / CLOCK_FREQUENCIES[Reg(0x6C) & 1] * Hlw();
}

void VideoInterface::UpdateParameters()
{
  const uint32_t equ = 3 * Equ();
  const uint32_t acv = 2 * Acv();
  m_odd_first = equ + Prb(false);
  const uint32_t odd_end = m_odd_first + acv;
  m_odd_last = odd_end - 1;
  const int32_t psb_diff = static_cast<int32_t>(Psb(false)) - static_cast<int32_t>(Psb(true));
  m_even_first = odd_end + Psb(false) + equ + Prb(true) - psb_diff;
  m_even_last = m_even_first + acv - 1;
}

void VideoInterface::UpdateInterrupts()
{
  bool active = false;
  for (int i = 0; i < 4; ++i)
  {
    const uint16_t hi = Reg(0x30 + 4 * i);
    active |= (hi & 0x8000) && (hi & 0x1000);
  }
  m_machine.pi.SetInterrupt(PI_INT_VI, active);
}

bool VideoInterface::HalfLineHasEffect(uint32_t half_line) const
{
  if (half_line == 0 || half_line == HalfLinesPerOddField() || half_line == m_odd_first ||
      half_line == m_even_first || half_line == m_odd_last || half_line == m_even_last ||
      half_line == m_next_si_poll)
  {
    return true;
  }
  uint32_t next = half_line + 1;
  if (next == TotalHalfLines())
    next = 0;
  for (int i = 0; i < 4; ++i)
  {
    const uint32_t reg = Interrupt(i);
    const uint32_t target = (reg & 0x7FF) > Hlw() ? 1 : 0;
    if (1 + next / 2 == ((reg >> 16) & 0x7FF) && (next & 1) == target)
      return true;
  }
  return false;
}

uint32_t VideoInterface::QuietHalfLinesAhead() const
{
  const uint32_t total = TotalHalfLines();
  uint32_t half_line = m_half_line;
  uint32_t quiet = 0;
  while (quiet < total && !HalfLineHasEffect(half_line))
  {
    ++quiet;
    half_line = half_line + 1 == total ? 0 : half_line + 1;
  }
  return quiet < total ? quiet : 0;
}

void VideoInterface::ApplyQuietHalfLine(uint64_t ticks)
{
  if (++m_half_line == TotalHalfLines())
    m_half_line = 0;
  if (!(m_half_line & 1))
    m_line_start_ticks = ticks;
}

void VideoInterface::CatchUpQuietHalfLines(uint64_t ticks)
{
  while (m_quiet != 0 && ticks >= m_last_update + m_batch_ticks)
  {
    m_last_update += m_batch_ticks;
    --m_quiet;
    ApplyQuietHalfLine(m_last_update);
  }
}

void VideoInterface::Update(uint64_t ticks)
{
  const uint32_t even_begin = HalfLinesPerOddField();
  const bool boundary = m_half_line == 0 || m_half_line == even_begin;

  if (m_half_line == m_odd_last || m_half_line == m_even_last)
  {
    ++m_fields;
    m_machine.OnFieldEnd(ticks);
  }

  if (m_half_line == m_next_si_poll)
  {
    m_machine.Throttle(ticks);
    m_machine.si.Poll();
    m_next_si_poll += 2 * m_machine.si.PollLines();
  }
  if (boundary)
    m_next_si_poll = m_half_line + SI_POLL_HALF_LINES;

  if (++m_half_line == TotalHalfLines())
    m_half_line = 0;
  if (!(m_half_line & 1))
    m_line_start_ticks = ticks;

  for (int i = 0; i < 4; ++i)
  {
    const uint32_t reg = Interrupt(i);
    const uint32_t target = (reg & 0x7FF) > Hlw() ? 1 : 0;
    if (1 + m_half_line / 2 == ((reg >> 16) & 0x7FF) && (m_half_line & 1) == target)
      Reg(0x30 + 4 * i) |= 0x8000;
  }
  UpdateInterrupts();
}

void VideoInterface::Event(void* user, uint64_t, int64_t late)
{
  VideoInterface& vi = *static_cast<VideoInterface*>(user);
  const uint64_t ticks = vi.m_machine.scheduler.Now() - static_cast<uint64_t>(late);
  while (vi.m_quiet != 0)
  {
    vi.m_last_update += vi.m_batch_ticks;
    --vi.m_quiet;
    vi.ApplyQuietHalfLine(vi.m_last_update);
  }
  vi.Update(ticks);
  vi.m_last_update = ticks;
  vi.m_batch_ticks = vi.TicksPerHalfLine();
  vi.m_quiet = vi.m_batch_ticks != 0 ? vi.QuietHalfLinesAhead() : 0;
  const int64_t cycles = static_cast<int64_t>(vi.m_quiet + 1) * vi.m_batch_ticks;
  vi.m_machine.scheduler.Schedule(cycles - late, vi.m_event);
}

void VideoInterface::BeginTimingChange()
{
  CatchUpQuietHalfLines(m_machine.scheduler.Now());
}

// The batch was planned with the old timing; replan the rest of it.
void VideoInterface::EndTimingChange()
{
  if (m_quiet == 0)
    return;
  const uint32_t ticks = TicksPerHalfLine();
  if (ticks == 0)
    return;
  m_last_update += m_batch_ticks;
  m_last_update -= ticks;
  m_batch_ticks = ticks;
  m_quiet = QuietHalfLinesAhead();
  Scheduler& scheduler = m_machine.scheduler;
  const uint64_t due = m_last_update + uint64_t{m_quiet + 1} * ticks;
  scheduler.Remove(m_event);
  scheduler.ScheduleAt(std::max(due, scheduler.Now()), m_event);
}

uint16_t VideoInterface::Read16(uint32_t offset)
{
  offset &= 0x7E;
  switch (offset)
  {
  case 0x2C:
    CatchUpQuietHalfLines(m_machine.scheduler.Now());
    return static_cast<uint16_t>(1 + m_half_line / 2);
  case 0x2E:
  {
    CatchUpQuietHalfLines(m_machine.scheduler.Now());
    const uint64_t elapsed = m_machine.scheduler.Now() - m_line_start_ticks;
    const uint32_t per_half_line = std::max<uint32_t>(TicksPerHalfLine(), 1);
    const uint64_t value = 1 + Hlw() * elapsed / per_half_line;
    return static_cast<uint16_t>(std::clamp<uint64_t>(value, 1, 2 * Hlw()));
  }
  case 0x68:
    return static_cast<uint16_t>(m_unknown_aa >> 16);
  case 0x6A:
    return static_cast<uint16_t>(m_unknown_aa);
  default:
    return offset < 0x76 ? Reg(offset) : 0;
  }
}

void VideoInterface::Write16(uint32_t offset, uint16_t value)
{
  offset &= 0x7E;
  switch (offset)
  {
  case 0x00:
  case 0x04:
  case 0x06:
  case 0x0C:
  case 0x0E:
  case 0x10:
  case 0x12:
  case 0x6C:
    BeginTimingChange();
    Reg(offset) = value;
    UpdateParameters();
    EndTimingChange();
    break;
  case 0x02:
  {
    BeginTimingChange();
    // ENB, NIN, DLR, LE0, LE1, FMT; RST resets the interrupts.
    Reg(0x02) = (Reg(0x02) & ~0x03FDu) | (value & 0x03FD);
    if (value & 0x2)
    {
      for (int i = 0; i < 4; ++i)
      {
        Reg(0x30 + 4 * i) = 0;
        Reg(0x32 + 4 * i) = 0;
      }
      UpdateInterrupts();
    }
    UpdateParameters();
    EndTimingChange();
    break;
  }
  case 0x1C:
  case 0x20:
  case 0x24:
  case 0x28:
    // A write with CLRPOFF set clears POFF.
    Reg(offset) = (value & 0xE000) ? value & ~0x1000 : value;
    break;
  case 0x2C:
  case 0x2E:
    break;
  case 0x30:
  case 0x34:
  case 0x38:
  case 0x3C:
    BeginTimingChange();
    Reg(offset) = value;
    UpdateInterrupts();
    EndTimingChange();
    break;
  case 0x32:
  case 0x36:
  case 0x3A:
  case 0x3E:
    BeginTimingChange();
    Reg(offset) = value;
    EndTimingChange();
    break;
  case 0x68:
    m_unknown_aa = (m_unknown_aa & 0xFFFF) | uint32_t{value} << 16;
    break;
  case 0x6A:
    m_unknown_aa = (m_unknown_aa & 0xFFFF0000) | value;
    break;
  default:
    if (offset < 0x76)
      Reg(offset) = value;
    break;
  }
}

void VideoInterface::DoState(StateIO& io)
{
  io.Do(m_regs);
  io.Do(m_unknown_aa);
  io.Do(m_half_line);
  io.Do(m_next_si_poll);
  io.Do(m_line_start_ticks);
  io.Do(m_quiet);
  io.Do(m_last_update);
  io.Do(m_batch_ticks);
  io.Do(m_fields);
  if (io.Reading())
  {
    UpdateParameters();
    UpdateInterrupts();
  }
}

// ---------------------------------------------------------------------------
// Serial interface

namespace
{
constexpr uint32_t SI_ERRSTAT = 0x80000000;
constexpr uint32_t SI_ERRLATCH = 0x40000000;
constexpr uint32_t COM_TSTART = 1u << 0;
constexpr uint32_t COM_RDSTINTMSK = 1u << 27;
constexpr uint32_t COM_RDSTINT = 1u << 28;
constexpr uint32_t COM_COMERR = 1u << 29;
constexpr uint32_t COM_TCINTMSK = 1u << 30;
constexpr uint32_t COM_TCINT = 1u << 31;
constexpr uint32_t PAD_USE_ORIGIN = 0x0080;

constexpr uint32_t RdstBit(int channel)
{
  return 0x20000000u >> (channel * 8);
}
constexpr uint32_t NorepBit(int channel)
{
  return 0x08000000u >> (channel * 8);
}

uint32_t PadHi(const SerialInterface::PadInput& pad)
{
  return pad.stick_y | uint32_t{pad.stick_x} << 8 | uint32_t(pad.buttons | PAD_USE_ORIGIN) << 16;
}

// The low word in each of the analog report modes.
uint32_t PadLo(const SerialInterface::PadInput& pad, uint8_t mode)
{
  switch (mode)
  {
  case 1:
    return (pad.analog_b >> 4) | (pad.analog_a >> 4) << 4 | uint32_t{pad.trigger_r} << 8 |
           uint32_t{pad.trigger_l} << 16 | uint32_t(pad.c_y >> 4) << 24 |
           uint32_t(pad.c_x >> 4) << 28;
  case 2:
    return pad.analog_b | uint32_t{pad.analog_a} << 8 | uint32_t(pad.trigger_r >> 4) << 16 |
           uint32_t(pad.trigger_l >> 4) << 20 | uint32_t(pad.c_y >> 4) << 24 |
           uint32_t(pad.c_x >> 4) << 28;
  case 3:
    return pad.trigger_r | uint32_t{pad.trigger_l} << 8 | uint32_t{pad.c_y} << 16 |
           uint32_t{pad.c_x} << 24;
  case 4:
    return pad.analog_b | uint32_t{pad.analog_a} << 8 | uint32_t{pad.c_y} << 16 |
           uint32_t{pad.c_x} << 24;
  default:
    return (pad.analog_b >> 4) | (pad.analog_a >> 4) << 4 | uint32_t(pad.trigger_r >> 4) << 8 |
           uint32_t(pad.trigger_l >> 4) << 12 | uint32_t{pad.c_y} << 16 | uint32_t{pad.c_x} << 24;
  }
}
}  // namespace

SerialInterface::SerialInterface(Machine& machine) : m_machine(machine)
{
}

bool SerialInterface::ReadPort(int port, PadInput* input) const
{
  return m_read_pad && m_read_pad(m_read_user, port, input);
}

void SerialInterface::UpdateInterrupts()
{
  if (m_status & (RdstBit(0) | RdstBit(1) | RdstBit(2) | RdstBit(3)))
    m_com_csr |= COM_RDSTINT;
  else
    m_com_csr &= ~COM_RDSTINT;
  const bool interrupt = ((m_com_csr & COM_RDSTINT) && (m_com_csr & COM_RDSTINTMSK)) ||
                         ((m_com_csr & COM_TCINT) && (m_com_csr & COM_TCINTMSK));
  m_machine.pi.SetInterrupt(PI_INT_SI, interrupt);
}

void SerialInterface::Poll()
{
  for (int i = 0; i < 4; ++i)
  {
    Channel& channel = m_channels[i];
    uint32_t errlatch = channel.in_hi & SI_ERRLATCH;
    PadInput pad;
    if (ReadPort(i, &pad))
    {
      channel.in_hi = PadHi(pad);
      channel.in_lo = PadLo(pad, channel.mode);
      m_status |= RdstBit(i);
    }
    else
    {
      m_status |= NorepBit(i);
      errlatch = SI_ERRLATCH;
      channel.in_hi |= SI_ERRSTAT;
    }
    channel.in_hi = (channel.in_hi & ~SI_ERRLATCH) | errlatch;
  }
  UpdateInterrupts();
}

int SerialInterface::RunBuffer(int index, uint8_t* buffer, int)
{
  PadInput pad;
  if (!ReadPort(index, &pad))
    return -1;
  Channel& channel = m_channels[index];
  switch (buffer[0])
  {
  case 0x00:  // status
  case 0xFF:  // reset
    buffer[0] = 0x09;  // standard controller
    buffer[1] = 0x00;
    buffer[2] = 0x00;
    return 3;
  case 0x40:  // direct read
  {
    const uint32_t hi = PadHi(pad);
    const uint32_t lo = PadLo(pad, channel.mode);
    for (int i = 0; i < 4; ++i)
    {
      buffer[i] = static_cast<uint8_t>(hi >> (24 - 8 * i));
      buffer[i + 4] = static_cast<uint8_t>(lo >> (24 - 8 * i));
    }
    return 8;
  }
  case 0x41:  // origin
  case 0x42:  // recalibrate
    std::memcpy(buffer, channel.origin.data(), channel.origin.size());
    return static_cast<int>(channel.origin.size());
  default:
    // SET_GAME_ID and anything else: accepted without a reply.
    return 0;
  }
}

uint32_t SerialInterface::Read32(uint32_t offset)
{
  if (offset >= 0x80)
  {
    const uint8_t* p = &m_buffer[(offset - 0x80) & 0x7C];
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
  }
  if (offset < 0x30)
  {
    Channel& channel = m_channels[offset / 0xC];
    switch (offset % 0xC)
    {
    case 0:
      return channel.out;
    case 4:
    case 8:
      m_status &= ~RdstBit(offset / 0xC);
      UpdateInterrupts();
      return offset % 0xC == 4 ? channel.in_hi : channel.in_lo;
    default:
      return 0;
    }
  }
  switch (offset)
  {
  case 0x30:
    return m_poll;
  case 0x34:
    return m_com_csr;
  case 0x38:
    return m_status;
  case 0x3C:
    return m_exi_clock;
  default:
    return 0;
  }
}

void SerialInterface::Write32(uint32_t offset, uint32_t value)
{
  if (offset >= 0x80)
  {
    uint8_t* p = &m_buffer[(offset - 0x80) & 0x7C];
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
    return;
  }
  if (offset < 0x30)
  {
    Channel& channel = m_channels[offset / 0xC];
    switch (offset % 0xC)
    {
    case 0:
      channel.out = value;
      break;
    case 4:
      channel.in_hi = value;
      break;
    case 8:
      channel.in_lo = value;
      break;
    }
    return;
  }
  switch (offset)
  {
  case 0x30:
    m_poll = value;
    break;
  case 0x34:
  {
    // CHANNEL, INLNGTH, OUTLNGTH and the two masks are plain fields.
    constexpr uint32_t fields = 0x6 | 0x7F00 | 0x7F0000 | COM_RDSTINTMSK | COM_TCINTMSK;
    m_com_csr = (m_com_csr & ~fields) | (value & fields);
    if (value & COM_RDSTINT)
      m_com_csr &= ~COM_RDSTINT;
    if (value & COM_TCINT)
      m_com_csr &= ~COM_TCINT;
    if (value & COM_TSTART)
    {
      const int channel = (m_com_csr >> 1) & 3;
      const int length = static_cast<int>((((m_com_csr >> 16) & 0x7F) - 1) & 0x7F) + 1;
      const int response = RunBuffer(channel, m_buffer.data(), length);
      m_com_csr &= ~(COM_TSTART | COM_COMERR);
      if (response < 0)
      {
        m_com_csr |= COM_COMERR;
        m_status |= NorepBit(channel);
      }
      m_com_csr |= COM_TCINT;
    }
    UpdateInterrupts();
    break;
  }
  case 0x38:
    m_status &= ~(value & 0x0F0F0F0F);
    if (value & 0x80000000)
    {
      for (int i = 0; i < 4; ++i)
      {
        const uint32_t command = m_channels[i].out;
        const bool polled = (m_poll >> (7 - i)) & 1;
        // CMD_WRITE sets the analog mode (rumble is the low bits of the other byte).
        if (((command >> 16) & 0xFF) == 0x40 && !polled)
          m_channels[i].mode = static_cast<uint8_t>(command & 0xFF);
      }
      m_status &= ~(0x80000000u | 0x10101010u);
    }
    break;
  case 0x3C:
    m_exi_clock = value;
    break;
  }
}

void SerialInterface::DoState(StateIO& io)
{
  for (Channel& channel : m_channels)
  {
    io.Do(channel.out);
    io.Do(channel.in_hi);
    io.Do(channel.in_lo);
    io.Do(channel.mode);
    io.Do(channel.origin);
  }
  io.Do(m_poll);
  io.Do(m_com_csr);
  io.Do(m_status);
  io.Do(m_exi_clock);
  io.Do(m_buffer);
  if (io.Reading())
    UpdateInterrupts();
}

// ---------------------------------------------------------------------------
// Expansion interface

namespace
{
constexpr uint32_t EXI_EXIINTMASK = 1u << 0;
constexpr uint32_t EXI_EXIINT = 1u << 1;
constexpr uint32_t EXI_TCINTMASK = 1u << 2;
constexpr uint32_t EXI_TCINT = 1u << 3;
constexpr uint32_t EXI_CLK = 7u << 4;
constexpr uint32_t EXI_CS = 7u << 7;
constexpr uint32_t EXI_EXTINTMASK = 1u << 10;
constexpr uint32_t EXI_EXTINT = 1u << 11;
constexpr uint32_t EXI_ROMDIS = 1u << 13;

constexpr uint32_t SRAM_BASE = 0x800000;
constexpr uint32_t SRAM_SIZE = 0x44;
constexpr uint32_t WII_RTC_BASE = 0x840000;
constexpr uint32_t GC_EPOCH = 0x386D4380;  // 2000-01-01

// Dolphin's default SRAM: stereo, OOBE done, valid checksums.
constexpr uint8_t DEFAULT_SRAM[0x44] = {
    0, 0, 0, 0,                                  // rtc
    0x00, 0x2C, 0xFF, 0xD0,                      // checksum, inverse
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // ead0, ead1, rtc_bias
    0, 0, 0, 0x20 | 0x08 | 0x04,                 // hoffset, ntd, language, flags
    'D', 'O', 'L', 'P', 'H', 'I', 'N', 'S', 'L', 'O', 'T', 'A',
    'D', 'O', 'L', 'P', 'H', 'I', 'N', 'S', 'L', 'O', 'T', 'B',
    0, 0, 0, 0,                                  // wireless keyboard id
    0, 0, 0, 0, 0, 0, 0, 0,                      // wireless pad ids
    0, 0,                                        // di error code, field_25
    0x6E, 0x6D,                                  // flash id checksums
    0, 0, 0, 0,                                  // gbs mode, field_3e
};
static_assert(sizeof(DEFAULT_SRAM) == 0x44);
}  // namespace

ExpansionInterface::ExpansionInterface(Machine& machine) : m_machine(machine)
{
  std::memcpy(m_sram.data(), DEFAULT_SRAM, sizeof(DEFAULT_SRAM));
}

void ExpansionInterface::LoadSram(const char* path)
{
  if (FILE* file = std::fopen(path, "rb"))
  {
    std::array<uint8_t, 0x44> sram;
    if (std::fread(sram.data(), 1, sram.size(), file) == sram.size())
      m_sram = sram;
    std::fclose(file);
  }
}

void ExpansionInterface::UpdateInterrupts()
{
  bool interrupt = false;
  for (const Channel& channel : m_channels)
  {
    const uint32_t s = channel.status;
    interrupt |= ((s & EXI_EXIINT) && (s & EXI_EXIINTMASK)) ||
                 ((s & EXI_TCINT) && (s & EXI_TCINTMASK)) ||
                 ((s & EXI_EXTINT) && (s & EXI_EXTINTMASK));
  }
  m_machine.pi.SetInterrupt(PI_INT_EXI, interrupt);
}

void ExpansionInterface::IplTransfer(uint8_t& byte)
{
  if (m_command_bytes < 4)
  {
    m_command = m_command << 8 | byte;
    byte = 0xFF;
    if (++m_command_bytes == 4)
    {
      // The RTC reads the emulated wall clock.
      const uint32_t rtc = static_cast<uint32_t>(m_machine.EmulatedTime()) - GC_EPOCH;
      m_sram[0] = static_cast<uint8_t>(rtc >> 24);
      m_sram[1] = static_cast<uint8_t>(rtc >> 16);
      m_sram[2] = static_cast<uint8_t>(rtc >> 8);
      m_sram[3] = static_cast<uint8_t>(rtc);
    }
    return;
  }
  const bool write = m_command >> 31;
  const uint32_t address = (m_command >> 6) & 0x1FFFFFF;
  if (address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE)
  {
    const uint32_t index = (address - SRAM_BASE + m_cursor++) % SRAM_SIZE;
    if (write)
      m_sram[index] = byte;
    else
      byte = m_sram[index];
  }
  else if (address == WII_RTC_BASE + 0x20)
  {
    if (write)
      m_rtc_flags = byte;
    else
      byte = m_rtc_flags;
  }
  else if (!write)
  {
    // No IPL ROM (fonts are the only thing a Wii game could want there).
    byte = 0;
  }
}

uint32_t ExpansionInterface::Read32(uint32_t offset)
{
  const int index = static_cast<int>(offset / 0x14);
  if (index >= 3)
    return 0;
  const Channel& channel = m_channels[index];
  switch (offset % 0x14)
  {
  case 0x00:
    return channel.status;  // EXT stays 0: no memory cards
  case 0x04:
    return channel.dma_address;
  case 0x08:
    return channel.dma_length;
  case 0x0C:
    return channel.control;
  case 0x10:
    return channel.data;
  default:
    return 0;
  }
}

void ExpansionInterface::Write32(uint32_t offset, uint32_t value)
{
  const int index = static_cast<int>(offset / 0x14);
  if (index >= 3)
    return;
  Channel& channel = m_channels[index];
  switch (offset % 0x14)
  {
  case 0x00:
  {
    uint32_t status = channel.status;
    status = (status & ~(EXI_EXIINTMASK | EXI_TCINTMASK | EXI_CLK)) |
             (value & (EXI_EXIINTMASK | EXI_TCINTMASK | EXI_CLK));
    if (value & EXI_EXIINT)
      status &= ~EXI_EXIINT;
    if (value & EXI_TCINT)
      status &= ~EXI_TCINT;
    if (index != 2)
    {
      status = (status & ~EXI_EXTINTMASK) | (value & EXI_EXTINTMASK);
      if (value & EXI_EXTINT)
        status &= ~EXI_EXTINT;
    }
    if (index == 0)
      status = (status & ~EXI_ROMDIS) | (value & EXI_ROMDIS);
    const uint32_t old_cs = (status >> 7) & 7;
    const uint32_t new_cs = (value >> 7) & 7;
    // Selecting the IPL starts a new command.
    if (index == 0 && (old_cs ^ new_cs) == 2 && new_cs)
    {
      m_command_bytes = 0;
      m_cursor = 0;
    }
    channel.status = (status & ~EXI_CS) | (value & EXI_CS);
    UpdateInterrupts();
    break;
  }
  case 0x04:
    channel.dma_address = value;
    break;
  case 0x08:
    channel.dma_length = value;
    break;
  case 0x0C:
  {
    channel.control = value;
    if (!(value & 1))
      break;
    const uint32_t cs = (channel.status >> 7) & 7;
    if (cs != 1 && cs != 2 && cs != 4)
      break;
    const bool ipl = IplSelected(index);
    const uint32_t rw = (value >> 2) & 3;
    if (!(value & 2))
    {
      const uint32_t length = ((value >> 4) & 3) + 1;
      if (rw == 0)
      {
        uint32_t result = 0;
        for (uint32_t i = 0; i < length; ++i)
        {
          uint8_t byte = 0;
          if (ipl)
            IplTransfer(byte);
          result |= uint32_t{byte} << (24 - 8 * i);
        }
        channel.data = result;
      }
      else if (rw == 1)
      {
        uint32_t data = channel.data;
        for (uint32_t i = 0; i < length; ++i, data <<= 8)
        {
          uint8_t byte = static_cast<uint8_t>(data >> 24);
          if (ipl)
            IplTransfer(byte);
        }
      }
    }
    else if (rw == 0 || rw == 1)
    {
      uint8_t* p = m_machine.memory.Physical(channel.dma_address & 0x1FFFFFFF, channel.dma_length);
      for (uint32_t i = 0; p && i < channel.dma_length; ++i)
      {
        uint8_t byte = rw == 1 ? p[i] : 0;
        if (ipl)
          IplTransfer(byte);
        if (rw == 0)
          p[i] = byte;
      }
    }
    channel.control &= ~1u;
    channel.status |= EXI_TCINT;
    UpdateInterrupts();
    break;
  }
  case 0x10:
    channel.data = value;
    break;
  }
}

void ExpansionInterface::DoState(StateIO& io)
{
  for (Channel& channel : m_channels)
  {
    io.Do(channel.status);
    io.Do(channel.dma_address);
    io.Do(channel.dma_length);
    io.Do(channel.control);
    io.Do(channel.data);
  }
  io.Do(m_sram);
  io.Do(m_command);
  io.Do(m_command_bytes);
  io.Do(m_cursor);
  io.Do(m_rtc_flags);
  if (io.Reading())
    UpdateInterrupts();
}

// ---------------------------------------------------------------------------
// Audio interface

namespace
{
constexpr uint32_t AI_PSTAT = 1u << 0;
constexpr uint32_t AI_AISFR = 1u << 1;
constexpr uint32_t AI_AIINTMSK = 1u << 2;
constexpr uint32_t AI_AIINT = 1u << 3;
constexpr uint32_t AI_AIINTVLD = 1u << 4;
constexpr uint32_t AI_SCRESET = 1u << 5;
constexpr uint32_t AI_AIDFR = 1u << 6;
constexpr uint64_t SAMPLE_RATE_DIVIDEND = 108000000;  // 54 MHz * 2
constexpr uint32_t DIVISOR_48K = 1125 * 2;
constexpr uint32_t DIVISOR_32K = DIVISOR_48K * 3 / 2;
}  // namespace

AudioInterface::AudioInterface(Machine& machine) : m_machine(machine)
{
  m_event = m_machine.scheduler.Register(Event, this);
  SetAisRate(true);
  SetAidRate(true);
}

void AudioInterface::SetAisRate(bool k48)
{
  m_control = k48 ? m_control | AI_AISFR : m_control & ~AI_AISFR;
  m_ais_divisor = k48 ? DIVISOR_48K : DIVISOR_32K;
  m_cycles_per_sample = uint64_t{Scheduler::CPU_CLOCK} * m_ais_divisor / SAMPLE_RATE_DIVIDEND;
}

void AudioInterface::SetAidRate(bool k32)
{
  m_control = k32 ? m_control | AI_AIDFR : m_control & ~AI_AIDFR;
  m_aid_divisor = k32 ? DIVISOR_32K : DIVISOR_48K;
}

int64_t AudioInterface::Period() const
{
  const uint64_t period = m_cycles_per_sample * (m_interrupt_timing - m_sample_counter);
  const uint64_t s_period = m_cycles_per_sample * SAMPLE_RATE_DIVIDEND / m_ais_divisor;
  return static_cast<int64_t>(period == 0 ? s_period : std::min(period, s_period));
}

void AudioInterface::UpdateInterrupts()
{
  m_machine.pi.SetInterrupt(PI_INT_AI, (m_control & AI_AIINT) && (m_control & AI_AIINTMSK));
}

void AudioInterface::IncreaseSampleCount(uint32_t amount)
{
  if (!Playing())
    return;
  const uint32_t old = m_sample_counter + 1;
  m_sample_counter += amount;
  if (m_interrupt_timing - old <= m_sample_counter - old)
  {
    m_control |= AI_AIINT;
    UpdateInterrupts();
  }
}

void AudioInterface::Reschedule()
{
  m_machine.scheduler.Remove(m_event);
  m_machine.scheduler.Schedule(Period(), m_event);
}

void AudioInterface::Event(void* user, uint64_t, int64_t late)
{
  AudioInterface& ai = *static_cast<AudioInterface*>(user);
  if (!ai.Playing())
    return;
  const uint64_t diff = ai.m_machine.scheduler.Now() - ai.m_last_cpu_time;
  if (diff > ai.m_cycles_per_sample)
  {
    const uint32_t samples = static_cast<uint32_t>(diff / ai.m_cycles_per_sample);
    ai.m_last_cpu_time += samples * ai.m_cycles_per_sample;
    ai.IncreaseSampleCount(samples);
  }
  ai.m_machine.scheduler.Schedule(ai.Period() - late, ai.m_event);
}

uint32_t AudioInterface::Read32(uint32_t offset)
{
  switch (offset)
  {
  case 0x00:
    return m_control;
  case 0x04:
    return m_volume;
  case 0x08:
  {
    const uint64_t streamed =
        Playing() ? m_machine.scheduler.Now() - m_last_cpu_time : m_last_cpu_time;
    return m_sample_counter + static_cast<uint32_t>(streamed / m_cycles_per_sample);
  }
  case 0x0C:
    return m_interrupt_timing;
  default:
    return 0;
  }
}

void AudioInterface::Write32(uint32_t offset, uint32_t value)
{
  switch (offset)
  {
  case 0x00:
    m_control = (m_control & ~(AI_AIINTMSK | AI_AIINTVLD)) | (value & (AI_AIINTMSK | AI_AIINTVLD));
    if ((value & AI_AISFR) != (m_control & AI_AISFR))
      SetAisRate(value & AI_AISFR);
    if ((value & AI_AIDFR) != (m_control & AI_AIDFR))
      SetAidRate(value & AI_AIDFR);
    if ((value & AI_PSTAT) != (m_control & AI_PSTAT))
    {
      m_control = (m_control & ~AI_PSTAT) | (value & AI_PSTAT);
      m_last_cpu_time = m_machine.scheduler.Now();
      Reschedule();
    }
    if (value & AI_AIINT)
      m_control &= ~AI_AIINT;
    if (value & AI_SCRESET)
    {
      m_sample_counter = 0;
      m_last_cpu_time = m_machine.scheduler.Now();
    }
    UpdateInterrupts();
    break;
  case 0x04:
    m_volume = value;
    break;
  case 0x08:
    m_sample_counter = value;
    m_last_cpu_time = m_machine.scheduler.Now();
    Reschedule();
    break;
  case 0x0C:
    m_interrupt_timing = value;
    Reschedule();
    break;
  }
}

void AudioInterface::DoState(StateIO& io)
{
  io.Do(m_control);
  io.Do(m_volume);
  io.Do(m_sample_counter);
  io.Do(m_interrupt_timing);
  io.Do(m_last_cpu_time);
  io.Do(m_ais_divisor);
  io.Do(m_aid_divisor);
  io.Do(m_cycles_per_sample);
  if (io.Reading())
    UpdateInterrupts();
}

// ---------------------------------------------------------------------------
// Command processor, pixel engine, memory interface

namespace
{
constexpr uint16_t PE_TOKEN_ENABLE = 1u << 0;
constexpr uint16_t PE_FINISH_ENABLE = 1u << 1;
constexpr uint16_t PE_TOKEN = 1u << 2;
constexpr uint16_t PE_FINISH = 1u << 3;

constexpr uint32_t POSTED_FINISH = 1u << 0;
constexpr uint32_t POSTED_TOKEN = 1u << 1;
constexpr uint32_t POSTED_TOKEN_INTERRUPT = 1u << 2;
}  // namespace

uint16_t GraphicsRegisters::ReadCp(uint32_t offset) const
{
  switch (offset &= 0x7E)
  {
  case 0x00:
    return 0x000C;  // the FIFO is always drained: read idle, command idle
  case 0x04:
    return 0;
  default:
    return m_cp[offset >> 1];
  }
}

void GraphicsRegisters::WriteCp(uint32_t offset, uint16_t value)
{
  m_cp[(offset >> 1) & 0x3F] = value;
}

uint16_t GraphicsRegisters::ReadPe(uint32_t offset) const
{
  switch (offset &= 0x7E)
  {
  case 0x00:
  case 0x02:
  case 0x04:
  case 0x06:
  case 0x08:
    return m_pe_conf[offset >> 1];
  case 0x0A:
    return m_pe_control;
  case 0x0E:
    return m_token;
  default:
    return 0;  // bounding box and performance counters are not modelled
  }
}

void GraphicsRegisters::WritePe(uint32_t offset, uint16_t value)
{
  switch (offset &= 0x7E)
  {
  case 0x00:
  case 0x02:
  case 0x04:
  case 0x06:
  case 0x08:
    m_pe_conf[offset >> 1] = value;
    break;
  case 0x0A:
    if (value & PE_TOKEN)
      m_signal_token = false;
    if (value & PE_FINISH)
      m_signal_finish = false;
    m_pe_control = value & (PE_TOKEN_ENABLE | PE_FINISH_ENABLE);
    UpdateInterrupts();
    break;
  }
}

void GraphicsRegisters::UpdateInterrupts()
{
  m_machine.pi.SetInterrupt(PI_INT_PE_TOKEN, m_signal_token && (m_pe_control & PE_TOKEN_ENABLE));
  m_machine.pi.SetInterrupt(PI_INT_PE_FINISH, m_signal_finish && (m_pe_control & PE_FINISH_ENABLE));
}

void GraphicsRegisters::PostDrawDone()
{
  m_posted.fetch_or(POSTED_FINISH, std::memory_order_release);
}

void GraphicsRegisters::PostToken(uint16_t token, bool interrupt)
{
  m_posted_token.store(token, std::memory_order_relaxed);
  m_posted.fetch_or(POSTED_TOKEN | (interrupt ? POSTED_TOKEN_INTERRUPT : 0),
                    std::memory_order_release);
}

void GraphicsRegisters::Poll()
{
  if (m_posted.load(std::memory_order_relaxed) == 0)
    return;
  const uint32_t posted = m_posted.exchange(0, std::memory_order_acquire);
  if (posted & POSTED_TOKEN)
    m_token = static_cast<uint16_t>(m_posted_token.load(std::memory_order_relaxed));
  if (posted & POSTED_TOKEN_INTERRUPT)
    m_signal_token = true;
  if (posted & POSTED_FINISH)
    m_signal_finish = true;
  UpdateInterrupts();
}

void GraphicsRegisters::DoState(StateIO& io)
{
  io.Do(m_cp);
  io.Do(m_mi);
  io.Do(m_pe_conf);
  io.Do(m_pe_control);
  io.Do(m_token);
  io.Do(m_signal_token);
  io.Do(m_signal_finish);
  if (io.Reading())
  {
    m_posted.store(0);
    UpdateInterrupts();
  }
}

// ---------------------------------------------------------------------------
// Hollywood IPC and GPIO

namespace
{
enum : uint32_t
{
  GPIO_SLOT_IN = 0x80,
  // Pins the PPC drives through GPIOB; the rest belong to Starlet.
  GPIO_PPC_OWNED = 0x20 | 0x80 | 0x100 | 0x200 | 0x4000 | 0x8000,
};
}  // namespace

Hollywood::Hollywood(Machine& machine)
{
  // Event types are saved by index: keep the slot the mailbox's event had.
  machine.scheduler.Register([](void*, uint64_t, int64_t) {}, nullptr);
}

uint32_t Hollywood::Read32(uint32_t offset) const
{
  switch (offset)
  {
  case 0x64:
    return 0xFFFFFFFF;  // AHBPROT: everything accessible
  case 0xC0:
  case 0xE0:
    return m_gpio_out;
  case 0xC4:
  case 0xE4:
    return m_gpio_dir;
  case 0xC8:
  case 0xE8:
    return GPIO_SLOT_IN;  // a disc is in the drive
  case 0x194:
    return m_resets;
  default:
    return 0;
  }
}

void Hollywood::Write32(uint32_t offset, uint32_t value)
{
  switch (offset)
  {
  case 0xC0:
    m_gpio_out = (value & GPIO_PPC_OWNED) | (m_gpio_out & ~GPIO_PPC_OWNED);
    break;
  case 0xC4:
    m_gpio_dir = (value & GPIO_PPC_OWNED) | (m_gpio_dir & ~GPIO_PPC_OWNED);
    break;
  case 0xE0:
    m_gpio_out = (m_gpio_out & GPIO_PPC_OWNED) | (value & ~GPIO_PPC_OWNED);
    break;
  case 0xE4:
    m_gpio_dir = (m_gpio_dir & GPIO_PPC_OWNED) | (value & ~GPIO_PPC_OWNED);
    break;
  case 0x194:
    m_resets = value;
    break;
  default:
    break;
  }
}

bool Hollywood::TakeMailboxReply(uint32_t* address)
{
  *address = std::exchange(m_mailbox_reply, 0);
  return *address != 0;
}

void Hollywood::DoState(StateIO& io)
{
  // The mailbox registers keep their place in the state.
  uint32_t ppc_msg = 0, arm_msg = 0, irq_flags = 0, irq_mask = 0;
  uint8_t ctrl = 0;
  io.Do(ppc_msg);
  io.Do(arm_msg);
  io.Do(ctrl);
  io.Do(irq_flags);
  io.Do(irq_mask);
  io.Do(m_gpio_dir);
  io.Do(m_gpio_out);
  io.Do(m_resets);
  // Y1: a reply the PPC had not taken yet.
  m_mailbox_reply = io.Reading() && (ctrl & 0x04) ? arm_msg : 0;
}
}  // namespace dualize
