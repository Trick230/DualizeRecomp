#pragma once

// The IOS kernel at the SDK's IPC boundary: the fd table, requests run as
// the SDK hands them over, replies queued in order after each device's delay
// (Dolphin's timings), routing of /dev/ paths to the native devices and other
// paths to the NAND, and the devices' timed events and periodic update. There
// is no mailbox: sdk/rvl_ipc.cpp replaces the SDK code that drove it.

#include "kernel/ios/devices.hpp"
#include "state.hpp"

#include <array>
#include <deque>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace dualize
{
class Machine;

class IosKernel
{
public:
  explicit IosKernel(Machine& machine);
  ~IosKernel();
  // Creates the devices; the disc and NAND come from config.
  void Start(const ios::Config& config);

  // The SDK's IPC client (natives in sdk/rvl_ipc.cpp) hands IOS the request
  // block at physical address; its reply comes back through PopReply after
  // the device's delay, with the IPC interrupt raised while any is waiting.
  void Submit(uint32_t address);
  bool PopReply(uint32_t* address);
  // A reply a state saved before SDK-level IPC had left in the mailbox.
  void AdoptMailboxReply(uint32_t address);

  ios::Nand& Nand() { return *m_nand; }
  ios::Es& Es() { return *m_es; }
  const ios::Host& Host() const { return m_host; }

  void DoState(StateIO& io);

private:
  static constexpr uint32_t MAX_FDS = 0x18;
  static constexpr uint64_t REQUEST_FLAG = 1ull << 32;

  void ExecuteCommand(uint32_t address);
  std::optional<ios::Reply> HandleCommand(uint32_t address);
  std::optional<ios::Reply> Open(ios::OpenRequest& request);
  void EnqueueReply(uint32_t address, uint32_t command, int32_t value, int64_t delay);
  int DeviceId(std::string_view name) const;
  void TraceRequest(uint32_t address) const;

  static void QueueEvent(void* user, uint64_t userdata, int64_t late);
  static void DeviceEvent(void* user, uint64_t userdata, int64_t late);
  static void UpdateEvent(void* user, uint64_t userdata, int64_t late);
  static void HostReply(void* user, uint32_t address, int32_t value, int64_t delay);
  static void HostSchedule(void* user, int64_t cycles, uint32_t device, uint32_t event,
                           uint32_t data);
  static uint64_t HostNow(void* user);

  Machine& m_machine;
  ios::Host m_host;
  ios::Memory m_memory;
  ios::Config m_config;
  int m_queue_event = -1, m_device_event = -1, m_update_event = -1;
  int64_t m_update_period = 0;

  // Fixed ids: pending device events and the fd table save them.
  std::vector<std::unique_ptr<ios::Device>> m_devices;
  ios::Nand* m_nand = nullptr;
  ios::Es* m_es = nullptr;
  std::array<int8_t, MAX_FDS> m_fds;  // device id, -1 free
  std::deque<uint32_t> m_replies;  // physical request addresses, in order
  uint64_t m_last_reply_time = 0;
  bool m_trace = false;
  std::unordered_set<uint32_t> m_traced;  // replies traced, until delivered
};
}  // namespace dualize
