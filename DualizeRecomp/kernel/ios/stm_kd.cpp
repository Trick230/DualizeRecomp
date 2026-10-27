// /dev/stm (state transition manager: power, reset, LEDs) and /dev/net/kd
// (WiiConnect24), as far as a game uses them.

#include "kernel/ios/devices.hpp"

#include <ctime>

namespace dualize::ios
{
namespace
{
enum : uint32_t
{
  IOCTL_STM_EVENTHOOK = 0x1000,
  IOCTL_STM_HOTRESET = 0x2001,
  IOCTL_STM_HOTRESET_FOR_PD = 0x2002,
  IOCTL_STM_SHUTDOWN = 0x2003,
  IOCTL_STM_IDLE = 0x2004,
  IOCTL_STM_WAKEUP = 0x2005,
  IOCTL_STM_GET_IDLEMODE = 0x3001,
  IOCTL_STM_RELEASE_EH = 0x3002,
  IOCTL_STM_READDDRREG = 0x4001,
  IOCTL_STM_READDDRREG2 = 0x4002,
  IOCTL_STM_VIDIMMING = 0x5001,
  IOCTL_STM_LEDFLASH = 0x6001,
  IOCTL_STM_LEDMODE = 0x6002,
  IOCTL_STM_READVER = 0x7001,
  IOCTL_STM_WRITEDMCU = 0x8001,

  STM_EVENT_RESET = 0x00020000,
  STM_EVENT_POWER = 0x00000800,
};

class StmEventHook;
StmEventHook* s_event_hook = nullptr;

// Holds the game's IOCTL_STM_EVENTHOOK until a console button is pressed.
class StmEventHook final : public Device
{
public:
  StmEventHook(const Host& host) : Device(host, "/dev/stm/eventhook") { s_event_hook = this; }
  ~StmEventHook() override
  {
    if (s_event_hook == this)
      s_event_hook = nullptr;
  }

  std::optional<Reply> IOCtl(const IOCtlRequest& request) override
  {
    if (request.request != IOCTL_STM_EVENTHOOK)
      return Reply{IPC_UNKNOWN};
    if (m_pending)
      return Reply{IPC_EEXIST};
    m_pending = request.address;
    return std::nullopt;
  }

  void DoState(StateIO& io) override
  {
    DoBaseState(io);
    io.Do(m_pending);
  }

  // Completes the hook with `event` in its output buffer; false if none.
  bool Trigger(uint32_t event)
  {
    if (!m_pending)
      return false;
    const IOCtlRequest request(m_memory, m_pending);
    m_memory.Write32(request.buffer_out, event);
    m_pending = 0;
    m_host.reply(m_host.user, request.address, IPC_SUCCESS, 0);
    return true;
  }

private:
  uint32_t m_pending = 0;  // request address, 0 when no hook is waiting
};

class StmImmediate final : public Device
{
public:
  StmImmediate(const Host& host, const Config& config)
      : Device(host, "/dev/stm/immediate"), m_config(config)
  {
  }

  std::optional<Reply> IOCtl(const IOCtlRequest& request) override
  {
    switch (request.request)
    {
    case IOCTL_STM_IDLE:
    case IOCTL_STM_SHUTDOWN:
      if (m_config.shutdown)
        m_config.shutdown(m_host.user);
      return Reply{IPC_SUCCESS};
    case IOCTL_STM_RELEASE_EH:
      // The hook returns with no event.
      return Reply{s_event_hook && s_event_hook->Trigger(0) ? IPC_SUCCESS : IPC_ENOENT};
    case IOCTL_STM_HOTRESET:
    case IOCTL_STM_VIDIMMING:
    case IOCTL_STM_LEDMODE:
    case IOCTL_STM_HOTRESET_FOR_PD:
    case IOCTL_STM_WAKEUP:
    case IOCTL_STM_GET_IDLEMODE:
    case IOCTL_STM_READDDRREG:
    case IOCTL_STM_READDDRREG2:
    case IOCTL_STM_LEDFLASH:
    case IOCTL_STM_READVER:
    case IOCTL_STM_WRITEDMCU:
      return Reply{IPC_SUCCESS};
    default:
      return Reply{IPC_UNKNOWN};
    }
  }

private:
  const Config& m_config;
};

// WiiConnect24 is offline: the scheduler calls succeed without doing anything.
class KdRequest final : public Device
{
public:
  explicit KdRequest(const Host& host) : Device(host, "/dev/net/kd/request") {}

