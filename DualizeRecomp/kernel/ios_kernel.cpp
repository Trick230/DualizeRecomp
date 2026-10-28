#include "kernel/ios_kernel.hpp"

#include "machine.hpp"

#include <cstdio>
#include <cstdlib>

namespace dualize
{
namespace
{
// A device's id is its index here.
// The last is Bluetooth's, which only the SDK's WUD would open (sdk/wud.hpp
// answers WUD instead); its slot keeps the saved state's layout.
constexpr std::string_view DEVICES[] = {
    "/dev/di",           "/dev/stm/eventhook",
    "/dev/stm/immediate", "/dev/net/kd/request",
    "/dev/net/kd/time",  "/dev/fs",
    "/dev/es",           "/dev/usb/oh1/57e/305",
};
constexpr int FS = 5;
constexpr int ES = 6;
constexpr int BLUETOOTH = 7;
constexpr uint32_t IPC_REPLY = 8;

// DUALIZE_IOS_TRACE=1 logs every request and its reply.
bool TraceEnabled()
{
  const char* v = std::getenv("DUALIZE_IOS_TRACE");
  return v && *v && *v != '0';
}

// The devices' state streams over the machine's.
ios::StateIO DeviceStream(StateIO& io)
{
  return ios::StateIO(
      &io, [](void* user, void* data, uint32_t size) { static_cast<StateIO*>(user)->Bytes(data, size); },
      io.Reading());
}
}  // namespace

IosKernel::IosKernel(Machine& machine) : m_machine(machine), m_memory(m_host), m_trace(TraceEnabled())
{
  m_fds.fill(-1);
  Scheduler& scheduler = machine.scheduler;
  m_queue_event = scheduler.Register(QueueEvent, this);
  m_device_event = scheduler.Register(DeviceEvent, this);
  m_update_event = scheduler.Register(UpdateEvent, this);
  m_update_period = Scheduler::CPU_CLOCK / 1500;
  m_host = ios::Host{
      .user = this,
      .mem1 = machine.memory.mem1,
      .mem1_size = Memory::MEM1_SIZE,
      .mem2 = machine.memory.mem2,
      .mem2_size = Memory::MEM2_SIZE,
      .ticks_per_second = Scheduler::CPU_CLOCK,
      .reply = HostReply,
      .schedule = HostSchedule,
      .now = HostNow,
  };
}

IosKernel::~IosKernel() = default;

void IosKernel::Start(const ios::Config& config)
{
  m_config = config;
  // Devices call these with Host::user, the kernel.
  m_config.emulated_time = [](void* user) {
    return static_cast<IosKernel*>(user)->m_machine.EmulatedTime();
  };
  m_config.shutdown = [](void* user) { static_cast<IosKernel*>(user)->m_machine.RequestStop(); };
  m_devices.clear();
  m_devices.resize(std::size(DEVICES));
  auto nand = ios::CreateFs(m_host, m_config);
  m_nand = nand.get();
  auto es = ios::CreateEs(m_host, *m_nand);
  m_es = es.get();
  m_devices[FS] = std::move(nand);
  m_devices[ES] = std::move(es);
  for (uint32_t id = 0; id < std::size(DEVICES); ++id)
  {
    if (!m_devices[id])
      m_devices[id] = ios::CreateDevice(m_host, m_config, DEVICES[id], id);
  }
  m_machine.scheduler.Schedule(m_update_period, m_update_event);
}

int IosKernel::DeviceId(std::string_view name) const
{
  for (int id = 0; id < static_cast<int>(std::size(DEVICES)); ++id)
  {
    if (DEVICES[id] == name)
      return m_devices[id] ? id : -1;
  }
  return -1;
}

void IosKernel::Submit(uint32_t address)
{
  ExecuteCommand(address);
}

bool IosKernel::PopReply(uint32_t* address)
{
  if (m_replies.empty())
    return false;
  *address = m_replies.front();
  m_replies.pop_front();
  if (m_trace && m_traced.erase(*address))
    std::fprintf(stderr, "[ios] %08x delivered\n", *address);
  if (m_replies.empty())
    m_machine.pi.SetInterrupt(PI_INT_WII_IPC, false);
  return true;
}

void IosKernel::ExecuteCommand(uint32_t address)
{
  const uint32_t command = m_memory.Read32(address);
  const bool trace = m_trace;
  if (trace)
    TraceRequest(address);
  std::optional<ios::Reply> reply = HandleCommand(address);
  if (!reply)
  {
    if (trace)
      std::fprintf(stderr, "[ios] %08x pending\n", address);
    return;
  }
  // Replies go out in order.
  const uint64_t now = m_machine.scheduler.Now();
  if (m_last_reply_time > now)
    reply->delay += static_cast<int64_t>(m_last_reply_time - now);
  m_last_reply_time = now + static_cast<uint64_t>(reply->delay);
  EnqueueReply(address, command, reply->value, reply->delay);
}

std::optional<ios::Reply> IosKernel::HandleCommand(uint32_t address)
{
  const ios::Memory& memory = m_memory;
  const ios::Request request(memory, address);
  if (request.command < ios::IPC_CMD_OPEN || request.command > ios::IPC_CMD_IOCTLV)
    return ios::Reply{ios::IPC_EINVAL, ios::Tb(978)};
  if (request.command == ios::IPC_CMD_OPEN)
  {
    ios::OpenRequest open(memory, address);
    return Open(open);
  }
  const uint32_t fd = static_cast<uint32_t>(request.fd);
  if (fd >= MAX_FDS || m_fds[fd] < 0)
    return ios::Reply{ios::IPC_EINVAL, ios::Tb(550)};
  ios::Device& device = *m_devices[m_fds[fd]];
  switch (request.command)
  {
  case ios::IPC_CMD_CLOSE:
    m_fds[fd] = -1;
    return device.Close(request.fd);
  case ios::IPC_CMD_READ:
    return device.Read(ios::ReadWriteRequest(memory, address));
  case ios::IPC_CMD_WRITE:
    return device.Write(ios::ReadWriteRequest(memory, address));
  case ios::IPC_CMD_SEEK:
    return device.Seek(ios::SeekRequest(memory, address));
  case ios::IPC_CMD_IOCTL:
    return device.IOCtl(ios::IOCtlRequest(memory, address));
  default:
    return device.IOCtlV(ios::IOCtlVRequest(memory, address));
  }
}

std::optional<ios::Reply> IosKernel::Open(ios::OpenRequest& request)
{
  int fd = -1;
  for (uint32_t i = 0; i < MAX_FDS && fd < 0; ++i)
  {
    if (m_fds[i] < 0)
      fd = static_cast<int>(i);
  }
  if (fd < 0)
    return ios::Reply{ios::IPC_EMAX, ios::Tb(5000)};
  request.fd = fd;

  int id = -1;
  if (request.path.starts_with("/dev/"))
    id = DeviceId(request.path);
  else if (request.path.starts_with('/'))
    id = FS;
  if (id < 0)
  {
    std::fprintf(stderr, "[native-ios] unknown device %s\n", request.path.c_str());
    return ios::Reply{ios::IPC_ENOENT, ios::Tb(3700)};
  }
  std::optional<ios::Reply> reply = m_devices[id]->Open(request);
  if (reply && reply->value >= ios::IPC_SUCCESS)
  {
    m_fds[fd] = static_cast<int8_t>(id);
    reply->value = fd;
  }
  return reply;
}

void IosKernel::EnqueueReply(uint32_t address, uint32_t command, int32_t value, int64_t delay)
{
  const ios::Memory& memory = m_memory;
  if (m_trace)
  {
    m_traced.insert(address);
    std::fprintf(stderr, "[ios] %08x reply %d in %lld\n", address, value, static_cast<long long>(delay));
  }
  memory.Write32(address + 4, static_cast<uint32_t>(value));
  // IOS writes the command it answers into the fd field and marks the block
  // a reply.
  memory.Write32(address + 8, command);
  memory.Write32(address, IPC_REPLY);
  m_machine.scheduler.Schedule(delay, m_queue_event, address);
}

void IosKernel::QueueEvent(void* user, uint64_t userdata, int64_t)
{
  IosKernel& kernel = *static_cast<IosKernel*>(user);
  // A request a state saved before SDK-level IPC had rung in but IOS had not
  // taken yet.
  if (userdata & REQUEST_FLAG)
  {
    kernel.Submit(static_cast<uint32_t>(userdata));
    return;
  }
  kernel.m_replies.push_back(static_cast<uint32_t>(userdata));
  kernel.m_machine.pi.SetInterrupt(PI_INT_WII_IPC, true);
}

void IosKernel::DeviceEvent(void* user, uint64_t userdata, int64_t)
{
  IosKernel& kernel = *static_cast<IosKernel*>(user);
  const uint32_t id = static_cast<uint32_t>(userdata >> 40);
  if (id < kernel.m_devices.size() && kernel.m_devices[id])
  {
    kernel.m_devices[id]->Event(static_cast<uint32_t>(userdata >> 32) & 0xFF,
                                static_cast<uint32_t>(userdata));
  }
}

void IosKernel::UpdateEvent(void* user, uint64_t, int64_t late)
{
  IosKernel& kernel = *static_cast<IosKernel*>(user);
  for (const auto& device : kernel.m_devices)
  {
    if (device && device->Active())
      device->Update();
  }
  kernel.m_machine.scheduler.Schedule(kernel.m_update_period - late, kernel.m_update_event);
}

void IosKernel::HostReply(void* user, uint32_t address, int32_t value, int64_t delay)
{
  IosKernel& kernel = *static_cast<IosKernel*>(user);
  kernel.EnqueueReply(address, kernel.m_memory.Read32(address), value, delay);
}

void IosKernel::HostSchedule(void* user, int64_t cycles, uint32_t device, uint32_t event,
                             uint32_t data)
{
  IosKernel& kernel = *static_cast<IosKernel*>(user);
  kernel.m_machine.scheduler.Schedule(
      cycles, kernel.m_device_event, uint64_t{device} << 40 | uint64_t{event & 0xFF} << 32 | data);
}

void IosKernel::TraceRequest(uint32_t address) const
{
  const ios::Memory& memory = m_memory;
  const ios::Request request(memory, address);
  const double ms = m_machine.scheduler.Now() * 1000.0 / Scheduler::CPU_CLOCK;
  switch (request.command)
  {
  case ios::IPC_CMD_OPEN:
  {
    const ios::OpenRequest open(memory, address);
    std::fprintf(stderr, "[ios] %08x %.3f open %s mode %u\n", address, ms, open.path.c_str(), open.flags);
    break;
  }
  case ios::IPC_CMD_IOCTL:
  {
    const ios::IOCtlRequest ioctl(memory, address);
    std::fprintf(stderr, "[ios] %08x %.3f fd %d ioctl %x in %u out %u\n", address, ms, request.fd,
                 ioctl.request, ioctl.buffer_in_size, ioctl.buffer_out_size);
    break;
  }
  case ios::IPC_CMD_IOCTLV:
  {
    const ios::IOCtlVRequest ioctlv(memory, address);
    std::fprintf(stderr, "[ios] %08x %.3f fd %d ioctlv %x in %zu io %zu\n", address, ms, request.fd,
                 ioctlv.request, ioctlv.in.size(), ioctlv.io.size());
    break;
  }
  default:
    std::fprintf(stderr, "[ios] %08x %.3f fd %d command %u size %u\n", address, ms, request.fd,
                 request.command, memory.Read32(address + 16));
  }
}

uint64_t IosKernel::HostNow(void* user)
{
  return static_cast<IosKernel*>(user)->m_machine.scheduler.Now();
}

void IosKernel::DoState(StateIO& io)
{
  io.Do(m_fds);
  io.Do(m_last_reply_time);
  // Requests: only states saved before SDK-level IPC have any (taken by IOS,
  // not executed yet); they run once the devices are loaded.
  std::vector<uint32_t> requests;
  io.Do(requests);
  std::vector<uint32_t> replies(m_replies.begin(), m_replies.end());
  io.Do(replies);
  if (io.Reading())
    m_replies.assign(replies.begin(), replies.end());
  ios::StateIO device_io = DeviceStream(io);
  for (size_t id = 0; id < m_devices.size(); ++id)
  {
    if (id == BLUETOOTH)
      m_machine.wud.DoState(device_io);
    else if (m_devices[id])
      m_devices[id]->DoState(device_io);
  }
  io.Marker(0x105C0DE);
  if (io.Reading())
  {
    for (uint32_t address : requests)
      Submit(address);
    m_machine.pi.SetInterrupt(PI_INT_WII_IPC, !m_replies.empty());
  }
}

void IosKernel::AdoptMailboxReply(uint32_t address)
{
  m_replies.push_front(address);
  m_machine.pi.SetInterrupt(PI_INT_WII_IPC, true);
}
}  // namespace dualize
