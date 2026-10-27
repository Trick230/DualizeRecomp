#pragma once

// The IOS kernel as the PPC sees it through the IPC mailbox: the fd table,
// request and reply queues with Dolphin's timings (acknowledge 500 timebase
// ticks after the doorbell, replies kept in order), routing of /dev/ paths to
// the native devices and other paths to the NAND, and the devices' timed
// events and periodic update.

#include "kernel/ios/devices.hpp"
#include "state.hpp"

#include <array>
#include <deque>
#include <memory>
#include <string_view>

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

  // Hollywood: the PPC rang the doorbell with a request at address.
  void EnqueueRequest(uint32_t address);
  // Hollywood: the mailbox may be free; sends the next ack or reply.
  void UpdateIpc();
  // The boot's ES_GenerateAck at IPC init.
  void InitIpc();

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
  std::deque<uint32_t> m_requests, m_replies;
  uint64_t m_last_reply_time = 0;
};
}  // namespace dualize
