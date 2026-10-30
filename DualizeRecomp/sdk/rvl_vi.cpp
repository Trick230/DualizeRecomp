// The Revolution SDK's video interface library at its boundary with the
// hardware. VIConfigure, VISetNextFrameBuffer, VISetBlack and the rest of the
// API only stage register values in the SDK's shadows; the retrace handler
// flushes them to the VI, and a few getters read the beam. Here the handler
// flushes the shadows into the host's video clock (hw/hw.hpp), the getters
// read its beam and the hardware init sets its timing, while the staging API
// runs as the game's code. There are no VI registers: what of the SDK still
// touches them (VIInit, OSFatal's video setup) reads them idle.
//
// The natives are found by their SDK code (config/sdk.sigs); the addresses
// and small-data offsets that depend on the game's link come from the
// natives list's defines (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <bit>

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Load16;
using dualize::Load32;
using dualize::Machine;
using dualize::Store32;
using dualize::VideoTiming;

// What the handler sends the video encoder over i2c, by the bit that asks
// for it: VISetCGMS, VISetWSS, VISetClosedCaption, VISetMacrovision, the
// gamma table, VISetTrapFilter, VISetRGBOverDrive and VISetRGBModeImm.
constexpr uint32_t ENCODER_UPDATES[8] = {VI_ENCODER_UPDATE_0, VI_ENCODER_UPDATE_1, VI_ENCODER_UPDATE_2,
                                         VI_ENCODER_UPDATE_3, VI_ENCODER_UPDATE_4, VI_ENCODER_UPDATE_5,
                                         VI_ENCODER_UPDATE_6, VI_ENCODER_UPDATE_7};

// SDK data
constexpr uint32_t SHADOW_REGS_SIZE = 64 * 2;
constexpr uint32_t HOR_VER_ADJUSTED_POS_Y = 10;
constexpr uint32_t HOR_VER_TV = 40;
constexpr uint32_t HOR_VER_TIMING = 84;
constexpr uint32_t HOR_VER_SIZE = 88;
// One flag per input source that has seen no input since the last retrace,
// past the shadows in the VI's data.
constexpr uint32_t IDLE_FLAGS = VI_SHADOW_REGS + VI_IDLE_FLAGS_OFFSET;
constexpr uint32_t IDLE_FLAG_COUNT = 10;
constexpr uint32_t OS_TV_MODE = 0x800000CC;
// VITiming
constexpr uint32_t TIMING_EQU = 0;  // u8
constexpr uint32_t TIMING_ACV = 2;
constexpr uint32_t TIMING_PRB_ODD = 4;
constexpr uint32_t TIMING_PRB_EVEN = 6;
constexpr uint32_t TIMING_PSB_ODD = 8;
constexpr uint32_t TIMING_PSB_EVEN = 10;
constexpr uint32_t TIMING_NHLINES = 24;
constexpr uint32_t TIMING_HLW = 26;
constexpr uint32_t TIMING_SIZE = 38;

// Small-data globals
// Screen dimming after a stretch without input (VIEnableDimming), and the
// longer stretch after which the SDK would hand over to a standby hook.
// r13 points into the middle of the 64 KiB small-data area.
constexpr int32_t SDA_LOW = -0x8000;
constexpr uint32_t SDA_SIZE = 0x10000;

constexpr uint32_t HANDLER_FRAME = 752;
constexpr uint32_t HANDLER_CONTEXT = 16;
constexpr uint32_t HANDLER_X = 10, HANDLER_Y = 8;
// The cable is component: the SDK offers progressive scan.
constexpr uint32_t COMPONENT_CABLE = 1;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// The VI's small-data globals.
class Globals
{
public:
  explicit Globals(CPUState* ctx)
      : m_base(GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[13] + SDA_LOW, SDA_SIZE))
  {
  }
  bool Ok() const { return m_base != nullptr; }
  uint32_t operator[](int32_t offset) const { return Load32(m_base + (offset - SDA_LOW)); }
  void Set(int32_t offset, uint32_t value) { Store32(m_base + (offset - SDA_LOW), value); }

private:
  uint8_t* m_base;
};

// The beam's half line in the frame, as the SDK works it out from the VI's
// line and position counters.
uint32_t HalfLine(Machine& machine, const uint8_t* timing)
{
  const uint32_t vct = machine.vi.Vct();
  const uint32_t hct = machine.vi.Hct();
  return (vct - 1) * 2 + (hct - 1) / Load16(timing + TIMING_HLW);
}

