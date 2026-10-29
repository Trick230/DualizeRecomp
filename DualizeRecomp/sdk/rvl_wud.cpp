// WUD at the SDK boundary (sdk/wud.hpp): the remotes' side of WUD in host
// code, and natives for the few places WPAD and WUD reach into the Bluetooth
// stack. WPAD itself (its command queue, handshakes, calibration, decoding
// and callbacks) stays the game's own code.
//
// The natives are found by their SDK code (config/sdk.sigs); the addresses
// and offsets below are NiGHTS' (defines, were it shared).

#include "sdk/wud.hpp"

#include "core/cpu.h"

#include "dolrecomp.hpp"
#include "machine.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dualize
{
namespace
{
// WUD's state (one block of its .bss).
constexpr uint32_t WUD_BASE = 0x806F1000;
constexpr uint32_t WUD_SIZE = 0x750;
constexpr uint32_t WUD_CALLBACKS = 0;        // three words, cleared by WUDInit
constexpr uint32_t WUD_STARTED = 16;         // byte
constexpr uint32_t WUD_LINKS = 1765;         // byte: connected devices
constexpr uint32_t WUD_RECV_CALLBACK = 1772;  // void (*)(u8 handle, u8* report)
constexpr uint32_t WUD_CONN_CALLBACK = 1776;  // void (*)(u8 handle, BOOL open)
constexpr uint32_t WUD_STATUS = 1800;        // byte
constexpr uint32_t WUD_BUFFER_HEAD = 1860, WUD_BUFFER_TAIL = 1862;
// Each handle's device address, as WUDGetDevAddr returns it; the addresses
// themselves live in WUD's device records.
constexpr uint32_t WUD_DEVICE_ADDRESSES = 0x806F1D48;
constexpr uint32_t WUD_DEVICE_RECORDS = 0x806F1124;
constexpr uint32_t WUD_DEVICE_RECORD_SIZE = 16;
constexpr int32_t WUD_INITIALIZED_SDA_OFFSET = -26048;  // word

// WUDGetStatus: 2 while WPAD hasn't registered its callbacks, 3 running.
constexpr uint8_t WUD_STATE_INITIALIZED = 2;
constexpr uint8_t WUD_STATE_RUNNING = 3;
constexpr uint8_t WUD_STATE_OFF = 0;

constexpr uint32_t SC_INIT = 0x80484564;
constexpr uint32_t OS_CANCEL_ALARM = 0x8044DF58;
constexpr int32_t OS_ALARM_QUEUE_SDA_OFFSET = -27664;  // head, tail
constexpr uint32_t OS_ALARM_NEXT = 20;
constexpr uint32_t OS_SWITCH_FIBER_EX = 0x8044F688;

// WPAD
constexpr uint32_t WPAD_CONTROL_BLOCKS = 0x806ED890;  // WPADCB* per channel
constexpr uint32_t WPAD_CONTROL_SIZE = 2464;
constexpr uint32_t WPAD_COMMAND_SIZE = 48;
constexpr uint32_t WPAD_MANAGER = 0x804BB08C;       // runs on WPAD's fiber
constexpr uint32_t WPAD_FIBER_STACK = 0x806EE8A0;   // its top
constexpr int32_t WPAD_RUMBLE_SDA_OFFSET = -26092;  // word

// The Bluetooth stack's code, whose alarms states from before this file
// may still hold: BTE and its USB transport, and WUD.
bool InBluetoothStack(uint32_t address)
{
  return (address >= 0x80487BD0 && address < 0x804B5A54) ||
         (address >= 0x804B8C30 && address < 0x804B9D00) ||
         (address >= 0x804C6E90 && address < 0x804CB79C);
}

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

uint8_t* Bytes(const Memory& memory, uint32_t address, uint32_t size)
{
  return memory.Effective(address, size);
}

// A remote's address as WUD keeps it: the Bluetooth address bytes reversed.
// The remotes are 11:02:19:79:00:<slot>, as SYSCONF pairs them.
std::array<uint8_t, 6> DeviceAddress(int handle)
{
  return {static_cast<uint8_t>(handle), 0x00, 0x79, 0x19, 0x02, 0x11};
}

// OSGetSystemTime
uint64_t SystemTime(const Cpu& cpu, const Memory& memory)
{
  const uint8_t* offset = Bytes(memory, 0x800030D8, 8);
  return cpu.Timebase() + (offset ? Load64(offset) : 0);
}
}  // namespace

// --- Wud ------------------------------------------------------------------------

Wud::Wud(Machine& machine) : m_machine(machine) {}

Wud::~Wud() = default;

void Wud::Start(const ios::WiimoteHost& host, const std::filesystem::path& eeprom_dir)
{
  m_host = host;
  for (int i = 0; i < REMOTES; ++i)
  {
    Remote& remote = m_remotes[i];
    remote = {};
    if (!(m_host.slots & (1u << i)))
      continue;
    remote.hid = std::make_unique<ios::Wiimote>(i, m_host, eeprom_dir, remote);
    remote.requesting = true;  // a remote that is on asks to connect
  }
  m_last_update = 0;
}

void Wud::Remote::SendInputReport(const uint8_t* report, uint32_t size)
{
  std::array<uint8_t, 23> entry{};
  entry[0] = static_cast<uint8_t>(std::min<uint32_t>(size, entry.size() - 1));
  std::memcpy(entry.data() + 1, report, entry[0]);
  reports.push_back(entry);
}

void Wud::Pump(Cpu& cpu)
{
  uint8_t* wud = Bytes(m_machine.memory, WUD_BASE, WUD_SIZE);
  if (!wud)
    return;
  // WPAD registers its callbacks on its first tick after WUDInit.
  if (wud[WUD_STATUS] == WUD_STATE_INITIALIZED && Load32(wud + WUD_RECV_CALLBACK) &&
      Load32(wud + WUD_CONN_CALLBACK))
    wud[WUD_STATUS] = WUD_STATE_RUNNING;
  if (wud[WUD_STATUS] != WUD_STATE_RUNNING)
    return;

  const uint64_t now = m_machine.scheduler.Now();
  if (now - m_last_update >= Scheduler::CPU_CLOCK / ios::Wiimote::UPDATE_FREQ)
  {
    m_last_update = now;
    UpdateRemotes();
  }
  for (int handle = 0; handle < REMOTES; ++handle)
  {
    Remote& remote = m_remotes[handle];
    if (remote.closing)
      Close(cpu, handle);
    else if (remote.hid && !remote.connected && remote.reconnect_wait == 0 && remote.requesting)
      Connect(cpu, handle);
    Deliver(cpu, handle);
  }
}

void Wud::UpdateRemotes()
{
  for (Remote& remote : m_remotes)
  {
    if (remote.reconnect_wait)
      --remote.reconnect_wait;
    if (!remote.hid)
      continue;
    remote.hid->PrepareInput();
    if (remote.connected)
      remote.hid->Update();
    // A button press wakes a dropped remote.
    else if (!remote.reconnect_wait && remote.hid->AnyButtonPressed())
      remote.requesting = true;
  }
}

void Wud::Connect(Cpu& cpu, int handle)
{
  const Memory& memory = m_machine.memory;
  const uint32_t record = WUD_DEVICE_RECORDS + handle * WUD_DEVICE_RECORD_SIZE;
  const std::array<uint8_t, 6> address = DeviceAddress(handle);
  uint8_t* bytes = Bytes(memory, record, address.size());
  uint8_t* pointer = Bytes(memory, WUD_DEVICE_ADDRESSES + handle * 4, 4);
  const uint8_t* wud = Bytes(memory, WUD_BASE, WUD_SIZE);
  if (!bytes || !pointer || !wud)
    return;
  std::memcpy(bytes, address.data(), address.size());
  Store32(pointer, record);

  Remote& remote = m_remotes[handle];
  remote.connected = true;
  remote.requesting = false;
  remote.reports.clear();
  remote.hid->Reset();
  // The remote's first report goes out as its HID channel opens, before WPAD
  // starts on it: with an extension plugged in, the status report that says so.
  remote.hid->PrepareInput();
  remote.hid->Update();
  WriteLinkCount();
  cpu.CallGuest(Load32(wud + WUD_CONN_CALLBACK), {static_cast<uint32_t>(handle), 1});
}

void Wud::Close(Cpu& cpu, int handle)
{
  Remote& remote = m_remotes[handle];
  remote.closing = false;
  if (!remote.connected)
    return;
  remote.connected = false;
  remote.reports.clear();
  remote.reconnect_wait = ios::Wiimote::UPDATE_FREQ;
  remote.hid->Reset();
  WriteLinkCount();
  if (const uint8_t* wud = Bytes(m_machine.memory, WUD_BASE, WUD_SIZE))
    cpu.CallGuest(Load32(wud + WUD_CONN_CALLBACK), {static_cast<uint32_t>(handle), 0});
}

void Wud::Deliver(Cpu& cpu, int handle)
{
  Remote& remote = m_remotes[handle];
  const uint8_t* wud = Bytes(m_machine.memory, WUD_BASE, WUD_SIZE);
  if (!remote.connected || !wud)
    return;
  CPUState& s = cpu.state;
  while (!remote.reports.empty())
  {
    const std::array<uint8_t, 23> entry = remote.reports.front();
    remote.reports.pop_front();
    // The report sits in a frame below the interrupted code's stack, as the
    // Bluetooth stack's buffer would.
    const uint32_t sp = s.gpr[1];
    const uint32_t report = (sp - 32) & ~7u;
    uint8_t* bytes = Bytes(m_machine.memory, report, 24);
    if (!bytes)
      return;
    std::memcpy(bytes, entry.data() + 1, entry[0]);
    s.gpr[1] = report - 16;
    cpu.CallGuest(Load32(wud + WUD_RECV_CALLBACK), {static_cast<uint32_t>(handle), report});
    s.gpr[1] = sp;
  }
}

void Wud::Send(uint8_t handle, const uint8_t* report, uint32_t size)
{
  if (handle < REMOTES && m_remotes[handle].connected)
    m_remotes[handle].hid->OutputReport(report, size);
}

void Wud::Disconnect(const uint8_t* address)
{
  for (int handle = 0; handle < REMOTES; ++handle)
  {
    if (std::memcmp(address, DeviceAddress(handle).data(), 6) == 0 && m_remotes[handle].connected)
      m_remotes[handle].closing = true;
  }
}

void Wud::Shutdown()
{
  for (Remote& remote : m_remotes)
  {
    if (remote.hid && remote.connected)
      remote.hid->Reset();
    remote.connected = remote.closing = remote.requesting = false;
    remote.reports.clear();
  }
  WriteLinkCount();
}

void Wud::WriteLinkCount() const
{
  if (uint8_t* wud = Bytes(m_machine.memory, WUD_BASE, WUD_SIZE))
    wud[WUD_LINKS] = static_cast<uint8_t>(
        std::count_if(m_remotes.begin(), m_remotes.end(), [](const Remote& r) { return r.connected; }));
}

void Wud::DoState(ios::StateIO& io)
{
  // /dev/usb/oh1/57e/305 as it saved itself: the device's name, type and
  // open flag, HCI and ACL endpoints, report clock, completed-packet counts,
  // scan enable, queued HCI events and ACL packets, then five remotes (the
  // fifth the Balance Board's slot): baseband state, HID linking flag,
  // reconnect wait, L2CAP channels and, on a slot with a remote, its state.
  constexpr int LEGACY_REMOTES = 5;
  constexpr uint8_t BASEBAND_INACTIVE = 0, BASEBAND_REQUEST = 1, BASEBAND_COMPLETE = 2;
  struct Endpoint
  {
    uint32_t request = 0, data = 0;
  };
  struct Channel
  {
    uint16_t psm = 0, remote_cid = 0, remote_mtu = 0;
    uint8_t state = 0;
  };
  struct AclPacket
  {
    std::array<uint8_t, 339> data;
    uint16_t size;
    uint16_t handle;
  };

  std::string name = "/dev/usb/oh1/57e/305";
  uint32_t type = 0;
  bool active = false;
  Endpoint hci, acl;
  uint64_t last_ticks = 0;
  std::array<uint32_t, LEGACY_REMOTES> packet_count{};
  uint8_t scan_enable = 0;
  uint32_t events = 0, packets = 0;
  io.Do(name);
  io.Do(type);
  io.Do(active);
  io.Do(hci);
  io.Do(acl);
  io.Do(last_ticks);
  io.Do(packet_count);
  io.Do(scan_enable);
  io.Do(events);
  for (uint32_t i = 0; i < events; ++i)
  {
    uint16_t size = 0;
    std::array<uint8_t, 260> data{};
    io.Do(size);
    io.Bytes(data.data(), std::min<uint32_t>(size, data.size()));
  }
  io.Do(packets);
  for (uint32_t i = 0; i < packets; ++i)
  {
    AclPacket packet{};
    io.Do(packet);
  }
  for (int i = 0; i < LEGACY_REMOTES; ++i)
  {
    Remote* remote = i < REMOTES ? &m_remotes[i] : nullptr;
    uint8_t baseband = BASEBAND_INACTIVE;
    if (remote && remote->connected)
      baseband = BASEBAND_COMPLETE;
    else if (remote && remote->requesting)
      baseband = BASEBAND_REQUEST;
    bool linking = false;
    uint32_t wait = remote ? remote->reconnect_wait : 0;
    uint32_t channels = 0;
    io.Do(baseband);
    io.Do(linking);
    io.Do(wait);
    io.Do(channels);
    for (uint32_t c = 0; c < channels; ++c)
    {
      uint16_t cid = 0;
      Channel channel;
      io.Do(cid);
      io.Do(channel);
    }
    if (!remote)
      continue;
    if (io.Reading())
    {
      remote->connected = baseband == BASEBAND_COMPLETE;
      remote->requesting = baseband == BASEBAND_REQUEST;
      remote->closing = false;
      remote->reconnect_wait = wait;
      remote->reports.clear();
    }
    if (remote->hid)
      remote->hid->DoState(io);
  }
  if (io.Reading())
    m_last_update = 0;
}

void Wud::AfterLoad(Cpu& cpu)
{
  // States from before this file hold the guest's Bluetooth stack mid-run:
  // its alarms would call into an IOS driver that is gone.
  const Memory& memory = m_machine.memory;
  const uint8_t* queue = Bytes(memory, cpu.state.gpr[13] + OS_ALARM_QUEUE_SDA_OFFSET, 4);
  std::vector<uint32_t> stale;
  for (uint32_t alarm = queue ? Load32(queue) : 0; alarm && stale.size() < 64;)
  {
    const uint8_t* bytes = Bytes(memory, alarm, OS_ALARM_NEXT + 4);
    if (!bytes)
      break;
    if (InBluetoothStack(Load32(bytes)))
      stale.push_back(alarm);
    alarm = Load32(bytes + OS_ALARM_NEXT);
  }
  for (uint32_t alarm : stale)
    cpu.CallGuest(OS_CANCEL_ALARM, {alarm});
  WriteLinkCount();
}
}  // namespace dualize

