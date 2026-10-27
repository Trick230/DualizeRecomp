#include "core/cpu.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dualize
{
namespace
{
constexpr uint32_t MSR_EE = 0x8000;
constexpr uint32_t MSR_ILE = 0x10000;
constexpr uint32_t EXCEPTION_MSR_CLEAR = 0x04EF36;
constexpr uint32_t SRR1_MASK = 0x87C0FFFF;

enum Spr : uint16_t
{
  SPR_DEC = 22,
  SPR_SDR = 25,
  SPR_TL_W = 284,
  SPR_TU_W = 285,
  SPR_PVR = 287,
  SPR_GQR0 = 912,
  SPR_HID2 = 920,
  SPR_WPAR = 921,
  SPR_DMAU = 922,
  SPR_DMAL = 923,
  SPR_UPMC1 = 937,
  SPR_UPMC2 = 938,
  SPR_UPMC3 = 941,
  SPR_UPMC4 = 942,
  SPR_PMC1 = 953,
  SPR_PMC2 = 954,
  SPR_PMC3 = 957,
  SPR_PMC4 = 958,
  SPR_HID0 = 1008,
  SPR_HID1 = 1009,
  SPR_IABR = 1010,
  SPR_HID4 = 1011,
  SPR_THRM1 = 1020,
  SPR_THRM2 = 1021,
  SPR_THRM3 = 1022,
};

bool RangesSorted(const GuestRange* ranges, uint32_t count)
{
  if (!ranges || count == 0)
    return false;
  for (uint32_t i = 0; i < count; ++i)
    if (ranges[i].start >= ranges[i].end || (i && ranges[i - 1].end > ranges[i].start))
      return false;
  return true;
}

bool FunctionsTileCode(const GuestModule& desc)
{
  if (!RangesSorted(desc.function_ranges, desc.num_functions) || !desc.functions || !desc.function_hashes)
    return false;
  uint32_t function = 0;
  for (uint32_t code = 0; code < desc.num_code_ranges; ++code)
  {
    uint32_t cursor = desc.code_ranges[code].start;
    while (function < desc.num_functions && desc.function_ranges[function].start < desc.code_ranges[code].end)
    {
      if (desc.function_ranges[function].start != cursor ||
          desc.function_ranges[function].end > desc.code_ranges[code].end)
        return false;
      cursor = desc.function_ranges[function++].end;
    }
    if (cursor != desc.code_ranges[code].end)
      return false;
  }
  return function == desc.num_functions;
}

// Index of an instruction in MEM1 then MEM2, or -1.
int LookupIndex(uint32_t address)
{
  if (address - 0x80000000u < Memory::MEM1_SIZE)
    return static_cast<int>((address - 0x80000000u) >> 2);
  if (address - 0x90000000u < Memory::MEM2_SIZE)
    return static_cast<int>((Memory::MEM1_SIZE >> 2) + ((address - 0x90000000u) >> 2));
  return -1;
}

Cpu& CpuOf(CPUState* state)
{
  return *static_cast<Cpu*>(state->external_user_data);
}
}  // namespace

extern "C" GuestCode dolrecomp_lookup(CPUState* ctx, u32 target)
{
  return CpuOf(ctx).Lookup(target);
}

Cpu::Cpu(Machine& machine) : m_machine(machine), m_scheduler(machine.scheduler)
{
  state.external_read = HookRead;
  state.external_write = HookWrite;
  state.external_read32 = HookRead32;
  state.external_write32 = HookWrite32;
  state.external_pointer = HookPointer;
  state.spr_read = HookSprRead;
  state.spr_write = HookSprWrite;
  state.cache_control = HookCacheControl;
  state.cache_range = HookCacheRange;
  state.instruction_fallback = HookFallback;
  state.read_timebase = HookReadTimebase;
  state.msr_written = HookMsrWritten;
  state.external_user_data = this;
  state.ram = machine.memory.mem1;
  state.ram_size = Memory::MEM1_SIZE;
  state.exram = machine.memory.mem2;
  state.exram_size = Memory::MEM2_SIZE;
  ppc_build_fast_memory(&state, true);
  state.flat_base = machine.memory.flat;
  state.gather_pipe_ptr = &m_gather_ptr;
  state.gather_pipe_base = m_gather;

  // Reset values (Broadway): HID1 reports the PLL setting, PVR the core.
  m_spr[SPR_PVR] = 0x00087102;
  m_spr[SPR_HID1] = 0x80000000;
  m_dec_event = m_scheduler.Register(DecrementerEvent, this);
  m_scheduler.BindDowncount(&state.downcount);
}

Cpu::~Cpu()
{
  PublishDirectCalls(false);
}

bool Cpu::AttachModule(std::string* error)
{
  const GuestModule& module = g_guest_module;
  if (!RangesSorted(module.code_ranges, module.num_code_ranges) || !FunctionsTileCode(module))
  {
    *error = "recompiled code has malformed code or function ranges";
    return false;
  }
  m_module = &module;
  BuildFunctionTables();
  return true;
}

void Cpu::BuildFunctionTables()
{
  const uint32_t functions = m_module->num_functions;
  m_function_state.assign(functions, FUNCTION_UNVERIFIED);
  m_direct_call_ok.assign(functions, 0);

  const uint32_t total = (Memory::MEM1_SIZE + Memory::MEM2_SIZE) >> 2;
  m_code_lines.assign((total / 8 + 63) / 64, 0);
  uint32_t mem1_code_end = 0;
  for (uint32_t i = 0; i < functions; ++i)
  {
    const GuestRange& function = m_module->function_ranges[i];
    if (function.start >= 0x80000000u && function.end <= 0x80000000u + Memory::MEM1_SIZE)
      mem1_code_end = std::max(mem1_code_end, function.end - 0x80000000u);
  }
  m_line_function.assign((mem1_code_end + (1u << FUNCTION_LINE_SHIFT) - 1) >> FUNCTION_LINE_SHIFT, NO_FUNCTION);
  for (uint32_t i = 0; i < functions; ++i)
  {
    const GuestRange& function = m_module->function_ranges[i];
    if (function.start >= 0x80000000u && function.end <= 0x80000000u + mem1_code_end)
    {
      // Ascending, so the first function to reach a line is its first.
      for (uint32_t line = (function.start - 0x80000000u) >> FUNCTION_LINE_SHIFT;
           line << FUNCTION_LINE_SHIFT < function.end - 0x80000000u; ++line)
        if (m_line_function[line] == NO_FUNCTION)
          m_line_function[line] = static_cast<int32_t>(i);
    }
    const int start = LookupIndex(function.start);
    if (start < 0)
      continue;
    const uint32_t first = static_cast<uint32_t>(start);
    const uint32_t last = std::min(first + ((function.end - function.start) >> 2), total);
    for (uint32_t line = first >> 3; line << 3 < last; ++line)
      m_code_lines[line >> 6] |= uint64_t{1} << (line & 63);
  }
  m_idle_pc = m_machine.config.idle_pc;
  PublishDirectCalls(true);
}

void Cpu::PublishDirectCalls(bool enable)
{
  if (m_module)
    guest_set_direct_calls(enable ? m_direct_call_ok.data() : nullptr, m_idle_pc);
}

int Cpu::FunctionAt(uint32_t pc) const
{
  const uint32_t line = (pc - 0x80000000u) >> FUNCTION_LINE_SHIFT;
  if (line >= m_line_function.size())
    return NO_FUNCTION;
  int32_t index = m_line_function[line];
  if (index == NO_FUNCTION)
    return NO_FUNCTION;
  const GuestRange* ranges = m_module->function_ranges;
  const uint32_t count = m_module->num_functions;
  while (ranges[index].end <= pc)
    if (static_cast<uint32_t>(++index) == count)
      return NO_FUNCTION;
  return ranges[index].start <= pc ? index : NO_FUNCTION;
}

void Cpu::SetFunctionState(uint32_t index, FunctionState function_state)
{
  m_function_state[index] = function_state;
  m_direct_call_ok[index] = function_state == FUNCTION_VERIFIED;
}

void Cpu::VerifyFunction(uint32_t index)
{
  const GuestRange& function = m_module->function_ranges[index];
  const uint8_t* bytes = m_machine.memory.Effective(function.start, function.end - function.start);
  ++m_stats.verifications;
  if (!bytes)
  {
    SetFunctionState(index, FUNCTION_FAILED);
    return;
  }
  // FNV-1a 64, as DolRecomp hashes the original text.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t i = 0, n = function.end - function.start; i < n; ++i)
  {
    hash ^= bytes[i];
    hash *= 0x100000001B3ull;
  }
  SetFunctionState(index, hash == m_module->function_hashes[index] ? FUNCTION_VERIFIED : FUNCTION_FAILED);
}

int Cpu::Dispatchable(uint32_t pc)
{
  const int function = FunctionAt(pc);
  if (function < 0)
    return NO_FUNCTION;
  if (m_function_state[function] == FUNCTION_UNVERIFIED)
    VerifyFunction(static_cast<uint32_t>(function));
  return m_function_state[function] == FUNCTION_VERIFIED ? function : NO_FUNCTION;
}

GuestCode Cpu::Lookup(uint32_t pc)
{
  const int function = Dispatchable(pc);
  return function < 0 ? nullptr : m_module->functions[function];
}

bool Cpu::LineMayHoldCode(uint32_t ea) const
{
  const int index = LookupIndex(ea & ~31u);
  return index >= 0 && static_cast<size_t>(index >> 9) < m_code_lines.size() &&
         ((m_code_lines[index >> 9] >> ((index >> 3) & 63)) & 1) != 0;
}

void Cpu::InvalidateICache(uint32_t address, uint32_t length)
{
  if (!m_module || length == 0)
    return;
  // Physical and uncached addresses name the same code as the cached mirror.
  if (address < 0x20000000u || (address >> 30) == 3)
    address = (address & 0x1FFFFFFF) | 0x80000000u;
  const uint32_t last = address + (length - 1);
  uint32_t lo = 0, hi = m_module->num_functions;
  while (lo < hi)
  {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (m_module->function_ranges[mid].end <= address)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (uint32_t i = lo; i < m_module->num_functions && m_module->function_ranges[i].start <= last; ++i)
    if (m_function_state[i] != FUNCTION_UNVERIFIED)
      SetFunctionState(i, FUNCTION_UNVERIFIED);
}

void Cpu::ResetCodeState()
{
  if (!m_module)
    return;
  for (uint32_t i = 0; i < m_module->num_functions; ++i)
    SetFunctionState(i, FUNCTION_UNVERIFIED);
}

// --- Time ---------------------------------------------------------------------

uint64_t Cpu::Timebase() const
{
  return m_tb_start_value + (m_scheduler.Now() - m_tb_start_ticks) / Scheduler::TIMER_RATIO;
}

void Cpu::SetTimebase(uint64_t value)
{
  m_tb_start_value = value;
  m_tb_start_ticks = m_scheduler.Now();
}

void Cpu::Charge(int64_t cycles)
{
  state.downcount -= cycles;
}

void Cpu::RequestInterruptCheck()
{
  // End the slice now: generated code leaves at its next loop or exit, and
  // the run loop delivers the interrupt.
  if ((state.msr & MSR_EE) && (m_external || m_decrementer))
    m_scheduler.ForceCheck(0);
}

uint32_t Cpu::Decrementer() const
{
  return m_dec_start_value -
         static_cast<uint32_t>((m_scheduler.Now() - m_dec_start_ticks) / Scheduler::TIMER_RATIO);
}

void Cpu::DecrementerSet()
{
  m_scheduler.Remove(m_dec_event);
  const uint32_t value = m_spr[SPR_DEC];
  if (value & 0x80000000u)
    return;
  m_dec_start_ticks = m_scheduler.Now();
  m_dec_start_value = value;
  m_scheduler.Schedule(static_cast<int64_t>(value) * Scheduler::TIMER_RATIO, m_dec_event);
}

void Cpu::SetExternalInterrupt(bool asserted)
{
  m_external = asserted;
  RequestInterruptCheck();
}

void Cpu::DecrementerEvent(void* user, uint64_t, int64_t)
{
  Cpu& cpu = *static_cast<Cpu*>(user);
  cpu.m_spr[SPR_DEC] = 0xFFFFFFFF;
  cpu.m_decrementer = true;
}

// --- Exceptions ---------------------------------------------------------------

void Cpu::TakeInterrupt(uint32_t vector)
{
  state.srr0 = state.pc;
  state.srr1 = state.msr & SRR1_MASK;
  state.msr = (state.msr & ~(EXCEPTION_MSR_CLEAR | 1u)) | ((state.msr & MSR_ILE) ? 1u : 0u);
  state.pc = vector;
}

void Cpu::CheckInterrupts()
{
  if (!(state.msr & MSR_EE))
    return;
  if (m_external)
  {
    TakeInterrupt(0x500);
  }
  else if (m_decrementer)
  {
    m_decrementer = false;
    TakeInterrupt(0x900);
  }
}

// --- Run loop -----------------------------------------------------------------

bool Cpu::RunNative(int function)
{
  do
  {
    const uint32_t pc = state.pc;
    const int64_t downcount_before = state.downcount;
    m_module->functions[function](&state);
    // Nothing ran: the entry switch has no case for this pc (an exception
    // return, a computed pointer), so the interpreter steps it.
    if (state.pc == pc && state.downcount == downcount_before && !state.exception)
      return false;
    ++m_stats.dispatches;
    // Every dispatch costs at least a cycle, so a burst always moves time.
    if (state.downcount == downcount_before)
      Charge(1);
    if (state.exception)
    {
      // The runtime already moved pc/msr/srr to the vector.
      state.exception = 0;
      state.program_exception = 0;
    }
    if ((m_external || m_decrementer) && (state.msr & MSR_EE))
      CheckInterrupts();
    if (state.pc == m_idle_pc && m_idle_pc)
      break;
    function = FunctionAt(state.pc);
  } while (state.downcount > 0 && function >= 0 && m_function_state[function] == FUNCTION_VERIFIED &&
           m_running);
  return true;
}

void Cpu::Run()
{
  m_running = true;
  while (m_running)
  {
    m_scheduler.Advance();
    m_machine.AfterEvents();
    CheckInterrupts();
    if (m_idle_pc && state.pc == m_idle_pc)
    {
      // The idle thread only wakes sleeping threads to poll, and nothing they
      // poll changes before the next event.
      m_machine.BeforeIdle();
      m_scheduler.Idle();
      continue;
    }
    while (state.downcount > 0 && m_running)
    {
      const int function = m_module ? Dispatchable(state.pc) : NO_FUNCTION;
      if (function >= 0)
      {
        if (!RunNative(function))
          ++m_stats.entry_misses;
        else if (m_idle_pc && state.pc == m_idle_pc)
          break;
        else
          continue;
      }
      Charge(Step());
      CheckInterrupts();
      if (m_idle_pc && state.pc == m_idle_pc)
        break;
    }
  }
}

void Cpu::RunFunction(uint32_t address)
{
  state.pc = address;
  state.lr = 0;
  while (state.pc != 0)
  {
    Charge(Step());
    if (state.downcount <= 0)
      m_scheduler.Advance();
  }
}

// --- Gather pipe --------------------------------------------------------------

size_t Cpu::CurrentGatherSize() const
{
  return static_cast<size_t>(m_gather_ptr - m_gather);
}

void Cpu::GatherPipeWrite(uint64_t value, uint32_t size)
{
  uint8_t* p = m_gather_ptr;
  switch (size)
  {
  case 1: *p = static_cast<uint8_t>(value); break;
  case 2: Store16(p, static_cast<uint16_t>(value)); break;
  case 4: Store32(p, static_cast<uint32_t>(value)); break;
  default: Store64(p, value); break;
  }
  m_gather_ptr = p + size;
  FlushGatherPipe();
}

void Cpu::FlushGatherPipe()
{
  const size_t count = CurrentGatherSize();
  const size_t bursts = count & ~size_t{31};
  if (!bursts)
    return;
  m_machine.GatherPipeBursts(m_gather, bursts);
  std::memmove(m_gather, m_gather + bursts, count - bursts);
  m_gather_ptr = m_gather + (count - bursts);
}

// --- Hooks --------------------------------------------------------------------

uint64_t Cpu::HookRead(CPUState* state, uint32_t ea, uint8_t size)
{
  Cpu& cpu = CpuOf(state);
  if (ea - Memory::L1_BASE < Memory::L1_SIZE && size <= Memory::L1_BASE + Memory::L1_SIZE - ea)
  {
    const uint8_t* p = cpu.m_machine.memory.l1 + (ea - Memory::L1_BASE);
    switch (size)
    {
    case 1: return *p;
    case 2: return Load16(p);
    case 4: return Load32(p);
    default: return Load64(p);
    }
  }
  if (size == 8)
    return static_cast<uint64_t>(HookRead(state, ea, 4)) << 32 | HookRead(state, ea + 4, 4);
  return cpu.m_machine.MmioRead(ea & 0x1FFFFFFF, size, state->pc);
}

void Cpu::HookWrite(CPUState* state, uint32_t ea, uint64_t value, uint8_t size)
{
  Cpu& cpu = CpuOf(state);
  if ((ea & 0xFFFFF000u) == 0xCC008000u)
  {
    cpu.GatherPipeWrite(value, size);
    return;
  }
  if (ea - Memory::L1_BASE < Memory::L1_SIZE && size <= Memory::L1_BASE + Memory::L1_SIZE - ea)
  {
    uint8_t* p = cpu.m_machine.memory.l1 + (ea - Memory::L1_BASE);
    switch (size)
    {
    case 1: *p = static_cast<uint8_t>(value); return;
    case 2: Store16(p, static_cast<uint16_t>(value)); return;
    case 4: Store32(p, static_cast<uint32_t>(value)); return;
    default: Store64(p, value); return;
    }
  }
  if (size == 8)
  {
    HookWrite(state, ea, value >> 32, 4);
    HookWrite(state, ea + 4, static_cast<uint32_t>(value), 4);
    return;
  }
  cpu.m_machine.MmioWrite(ea & 0x1FFFFFFF, static_cast<uint32_t>(value), size, state->pc);
}

uint32_t Cpu::HookRead32(CPUState* state, uint32_t ea, uint8_t)
{
  return static_cast<uint32_t>(HookRead(state, ea, 4));
}

void Cpu::HookWrite32(CPUState* state, uint32_t ea, uint32_t value, uint8_t)
{
  HookWrite(state, ea, value, 4);
}

void* Cpu::HookPointer(CPUState* state, uint32_t ea, uint32_t size)
{
  if (size && ea - Memory::L1_BASE < Memory::L1_SIZE && size <= Memory::L1_BASE + Memory::L1_SIZE - ea)
    return CpuOf(state).m_machine.memory.l1 + (ea - Memory::L1_BASE);
  return nullptr;
}

uint32_t Cpu::HookSprRead(CPUState* state, uint16_t spr, uint32_t cia)
{
  Cpu& cpu = CpuOf(state);
  if (spr >= 1024)
  {
    ppc_program_exception(state, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
  }
  switch (spr)
  {
  case SPR_DEC:
    if ((cpu.m_spr[SPR_DEC] & 0x80000000u) == 0)
      cpu.m_spr[SPR_DEC] = cpu.Decrementer();
    break;
  case SPR_WPAR:
    // BNE: the pipe holds data (bit 0).
    cpu.m_spr[SPR_WPAR] = (cpu.m_spr[SPR_WPAR] & ~1u) | (cpu.GatherPipeEmpty() ? 0u : 1u);
    break;
  case SPR_UPMC1: return cpu.m_spr[SPR_PMC1];
  case SPR_UPMC2: return cpu.m_spr[SPR_PMC2];
  case SPR_UPMC3: return cpu.m_spr[SPR_PMC3];
  case SPR_UPMC4: return cpu.m_spr[SPR_PMC4];
  case SPR_IABR: return cpu.m_spr[SPR_IABR] & ~1u;
  default: break;
  }
  return cpu.m_spr[spr];
}

void Cpu::HookSprWrite(CPUState* state, uint16_t spr, uint32_t value, uint32_t cia)
{
  Cpu& cpu = CpuOf(state);
  if (spr >= 1024)
  {
    ppc_program_exception(state, PPC_PROGRAM_ILLEGAL, cia);
    return;
  }
  const uint32_t old_value = cpu.m_spr[spr];
  cpu.m_spr[spr] = value;
  switch (spr)
  {
  case SPR_TL_W:
    cpu.SetTimebase((cpu.Timebase() & 0xFFFFFFFF00000000ull) | value);
    return;
  case SPR_TU_W:
    cpu.SetTimebase(static_cast<uint64_t>(value) << 32 | (cpu.Timebase() & 0xFFFFFFFFu));
    return;
  case SPR_PVR:
    cpu.m_spr[SPR_PVR] = old_value;
    return;
  case SPR_HID0:
    // ICFI (flash invalidate) self-clears; module functions follow icbi and the
    // per-line invalidations, never the whole-cache flash.
    cpu.m_spr[SPR_HID0] &= ~0x800u;
    return;
  case SPR_HID1:
    cpu.m_spr[SPR_HID1] &= 0xF8000000u;
    return;
  case SPR_WPAR:
    cpu.ResetGatherPipe();
    return;
  case SPR_DMAL:
    cpu.LockedCacheDma();
    return;
  case SPR_DEC:
    if ((old_value >> 31) == 0 && (value >> 31) != 0)
    {
      cpu.m_decrementer = true;
      cpu.RequestInterruptCheck();
    }
    cpu.DecrementerSet();
    return;
  case SPR_THRM1:
  case SPR_THRM2:
  case SPR_THRM3:
  {
    // Thermal assist: report 42 degrees against the threshold, as Dolphin.
    const bool enabled = cpu.m_spr[SPR_THRM3] & 1u;
    for (uint32_t reg : {SPR_THRM1, SPR_THRM2})
    {
      uint32_t& thrm = cpu.m_spr[reg];
      const bool valid = thrm & 1u;
      thrm &= ~0xC0000000u;
      if (enabled && valid)
      {
        const uint32_t threshold = (thrm >> 23) & 0x7F;
        const bool tid = (thrm >> 2) & 1u;
        thrm |= 0x40000000u | ((tid ? 42u < threshold : 42u > threshold) ? 0x80000000u : 0u);
      }
    }
    return;
  }
  default:
    return;
  }
}

void Cpu::LockedCacheDma()
{
  uint32_t& dmal = m_spr[SPR_DMAL];
  const uint32_t dmau = m_spr[SPR_DMAU];
  if (dmal & 2u)  // DMA_T
  {
    const uint32_t mem_address = dmau & ~31u;
    const uint32_t cache_address = dmal & ~31u;
    uint32_t length = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
    if (length == 0)
      length = 128;
    const uint32_t bytes = length * 32;
    // The DMA engine sees physical addresses: the SDK passes either form.
    uint8_t* ram = m_machine.memory.Physical(mem_address & 0x1FFFFFFF, bytes);
    const uint32_t offset = cache_address - Memory::L1_BASE;
    if (ram && offset < Memory::L1_SIZE && bytes <= Memory::L1_SIZE - offset)
    {
      uint8_t* cache = m_machine.memory.l1 + offset;
      if (dmal & 0x10u)  // DMA_LD: memory to cache
        std::memcpy(cache, ram, bytes);
      else
        std::memcpy(ram, cache, bytes);
    }
  }
  dmal &= ~2u;
}

void Cpu::HookCacheControl(CPUState* state, uint8_t operation, uint32_t ea, uint32_t)
{
  Cpu& cpu = CpuOf(state);
  // Without data cache emulation a block op only matters to code: each drops
  // the icache line, which lines holding no module code can skip.
  if (operation == PPC_CACHE_ICBI || cpu.LineMayHoldCode(ea))
    cpu.InvalidateICache(ea & ~31u, 32);
}

void Cpu::HookCacheRange(CPUState* state, uint8_t operation, uint32_t ea, uint32_t lines)
{
  Cpu& cpu = CpuOf(state);
  for (uint32_t i = 0; i < lines; ++i)
  {
    const uint32_t line = ea + i * 32u;
    if (operation == PPC_CACHE_ICBI || cpu.LineMayHoldCode(line))
      cpu.InvalidateICache(line & ~31u, 32);
  }
}

uint64_t Cpu::HookReadTimebase(CPUState* state)
{
  return CpuOf(state).Timebase();
}

void Cpu::HookMsrWritten(CPUState* state)
{
  CpuOf(state).RequestInterruptCheck();
}

void Cpu::HookFallback(CPUState* state, uint32_t raw, uint32_t cia)
{
  Cpu& cpu = CpuOf(state);
  ++cpu.m_stats.fallbacks;
  state->pc = cia;
  cpu.Charge(cpu.Execute(raw, cia));
}

// --- Savestate ----------------------------------------------------------------

void Cpu::DoState(StateIO& io)
{
  io.Do(state.gpr);
  io.Do(state.fpr);
  io.Do(state.ps1);
  io.Do(state.pc);
  io.Do(state.lr);
  io.Do(state.ctr);
  io.Do(state.cr);
  io.Do(state.xer);
  io.Do(state.fpscr);
  io.Do(state.msr);
  io.Do(state.srr0);
  io.Do(state.srr1);
  io.Do(state.dar);
  io.Do(state.dsisr);
  io.Do(state.ear);
  io.Do(state.hid2);
  io.Do(state.sr);
  io.Do(state.gqr);
  io.Do(state.reserve_addr);
  io.Do(state.reserve_valid);
  io.Do(state.locked_cache_tag);
  io.Do(state.locked_cache_valid);
  io.Do(m_spr);
  io.Do(m_tb_start_value);
  io.Do(m_tb_start_ticks);
  io.Do(m_dec_start_value);
  io.Do(m_dec_start_ticks);
  io.Do(m_decrementer);
  uint32_t gather = static_cast<uint32_t>(CurrentGatherSize());
  io.Do(gather);
  io.Bytes(m_gather, sizeof(m_gather));
  if (io.Reading())
  {
    m_gather_ptr = m_gather + std::min<uint32_t>(gather, 31);
    state.exception = 0;
    state.program_exception = 0;
    ppc_fpscr_control_updated(&state);
    ResetCodeState();
  }
  io.Marker(0xC0DE0001);
}
}  // namespace dualize
