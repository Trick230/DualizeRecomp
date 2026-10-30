// Of the functions recompiled over one address (the resident code's and each
// overlay's), the CPU runs the one whose guest text is in RAM; an overlay runs
// wherever the game linked it, found by name (RSO) or module id (REL).

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
// Overlay C: compiled with its header at C_BASE, its lis taking an address in
// it (the half its placement changes is cleared for the hash).
constexpr uint32_t C_BASE = 0x80200000u;
constexpr uint32_t C_CODE = C_BASE + 0x40;
constexpr uint32_t STAGE_C[] = {0x3C600000u, 0x4E800020u};
// Overlay D: REL module 7, compiled with its header at D_BASE and its bss
// (section 3) at D_BSS.
constexpr uint32_t D_ID = 7;
constexpr uint32_t D_BASE = 0x80400000u;
constexpr uint32_t D_CODE = D_BASE + 0x40;
constexpr uint32_t D_BSS = 0x80480000u;
constexpr uint32_t STAGE_D[] = {0x38600007u, 0x4E800020u};
// The resident code's second function holds a word whose bits 8-15 depend on
// where an overlay loads.
constexpr uint32_t RESIDENT_MASK = 0xFFFF00FFu;

void resident_0(CPUState*) {}
void resident_1(CPUState*) {}
void stage_a(CPUState*) {}
void stage_b_0(CPUState*) {}
void stage_b_1(CPUState*) {}
void stage_c(CPUState*) {}
void stage_d(CPUState*) {}

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
const GuestRange s_c_ranges[] = {{C_CODE, C_CODE + 8}};
const GuestRange s_d_ranges[] = {{D_CODE, D_CODE + 8}};
const GuestRange s_function_ranges[] = {
    {CODE, CODE + 8},      {CODE + 8, CODE + 16},  {CODE, CODE + 16},   {CODE + 8, CODE + 16},
    {CODE + 16, CODE + 24}, {C_CODE, C_CODE + 8}, {D_CODE, D_CODE + 8},
};
const GuestCode s_functions[] = {resident_0, resident_1, stage_a, stage_b_0, stage_b_1, stage_c, stage_d};
const uint64_t s_function_hashes[] = {
    Hash(RESIDENT, 2),       Hash(RESIDENT + 2, 2), Hash(STAGE_A, 4), Hash(STAGE_B, 2),
    Hash(STAGE_B + 2, 2),    Hash(STAGE_C, 2),         Hash(STAGE_D, 2),
};
const GuestReloc s_resident_relocs[] = {{CODE + 8, RESIDENT_MASK}};
const GuestReloc s_c_relocs[] = {{C_CODE, 0xFFFF0000u}};
const GuestOverlay s_overlays[] = {
    {"a", 0, s_a_ranges, 1, 2, 1},
    {"b", 0, s_b_ranges, 1, 3, 2},
    {"files/rso/stagec.rso", 0, s_c_ranges, 1, 5, 1, C_BASE, 0, 0, s_c_relocs, 1},
    {"files/rels/staged.rel", D_ID, s_d_ranges, 1, 6, 1, D_BASE, D_BSS, 3, nullptr, 0},
};
}  // namespace

extern "C" const GuestModule g_guest_module = {
    s_code_ranges,     1, s_function_ranges, s_functions, s_function_hashes, 7, s_overlays, 4,
    s_resident_relocs, 1,
};

extern "C" void guest_set_direct_calls(const unsigned char*, u32) {}
extern "C"
{
u32 dolrecomp_overlay_shift[4];
u32 dolrecomp_overlay_bss_shift[4];
}

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
  CHECK(cpu.AttachModule(0, 0, &error), "attach: %s", error.c_str());

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

