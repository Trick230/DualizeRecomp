#include "machine.hpp"

#include "boot.hpp"
#include "kernel/ios/directory_disc.hpp"
#include "profiler.hpp"

#include <aurora/fifo.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dualize
{
namespace
{
constexpr uint32_t GC_EPOCH = 0x386D4380;  // 2000-01-01, Unix seconds
constexpr int64_t DSP_UPDATE_PERIOD = Scheduler::CPU_CLOCK / 1000;
constexpr uint32_t STATE_MAGIC = 0x5354474D;  // "MGTS"
constexpr uint32_t STATE_VERSION = 3;
// Version 1 still kept the SI and EXI register files, versions 1 and 2 the
// VI's.
constexpr uint32_t STATE_VERSION_REGISTER_SI_EXI = 1;
constexpr uint32_t STATE_VERSION_REGISTER_VI = 2;

// Seconds since 1970 on the local clock.
int64_t LocalTimeNow()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{}, utc{};
#ifdef _WIN32
  localtime_s(&local, &now);
  gmtime_s(&utc, &now);
#else
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
#endif
  utc.tm_isdst = local.tm_isdst;
  return static_cast<int64_t>(now) + static_cast<int64_t>(std::difftime(std::mktime(&local), std::mktime(&utc)));
}

// Sleeps until target to within microseconds: a high-resolution waitable
// timer to just short of it (a plain sleep rounds to Windows' 15.6 ms timer
// tick), then a short spin.
void SleepUntil(std::chrono::steady_clock::time_point target)
{
  using namespace std::chrono;
  constexpr auto SPIN = microseconds(500);
  const auto now = steady_clock::now();
  if (target - now > SPIN)
  {
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
    static const HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -duration_cast<duration<int64_t, std::ratio<1, 10'000'000>>>(target - SPIN - now).count();
    if (!timer || !SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) ||
        WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0)
#endif
      std::this_thread::sleep_until(target - SPIN);
  }
  while (steady_clock::now() < target)
    std::this_thread::yield();
}

Machine& MachineOf(void* user)
{
  return *static_cast<Machine*>(user);
}

// What a physical gamepad drives: the GameCube pad on port 1 (the default;
// the game then shows the GameCube's button prompts), the Classic Controller
// on Wii Remote 1 with DUALIZE_PAD_TARGET=classic, or nothing with =none.
// Never two at once, so a game reading both doesn't see each press twice.
enum class PadTarget
{
  Gc,
  Classic,
  None,
};

PadTarget Target()
{
  static const PadTarget target = [] {
    const char* v = std::getenv("DUALIZE_PAD_TARGET");
    const std::string_view name = v ? v : "";
    return name == "classic" ? PadTarget::Classic : name == "none" ? PadTarget::None : PadTarget::Gc;
  }();
  return target;
}

bool PadDrivesGc()
{
  return Target() == PadTarget::Gc;
}

bool PadDrivesClassic()
{
  return Target() == PadTarget::Classic;
}

// Positional: the bottom face button is the Classic Controller's B.
constexpr std::pair<uint32_t, uint16_t> CLASSIC_MAP[] = {
    {hid::PAD_EAST, ios::CLASSIC_A},          {hid::PAD_SOUTH, ios::CLASSIC_B},
    {hid::PAD_NORTH, ios::CLASSIC_X},         {hid::PAD_WEST, ios::CLASSIC_Y},
    {hid::PAD_LEFT_SHOULDER, ios::CLASSIC_ZL}, {hid::PAD_RIGHT_SHOULDER, ios::CLASSIC_ZR},
    {hid::PAD_START, ios::CLASSIC_PLUS},      {hid::PAD_BACK, ios::CLASSIC_MINUS},
    {hid::PAD_GUIDE, ios::CLASSIC_HOME},      {hid::PAD_DPAD_UP, ios::CLASSIC_UP},
    {hid::PAD_DPAD_DOWN, ios::CLASSIC_DOWN},  {hid::PAD_DPAD_LEFT, ios::CLASSIC_LEFT},
    {hid::PAD_DPAD_RIGHT, ios::CLASSIC_RIGHT},
};
// Above this an analog trigger also reports its digital click.
constexpr float TRIGGER_CLICK = 0.9f;