// BOOL WUDInit(void): WUD's state without the Bluetooth stack it would start.
extern "C" bool rvl_WUDInit(CPUState* ctx)
{
  using namespace dualize;
  Cpu& cpu = CpuOf(ctx);
  const Memory& memory = GetMachine(cpu).memory;
  uint8_t* initialized = Bytes(memory, ctx->gpr[13] + WUD_INITIALIZED_SDA_OFFSET, 4);
  uint8_t* wud = Bytes(memory, WUD_BASE, WUD_SIZE);
  if (!initialized || !wud)
    return false;
  if (Load32(initialized))
  {
    ctx->gpr[3] = 0;
    return true;
  }
  for (uint32_t i = 0; i < 3; ++i)
    Store32(wud + WUD_CALLBACKS + i * 4, 0);
  Store32(wud + WUD_RECV_CALLBACK, 0);
  Store32(wud + WUD_CONN_CALLBACK, 0);
  wud[WUD_STARTED] = 1;
  wud[WUD_LINKS] = 0;
  Store16(wud + WUD_BUFFER_HEAD, 0);
  Store16(wud + WUD_BUFFER_TAIL, 0);
  cpu.CallGuest(SC_INIT);
  wud[WUD_STATUS] = WUD_STATE_INITIALIZED;
  Store32(initialized, 1);
  ctx->gpr[3] = 1;
  ctx->downcount -= 40;
  return true;
}