// Overlay C linked 0x2000 past where it was compiled for: found by its name in
// the game's RSO list, its code runs there with the shift published.
bool TestMovedOverlay()
{
  Memory memory;
  CHECK(memory.mem1 != nullptr, "no guest memory");
  Scheduler scheduler;
  Host host;
  Cpu cpu(host, memory, scheduler);
  std::string error;
  constexpr uint32_t LIST = 0x80003168u, NAME = 0x80300000u, SHIFT = 0x2000u;
  CHECK(cpu.AttachModule(0, LIST, &error), "attach: %s", error.c_str());

  // Linked where it was compiled for, before the list names it.
  const uint32_t linked[] = {0x3C608020u, 0x4E800020u};
  Store(memory, C_CODE, linked, 2);
  CHECK(cpu.Lookup(C_CODE) == stage_c && dolrecomp_overlay_shift[2] == 0, "overlay C in place");

  // Moved: the header names the module, the lis takes the moved address.
  const char name[] = "D:\\work\\stagec\\bin\\stagec.plf";
  std::memcpy(memory.Effective(NAME, sizeof(name)), name, sizeof(name));
  const uint32_t header[] = {0, 0, 0, 0, NAME, sizeof(name) - 1};
  Store(memory, C_BASE + SHIFT, header, 6);
  const uint32_t list[] = {C_BASE + SHIFT};
  Store(memory, LIST, list, 1);
  const uint32_t moved[] = {0x3C608020u + 1, 0x4E800020u};
  Store(memory, C_CODE + SHIFT, moved, 2);
  const uint32_t gone[] = {0, 0};
  Store(memory, C_CODE, gone, 2);
  cpu.InvalidateICache(C_CODE, 8);
  cpu.InvalidateICache(C_CODE + SHIFT, 8);
  CHECK(cpu.Lookup(C_CODE + SHIFT) == stage_c && cpu.Lookup(C_CODE + SHIFT + 4) == stage_c,
        "overlay C where it was linked");
  CHECK(dolrecomp_overlay_shift[2] == SHIFT, "overlay C's shift");
  CHECK(cpu.Lookup(C_CODE) == nullptr, "overlay C where it no longer is");
  return true;
}

// Overlay D linked 0x3000 past where it was compiled for, its bss 0x100 past:
// found by its id in the OS's module list.
bool TestMovedRel()
{
  Memory memory;
  CHECK(memory.mem1 != nullptr, "no guest memory");
  Scheduler scheduler;
  Host host;
  Cpu cpu(host, memory, scheduler);
  std::string error;
  constexpr uint32_t LIST = 0x800030C8u, SHIFT = 0x3000u, BSS_SHIFT = 0x100u;
  CHECK(cpu.AttachModule(0, 0, &error), "attach: %s", error.c_str());

  // Another module first in the list; D's header: id, next, its 4 sections'
  // table (made absolute by the linker) with the bss's address in entry 3.
  const uint32_t other = 0x80500000u, header = D_BASE + SHIFT, table = header + 0x60;
  const uint32_t other_header[] = {3, header};
  Store(memory, other, other_header, 2);
  const uint32_t d_header[] = {D_ID, 0, 0, 4, table};
  Store(memory, header, d_header, 5);
  const uint32_t d_bss_entry[] = {D_BSS + BSS_SHIFT, 0x20};
  Store(memory, table + 3 * 8, d_bss_entry, 2);
  const uint32_t list[] = {other};
  Store(memory, LIST, list, 1);
  Store(memory, D_CODE + SHIFT, STAGE_D, 2);
  CHECK(cpu.Lookup(D_CODE + SHIFT) == stage_d, "overlay D where it was linked");
  CHECK(dolrecomp_overlay_shift[3] == SHIFT && dolrecomp_overlay_bss_shift[3] == BSS_SHIFT, "overlay D's shifts");

  // The resident word that depends on an overlay's placement is left out of
  // its function's check.
  const uint32_t resident[] = {0x38600001u, 0x4E800020u, 0x38601202u, 0x4E800020u};
  Store(memory, CODE, resident, 4);
  CHECK(cpu.Lookup(CODE + 8) == resident_1, "resident code with a word an overlay's placement sets");
  return true;
}
}  // namespace

int main()
{
  if (!TestDispatch() || !TestMovedOverlay() || !TestMovedRel())
    return 1;
  std::printf("overlay dispatch tests passed\n");
  return 0;
}
