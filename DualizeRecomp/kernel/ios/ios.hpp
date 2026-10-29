#pragma once

// Native IOS: the Wii's Starlet-side services the game talks to over IPC
// (/dev/di, /dev/fs and NAND files, /dev/stm, /dev/net/kd), as host code.
// Independent of Dolphin; the host supplies guest memory, timed events and the
// IPC reply path, so the same devices serve any chassis.

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dualize::ios
{
// IOS return codes.
enum : int32_t
{
  IPC_SUCCESS = 0,
  IPC_EACCES = -1,
  IPC_EEXIST = -2,
  IPC_EINVAL = -4,
  IPC_EMAX = -5,
  IPC_ENOENT = -6,
  IPC_EQUEUEFULL = -8,
  IPC_EIO = -12,
  IPC_ENOMEM = -22,
  IPC_UNKNOWN = -9,
};

enum Command : uint32_t
{
  IPC_CMD_OPEN = 1,
  IPC_CMD_CLOSE = 2,
  IPC_CMD_READ = 3,
  IPC_CMD_WRITE = 4,
  IPC_CMD_SEEK = 5,
  IPC_CMD_IOCTL = 6,
  IPC_CMD_IOCTLV = 7,
};

// Timebase ticks (CPU clock / 12) in CPU ticks, the unit of every delay here.
constexpr int64_t Tb(uint64_t timebase_ticks)
{
  return static_cast<int64_t>(timebase_ticks * 12);
}
// IOS's cost of a request without a better known time.
constexpr int64_t DEFAULT_REPLY_DELAY = Tb(4000);
constexpr int64_t IPC_OVERHEAD = Tb(2700);

struct Host
{
  void* user = nullptr;
  // MEM1 and MEM2, mapped for the devices' lifetime.
  uint8_t* mem1 = nullptr;
  uint32_t mem1_size = 0;
  uint8_t* mem2 = nullptr;
  uint32_t mem2_size = 0;
  uint32_t ticks_per_second = 0;  // CPU clock
  // Sends the reply of the request at `address` after delay CPU ticks.
  void (*reply)(void* user, uint32_t address, int32_t value, int64_t delay) = nullptr;
  // Calls Device::Event(event, data) of the device `device` after cycles CPU ticks.
  void (*schedule)(void* user, int64_t cycles, uint32_t device, uint32_t event,
                   uint32_t data) = nullptr;
  uint64_t (*now)(void* user) = nullptr;  // CPU ticks
  // A device wrote size bytes at a physical address the way a DMA does: code
  // there must be fetched anew (the CPU's caches never see a DMA).
  void (*dma_written)(void* user, uint32_t address, uint32_t size) = nullptr;
};

// Guest memory as IOS sees it: physical addresses (virtual ones are masked).
class Memory
{
public:
  explicit Memory(const Host& host) : m_host(host) {}

  // After a device's DMA wrote [address, address + size).
  void DmaWritten(uint32_t address, uint32_t size) const
  {
    if (m_host.dma_written && size)
      m_host.dma_written(m_host.user, address & 0x1FFFFFFF, size);
  }

  // Host pointer to size bytes at address, or null if they leave MEM1/MEM2.
  uint8_t* Pointer(uint32_t address, uint32_t size) const
  {
    const uint32_t physical = address & 0x1FFFFFFF;
    if (physical < m_host.mem1_size && size <= m_host.mem1_size - physical)
      return m_host.mem1 + physical;
    const uint32_t offset = physical - 0x10000000;
    if (m_host.mem2 && offset < m_host.mem2_size && size <= m_host.mem2_size - offset)
      return m_host.mem2 + offset;
    return nullptr;
  }

  uint8_t Read8(uint32_t address) const
  {
    const uint8_t* p = Pointer(address, 1);
    return p ? *p : 0;
  }
  uint16_t Read16(uint32_t address) const
  {
    const uint8_t* p = Pointer(address, 2);
    return p ? static_cast<uint16_t>(p[0] << 8 | p[1]) : 0;
  }
  uint32_t Read32(uint32_t address) const
  {
    const uint8_t* p = Pointer(address, 4);
    uint32_t v = 0;
    if (p)
      std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
  }
  uint64_t Read64(uint32_t address) const
  {
    return static_cast<uint64_t>(Read32(address)) << 32 | Read32(address + 4);
  }
  void Write8(uint32_t address, uint8_t value) const
  {
    if (uint8_t* p = Pointer(address, 1))
      *p = value;
  }
  void Write16(uint32_t address, uint16_t value) const
  {
    if (uint8_t* p = Pointer(address, 2))
    {
      p[0] = static_cast<uint8_t>(value >> 8);
      p[1] = static_cast<uint8_t>(value);
    }
  }
  void Write32(uint32_t address, uint32_t value) const
  {
    if (uint8_t* p = Pointer(address, 4))
    {
      value = __builtin_bswap32(value);
      std::memcpy(p, &value, 4);
    }
  }
  void Write64(uint32_t address, uint64_t value) const
  {
    Write32(address, static_cast<uint32_t>(value >> 32));
    Write32(address + 4, static_cast<uint32_t>(value));
  }
  bool CopyTo(uint32_t address, const void* data, uint32_t size) const
  {
    uint8_t* p = Pointer(address, size);
    if (!p)
      return false;
    std::memcpy(p, data, size);
    return true;
  }
  bool CopyFrom(void* data, uint32_t address, uint32_t size) const
  {
    const uint8_t* p = Pointer(address, size);
    if (!p)
      return false;
    std::memcpy(data, p, size);
    return true;
  }
  void Fill(uint32_t address, uint8_t value, uint32_t size) const
  {
    if (uint8_t* p = Pointer(address, size))
      std::memset(p, value, size);
  }
  // A NUL-terminated string of at most max_size bytes.
  std::string ReadString(uint32_t address, uint32_t max_size) const
  {
    std::string s;
    for (uint32_t i = 0; i < max_size; ++i)
    {
      const uint8_t* p = Pointer(address + i, 1);
      if (!p || *p == 0)
        break;
      s.push_back(static_cast<char>(*p));
    }
    return s;
  }

private:
  const Host& m_host;
};

// An IPC request block in guest memory (0x40 bytes; fields big-endian).
struct Request
{
  uint32_t address = 0;
  uint32_t command = 0;
  int32_t fd = 0;

  Request(const Memory& memory, uint32_t address_)
      : address(address_), command(memory.Read32(address_)),
        fd(static_cast<int32_t>(memory.Read32(address_ + 8)))
  {
  }
};

struct OpenRequest : Request
{
  std::string path;
  uint32_t flags;
  OpenRequest(const Memory& memory, uint32_t address_)
      : Request(memory, address_), path(memory.ReadString(memory.Read32(address_ + 12), 64)),
        flags(memory.Read32(address_ + 16))
  {
  }
};

struct ReadWriteRequest : Request
{
  uint32_t buffer;
  uint32_t size;
  ReadWriteRequest(const Memory& memory, uint32_t address_)
      : Request(memory, address_), buffer(memory.Read32(address_ + 12)),
        size(memory.Read32(address_ + 16))
  {
  }
};

struct SeekRequest : Request
{
  int32_t offset;
  uint32_t mode;
  SeekRequest(const Memory& memory, uint32_t address_)
      : Request(memory, address_), offset(static_cast<int32_t>(memory.Read32(address_ + 12))),
        mode(memory.Read32(address_ + 16))
  {
  }
};

struct IOCtlRequest : Request
{
  uint32_t request;
  uint32_t buffer_in, buffer_in_size;
  uint32_t buffer_out, buffer_out_size;
  IOCtlRequest(const Memory& memory, uint32_t address_)
      : Request(memory, address_), request(memory.Read32(address_ + 12)),
        buffer_in(memory.Read32(address_ + 16)), buffer_in_size(memory.Read32(address_ + 20)),
        buffer_out(memory.Read32(address_ + 24)), buffer_out_size(memory.Read32(address_ + 28))
  {
  }
};

struct IOCtlVRequest : Request
{
  struct Vector
  {
    uint32_t address;
    uint32_t size;
  };
  uint32_t request;
  std::vector<Vector> in;  // read by IOS
  std::vector<Vector> io;  // written by IOS

  IOCtlVRequest(const Memory& memory, uint32_t address_)
      : Request(memory, address_), request(memory.Read32(address_ + 12))
  {
    const uint32_t in_count = memory.Read32(address_ + 16);
    const uint32_t io_count = memory.Read32(address_ + 20);
    const uint32_t vectors = memory.Read32(address_ + 24);
    for (uint32_t i = 0; i < in_count + io_count && i < 64; ++i)
    {
      const Vector v{memory.Read32(vectors + i * 8), memory.Read32(vectors + i * 8 + 4)};
      (i < in_count ? in : io).push_back(v);
    }
  }

  bool HasVectors(size_t in_count, size_t io_count) const
  {
    if (in.size() != in_count || io.size() != io_count)
      return false;
    for (const Vector& v : in)
      if (v.address == 0)
        return false;
    for (const Vector& v : io)
      if (v.address == 0)
        return false;
    return true;
  }
};

// Return value plus how long IOS takes to send it, in CPU ticks.
struct Reply
{
  int32_t value;
  int64_t delay = DEFAULT_REPLY_DELAY;
};

// Savestate stream in the encoding of Dolphin's PointerWrap (raw little-endian
// PODs, bools as one byte, u32-counted strings and containers), so the native
// devices read and write the same state as the Dolphin devices they replace.
class StateIO
{
public:
  StateIO(void* user, void (*bytes)(void*, void*, uint32_t), bool reading)
      : m_user(user), m_bytes(bytes), m_reading(reading)
  {
  }
  bool Reading() const { return m_reading; }

  void Bytes(void* data, uint32_t size) { m_bytes(m_user, data, size); }
  template <typename T>
  void Do(T& value)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    Bytes(&value, sizeof(T));
  }
  void Do(bool& value)
  {
    uint8_t v = value;
    Do(v);
    value = v != 0;
  }
  void Do(std::string& value)
  {
    uint32_t size = static_cast<uint32_t>(value.size());
    Do(size);
    value.resize(size);
    if (size)
      Bytes(value.data(), size);
  }
  template <typename T>
  void Do(std::optional<T>& value)
  {
    bool present = value.has_value();
    Do(present);
    if (m_reading)
      value = present ? std::optional<T>(T{}) : std::nullopt;
    if (present)
      Do(*value);
  }

private:
  void* m_user;
  void (*m_bytes)(void*, void*, uint32_t);
  bool m_reading;
};