const uint8_t* CurrentTiming(CPUState* ctx, const Globals& globals)
{
  return GetMachine(CpuOf(ctx)).memory.Effective(globals[VI_CURRENT_TIMING_SDA], TIMING_SIZE);
}

// VI_NTSC, VI_PAL, VI_MPAL, VI_DEBUG... to the class whose filters and
// dimming times apply.
uint32_t TvClass(uint32_t tv)
{
  switch (tv)
  {
  case 1:
  case 4:
    return 1;
  case 2:
  case 5:
    return tv;
  default:
    return 0;
  }
}

// The staged registers to the video clock; the rest (frame buffers,
// filters, display window) only feed the scan-out the host does not do.
void Flush(Machine& machine, const uint8_t* shadow, uint64_t changed)
{
  VideoTiming timing = machine.vi.Timing();
  bool retimed = false;
  while (changed)
  {
    const int index = std::countl_zero(changed);
    changed &= ~(uint64_t{1} << (63 - index));
    const uint32_t offset = static_cast<uint32_t>(index) * 2;
    const uint32_t value = Load16(shadow + offset);
    switch (offset)
    {
    case 0x00:
      timing.equ = value & 0xF;
      timing.acv = (value >> 4) & 0x3FF;
      break;
    case 0x02:
      timing.non_interlaced = (value & 4) != 0;
      break;
    case 0x06:
      timing.hlw = value & 0x3FF;
      break;
    case 0x0C:
    case 0x10:
      timing.psb[(offset - 0x0C) / 4] = value & 0x3FF;
      break;
    case 0x0E:
    case 0x12:
      timing.prb[(offset - 0x0E) / 4] = value & 0x3FF;
      break;
    case 0x30:
    case 0x34:
    case 0x38:
    case 0x3C:
      timing.interrupts[(offset - 0x30) / 4].vct = value & 0x7FF;
      timing.interrupts[(offset - 0x30) / 4].enabled = (value & 0x1000) != 0;
      break;
    case 0x32:
    case 0x36:
    case 0x3A:
    case 0x3E:
      timing.interrupts[(offset - 0x32) / 4].hct = value & 0x7FF;
      break;
    case 0x6C:
      timing.clock54 = (value & 1) != 0;
      break;
    default:
      continue;
    }
    retimed = true;
  }
  if (retimed)
    machine.vi.Configure(timing);
}

