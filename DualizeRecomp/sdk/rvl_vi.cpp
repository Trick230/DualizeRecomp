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
// and offsets below are NiGHTS' (defines, were it shared).

#include "core/cpu.h"

#include "dolrecomp.hpp"
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

// SDK functions
constexpr uint32_t OS_SET_CURRENT_CONTEXT = 0x8044F490;
constexpr uint32_t OS_CLEAR_CONTEXT = 0x8044F6B8;
constexpr uint32_t OS_WAKEUP_THREAD = 0x804570DC;
constexpr uint32_t OS_SET_VI_FORCE_DIMMING = 0x80458028;  // BOOL (BOOL, u32, u32)
constexpr uint32_t SI_REFRESH_SAMPLING_RATE = 0x8044C70C;
constexpr uint32_t VI_GET_TIMING = 0x80462290;  // VITiming* getTiming(VITVMode)
constexpr uint32_t VI_DISPLAY_POSITION_TO_XY = 0x80463E9C;
constexpr uint32_t VI_SET_YUVSEL = 0x80464C64;
constexpr uint32_t VI_SET_FILTER_4_EURGB60 = 0x80464D08;
// What the handler sends the video encoder over i2c, by the bit that asks
// for it: VISetCGMS, VISetWSS, VISetClosedCaption, VISetMacrovision, the
// gamma table, VISetTrapFilter, VISetRGBOverDrive and VISetRGBModeImm.
constexpr uint32_t ENCODER_UPDATES[8] = {0x80464D48, 0x80464DA0, 0x80464E04, 0x80464E70,
                                         0x80465D8C, 0x80465DA4, 0x80465E00, 0x80465E94};