// --- native GX host (render thread) ---
bool GxResolve(void* user, uint32_t address, uint32_t size, const void** data, uint32_t* available)
{
  const Memory& memory = MachineOf(user).memory;
  const uint32_t physical = address & 0x1FFFFFFF;
  uint32_t limit;
  if (physical < Memory::MEM1_SIZE)
    limit = Memory::MEM1_SIZE - physical;
  else if (physical - Memory::MEM2_BASE < Memory::MEM2_SIZE)
    limit = Memory::MEM2_SIZE - (physical - Memory::MEM2_BASE);
  else
    return false;
  if (size > limit)
    return false;
  *data = memory.Physical(physical, 1);
  *available = limit;
  return true;
}
void GxDrawDone(void* user)
{
  MachineOf(user).gfx.PostDrawDone();
}
void GxToken(void* user, uint16_t token, bool interrupt)
{
  MachineOf(user).gfx.PostToken(token, interrupt);
}
void GxQuit(void* user)
{
  MachineOf(user).RequestStop();
}

// The aspect the console shows games at: SYSCONF's IPL.AR (a byte, 1 for
// 16:9). Games render the same framebuffer either way, squeezed for 16:9.
float DisplayAspect(ios::Nand& nand)
{
  constexpr float WIDE = 16.0f / 9.0f, STANDARD = 4.0f / 3.0f;
  std::vector<uint8_t> sysconf;
  if (nand.ReadFile("/shared2/sys/SYSCONF", &sysconf) != ios::IPC_SUCCESS || sysconf.size() < 6)
    return WIDE;
  const auto be16 = [&](size_t at) { return static_cast<uint16_t>(sysconf[at] << 8 | sysconf[at + 1]); };
  const uint16_t entries = be16(4);
  for (uint16_t e = 0; e < entries && 6 + 2 * e + 2 <= sysconf.size(); ++e)
  {
    const uint16_t offset = be16(6 + 2 * e);
    // A byte (type 3) named IPL.AR.
    if (offset + 1 + 6 + 1 > sysconf.size() || sysconf[offset] != (3 << 5 | 5) ||
        std::memcmp(&sysconf[offset + 1], "IPL.AR", 6) != 0)
      continue;
    return sysconf[offset + 7] != 0 ? WIDE : STANDARD;
  }
  return WIDE;
}
}  // namespace

Machine::Machine(MachineConfig config_)
    : config(std::move(config_)), cpu(*this, memory, scheduler), pi(*this), vi(*this), exi(*this), ai(*this),
      gfx(*this), hollywood(*this), ios(*this), wud(*this)
{
  m_dsp_event = scheduler.Register(DspEvent, this);
  m_dsp_interrupt_event = scheduler.Register(DspInterruptEvent, this);
  m_aram_event = scheduler.Register(AramEvent, this);
  m_audio_dma_event = scheduler.Register(AudioDmaEvent, this);
  m_dsp_mix_event = scheduler.Register(DspMixEvent, this);
}

Machine::~Machine()
{
  // Audio and gamepads first: the renderer's shutdown ends with SDL_Quit,
  // which frees their streams and devices under them.
  m_gamepads.Stop();
  m_audio.Stop();
  if (m_gx_started)
    aurora_fifo_stop();
  dsp.reset();
}

