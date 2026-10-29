#pragma once

// Guest RAM: MEM1 (24 MiB at physical 0), MEM2 (64 MiB at physical 0x10000000)
// and the 16 KiB locked L1 cache window the OS maps at 0xE0000000. The OS's
// BATs map MEM1/MEM2 cached at 0x80000000/0x90000000 and uncached at
// 0xC0000000/0xD0000000, which is all translation NiGHTS ever uses.
//
// All of it lives in one shared-memory section, viewed into a 4 GiB host
// reservation at those effective addresses (`flat`), so recompiled code
// reaches guest memory as flat + ea. Nothing is mapped anywhere else: the
// hardware window goes through the CPU's hooks, and a stray access anywhere
// in the reservation gets a page of zeros (logged) instead of a crash.

#include <cstdint>
#include <cstring>

namespace dualize
{
inline uint16_t Load16(const uint8_t* p)
{
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline uint32_t Load32(const uint8_t* p)
{
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline uint64_t Load64(const uint8_t* p)
{
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
inline void Store16(uint8_t* p, uint16_t v)
{
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void Store32(uint8_t* p, uint32_t v)
{
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void Store64(uint8_t* p, uint64_t v)
{
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}

class Memory
{
public:
  static constexpr uint32_t MEM1_SIZE = 0x01800000;
  static constexpr uint32_t MEM2_SIZE = 0x04000000;
  static constexpr uint32_t MEM2_BASE = 0x10000000;  // physical
  static constexpr uint32_t L1_SIZE = 0x4000;
  static constexpr uint32_t L1_BASE = 0xE0000000;  // effective

  // On failure (the host refused the reservation) every pointer stays null.
  Memory();
  ~Memory();
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;

  // Host bytes for size bytes at a physical address, or null outside RAM.
  uint8_t* Physical(uint32_t physical, uint32_t size) const
  {
    if (physical < MEM1_SIZE && size <= MEM1_SIZE - physical)
      return mem1 + physical;
    const uint32_t offset = physical - MEM2_BASE;
    if (offset < MEM2_SIZE && size <= MEM2_SIZE - offset)
      return mem2 + offset;
    return nullptr;
  }
  // The same for an effective address under the OS's BAT mapping.
  uint8_t* Effective(uint32_t address, uint32_t size) const
  {
    if ((address >> 30) == 2 || (address >> 30) == 3)
      return Physical(address & 0x1FFFFFFF, size);
    return nullptr;
  }

  uint32_t Read32(uint32_t physical) const
  {
    const uint8_t* p = Physical(physical, 4);
    return p ? Load32(p) : 0;
  }
  void Write32(uint32_t physical, uint32_t value) const
  {
    if (uint8_t* p = Physical(physical, 4))
      Store32(p, value);
  }
  void Write16(uint32_t physical, uint16_t value) const
  {
    if (uint8_t* p = Physical(physical, 2))
      Store16(p, value);
  }
  void Write8(uint32_t physical, uint8_t value) const
  {
    if (uint8_t* p = Physical(physical, 1))
      *p = value;
  }
  bool CopyTo(uint32_t physical, const void* data, uint32_t size) const
  {
    uint8_t* p = Physical(physical, size);
    if (p)
      std::memcpy(p, data, size);
    return p != nullptr;
  }

  // The 4 GiB effective address space, and the RAM in it at its cached
  // addresses.
  uint8_t* flat = nullptr;
  uint8_t* mem1 = nullptr;
  uint8_t* mem2 = nullptr;
  uint8_t* l1 = nullptr;

private:
  void* m_section = nullptr;  // Windows: the section handle
  int m_fd = -1;              // elsewhere: the shared-memory file
};
}  // namespace dualize