  std::optional<Reply> IOCtl(const IOCtlRequest& request) override
  {
    enum : uint32_t
    {
      SUSPEND_SCHEDULER = 0x01,
      EXEC_TRY_SUSPEND_SCHEDULER = 0x02,
      EXEC_RESUME_SCHEDULER = 0x03,
      STARTUP_SOCKET = 0x06,
      CLEANUP_SOCKET = 0x07,
      LOCK_SOCKET = 0x08,
      UNLOCK_SOCKET = 0x09,
    };
    switch (request.request)
    {
    case SUSPEND_SCHEDULER:
    case EXEC_RESUME_SCHEDULER:
      m_memory.Write32(request.buffer_out, 0);
      break;
    case STARTUP_SOCKET:
      m_memory.Write32(request.buffer_out, 0);
      m_memory.Write32(request.buffer_out + 4, 0);
      break;
    case EXEC_TRY_SUSPEND_SCHEDULER:
    case CLEANUP_SOCKET:
    case LOCK_SOCKET:
    case UNLOCK_SOCKET:
      break;
    default:
      break;
    }
    return Reply{IPC_SUCCESS};
  }
};

class KdTime final : public Device
{
public:
  KdTime(const Host& host, const Config& config) : Device(host, "/dev/net/kd/time"), m_config(config)
  {
  }

  std::optional<Reply> IOCtl(const IOCtlRequest& request) override
  {
    enum : uint32_t
    {
      GET_UNIVERSAL_TIME = 0x14,
      SET_UNIVERSAL_TIME = 0x15,
      UNIMPLEMENTED = 0x16,
      SET_RTC_COUNTER = 0x17,
      GET_TIME_DIFF = 0x18,
    };
    int32_t result = 0;
    switch (request.request)
    {
    case GET_UNIVERSAL_TIME:
      m_memory.Write64(request.buffer_out + 4, AdjustedUtc());
      break;
    case SET_UNIVERSAL_TIME:
      m_utc_diff = static_cast<int64_t>(m_memory.Read64(request.buffer_in) - EmulatedUtc());
      break;
    case SET_RTC_COUNTER:
      m_rtc = m_memory.Read32(request.buffer_in);
      break;
    case GET_TIME_DIFF:
      m_memory.Write64(request.buffer_out + 4, AdjustedUtc() - m_rtc);
      break;
    case UNIMPLEMENTED:
      result = -9;
      break;
    default:
      break;
    }
    m_memory.Write32(request.buffer_out, 0);
    return Reply{result};
  }

private:
  // The emulated clock as UTC (the Wii keeps local time in its RTC).
  uint64_t EmulatedUtc() const
  {
    const time_t now = m_config.emulated_time ? m_config.emulated_time(m_host.user) : time(nullptr);
    tm gm{};
#ifdef _WIN32
    gmtime_s(&gm, &now);
#else
    gmtime_r(&now, &gm);
#endif
    const int64_t dst = gm.tm_isdst == 1 ? 3600 : 0;
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<uint32_t>(mktime(&gm))) - dst);
  }
  uint64_t AdjustedUtc() const { return EmulatedUtc() + m_utc_diff; }

  const Config& m_config;
  uint64_t m_rtc = 0;
  int64_t m_utc_diff = 0;
};
}  // namespace

void PressResetButton()
{
  if (s_event_hook)
    s_event_hook->Trigger(STM_EVENT_RESET);
}

void PressPowerButton()
{
  if (s_event_hook)
    s_event_hook->Trigger(STM_EVENT_POWER);
}

std::unique_ptr<Device> CreateStmEventHook(const Host& host)
{
  return std::make_unique<StmEventHook>(host);
}
std::unique_ptr<Device> CreateStmImmediate(const Host& host, const Config& config)
{
  return std::make_unique<StmImmediate>(host, config);
}
std::unique_ptr<Device> CreateKdRequest(const Host& host)
{
  return std::make_unique<KdRequest>(host);
}
std::unique_ptr<Device> CreateKdTime(const Host& host, const Config& config)
{
  return std::make_unique<KdTime>(host, config);
}
}  // namespace dualize::ios