bool Machine::Start(std::string* error)
{
  if (!memory.flat)
  {
    *error = "cannot reserve the guest address space";
    return false;
  }
  disc = ios::OpenDirectoryDisc(config.game, error);
  if (!disc)
    return false;
  if (!cpu.AttachModule(config.idle_pc, error))
    return false;

  m_boot_time = LocalTimeNow();
  ios::Config ios_config;
  ios_config.nand_root = config.user / "Wii";
  ios_config.disc = disc;
  // Wii Remote 1, with a Classic Controller.
  const ios::WiimoteHost wiimotes{
      .user = this,
      .slots = 1,
      .read = ReadWiimote,
      .rumble = [](void* user, int index, bool on) {
        if (PadDrivesClassic())
          MachineOf(user).m_gamepads.SetRumble(index, on ? 1.0f : 0.0f);
      },
      .speaker = SpeakerEnabled() ? WiimoteSpeaker : nullptr,
  };
  ios.Start(ios_config);
  ios::EnsureSysconf(ios.Nand());
  ios::PairWiimotes(ios.Nand());
  wud.Start(wiimotes, ios_config.nand_root);
  exi.LoadSram((config.user / "GC" / "SRAM.raw").string().c_str());

  StartDsp();
  m_gamepads.Start();

  const std::filesystem::path cache = config.user / "Cache" / "NativeGX";
  std::error_code ec;
  std::filesystem::create_directories(cache, ec);
  const std::string cache_path = cache.string();
  const std::string texture_path = (config.user / "Load" / "Textures").string();
  const AuroraFifoConfig gx_config{
      .title = "NiGHTS: Journey of Dreams",
      .cache_path = cache_path.c_str(),
      .texture_path = texture_path.c_str(),
      .width = config.width,
      .height = config.height,
      .vsync = config.vsync,
      .interpolate = config.interpolate && config.throttle,
      .present_rate = config.present_rate,
      .aspect = DisplayAspect(ios.Nand()),
      .msaa = config.msaa,
      .render_scale = config.render_scale,
      // Writes each texture no pack replaces to <cache>/texture_dumps, for making packs.
      .dump_textures = std::getenv("DUALIZE_DUMP_TEXTURES") != nullptr,
      // One line per second of render-thread counters.
      .stats = std::getenv("DUALIZE_GX_STATS") != nullptr,
  };
  const AuroraFifoHost gx_host{
      .user = this,
      .resolve = GxResolve,
      .draw_done = GxDrawDone,
      .token = GxToken,
      .quit = GxQuit,
      .thread_started = [](void*) { profiler::RegisterThisThread("gx"); },
  };
  if (!aurora_fifo_start(&gx_config, &gx_host))
  {
    *error = "the native GX renderer could not start";
    return false;
  }
  m_gx_started = true;

  // The timebase counts from the console's RTC at power-on.
  cpu.SetTimebase(uint64_t{Scheduler::CPU_CLOCK / Scheduler::TIMER_RATIO} *
                  static_cast<uint64_t>(m_boot_time - GC_EPOCH));
  if (!BootWiiDisc(*this, error))
    return false;
  scheduler.Schedule(0, m_dsp_event);
  scheduler.Schedule(Scheduler::CPU_CLOCK * int64_t{ai.DmaDivisor()} / 13500000, m_audio_dma_event);
  return true;
}

void Machine::StartDsp()
{
  apu::DspHost host;
  host.user = this;
  host.ram = memory.mem1;
  host.ram_size = Memory::MEM1_SIZE;
  host.exram = memory.mem2;
  host.exram_size = Memory::MEM2_SIZE;
  host.set_interrupt = [](void* user, bool asserted) { MachineOf(user).pi.SetInterrupt(PI_INT_DSP, asserted); };
  host.schedule_interrupt = [](void* user, int64_t cycles, uint32_t type) {
    Machine& m = MachineOf(user);
    m.scheduler.Schedule(cycles, m.m_dsp_interrupt_event, type);
  };
  host.schedule_aram_done = [](void* user, int64_t cycles) {
    Machine& m = MachineOf(user);
    m.scheduler.Schedule(cycles, m.m_aram_event);
  };
  host.schedule_mix_done = [](void* user, int64_t cycles) {
    Machine& m = MachineOf(user);
    m.scheduler.Schedule(cycles, m.m_dsp_mix_event);
  };
  host.timebase = [](void* user) { return MachineOf(user).cpu.Timebase(); };
  host.force_exception_check = [](void* user, int64_t cycles) { MachineOf(user).scheduler.ForceCheck(cycles); };
  host.push_samples = [](void* user, const int16_t* samples, uint32_t count) {
    Machine& m = MachineOf(user);
    m.m_audio.Push(samples, count, m.ai.DmaDivisor());
  };
  m_audio.Start();
  dsp = std::make_unique<apu::DspCore>(
      host, std::vector<std::string>{(config.user / "GC" / "dsp_coef.bin").string()});
}