// The handler's tail after the callbacks: the dimming timers. The hooks the
// SDK calls when the standby time runs out are empty in this build.
void Dimming(Cpu& cpu, Globals& g, uint8_t* idle_flags)
{
  if (g[VI_DIM_RESET_SDA] == 1 && cpu.CallGuest(OS_SET_VI_FORCE_DIMMING, {0, 0, 0}) == 1)
  {
    g.Set(VI_DIM_RESET_SDA, 0);
    g.Set(VI_DIM_COUNT_SDA, 0);
  }
  for (uint32_t i = 0; i < IDLE_FLAG_COUNT; ++i)
  {
    if (!Load32(idle_flags + i * 4))
    {
      Store32(idle_flags, 0);
      break;
    }
  }
  if (g[VI_IDLE_PAD_SDA] && g[VI_IDLE_REMOTE_SDA] && Load32(idle_flags))
  {
    if (g[VI_DIM_ENABLED_SDA] == 1 && g[VI_DIM_COUNT_SDA] != 0xFFFFFFFF)
      g.Set(VI_DIM_COUNT_SDA, g[VI_DIM_COUNT_SDA] + 1);
    if (g[VI_STANDBY_ENABLED_SDA] == 1 && g[VI_STANDBY_COUNT_SDA] != 0xFFFFFFFF)
      g.Set(VI_STANDBY_COUNT_SDA, g[VI_STANDBY_COUNT_SDA] + 1);
  }
  else
  {
    if (g[VI_DIM_COUNT_SDA] >= g[VI_DIM_TIME_SDA])
      g.Set(VI_UNDIM_REQUEST_SDA, 1);
    g.Set(VI_DIM_COUNT_SDA, 0);
    g.Set(VI_STANDBY_COUNT_SDA, 0);
    g.Set(VI_DIM_TIME_SDA, g[VI_DIM_TIME_SETTING_SDA]);
  }
  if (g[VI_DIM_LAST_ENABLED_SDA] != g[VI_DIM_ENABLED_SDA])
  {
    if (g[VI_DIM_ENABLED_SDA] == 0 && g[VI_DIM_COUNT_SDA] >= g[VI_DIM_TIME_SDA])
      g.Set(VI_UNDIM_REQUEST_SDA, 1);
    g.Set(VI_DIM_COUNT_SDA, 0);
    g.Set(VI_DIM_TIME_SDA, g[VI_DIM_TIME_SETTING_SDA]);
  }
  if (g[VI_DIM_COUNT_SDA] == g[VI_DIM_TIME_SDA])
    g.Set(VI_DIM_REQUEST_SDA, 1);
  if (g[VI_UNDIM_REQUEST_SDA] && cpu.CallGuest(OS_SET_VI_FORCE_DIMMING, {0, 2, 2}) == 1)
  {
    g.Set(VI_UNDIM_REQUEST_SDA, 0);
    g.Set(VI_DIMMED_SDA, 0);
  }
  if (g[VI_DIM_REQUEST_SDA] && cpu.CallGuest(OS_SET_VI_FORCE_DIMMING, {1, 2, 2}) == 1)
  {
    g.Set(VI_DIM_REQUEST_SDA, 0);
    g.Set(VI_DIMMED_SDA, 1);
  }
  if (g[VI_STANDBY_LAST_ENABLED_SDA] != g[VI_STANDBY_ENABLED_SDA])
    g.Set(VI_STANDBY_COUNT_SDA, 0);
  g.Set(VI_IDLE_PAD_SDA, 1);
  g.Set(VI_IDLE_REMOTE_SDA, 1);
  for (uint32_t i = 0; i < IDLE_FLAG_COUNT; ++i)
    Store32(idle_flags + i * 4, 1);
  g.Set(VI_DIM_LAST_ENABLED_SDA, g[VI_DIM_ENABLED_SDA]);
  g.Set(VI_STANDBY_LAST_ENABLED_SDA, g[VI_STANDBY_ENABLED_SDA]);
  if (g[VI_DIM_TIME_SETTING_SDA] > g[VI_DIM_COUNT_SDA] && g[VI_DIMMED_SDA] == 0)
    g.Set(VI_DIM_TIME_SDA, g[VI_DIM_TIME_SETTING_SDA]);
}
}  // namespace

