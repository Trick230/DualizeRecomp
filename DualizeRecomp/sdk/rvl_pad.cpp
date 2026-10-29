// The Revolution SDK's GameCube pad library at its API. PAD probes each port
// with SI commands (type, origin), keeps the controllers polled by the SI and
// builds each PADStatus from the polled report. Here PADRead takes the
// host's pad straight into the report and runs the SDK's own MakeStatus on it
// (origin, analog mode and the VI dimming reset), so the SI is left with
// nothing to do: its init and the sampling rate the VI driver refreshes keep
// only their globals. NiGHTS itself plays with the Wii Remote; PAD reads a
// GameCube pad when one drives a port.
//
// The natives are pinned to their SDK bytes (R7EE8P.natives); the addresses
// and offsets below belong to that build.

#include "core/cpu.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <algorithm>

namespace
{
using dualize::Cpu;
using dualize::GcPadInput;
using dualize::Load16;
using dualize::Load32;
using dualize::Machine;
using dualize::Store16;
using dualize::Store32;

// Small-data globals
constexpr int32_t PAD_INITIALIZED_SDA = -26944;
constexpr int32_t PAD_PENDING_SPEC_SDA = -26976;
constexpr int32_t PAD_SPEC_SDA = -29584;
constexpr int32_t PAD_MAKE_STATUS_SDA = -29580;  // void (*)(s32 chan, PADStatus*, u32 data[2])
constexpr int32_t PAD_ANALOG_MODE_SDA = -29588;  // the mode in bits 8-10
constexpr int32_t SI_INITIALIZED_SDA = -27756;
constexpr int32_t SI_SAMPLING_RATE_SDA = -27744;

// MakeStatus for the controller specs (PADSetSpec) 1 and 2 to 5.
constexpr uint32_t SPEC1_MAKE_STATUS = 0x804682E4;
constexpr uint32_t SPEC2_MAKE_STATUS = 0x804683FC;

constexpr uint32_t PAD_STATUS_SIZE = 12;
constexpr uint32_t PAD_STATUS_ERR = 10;
constexpr int8_t PAD_ERR_NONE = 0;
constexpr int8_t PAD_ERR_NO_CONTROLLER = -1;
constexpr uint16_t PAD_USE_ORIGIN = 0x0080;
constexpr uint32_t SI_MAX_SAMPLING_RATE = 11;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

uint8_t* Sda(CPUState* ctx, int32_t offset)
{
  return CpuOf(ctx).GetMachine().memory.Effective(ctx->gpr[13] + offset, 4);
}

// The controller's report: buttons and main stick, then the rest in the
// layout of each analog mode.
uint32_t ReportHi(const GcPadInput& pad)
{
  return pad.stick_y | uint32_t{pad.stick_x} << 8 | uint32_t(pad.buttons | PAD_USE_ORIGIN) << 16;
}

uint32_t ReportLo(const GcPadInput& pad, uint32_t mode)
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

void ReturnTrue(CPUState* ctx, int cycles)
{
  ctx->gpr[3] = 1;
  ctx->downcount -= cycles;
}
}  // namespace

// BOOL PADInit(void): takes a pending PADSetSpec. The ports need no probing.
extern "C" bool rvl_PADInit(CPUState* ctx)
{
  uint8_t* initialized = Sda(ctx, PAD_INITIALIZED_SDA);
  uint8_t* pending = Sda(ctx, PAD_PENDING_SPEC_SDA);
  uint8_t* spec = Sda(ctx, PAD_SPEC_SDA);
  uint8_t* make_status = Sda(ctx, PAD_MAKE_STATUS_SDA);
  if (!initialized || !pending || !spec || !make_status)
    return false;
  if (!Load32(initialized))
  {
    if (const int32_t next = static_cast<int32_t>(Load32(pending)))
    {
      Store32(pending, 0);
      if (next == 1)
        Store32(make_status, SPEC1_MAKE_STATUS);
      else if (next >= 2 && next < 6)
        Store32(make_status, SPEC2_MAKE_STATUS);
      Store32(spec, static_cast<uint32_t>(next));
    }
    Store32(initialized, 1);
  }
  ReturnTrue(ctx, 60);
  return true;
}