void Machine::DspEvent(void* user, uint64_t, int64_t late)
{
  Machine& m = MachineOf(user);
  m.dsp->Update();
  m.scheduler.Schedule(DSP_UPDATE_PERIOD - late, m.m_dsp_event);
}
void Machine::DspInterruptEvent(void* user, uint64_t type, int64_t)
{
  MachineOf(user).dsp->RaiseInterrupt(static_cast<uint32_t>(type));
}
void Machine::AramEvent(void* user, uint64_t, int64_t)
{
  MachineOf(user).dsp->CompleteAramDma();
}
void Machine::DspMixEvent(void* user, uint64_t, int64_t)
{
  MachineOf(user).dsp->CompleteMix();
}
void Machine::AudioDmaEvent(void* user, uint64_t, int64_t late)
{
  Machine& m = MachineOf(user);
  m.dsp->UpdateAudioDma();
  // One 32-byte step: 8 stereo frames at 108 MHz / divisor (32 kHz: 250 us).
  const int64_t period = Scheduler::CPU_CLOCK * int64_t{m.ai.DmaDivisor()} / 13500000;
  m.scheduler.Schedule(period - late, m.m_audio_dma_event);
}

bool Machine::SpeakerEnabled()
{
  // DUALIZE_WIIMOTE_SPEAKER=0 silences the remotes' speakers.
  const char* v = std::getenv("DUALIZE_WIIMOTE_SPEAKER");
  return !(v && *v == '0');
}

void Machine::SetAutomationPad(int port, const AutomationPad* pad)
{
  if (port < 0 || port >= AUTOMATION_PORTS)
    return;
  std::lock_guard lock(m_automation_mutex);
  m_automation_pads[port] = pad ? *pad : AutomationPad{};
  m_automation_active[port].store(pad != nullptr, std::memory_order_release);
}

bool Machine::ReadAutomationPad(int port, AutomationPad* pad)
{
  if (port < 0 || port >= AUTOMATION_PORTS ||
      !m_automation_active[port].load(std::memory_order_acquire))
    return false;
  std::lock_guard lock(m_automation_mutex);
  *pad = m_automation_pads[port];
  return true;
}

namespace
{
// The larger deflection wins, so automation and a held pad both register.
float Merge(float a, float b)
{
  return std::fabs(b) > std::fabs(a) ? b : a;
}
}  // namespace

void Machine::ReadWiimote(void* user, int index, ios::WiimoteInput* input)
{
  Machine& m = MachineOf(user);
  input->classic = true;
  hid::PadState pad;
  if (PadDrivesClassic() && m.m_gamepads.Read(index, &pad))
  {
    for (const auto& [from, to] : CLASSIC_MAP)
    {
      if (pad.buttons & from)
        input->classic_buttons |= to;
    }
    if (pad.left_trigger >= TRIGGER_CLICK)
      input->classic_buttons |= ios::CLASSIC_L;
    if (pad.right_trigger >= TRIGGER_CLICK)
      input->classic_buttons |= ios::CLASSIC_R;
    input->left_x = pad.left_x;
    input->left_y = pad.left_y;
    input->right_x = pad.right_x;
    input->right_y = pad.right_y;
    input->left_trigger = pad.left_trigger;
    input->right_trigger = pad.right_trigger;
  }
  AutomationPad automation;
  if (m.ReadAutomationPad(index, &automation))
  {
    const ios::WiimoteInput& a = automation.wii;
    input->buttons |= a.buttons;
    input->classic_buttons |= a.classic_buttons;
    input->left_x = Merge(input->left_x, a.left_x);
    input->left_y = Merge(input->left_y, a.left_y);
    input->right_x = Merge(input->right_x, a.right_x);
    input->right_y = Merge(input->right_y, a.right_y);
    input->left_trigger = std::max(input->left_trigger, a.left_trigger);
    input->right_trigger = std::max(input->right_trigger, a.right_trigger);
  }
}