// void WUDShutdown(void): the links drop and WUD is off at once.
extern "C" bool rvl_WUDShutdown(CPUState* ctx)
{
  using namespace dualize;
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  uint8_t* wud = Bytes(machine.memory, WUD_BASE, WUD_SIZE);
  if (!wud)
    return false;
  machine.wud.Shutdown();
  wud[WUD_STATUS] = WUD_STATE_OFF;
  ctx->downcount -= 20;
  return true;
}

// WPAD's alarm handler: runs the host side of WUD, then switches to WPAD's
// fiber for its manager as the original does.
extern "C" bool rvl_WPADAlarmHandler(CPUState* ctx)
{
  using namespace dualize;
  Cpu& cpu = CpuOf(ctx);
  GetMachine(cpu).wud.Pump(cpu);
  ctx->gpr[5] = 0;
  ctx->gpr[6] = 0;
  ctx->gpr[7] = WPAD_MANAGER;
  ctx->gpr[8] = WPAD_FIBER_STACK;
  ctx->pc = OS_SWITCH_FIBER_EX;
  ctx->downcount -= 8;
  return true;
}

// void WPADiSendData(s32 chan, WPADCommand command): WPAD's bookkeeping for a
// command (busy status, the reply it waits for, its timeout), then the
// output report straight to the remote.
extern "C" bool rvl_WPADiSendData(CPUState* ctx)
{
  using namespace dualize;
  constexpr uint32_t CB_CALLBACK = 2204, CB_STATUS = 2236, CB_STATUS_CALLBACK = 2128;
  constexpr uint32_t CB_STATUS_PENDING = 2240, CB_HANDLE = 2243, CB_SPEAKER_PENDING = 2255;
  constexpr uint32_t CB_RUMBLE = 2260, CB_TIMEOUT = 2312, CB_WAITING = 2320;
  constexpr uint32_t CB_READ_ADDRESS = 2420, CB_READ_BUFFER = 2424, CB_READ_SIZE = 2432;
  constexpr uint32_t CB_READ_DONE = 2428, CB_LAST_REPORT = 2439;
  constexpr uint32_t COMMAND_DATA = 4, COMMAND_SIZE_FIELD = 26, COMMAND_READ_SIZE = 32;
  constexpr uint32_t COMMAND_READ_ADDRESS = 28, COMMAND_READ_BUFFER = 36;
  constexpr uint32_t COMMAND_STATUS_CALLBACK = 40, COMMAND_CALLBACK = 44;
  constexpr uint8_t OUT_RUMBLE = 0x10, OUT_REQUEST_STATUS = 0x15, OUT_WRITE_DATA = 0x16;
  constexpr uint8_t OUT_READ_DATA = 0x17, OUT_SPEAKER_DATA = 0x18, OUT_WAIT = 0xFF;

  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const Memory& memory = machine.memory;
  const uint8_t* table = Bytes(memory, WPAD_CONTROL_BLOCKS + (ctx->gpr[3] & 3) * 4, 4);
  uint8_t* command = Bytes(memory, ctx->gpr[4], WPAD_COMMAND_SIZE);
  uint8_t* control = table ? Bytes(memory, Load32(table), WPAD_CONTROL_SIZE) : nullptr;
  const uint8_t* rumble_enabled = Bytes(memory, ctx->gpr[13] + WPAD_RUMBLE_SDA_OFFSET, 4);
  if (!command || !control || !rumble_enabled)
    return false;
  ctx->downcount -= 60;

  const uint8_t id = command[3];
  const uint32_t size = std::min<uint32_t>(Load16(command + COMMAND_SIZE_FIELD), 21);
  uint8_t* data = command + COMMAND_DATA;
  const int8_t handle = static_cast<int8_t>(control[CB_HANDLE]);
  const uint32_t status = Load32(control + CB_STATUS);
  if (handle < 0)
    return true;
  Store32(control + CB_STATUS, static_cast<uint32_t>(-2));  // busy
  const bool rumble = Load32(control + CB_RUMBLE) & Load32(rumble_enabled);

  auto set_timeout = [&](uint64_t ticks, bool waiting) {
    const uint64_t at = SystemTime(cpu, memory) + ticks;
    Store32(control + CB_TIMEOUT, static_cast<uint32_t>(at >> 32));
    Store32(control + CB_TIMEOUT + 4, static_cast<uint32_t>(at));
    control[CB_WAITING] = waiting;
  };
  switch (id)
  {
  case OUT_RUMBLE:
    Store32(control + CB_STATUS, status);
    break;
  case OUT_SPEAKER_DATA:
    Store32(control + CB_STATUS, status);
    --control[CB_SPEAKER_PENDING];
    break;
  case OUT_WAIT:
    // A pause between commands: nothing is sent. The SDK leaves interrupts
    // disabled on this path.
    set_timeout(Load32(data), true);
    ctx->msr &= ~PPC_MSR_EE;
    return true;
  default:
    if (id == OUT_READ_DATA)
    {
      Store32(control + CB_READ_DONE, 0);
      Store32(control + CB_READ_BUFFER, Load32(command + COMMAND_READ_BUFFER));
      Store16(control + CB_READ_SIZE, Load16(command + COMMAND_READ_SIZE));
      Store32(control + CB_READ_ADDRESS, Load32(command + COMMAND_READ_ADDRESS));
    }
    else if (id == OUT_REQUEST_STATUS)
    {
      Store32(control + CB_STATUS, status);
      Store32(control + CB_STATUS_CALLBACK, Load32(command + COMMAND_STATUS_CALLBACK));
      control[CB_STATUS_PENDING] = 1;
    }
    else if (id != OUT_WRITE_DATA)
    {
      data[0] |= 2;  // the remote acknowledges it (a write always does)
    }
    Store32(control + CB_CALLBACK, Load32(command + COMMAND_CALLBACK));
    control[CB_LAST_REPORT] = id;
    // The reply is due within two seconds of timebase.
    const uint8_t* bus_clock = Bytes(memory, 0x800000F8, 4);
    set_timeout(bus_clock ? (Load32(bus_clock) >> 1) & 0x7FFFFFFEu : 0, false);
    break;
  }

  uint8_t report[22];
  report[0] = id;
  std::memcpy(report + 1, data, size);
  if (size)
    report[1] = static_cast<uint8_t>(rumble ? report[1] | 1 : report[1] & ~1);
  machine.wud.Send(static_cast<uint8_t>(handle), report, size + 1);
  return true;
}

// The Bluetooth stack's entry points WUD and WPAD still reach.

// StartSyncDevice: no pairing to search for; WUDStartSyncSimple reports the
// sync as failed.
extern "C" bool rvl_StartSyncDevice(CPUState* ctx)
{
  ctx->gpr[3] = 0;
  return true;
}

extern "C" bool rvl_BTA_DmSetVisibility(CPUState*)
{
  return true;
}

extern "C" bool rvl_BTA_DmDisable(CPUState*)
{
  return true;
}

// WPAD's flow control asks for the ACL queue; the host's links never queue.
extern "C" bool rvl_BTA_HhGetAclQueueInfo(CPUState*)
{
  return true;
}

extern "C" bool rvl_btm_remove_acl(CPUState* ctx)
{
  using namespace dualize;
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  if (const uint8_t* address = Bytes(machine.memory, ctx->gpr[3], 6))
    machine.wud.Disconnect(address);
  ctx->gpr[3] = 0;
  return true;
}

extern "C" bool rvl_BTM_SetAfhChannels(CPUState* ctx)
{
  ctx->gpr[3] = 0;
  return true;
}