class Device
{
public:
  Device(const Host& host, std::string name) : m_host(host), m_memory(host), m_name(std::move(name))
  {
  }
  virtual ~Device() = default;

  const std::string& Name() const { return m_name; }

  // Each returns the reply to send, or nullopt when the device replies later
  // through Host::reply.
  virtual std::optional<Reply> Open(const OpenRequest&)
  {
    m_active = true;
    return Reply{IPC_SUCCESS};
  }
  virtual std::optional<Reply> Close(int32_t)
  {
    m_active = false;
    return Reply{IPC_SUCCESS};
  }
  virtual std::optional<Reply> Read(const ReadWriteRequest&) { return Reply{IPC_EINVAL}; }
  virtual std::optional<Reply> Write(const ReadWriteRequest&) { return Reply{IPC_EINVAL}; }
  virtual std::optional<Reply> Seek(const SeekRequest&) { return Reply{IPC_EINVAL}; }
  virtual std::optional<Reply> IOCtl(const IOCtlRequest&) { return Reply{IPC_EINVAL}; }
  virtual std::optional<Reply> IOCtlV(const IOCtlVRequest&) { return Reply{IPC_EINVAL}; }
  // A Host::schedule callback.
  virtual void Event(uint32_t, uint32_t) {}
  // Called at IOS's device update rate while the device is open.
  virtual void Update() {}
  bool Active() const { return m_active; }

  // Same layout as Dolphin's Device::DoState plus the device's own fields.
  virtual void DoState(StateIO& io) { DoBaseState(io); }

protected:
  void DoBaseState(StateIO& io)
  {
    io.Do(m_name);
    uint32_t type = 0;  // Dolphin's DeviceType::Static
    io.Do(type);
    io.Do(m_active);
  }

  const Host& m_host;
  Memory m_memory;
  std::string m_name;
  bool m_active = false;
};
}  // namespace dualize::ios