void Machine::WiimoteSpeaker(void* user, int index, const int16_t* samples, uint32_t count,
                             uint32_t rate, uint8_t left_volume, uint8_t right_volume)
{
  MachineOf(user).m_audio.PushSpeaker(index, samples, count, rate, left_volume, right_volume);
}

bool Machine::ReadGcPad(int port, GcPadInput* input)
{
  hid::PadState pad;
  const bool plugged = PadDrivesGc() && m_gamepads.Read(port, &pad);
  AutomationPad automation;
  const bool automated = ReadAutomationPad(port, &automation) && automation.gc;
  if (!automated)
    automation = {};
  // Unplugged as long as nothing drives the port.
  if (!plugged && !automated)
    return false;
  if (!plugged)
    pad = {};
  const auto axis = [](float v) { return static_cast<uint8_t>(std::clamp(0x80 + std::lround(v * 127.0f), 0L, 255L)); };
  const auto trigger = [](float v) { return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L)); };
  uint16_t buttons = automation.gc_buttons;
  constexpr std::pair<uint32_t, uint16_t> map[] = {
      {hid::PAD_SOUTH, 0x0100}, {hid::PAD_WEST, 0x0200},  {hid::PAD_EAST, 0x0400},
      {hid::PAD_NORTH, 0x0800}, {hid::PAD_START, 0x1000}, {hid::PAD_RIGHT_SHOULDER, 0x0010},
      {hid::PAD_DPAD_UP, 0x0008}, {hid::PAD_DPAD_DOWN, 0x0004}, {hid::PAD_DPAD_LEFT, 0x0001},
      {hid::PAD_DPAD_RIGHT, 0x0002},
  };
  for (const auto& [from, to] : map)
  {
    if (pad.buttons & from)
      buttons |= to;
  }
  const float left = std::max(pad.left_trigger, automation.gc_left);
  const float right = std::max(pad.right_trigger, automation.gc_right);
  if (left >= TRIGGER_CLICK)
    buttons |= 0x0040;
  if (right >= TRIGGER_CLICK)
    buttons |= 0x0020;
  *input = {buttons,
            axis(Merge(pad.left_x, automation.gc_main_x)),
            axis(Merge(pad.left_y, automation.gc_main_y)),
            axis(Merge(pad.right_x, automation.gc_c_x)),
            axis(Merge(pad.right_y, automation.gc_c_y)),
            trigger(left),
            trigger(right),
            0,
            0};
  return true;
}

void Machine::SetGcRumble(int port, bool on)
{
  if (PadDrivesGc())
    m_gamepads.SetRumble(port, on ? 1.0f : 0.0f);
}

// --- MMIO ---------------------------------------------------------------------

uint32_t Machine::MmioRead(uint32_t physical, uint32_t size, uint32_t pc)
{
  const auto read16 = [&](auto&& reg16) -> uint32_t {
    const uint32_t aligned = physical & ~1u;
    if (size == 4)
      return reg16(aligned) << 16 | reg16(aligned + 2);
    const uint32_t value = reg16(aligned);
    return size == 2 ? value : (physical & 1 ? value & 0xFF : value >> 8);
  };
  const auto read32 = [&](auto&& reg32) -> uint32_t {
    const uint32_t value = reg32(physical & ~3u);
    if (size == 4)
      return value;
    const uint32_t shift = (4 - size - (physical & 3)) * 8;
    return (value >> shift) & (size == 2 ? 0xFFFF : 0xFF);
  };
  switch (physical >> 12)
  {
  case 0x0C000:
    return read16([&](uint32_t a) { return uint32_t{gfx.ReadCp(a & 0xFFF)}; });
  case 0x0C001:
    return read16([&](uint32_t a) { return uint32_t{gfx.ReadPe(a & 0xFFF)}; });
  case 0x0C002:
    // The VI lives at the SDK (sdk/rvl_vi.cpp); what of it still touches the
    // registers (VIInit, OSFatal's video setup) reads them idle.
    return 0;
  case 0x0C003:
    return read32([&](uint32_t a) { return pi.Read32(a & 0xFFF); });
  case 0x0C004:
    return read16([&](uint32_t a) { return uint32_t{gfx.ReadMi(a & 0xFFF)}; });
  case 0x0C005:
    return read16([&](uint32_t a) { return uint32_t{dsp->Read(a & 0xFFFF)}; });
  case 0x0C006:
  case 0x0D006:
  {
    const uint32_t offset = physical & 0xFFF;
    if (offset >= 0xC00)
      return read32([&](uint32_t a) { return ai.Read32(a & 0x3FF); });
    // The SI and EXI live at the SDK (sdk/rvl_pad.cpp, sdk/rvl_exi.cpp);
    // what of the SDK still touches their registers reads them idle.
    if (offset >= 0x400)
      return 0;
    // DI: only the config register is the PPC's; IOS drives the drive.
    return read32([&](uint32_t a) { return (a & 0x3FF) == 0x24 ? 1u : 0u; });
  }
  case 0x0D000:
  case 0x0D800:
    return read32([&](uint32_t a) { return hollywood.Read32(a & 0x3FF); });
  default:
    std::fprintf(stderr, "[native] unmapped read%u %08x at %08x\n", size * 8, physical, pc);
    return 0;
  }
}