// void __VIRetraceHandler(__OSInterrupt, OSContext* context)
// Runs inside OSDispatchInterrupt, with the scheduler off.
extern "C" bool rvl_VIRetraceHandler(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const dualize::Memory& memory = machine.memory;
  Globals g(ctx);
  const uint32_t context = ctx->gpr[4];
  const uint32_t sp = ctx->gpr[1];
  const uint32_t frame = sp - HANDLER_FRAME;
  uint8_t* frame_bytes = memory.Effective(frame, HANDLER_FRAME);
  const uint8_t* shadow = memory.Effective(VI_SHADOW_REGS, SHADOW_REGS_SIZE);
  const uint8_t* hor_ver = memory.Effective(VI_HOR_VER, HOR_VER_SIZE);
  uint8_t* idle_flags = memory.Effective(IDLE_FLAGS, IDLE_FLAG_COUNT * 4);
  if (!g.Ok() || !frame_bytes || !shadow || !hor_ver || !idle_flags)
    return false;
  ctx->downcount -= 200;
  Store32(frame_bytes, sp);
  ctx->gpr[1] = frame;
  const uint32_t scratch = frame + HANDLER_CONTEXT;

  const uint32_t interrupts = machine.vi.TakeInterrupts();
  if (interrupts & 0xC)
  {
    // Display interrupts 2 and 3: the display position callback.
    cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
    cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
    if (const uint32_t callback = g[VI_POSITION_CALLBACK_SDA])
    {
      const uint32_t vct = machine.vi.Vct();
      const uint32_t hct = machine.vi.Hct();
      cpu.CallGuest(VI_DISPLAY_POSITION_TO_XY, {hct, vct, frame + HANDLER_X, frame + HANDLER_Y});
      const auto position = [&](uint32_t offset) {
        return static_cast<uint32_t>(static_cast<int16_t>(Load16(frame_bytes + offset)));
      };
      cpu.CallGuest(callback, {position(HANDLER_X), position(HANDLER_Y)});
    }
    cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
    cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {context});
    ctx->gpr[1] = sp;
    return true;
  }

  g.Set(VI_RETRACE_COUNT_SDA, g[VI_RETRACE_COUNT_SDA] + 1);
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
  if (const uint32_t callback = g[VI_PRE_RETRACE_CALLBACK_SDA])
    cpu.CallGuest(callback, {g[VI_RETRACE_COUNT_SDA]});

  if (g[VI_BEAM_CHECK_SDA])
  {
    const uint8_t* timing = CurrentTiming(ctx, g);
    const uint32_t vct = machine.vi.Vct();
    if (timing && vct != 1 && vct != Load16(timing + TIMING_NHLINES) / 2 + 1)
      g.Set(VI_BEAM_CHECK_MISSES_SDA, g[VI_BEAM_CHECK_MISSES_SDA] + 1);
  }

  if (g[VI_FLUSH_PENDING_SDA])
  {
    bool flush = true;
    if (g[VI_FLUSH_IN_FIRST_FIELD_SDA] == 1)
    {
      const uint8_t* timing = CurrentTiming(ctx, g);
      flush = timing && HalfLine(machine, timing) < Load16(timing + TIMING_NHLINES);
    }
    if (flush)
    {
      Flush(machine, shadow, uint64_t{g[VI_CHANGED_HI_SDA]} << 32 | g[VI_CHANGED_LO_SDA]);
      g.Set(VI_CHANGED_HI_SDA, 0);
      g.Set(VI_CHANGED_LO_SDA, 0);
      g.Set(VI_FLUSH_IN_FIRST_FIELD_SDA, 0);
      g.Set(VI_CURRENT_TIMING_SDA, Load32(hor_ver + HOR_VER_TIMING));
      g.Set(VI_CURRENT_TV_SDA, Load32(hor_ver + HOR_VER_TV));
      g.Set(VI_CURRENT_DISPLAY_SDA, g[VI_STAGED_DISPLAY_SDA]);
      g.Set(VI_FLUSH_PENDING_SDA, 0);
      cpu.CallGuest(SI_REFRESH_SAMPLING_RATE);
    }
  }

  if (g[VI_LAST_YUVSEL_SDA] != COMPONENT_CABLE)
    cpu.CallGuest(VI_SET_YUVSEL, {COMPONENT_CABLE});
  g.Set(VI_LAST_YUVSEL_SDA, COMPONENT_CABLE);

  const uint32_t tv_class = TvClass(g[VI_CURRENT_TV_SDA]);
  if (tv_class != g[VI_LAST_TV_CLASS_SDA])
  {
    cpu.CallGuest(VI_SET_FILTER_4_EURGB60, {tv_class == 5 ? 1u : 0u});
    // Retraces without input before the screen dims, by the dimming time
    // setting, and before the standby hook.
    const uint32_t mode = g[VI_DIM_TIME_MODE_SDA];
    if (tv_class == 1)
    {
      g.Set(VI_DIM_TIME_SETTING_SDA, mode == 1 ? 30000 : mode == 2 ? 45000 : 15000);
      g.Set(VI_STANDBY_TIME_SDA, 90000);
    }
    else
    {
      g.Set(VI_DIM_TIME_SETTING_SDA, mode == 1 ? 36000 : mode == 2 ? 54000 : 18000);
      g.Set(VI_STANDBY_TIME_SDA, 108000);
    }
    g.Set(VI_DIM_COUNT_SDA, 0);
    g.Set(VI_STANDBY_COUNT_SDA, 0);
  }
  g.Set(VI_LAST_TV_CLASS_SDA, tv_class);

  if (g[VI_ENCODER_PENDING_SDA])
  {
    while (const uint32_t bits = g[VI_ENCODER_UPDATE_BITS_SDA])
    {
      const int bit = 31 - std::countl_zero(bits);
      if (bit < 8)
        cpu.CallGuest(ENCODER_UPDATES[bit]);
      g.Set(VI_ENCODER_UPDATE_BITS_SDA, bits & ~(1u << bit));
    }
    g.Set(VI_ENCODER_PENDING_SDA, 0);
  }

  if (const uint32_t callback = g[VI_POST_RETRACE_CALLBACK_SDA])
  {
    cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
    cpu.CallGuest(callback, {g[VI_RETRACE_COUNT_SDA]});
  }
  cpu.CallGuest(OS_WAKEUP_THREAD, {ctx->gpr[13] + VI_RETRACE_QUEUE_SDA});
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {context});
  Dimming(cpu, g, idle_flags);
  ctx->gpr[1] = sp;
  return true;
}