// BOOL PADReset(u32 mask), BOOL PADRecalibrate(u32 mask): a host pad is
// always ready and centred.
extern "C" bool rvl_PADReset(CPUState* ctx)
{
  ReturnTrue(ctx, 40);
  return true;
}

extern "C" bool rvl_PADRecalibrate(CPUState* ctx)
{
  ReturnTrue(ctx, 40);
  return true;
}

// u32 PADRead(PADStatus status[4]): returns the ports whose controller has
// a rumble motor.
extern "C" bool rvl_PADRead(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = cpu.GetMachine();
  const uint32_t status = ctx->gpr[3];
  uint8_t* statuses = machine.memory.Effective(status, 4 * PAD_STATUS_SIZE);
  const uint8_t* mode = Sda(ctx, PAD_ANALOG_MODE_SDA);
  const uint8_t* make_status = Sda(ctx, PAD_MAKE_STATUS_SDA);
  // MakeStatus reads the report from a buffer below the caller's frame.
  const uint32_t sp = ctx->gpr[1];
  const uint32_t report = sp - 16;
  uint8_t* report_bytes = machine.memory.Effective(report, 8);
  if (!statuses || !mode || !make_status || !report_bytes)
    return false;

  uint32_t motors = 0;
  for (int port = 0; port < 4; ++port)
  {
    uint8_t* s = statuses + port * PAD_STATUS_SIZE;
    GcPadInput pad;
    if (!machine.ReadGcPad(port, &pad))
    {
      std::fill_n(s, PAD_STATUS_ERR, 0);
      s[PAD_STATUS_ERR] = static_cast<uint8_t>(PAD_ERR_NO_CONTROLLER);
      continue;
    }
    Store32(report_bytes, ReportHi(pad));
    Store32(report_bytes + 4, ReportLo(pad, (Load32(mode) >> 8) & 7));
    ctx->gpr[1] = sp - 32;
    cpu.CallGuest(Load32(make_status),
                  {static_cast<uint32_t>(port), status + port * PAD_STATUS_SIZE, report});
    ctx->gpr[1] = sp;
    // The report's origin flag is the SI's, not a button.
    Store16(s, Load16(s) & ~PAD_USE_ORIGIN);
    s[PAD_STATUS_ERR] = static_cast<uint8_t>(PAD_ERR_NONE);
    motors |= 0x80000000u >> port;
  }
  ctx->gpr[3] = motors;
  ctx->downcount -= 120;
  return true;
}

// void PADControlMotor(s32 chan, u32 command): PAD_MOTOR_RUMBLE (1) starts
// the host pad's rumble, the stops (0, 2) end it.
extern "C" bool rvl_PADControlMotor(CPUState* ctx)
{
  if (ctx->gpr[3] < 4)
    CpuOf(ctx).GetMachine().SetGcRumble(static_cast<int>(ctx->gpr[3]), ctx->gpr[4] == 1);
  ctx->downcount -= 40;
  return true;
}

// void SIInit(void)
extern "C" bool rvl_SIInit(CPUState* ctx)
{
  uint8_t* initialized = Sda(ctx, SI_INITIALIZED_SDA);
  if (!initialized)
    return false;
  Store32(initialized, 1);
  ctx->downcount -= 40;
  return true;
}

// void SISetSamplingRate(u32 msec): the rate the VI driver re-applies after
// a mode change; with nothing to poll only the global is left.
extern "C" bool rvl_SISetSamplingRate(CPUState* ctx)
{
  uint8_t* rate = Sda(ctx, SI_SAMPLING_RATE_SDA);
  if (!rate)
    return false;
  Store32(rate, std::min(ctx->gpr[3], SI_MAX_SAMPLING_RATE));
  ctx->downcount -= 30;
  return true;
}