void Machine::MmioWrite(uint32_t physical, uint32_t value, uint32_t size, uint32_t pc)
{
  const auto write16 = [&](auto&& reg16) {
    const uint32_t aligned = physical & ~1u;
    if (size == 4)
    {
      reg16(aligned, static_cast<uint16_t>(value >> 16));
      reg16(aligned + 2, static_cast<uint16_t>(value));
    }
    else
    {
      // A byte store reaches the register as the halfword it lands in.
      reg16(aligned, static_cast<uint16_t>(size == 2 ? value : value << (physical & 1 ? 0 : 8)));
    }
  };
  const auto write32 = [&](auto&& reg32) {
    if (size == 4)
    {
      reg32(physical, value);
      return;
    }
    const uint32_t shift = (4 - size - (physical & 3)) * 8;
    reg32(physical & ~3u, value << shift);
  };
  switch (physical >> 12)
  {
  case 0x0C000:
    return write16([&](uint32_t a, uint16_t v) { gfx.WriteCp(a & 0xFFF, v); });
  case 0x0C001:
    return write16([&](uint32_t a, uint16_t v) { gfx.WritePe(a & 0xFFF, v); });
  case 0x0C002:
    return;  // the VI lives at the SDK
  case 0x0C003:
    return write32([&](uint32_t a, uint32_t v) { pi.Write32(a & 0xFFF, v); });
  case 0x0C004:
    return write16([&](uint32_t a, uint16_t v) { gfx.WriteMi(a & 0xFFF, v); });
  case 0x0C005:
    return write16([&](uint32_t a, uint16_t v) { dsp->Write(a & 0xFFFF, v); });
  case 0x0C006:
  case 0x0D006:
  {
    const uint32_t offset = physical & 0xFFF;
    if (offset >= 0xC00)
      return write32([&](uint32_t a, uint32_t v) { ai.Write32(a & 0x3FF, v); });
    return;  // DI registers belong to IOS; SI and EXI live at the SDK
  }
  case 0x0D000:
  case 0x0D800:
    return write32([&](uint32_t a, uint32_t v) { hollywood.Write32(a & 0x3FF, v); });
  default:
    std::fprintf(stderr, "[native] unmapped write%u %08x = %08x at %08x\n", size * 8, physical,
                 value, pc);
  }
}

void Machine::GatherPipeBursts(const uint8_t* data, size_t size)
{
  m_gx_bytes += size;
  aurora_fifo_submit(data, size);
}

// --- Run loop -----------------------------------------------------------------

void Machine::AfterEvents()
{
  gfx.Poll();
  if (m_requests.load(std::memory_order_relaxed))
    ServiceRequests();
}

bool Machine::AfterEventsInPlace()
{
  gfx.Poll();
  return !m_requests.load(std::memory_order_relaxed);
}