// SDK data
constexpr uint32_t SHADOW_REGS = 0x80694EC0;  // u16 per register, as last staged
constexpr uint32_t SHADOW_REGS_SIZE = 64 * 2;
constexpr uint32_t HOR_VER = 0x80694FB0;  // the staged mode
constexpr uint32_t HOR_VER_ADJUSTED_POS_Y = 10;
constexpr uint32_t HOR_VER_TV = 40;
constexpr uint32_t HOR_VER_TIMING = 84;
constexpr uint32_t HOR_VER_SIZE = 88;
// One flag per input source that has seen no input since the last retrace.
constexpr uint32_t IDLE_FLAGS = 0x80695008;
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
constexpr int32_t RETRACE_COUNT = -27036;
constexpr int32_t PRE_RETRACE_CALLBACK = -27080;
constexpr int32_t POST_RETRACE_CALLBACK = -27084;
constexpr int32_t RETRACE_QUEUE = -27072;
constexpr int32_t POSITION_CALLBACK = -27168;
constexpr int32_t FLUSH_PENDING = -27040;
constexpr int32_t FLUSH_IN_FIRST_FIELD = -27144;  // a scan mode change waits for it
constexpr int32_t CHANGED_HI = -27136;  // u64 of staged registers, register 0 first
constexpr int32_t CHANGED_LO = -27132;
constexpr int32_t CURRENT_TIMING = -27092;
constexpr int32_t CURRENT_TV = -27096;
constexpr int32_t CURRENT_DISPLAY = -27104;
constexpr int32_t STAGED_DISPLAY = -27100;
constexpr int32_t BEAM_CHECK = -27200;  // counts retraces off the expected line
constexpr int32_t BEAM_CHECK_MISSES = -27204;
constexpr int32_t ENCODER_PENDING = -27044;
constexpr int32_t ENCODER_UPDATE_BITS = -27024;
constexpr int32_t LAST_YUVSEL = -29656;
constexpr int32_t LAST_TV_CLASS = -29652;
// Screen dimming after a stretch without input (VIEnableDimming), and the
// longer stretch after which the SDK would hand over to a standby hook.
constexpr int32_t DIM_TIME_MODE = -27056;
constexpr int32_t DIM_RESET = -27196;
constexpr int32_t DIM_ENABLED = -27048;
constexpr int32_t DIM_LAST_ENABLED = -29648;
constexpr int32_t DIM_COUNT = -27180;
constexpr int32_t DIM_TIME = -27192;
constexpr int32_t DIM_TIME_SETTING = -27188;
constexpr int32_t DIM_REQUEST = -27116;
constexpr int32_t UNDIM_REQUEST = -27112;
constexpr int32_t DIMMED = -27172;
constexpr int32_t STANDBY_ENABLED = -27052;
constexpr int32_t STANDBY_LAST_ENABLED = -29644;
constexpr int32_t STANDBY_COUNT = -27176;
constexpr int32_t STANDBY_TIME = -27184;
constexpr int32_t IDLE_PAD = -27060;
constexpr int32_t IDLE_REMOTE = -27064;
constexpr int32_t SDA_LOW = -29664;
constexpr uint32_t SDA_SIZE = 29664 - 27000;

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
  return GetMachine(CpuOf(ctx)).memory.Effective(globals[CURRENT_TIMING], TIMING_SIZE);
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
  if (g[DIM_RESET] == 1 && cpu.CallGuest(OS_SET_VI_FORCE_DIMMING, {0, 0, 0}) == 1)
  {
    g.Set(DIM_RESET, 0);
    g.Set(DIM_COUNT, 0);
  }
  for (uint32_t i = 0; i < IDLE_FLAG_COUNT; ++i)
  {
    if (!Load32(idle_flags + i * 4))
    {
      Store32(idle_flags, 0);
      break;
    }
  }
  if (g[IDLE_PAD] && g[IDLE_REMOTE] && Load32(idle_flags))
  {
    if (g[DIM_ENABLED] == 1 && g[DIM_COUNT] != 0xFFFFFFFF)
      g.Set(DIM_COUNT, g[DIM_COUNT] + 1);
    if (g[STANDBY_ENABLED] == 1 && g[STANDBY_COUNT] != 0xFFFFFFFF)
      g.Set(STANDBY_COUNT, g[STANDBY_COUNT] + 1);
  }
  else
  {
    if (g[DIM_COUNT] >= g[DIM_TIME])
      g.Set(UNDIM_REQUEST, 1);
    g.Set(DIM_COUNT, 0);
    g.Set(STANDBY_COUNT, 0);
    g.Set(DIM_TIME, g[DIM_TIME_SETTING]);
  }
  if (g[DIM_LAST_ENABLED] != g[DIM_ENABLED])
  {
    if (g[DIM_ENABLED] == 0 && g[DIM_COUNT] >= g[DIM_TIME])
      g.Set(UNDIM_REQUEST, 1);
    g.Set(DIM_COUNT, 0);
    g.Set(DIM_TIME, g[DIM_TIME_SETTING]);
  }
  if (g[DIM_COUNT] == g[DIM_TIME])
    g.Set(DIM_REQUEST, 1);
  if (g[UNDIM_REQUEST] && cpu.CallGuest(OS_SET_VI_FORCE_DIMMING, {0, 2, 2}) == 1)
  {
    g.Set(UNDIM_REQUEST, 0);
    g.Set(DIMMED, 0);
  }
  if (g[DIM_REQUEST] && cpu.CallGuest(OS_SET_VI_FORCE_DIMMING, {1, 2, 2}) == 1)
  {
    g.Set(DIM_REQUEST, 0);
    g.Set(DIMMED, 1);
  }
  if (g[STANDBY_LAST_ENABLED] != g[STANDBY_ENABLED])
    g.Set(STANDBY_COUNT, 0);
  g.Set(IDLE_PAD, 1);
  g.Set(IDLE_REMOTE, 1);
  for (uint32_t i = 0; i < IDLE_FLAG_COUNT; ++i)
    Store32(idle_flags + i * 4, 1);
  g.Set(DIM_LAST_ENABLED, g[DIM_ENABLED]);
  g.Set(STANDBY_LAST_ENABLED, g[STANDBY_ENABLED]);
  if (g[DIM_TIME_SETTING] > g[DIM_COUNT] && g[DIMMED] == 0)
    g.Set(DIM_TIME, g[DIM_TIME_SETTING]);
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
  const uint8_t* shadow = memory.Effective(SHADOW_REGS, SHADOW_REGS_SIZE);
  const uint8_t* hor_ver = memory.Effective(HOR_VER, HOR_VER_SIZE);
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
    if (const uint32_t callback = g[POSITION_CALLBACK])
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

  g.Set(RETRACE_COUNT, g[RETRACE_COUNT] + 1);
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
  if (const uint32_t callback = g[PRE_RETRACE_CALLBACK])
    cpu.CallGuest(callback, {g[RETRACE_COUNT]});

  if (g[BEAM_CHECK])
  {
    const uint8_t* timing = CurrentTiming(ctx, g);
    const uint32_t vct = machine.vi.Vct();
    if (timing && vct != 1 && vct != Load16(timing + TIMING_NHLINES) / 2 + 1)
      g.Set(BEAM_CHECK_MISSES, g[BEAM_CHECK_MISSES] + 1);
  }

  if (g[FLUSH_PENDING])
  {
    bool flush = true;
    if (g[FLUSH_IN_FIRST_FIELD] == 1)
    {
      const uint8_t* timing = CurrentTiming(ctx, g);
      flush = timing && HalfLine(machine, timing) < Load16(timing + TIMING_NHLINES);
    }
    if (flush)
    {
      Flush(machine, shadow, uint64_t{g[CHANGED_HI]} << 32 | g[CHANGED_LO]);
      g.Set(CHANGED_HI, 0);
      g.Set(CHANGED_LO, 0);
      g.Set(FLUSH_IN_FIRST_FIELD, 0);
      g.Set(CURRENT_TIMING, Load32(hor_ver + HOR_VER_TIMING));
      g.Set(CURRENT_TV, Load32(hor_ver + HOR_VER_TV));
      g.Set(CURRENT_DISPLAY, g[STAGED_DISPLAY]);
      g.Set(FLUSH_PENDING, 0);
      cpu.CallGuest(SI_REFRESH_SAMPLING_RATE);
    }
  }

  if (g[LAST_YUVSEL] != COMPONENT_CABLE)
    cpu.CallGuest(VI_SET_YUVSEL, {COMPONENT_CABLE});
  g.Set(LAST_YUVSEL, COMPONENT_CABLE);

  const uint32_t tv_class = TvClass(g[CURRENT_TV]);
  if (tv_class != g[LAST_TV_CLASS])
  {
    cpu.CallGuest(VI_SET_FILTER_4_EURGB60, {tv_class == 5 ? 1u : 0u});
    // Retraces without input before the screen dims, by the dimming time
    // setting, and before the standby hook.
    const uint32_t mode = g[DIM_TIME_MODE];
    if (tv_class == 1)
    {
      g.Set(DIM_TIME_SETTING, mode == 1 ? 30000 : mode == 2 ? 45000 : 15000);
      g.Set(STANDBY_TIME, 90000);
    }
    else
    {
      g.Set(DIM_TIME_SETTING, mode == 1 ? 36000 : mode == 2 ? 54000 : 18000);
      g.Set(STANDBY_TIME, 108000);
    }
    g.Set(DIM_COUNT, 0);
    g.Set(STANDBY_COUNT, 0);
  }
  g.Set(LAST_TV_CLASS, tv_class);

  if (g[ENCODER_PENDING])
  {
    while (const uint32_t bits = g[ENCODER_UPDATE_BITS])
    {
      const int bit = 31 - std::countl_zero(bits);
      if (bit < 8)
        cpu.CallGuest(ENCODER_UPDATES[bit]);
      g.Set(ENCODER_UPDATE_BITS, bits & ~(1u << bit));
    }
    g.Set(ENCODER_PENDING, 0);
  }

  if (const uint32_t callback = g[POST_RETRACE_CALLBACK])
  {
    cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
    cpu.CallGuest(callback, {g[RETRACE_COUNT]});
  }
  cpu.CallGuest(OS_WAKEUP_THREAD, {ctx->gpr[13] + RETRACE_QUEUE});
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
  const uint8_t* hor_ver = machine.memory.Effective(HOR_VER, HOR_VER_SIZE);
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