// void __VIInit(VITVMode mode): the VI's own timing for the mode, display
// interrupts at the frame's end and start.
extern "C" bool rvl_VIInitHardware(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const uint32_t mode = ctx->gpr[3];
  uint8_t* tv_mode = machine.memory.Effective(OS_TV_MODE, 4);
  if (!tv_mode)
    return false;
  Store32(tv_mode, mode >> 2);
  const uint8_t* timing = machine.memory.Effective(cpu.CallGuest(VI_GET_TIMING, {mode}), TIMING_SIZE);
  if (!timing)
    return false;
  ctx->downcount -= 100;

  // Without an active video window, the VI folds it into the blanking.
  const uint32_t acv = 2 * Load16(timing + TIMING_ACV);
  const uint32_t hlw = Load16(timing + TIMING_HLW);
  const bool progressive = (mode & 3) > 1;
  VideoTiming vt = machine.vi.Timing();
  vt.equ = timing[TIMING_EQU];
  vt.acv = 0;
  vt.hlw = hlw;
  vt.prb[0] = Load16(timing + TIMING_PRB_ODD) + acv - 2;
  vt.prb[1] = Load16(timing + TIMING_PRB_EVEN) + acv - 2;
  vt.psb[0] = Load16(timing + TIMING_PSB_ODD) + 2;
  vt.psb[1] = Load16(timing + TIMING_PSB_EVEN) + 2;
  vt.clock54 = progressive;
  vt.non_interlaced = progressive || (mode & 1) != 0;
  vt.interrupts[0] = {Load16(timing + TIMING_NHLINES) / 2u + 1, hlw + 1, true};
  vt.interrupts[1] = {1, 1, true};
  machine.vi.Configure(vt);
  machine.vi.TakeInterrupts();
  return true;
}

// u32 VIGetNextField(void)
extern "C" bool rvl_VIGetNextField(CPUState* ctx)
{
  Machine& machine = GetMachine(CpuOf(ctx));
  const Globals g(ctx);
  const uint8_t* timing = g.Ok() ? CurrentTiming(ctx, g) : nullptr;
  const uint8_t* hor_ver = machine.memory.Effective(VI_HOR_VER, HOR_VER_SIZE);
  if (!timing || !hor_ver)
    return false;
  const uint32_t first_field = HalfLine(machine, timing) < Load16(timing + TIMING_NHLINES);
  ctx->gpr[3] = (first_field ^ 1) ^ (Load16(hor_ver + HOR_VER_ADJUSTED_POS_Y) & 1);
  ctx->downcount -= 40;
  return true;
}

// u32 VIGetCurrentLine(void)
extern "C" bool rvl_VIGetCurrentLine(CPUState* ctx)
{
  Machine& machine = GetMachine(CpuOf(ctx));
  const Globals g(ctx);
  const uint8_t* timing = g.Ok() ? CurrentTiming(ctx, g) : nullptr;
  if (!timing)
    return false;
  const uint32_t lines = Load16(timing + TIMING_NHLINES);
  uint32_t half_line = HalfLine(machine, timing);
  if (half_line >= lines)
    half_line -= lines;
  ctx->gpr[3] = half_line >> 1;
  ctx->downcount -= 40;
  return true;
}

// VIScanMode VIGetScanMode(void): what the VI scans out now.
extern "C" bool rvl_VIGetScanMode(CPUState* ctx)
{
  const VideoTiming& timing = GetMachine(CpuOf(ctx)).vi.Timing();
  ctx->gpr[3] = timing.clock54 ? 2 : timing.non_interlaced ? 1 : 0;
  ctx->downcount -= 20;
  return true;
}

// u32 VIGetDTVStatus(void)
extern "C" bool rvl_VIGetDTVStatus(CPUState* ctx)
{
  ctx->gpr[3] = COMPONENT_CABLE;
  ctx->downcount -= 20;
  return true;
}