void Machine::BeforeIdle()
{
  // The idle loop waits on what the GPU finishes; let it finish first.
  aurora_fifo_drain();
  gfx.Poll();
}

void Machine::Run()
{
  m_throttle_host = m_report_host = Clock::now();
  m_throttle_ticks = m_report_ticks = scheduler.Now();
  cpu.Run();
}

void Machine::RequestStop()
{
  m_stop = true;
  m_requests = true;
}

void Machine::RequestSave(std::filesystem::path path)
{
  std::lock_guard lock(m_request_mutex);
  m_save_path = std::move(path);
  m_requests = true;
}

void Machine::RequestLoad(std::filesystem::path path)
{
  std::lock_guard lock(m_request_mutex);
  m_load_path = std::move(path);
  m_requests = true;
}

void Machine::ServiceRequests()
{
  m_requests = false;
  if (m_stop)
  {
    cpu.Stop();
    return;
  }
  std::filesystem::path save, load;
  {
    std::lock_guard lock(m_request_mutex);
    save = std::exchange(m_save_path, {});
    load = std::exchange(m_load_path, {});
  }
  std::string error;
  if (!save.empty())
  {
    if (SaveState(save, &error))
      std::fprintf(stderr, "[native] saved %s\n", save.string().c_str());
    else
      std::fprintf(stderr, "[native] save failed: %s\n", error.c_str());
  }
  if (!load.empty())
  {
    if (LoadState(load, &error))
      std::fprintf(stderr, "[native] loaded %s\n", load.string().c_str());
    else
      std::fprintf(stderr, "[native] load failed: %s\n", error.c_str());
  }
}

void Machine::Throttle(uint64_t ticks)
{
  if (!config.throttle)
    return;
  const Clock::time_point now = Clock::now();
  const auto guest = std::chrono::nanoseconds(
      static_cast<int64_t>((ticks - m_throttle_ticks) * 1e9 / Scheduler::CPU_CLOCK));
  const Clock::time_point target = m_throttle_host + guest;
  if (target > now)
  {
    SleepUntil(target);
  }
  else if (now - target > std::chrono::milliseconds(100))
  {
    // Too far behind to catch up: run on from here at full speed.
    m_throttle_host = now;
    m_throttle_ticks = ticks;
  }
}

void Machine::OnFieldEnd(uint64_t ticks)
{
  m_fields.store(vi.Fields(), std::memory_order_relaxed);
  const Clock::time_point now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - m_report_host).count();
  if (seconds < 2.0)
    return;
  const uint64_t fields = vi.Fields();
  const double guest = static_cast<double>(ticks - m_report_ticks) / Scheduler::CPU_CLOCK;
  const Cpu::Stats& stats = cpu.GetStats();
  std::fprintf(stderr,
               "[native] %.1f VPS, speed %.0f%%, GX %.1f KB/s, pc %08x, dispatches %llu, "
               "safepoints %llu, interpreted %llu\n",
               (fields - m_report_fields) / seconds, 100.0 * guest / seconds,
               (m_gx_bytes - m_report_gx_bytes) / seconds / 1024.0, cpu.state.pc,
               static_cast<unsigned long long>(stats.dispatches - m_report_stats.dispatches),
               static_cast<unsigned long long>(stats.safepoints - m_report_stats.safepoints),
               static_cast<unsigned long long>(stats.interpreted - m_report_stats.interpreted));
  m_report_stats = stats;
  m_report_gx_bytes = m_gx_bytes;
  m_report_host = now;
  m_report_ticks = ticks;
  m_report_fields = fields;
}

int64_t Machine::EmulatedTime() const
{
  return m_boot_time + static_cast<int64_t>(scheduler.Now() / Scheduler::CPU_CLOCK);
}

// --- Savestates -----------------------------------------------------------------

