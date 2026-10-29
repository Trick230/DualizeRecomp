// Of the functions recompiled over one address (the resident code's and each
// overlay's), the CPU runs the one whose guest text is in RAM.

#include "cpu/cpu.hpp"

#include <cstdio>
#include <cstring>

using namespace dualize;

#define CHECK(cond, ...)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
    {                                                                                              \
      std::fprintf(stderr, __VA_ARGS__);                                                           \
      std::fprintf(stderr, "\n");                                                                  \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

namespace dualize
{
uint32_t CpuHost::MmioRead(uint32_t, uint32_t, uint32_t)
{
  return 0;
}
void CpuHost::MmioWrite(uint32_t, uint32_t, uint32_t, uint32_t) {}
void CpuHost::GatherPipeBursts(const uint8_t*, size_t) {}
void CpuHost::AfterEvents() {}
bool CpuHost::AfterEventsInPlace()
{
  return true;
}
void CpuHost::BeforeIdle() {}
}  // namespace dualize

namespace
{
class Host : public CpuHost
{
};

constexpr uint32_t CODE = 0x80100000u;
// Four words each: what the resident code, overlay A and overlay B were
// built from.
constexpr uint32_t RESIDENT[] = {0x38600001u, 0x4E800020u, 0x38600002u, 0x4E800020u};
constexpr uint32_t STAGE_A[] = {0x38600003u, 0x60000000u, 0x60000000u, 0x4E800020u};
constexpr uint32_t STAGE_B[] = {0x38600004u, 0x4E800020u, 0x38600005u, 0x4E800020u};

void resident_0(CPUState*) {}
void resident_1(CPUState*) {}
void stage_a(CPUState*) {}
void stage_b_0(CPUState*) {}
void stage_b_1(CPUState*) {}

uint64_t Hash(const uint32_t* words, uint32_t count)
{
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < count * 4; ++i)
  {
    hash ^= static_cast<uint8_t>(words[i / 4] >> (24 - 8 * (i % 4)));
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// Resident: two functions over [CODE, CODE + 16). Overlay A: one over the
// same. Overlay B: two over [CODE + 8, CODE + 24).
const GuestRange s_code_ranges[] = {{CODE, CODE + 16}};
const GuestRange s_a_ranges[] = {{CODE, CODE + 16}};
const GuestRange s_b_ranges[] = {{CODE + 8, CODE + 24}};
const GuestRange s_function_ranges[] = {
    {CODE, CODE + 8}, {CODE + 8, CODE + 16}, {CODE, CODE + 16}, {CODE + 8, CODE + 16}, {CODE + 16, CODE + 24},
};
const GuestCode s_functions[] = {resident_0, resident_1, stage_a, stage_b_0, stage_b_1};
const uint64_t s_function_hashes[] = {
    Hash(RESIDENT, 2), Hash(RESIDENT + 2, 2), Hash(STAGE_A, 4), Hash(STAGE_B, 2), Hash(STAGE_B + 2, 2),
};
const GuestOverlay s_overlays[] = {{"a", s_a_ranges, 1, 2, 1}, {"b", s_b_ranges, 1, 3, 2}};
}  // namespace

extern "C" const GuestModule g_guest_module = {
    s_code_ranges, 1, s_function_ranges, s_functions, s_function_hashes, 5, s_overlays, 2,
};

extern "C" void guest_set_direct_calls(const unsigned char*, u32) {}

namespace
{
void Store(const Memory& memory, uint32_t address, const uint32_t* words, uint32_t count)
{
  uint8_t* bytes = memory.Effective(address, count * 4);
  for (uint32_t i = 0; i < count; ++i)
  {
    bytes[i * 4] = static_cast<uint8_t>(words[i] >> 24);
    bytes[i * 4 + 1] = static_cast<uint8_t>(words[i] >> 16);
    bytes[i * 4 + 2] = static_cast<uint8_t>(words[i] >> 8);
    bytes[i * 4 + 3] = static_cast<uint8_t>(words[i]);
  }
}

bool TestDispatch()
{
  Memory memory;
  CHECK(memory.mem1 != nullptr, "no guest memory");
  Scheduler scheduler;
  Host host;
  Cpu cpu(host, memory, scheduler);
  std::string error;
  CHECK(cpu.AttachModule(0, &error), "attach: %s", error.c_str());

  // The resident text: its functions run.
  Store(memory, CODE, RESIDENT, 4);
  CHECK(cpu.Lookup(CODE) == resident_0 && cpu.Lookup(CODE + 12) == resident_1, "resident text");

  // Overlay A loads over it: the resident functions fail their check and A's
  // runs over the whole range.
  Store(memory, CODE, STAGE_A, 4);
  CHECK(cpu.InvalidateICache(CODE, 16), "verified code over the range wasn't retired");
  CHECK(cpu.Lookup(CODE) == stage_a && cpu.Lookup(CODE + 8) == stage_a, "overlay A");

  // Overlay B over A's second half: B's functions there, and nothing matches
  // what is left of A.
  Store(memory, CODE + 8, STAGE_B, 4);
  cpu.InvalidateICache(CODE + 8, 16);
  CHECK(cpu.Lookup(CODE + 8) == stage_b_0 && cpu.Lookup(CODE + 20) == stage_b_1, "overlay B");
  CHECK(cpu.Lookup(CODE) == nullptr, "a function ran over text it wasn't built from");

  // The resident text back over the first 16 bytes; B's tail is still there.
  Store(memory, CODE, RESIDENT, 4);
  cpu.InvalidateICache(CODE, 16);
  CHECK(cpu.Lookup(CODE) == resident_0 && cpu.Lookup(CODE + 8) == resident_1, "resident text again");
  CHECK(cpu.Lookup(CODE + 16) == stage_b_1, "overlay B's tail");
  CHECK(cpu.Lookup(CODE + 24) == nullptr, "a function past the code");
  return true;
}
}  // namespace

int main()
{
  if (!TestDispatch())
    return 1;
  std::printf("overlay dispatch tests passed\n");
  return 0;
}
