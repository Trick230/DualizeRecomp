#include "kernel/ios/devices.hpp"

namespace dualize::ios
{
std::unique_ptr<Device> CreateDi(const Host& host, const Config& config, uint32_t id);
std::unique_ptr<Device> CreateStmEventHook(const Host& host);
std::unique_ptr<Device> CreateStmImmediate(const Host& host, const Config& config);
std::unique_ptr<Device> CreateKdRequest(const Host& host);
std::unique_ptr<Device> CreateKdTime(const Host& host, const Config& config);

std::unique_ptr<Device> CreateDevice(const Host& host, const Config& config, std::string_view name,
                                     uint32_t id)
{
  if (name == "/dev/di")
    return config.disc ? CreateDi(host, config, id) : nullptr;
  if (name == "/dev/stm/eventhook")
    return CreateStmEventHook(host);
  if (name == "/dev/stm/immediate")
    return CreateStmImmediate(host, config);
  if (name == "/dev/net/kd/request")
    return CreateKdRequest(host);
  if (name == "/dev/net/kd/time")
    return CreateKdTime(host, config);
  return nullptr;
}
}  // namespace dualize::ios