void Machine::DoState(StateIO& io, uint32_t version)
{
  io.Do(m_boot_time);
  io.Bytes(memory.mem1, Memory::MEM1_SIZE);
  io.Bytes(memory.mem2, Memory::MEM2_SIZE);
  io.Bytes(memory.l1, Memory::L1_SIZE);
  io.Marker(0x3E3E0001);
  scheduler.DoState(io);
  cpu.DoState(io);
  pi.DoState(io);
  if (version <= STATE_VERSION_REGISTER_VI)
    vi.DoLegacyState(io);
  else
    vi.DoState(io);
  if (version == STATE_VERSION_REGISTER_SI_EXI)
  {
    exi.DoLegacyState(io);
    pi.SetInterrupt(PI_INT_SI | PI_INT_EXI, false);
  }
  else
  {
    exi.DoState(io);
  }
  ai.DoState(io);
  gfx.DoState(io);
  hollywood.DoState(io);
  io.Marker(0x3E3E0002);
  ios.DoState(io);
  if (uint32_t reply; io.Reading() && hollywood.TakeMailboxReply(&reply))
    ios.AdoptMailboxReply(reply);
  const apu::DspStateIO dsp_io{
      .user = &io,
      .bytes = [](void* user, void* data, uint32_t size) { static_cast<StateIO*>(user)->Bytes(data, size); },
      .reading = io.Reading(),
  };
  dsp->DoState(dsp_io);
  io.Marker(0x3E3E0003);
}

bool Machine::SaveState(const std::filesystem::path& path, std::string* error)
{
  aurora_fifo_drain();
  gfx.Poll();
  std::vector<uint8_t> buffer;
  buffer.reserve(Memory::MEM1_SIZE + Memory::MEM2_SIZE + (1 << 20));
  StateIO io(buffer, false);
  uint32_t magic = STATE_MAGIC, version = STATE_VERSION;
  io.Do(magic);
  io.Do(version);
  DoState(io, version);
  size_t gx_size = 0;
  const uint8_t* gx = aurora_fifo_save_state(&gx_size);
  std::vector<uint8_t> gx_state(gx, gx + gx_size);
  io.Do(gx_state);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
  if (!file)
  {
    *error = "can't write " + path.string();
    return false;
  }
  return true;
}

bool Machine::LoadState(const std::filesystem::path& path, std::string* error)
{
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  StateIO io(buffer, true);
  uint32_t magic = 0, version = 0;
  io.Do(magic);
  io.Do(version);
  if (magic != STATE_MAGIC || version < STATE_VERSION_REGISTER_SI_EXI || version > STATE_VERSION)
  {
    *error = path.string() + " is not a native state";
    return false;
  }
  aurora_fifo_drain();
  DoState(io, version);
  std::vector<uint8_t> gx_state;
  io.Do(gx_state);
  if (!io.Ok())
  {
    // The machine is half overwritten; nothing sane can continue.
    *error = path.string() + " is corrupt";
    RequestStop();
    return false;
  }
  aurora_fifo_load_state(gx_state.data(), gx_state.size());
  wud.AfterLoad(cpu);
  // The guest clock and field count jumped; report and throttle from here.
  m_throttle_host = m_report_host = Clock::now();
  m_throttle_ticks = m_report_ticks = scheduler.Now();
  m_report_fields = vi.Fields();
  return true;
}
}  // namespace dualize

// The CPU's host interface (cpu/cpu.hpp), bound at link time.
uint32_t dualize::CpuHost::MmioRead(uint32_t physical, uint32_t size, uint32_t pc)
{
  return static_cast<dualize::Machine*>(this)->MmioRead(physical, size, pc);
}

void dualize::CpuHost::MmioWrite(uint32_t physical, uint32_t value, uint32_t size, uint32_t pc)
{
  static_cast<dualize::Machine*>(this)->MmioWrite(physical, value, size, pc);
}

void dualize::CpuHost::GatherPipeBursts(const uint8_t* data, size_t size)
{
  static_cast<dualize::Machine*>(this)->GatherPipeBursts(data, size);
}

void dualize::CpuHost::AfterEvents()
{
  static_cast<dualize::Machine*>(this)->AfterEvents();
}

bool dualize::CpuHost::AfterEventsInPlace()
{
  return static_cast<dualize::Machine*>(this)->AfterEventsInPlace();
}

void dualize::CpuHost::BeforeIdle()
{
  static_cast<dualize::Machine*>(this)->BeforeIdle();
}
