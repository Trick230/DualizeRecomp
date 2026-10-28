#include "core/cpu.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

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

extern "C" int dolrecomp_safepoint(CPUState* ctx, u32 pc)
{
  return CpuOf(ctx).Safepoint(pc);
}

extern "C" void dualize_context_loaded(CPUState* ctx, u32 context)
{
  CpuOf(ctx).ContextLoaded(context);
}

extern "C" void dualize_run_to(CPUState* ctx, u32 pc)
{
  CpuOf(ctx).RunTo(pc);
}

// DolRecomp's direct-call depth: each host stack has its own.
extern "C" unsigned dolrecomp_call_depth;

struct Cpu::Fiber
{
  void* handle = nullptr;
  uint32_t context = 0;
  // Parked: where its thread's saved context resumes.
  uint32_t pc = 0, sp = 0;
  // The run's per-stack state while it is parked.
  unsigned call_depth = 0;
  uint32_t guest_calls = 0;
  bool in_handler = false;
  uint64_t handler_loads = 0;
};

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
  DropFibers();
#ifdef _WIN32
  for (const std::unique_ptr<Fiber>& fiber : m_fibers)
    DeleteFiber(fiber->handle);
  if (m_home)
    ConvertFiberToThread();
#endif
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
  FlushDispatchCache();
  m_idle_pc = m_machine.config.idle_pc;
  PublishDirectCalls(true);
}

void Cpu::PublishDirectCalls(bool enable)
{
  if (!m_module)
    return;
  guest_set_direct_calls(enable ? m_direct_call_ok.data() : nullptr, m_idle_pc);
  dolrecomp_lookup_cache = enable ? m_dispatch_cache.data() : nullptr;
  dolrecomp_lookup_cache_mask = DISPATCH_CACHE_SIZE - 1;
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

void Cpu::FlushDispatchCache()
{
  // No instruction lives at an odd pc.
  m_dispatch_cache.fill({1, NO_FUNCTION, nullptr});
}

const Cpu::DispatchEntry* Cpu::Dispatch(uint32_t pc)
{
  DispatchEntry& entry = m_dispatch_cache[(pc >> 2) & (DISPATCH_CACHE_SIZE - 1)];
  if (entry.pc == pc)
    return &entry;
  const int function = FunctionAt(pc);
  if (function < 0)
    return nullptr;
  if (m_function_state[function] == FUNCTION_UNVERIFIED)
    VerifyFunction(static_cast<uint32_t>(function));
  if (m_function_state[function] != FUNCTION_VERIFIED)
    return nullptr;
  entry = {pc, function, m_module->functions[function]};
  return &entry;
}

int Cpu::Dispatchable(uint32_t pc)
{
  const DispatchEntry* entry = Dispatch(pc);
  return entry ? entry->function : NO_FUNCTION;
}

GuestCode Cpu::Lookup(uint32_t pc)
{
  const DispatchEntry* entry = Dispatch(pc);
  return entry ? entry->code : nullptr;
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
  bool flush = false;
  for (uint32_t i = lo; i < m_module->num_functions && m_module->function_ranges[i].start <= last; ++i)
    if (m_function_state[i] != FUNCTION_UNVERIFIED)
    {
      flush |= m_function_state[i] == FUNCTION_VERIFIED;
      SetFunctionState(i, FUNCTION_UNVERIFIED);
    }
  if (flush)
    FlushDispatchCache();
}

void Cpu::ResetCodeState()
{
  if (!m_module)
    return;
  for (uint32_t i = 0; i < m_module->num_functions; ++i)
    SetFunctionState(i, FUNCTION_UNVERIFIED);
  FlushDispatchCache();
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

void Cpu::SetDecrementer(uint32_t value)
{
  m_spr[SPR_DEC] = value;
  DecrementerSet();
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
  if (vector == 0x500 || vector == 0x900)
    EnterOsHandler(vector == 0x500 ? 4 : 8);
}

// The OS's external and decrementer vectors (OSExceptionInit copies one stub
// to each, patching the exception number) and the handler each enters through
// __OSExceptionTable (the same 20 instructions before a branch to
// OSDispatchInterrupt or DecrementerExceptionCallback).
namespace
{
constexpr uint32_t OS_VECTOR_STUB[] = {
    0x7C9043A6, 0x808000C0, 0x9064000C, 0x7C7042A6, 0x90640010, 0x90A40014, 0xA06401A2, 0x60630002,
    0xB06401A2, 0x7C600026, 0x90640080, 0x7C6802A6, 0x90640084, 0x7C6902A6, 0x90640088, 0x7C6102A6,
    0x9064008C, 0x7C7A02A6, 0x90640198, 0x7C7B02A6, 0x9064019C, 0x7C651B78, 0x60000000, 0x7C6000A6,
    0x60630030, 0x7C7B03A6, 0x38600000, 0x808000D4, 0x54A507BD, 0x40820014, 0x3CA08045, 0x38A5DA74,
    0x7CBA03A6, 0x4C000064, 0x546515BA, 0x80A53000, 0x7CBA03A6, 0x4C000064,
};
constexpr uint32_t OS_VECTOR_NUMBER_WORD = 26;  // li r3, <exception number>
constexpr uint32_t OS_SAVE_HANDLER[] = {
    0x90040000, 0x90240004, 0x90440008, 0xBCC40018, 0x7C11E2A6, 0x900401A8, 0x7C12E2A6,
    0x900401AC, 0x7C13E2A6, 0x900401B0, 0x7C14E2A6, 0x900401B4, 0x7C15E2A6, 0x900401B8,
    0x7C16E2A6, 0x900401BC, 0x7C17E2A6, 0x900401C0, 0x9421FFF8,
};
// The system call vector: mfspr r9, HID0; ori r10, r9, 8; mtspr HID0, r10;
// isync; sync; mtspr HID0, r9; rfi.
constexpr uint32_t OS_SYNC_HANDLER[] = {
    0x7D30FAA6, 0x612A0008, 0x7D50FBA6, 0x4C00012C, 0x7C0004AC, 0x7D30FBA6, 0x4C000064,
};
constexpr uint32_t OS_EXCEPTION_TABLE = 0x80003000;
constexpr uint32_t OS_CURRENT_CONTEXT = 0x800000D4;
constexpr uint32_t SPR_SPRG0 = 272;

// OSContext
constexpr uint32_t CONTEXT_CR = 0x80, CONTEXT_LR = 0x84, CONTEXT_CTR = 0x88, CONTEXT_XER = 0x8C;
constexpr uint32_t CONTEXT_SRR0 = 0x198, CONTEXT_SRR1 = 0x19C, CONTEXT_STATE = 0x1A2;
constexpr uint32_t CONTEXT_GQR = 0x1A4, CONTEXT_SIZE = 0x2C8;
constexpr uint16_t CONTEXT_STATE_EXC = 2;

bool CodeMatches(const uint8_t* code, const uint32_t* words, uint32_t count, uint32_t skip = ~0u)
{
  for (uint32_t i = 0; i < count; ++i)
    if (i != skip && Load32(code + i * 4) != words[i])
      return false;
  return true;
}
}  // namespace

void Cpu::EnterOsHandler(uint32_t number)
{
  // Does in one step what the vector stub (and, for interrupts, the save
  // handler) would do on the interpreter: the interrupted state (already in
  // srr0/srr1) goes to the current OSContext, and the OS's handler starts at
  // its first instruction. Anything unexpected leaves pc at the vector for the
  // interpreter.
  const Memory& memory = m_machine.memory;
  const uint32_t vector = state.pc;
  const uint8_t* stub = memory.Physical(vector, sizeof(OS_VECTOR_STUB));  // real mode
  if (!stub || !(state.srr1 & 2) ||  // RI clear: the stub takes the unrecoverable path
      !CodeMatches(stub, OS_VECTOR_STUB, std::size(OS_VECTOR_STUB), OS_VECTOR_NUMBER_WORD) ||
      Load32(stub + OS_VECTOR_NUMBER_WORD * 4) != (0x38600000u | number))
    return;
  const uint32_t handler = memory.Read32((OS_EXCEPTION_TABLE + number * 4) & 0x1FFFFFFF);
  const uint32_t context_address = memory.Read32(OS_CURRENT_CONTEXT & 0x1FFFFFFF);
  uint8_t* context = memory.Effective(context_address, CONTEXT_SIZE);
  if (!context)
    return;
  // An interrupt's handler saves the rest and branches on to the dispatcher.
  const uint8_t* code = memory.Effective(handler, sizeof(OS_SAVE_HANDLER) + 4);
  uint32_t target = handler;
  uint8_t* frame = nullptr;
  if (code && CodeMatches(code, OS_SAVE_HANDLER, std::size(OS_SAVE_HANDLER)))
  {
    const uint32_t branch = Load32(code + sizeof(OS_SAVE_HANDLER));
    frame = memory.Effective(state.gpr[1] - 8, 4);
    if ((branch & 0xFC000003u) != 0x48000000u || !frame)  // b, not linked, relative
      return;
    target = handler + sizeof(OS_SAVE_HANDLER) +
             static_cast<uint32_t>(static_cast<int32_t>(branch << 6) >> 6 & ~3);
  }

  for (uint32_t i = 3; i < 6; ++i)
    Store32(context + i * 4, state.gpr[i]);
  Store32(context + CONTEXT_CR, state.cr);
  Store32(context + CONTEXT_LR, state.lr);
  Store32(context + CONTEXT_CTR, state.ctr);
  Store32(context + CONTEXT_XER, state.xer);
  Store32(context + CONTEXT_SRR0, state.srr0);
  Store32(context + CONTEXT_SRR1, state.srr1);
  Store16(context + CONTEXT_STATE, Load16(context + CONTEXT_STATE) | CONTEXT_STATE_EXC);
  int64_t cycles = std::size(OS_VECTOR_STUB);
  if (frame)
  {
    for (uint32_t i = 0; i < 32; ++i)
      if (i < 3 || i > 5)
        Store32(context + i * 4, state.gpr[i]);
    for (uint32_t i = 1; i < 8; ++i)
      Store32(context + CONTEXT_GQR + i * 4, state.gqr[i]);
    Store32(frame, state.gpr[1]);  // stwu r1, -8(r1)
    state.gpr[1] -= 8;
    cycles += std::size(OS_SAVE_HANDLER) + 1;
  }

  // Registers as the stub (and handler) leave them.
  m_spr[SPR_SPRG0] = state.gpr[4];
  state.gpr[3] = number;
  state.gpr[4] = context_address;
  state.gpr[5] = handler;
  state.cr = (state.cr & 0x0FFFFFFFu) | (0x4u | (state.xer >> 31)) << 28;  // rlwinm. r5, srr1 & RI
  state.msr |= 0x30;  // the stub's rfi turns translation on
  state.pc = target;
  Charge(cycles);
  ++m_stats.native_exceptions;
}

void Cpu::EnterException()
{
  switch (state.pc)
  {
  case 0x800:  // floating point unavailable: OSSwitchFPUContext
    EnterOsHandler(7);
    return;
  case 0xC00:
  {
    // The OS's system call handler only orders memory (HID0[ABE] around a
    // sync); PPCSync and the cache range functions call it.
    const uint8_t* code = m_machine.memory.Physical(0xC00, sizeof(OS_SYNC_HANDLER));
    if (!code || !CodeMatches(code, OS_SYNC_HANDLER, std::size(OS_SYNC_HANDLER)))
      return;
    state.gpr[9] = m_spr[SPR_HID0];
    state.gpr[10] = m_spr[SPR_HID0] | 8;
    state.msr = (state.msr & ~SRR1_MASK) | (state.srr1 & SRR1_MASK);
    state.pc = state.srr0;
    Charge(std::size(OS_SYNC_HANDLER));
    ++m_stats.native_exceptions;
    return;
  }
  default:
    return;
  }
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
      EnterException();
    }
    if (m_in_handler && m_context_loads != m_handler_loads)
      break;
    if ((m_external || m_decrementer) && (state.msr & MSR_EE))
      CheckInterrupts();
    if (state.pc == m_idle_pc && m_idle_pc)
      break;
    function = FunctionAt(state.pc);
  } while (state.downcount > 0 && function >= 0 && m_function_state[function] == FUNCTION_VERIFIED &&
           m_running);
  return true;
}

bool Cpu::Safepoint(uint32_t pc)
{
  // As far as events can tell, the CPU is at the loop target.
  state.pc = pc;
  ++m_stats.safepoints;
  if (m_running && !(m_idle_pc && pc == m_idle_pc))
  {
    m_scheduler.Advance();
    if (m_machine.AfterEventsInPlace() && m_running &&
        (!((state.msr & MSR_EE) && (m_external || m_decrementer)) || InterruptInPlace(pc)))
      return false;
  }
  // Leaving: keep the slice ended. A safepoint at a function's return names
  // its caller's continuation, which the caller can't tell from a plain
  // return, so it runs on until its own next safepoint, which must leave too.
  m_scheduler.ForceCheck(0);
  return true;
}

bool Cpu::InterruptInPlace(uint32_t pc)
{
  // Inside a guest call the interrupt waits for the call's end, as it would
  // from the run loop.
  if (m_in_handler || m_guest_calls)
    return false;
  const uint32_t sp = state.gpr[1];
  CheckInterrupts();
  m_in_handler = true;
  m_handler_loads = m_context_loads;
  // A handler that turns interrupts back on before its end is left to the
  // run loop too.
  while (m_context_loads == m_handler_loads && !(state.msr & MSR_EE))
  {
    if (state.downcount <= 0)
      m_scheduler.Advance();
    const int function = m_module ? Dispatchable(state.pc) : NO_FUNCTION;
    if (function >= 0 && RunNative(function))
      continue;
    Charge(Step());
  }
  m_in_handler = false;
  return state.pc == pc && state.gpr[1] == sp && m_running;
}

// --- Fibers -------------------------------------------------------------------

Cpu::Fiber* Cpu::NewFiber(uint32_t context)
{
#ifdef _WIN32
  // A guest thread's direct calls nest as deep as on the host thread.
  constexpr size_t STACK_COMMIT = 256 * 1024, STACK_RESERVE = 32 * 1024 * 1024;
  void* handle = CreateFiberEx(
      STACK_COMMIT, STACK_RESERVE, 0, [](void* cpu) { FiberMain(*static_cast<Cpu*>(cpu)); }, this);
  if (!handle)
    return nullptr;
  m_fibers.push_back(std::make_unique<Fiber>());
  m_fibers.back()->handle = handle;
  m_fibers.back()->context = context;
  return m_fibers.back().get();
#else
  (void)context;
  return nullptr;
#endif
}

void Cpu::FiberMain(Cpu& cpu)
{
  for (;;)
  {
    cpu.RunLoop();
#ifdef _WIN32
    SwitchToFiber(cpu.m_home);
#endif
  }
}

void Cpu::SwitchTo(Fiber* fiber)
{
#ifdef _WIN32
  Fiber* self = m_fiber;
  self->call_depth = dolrecomp_call_depth;
  self->guest_calls = m_guest_calls;
  self->in_handler = m_in_handler;
  self->handler_loads = m_handler_loads;
  m_fiber = fiber;
  dolrecomp_call_depth = fiber->call_depth;
  m_guest_calls = fiber->guest_calls;
  m_in_handler = fiber->in_handler;
  m_handler_loads = fiber->handler_loads;
  SwitchToFiber(fiber->handle);
#else
  (void)fiber;
#endif
}

void Cpu::ContextLoaded(uint32_t context)
{
  ++m_context_loads;
  Fiber* self = m_fiber;
  if (!self || context == self->context)
    return;
  if (!self->context)
  {
    // A fiber nothing named yet carries on as this thread's.
    self->context = context;
    return;
  }
#ifdef _WIN32
  // The thread leaving resumes where its saved context says.
  constexpr uint32_t CONTEXT_R1 = 4, CONTEXT_SRR0 = 408;
  const uint8_t* saved = m_machine.memory.Effective(self->context, CONTEXT_SRR0 + 4);
  if (!saved)
  {
    self->context = context;
    return;
  }
  self->pc = Load32(saved + CONTEXT_SRR0) & ~3u;
  self->sp = Load32(saved + CONTEXT_R1);

  Fiber* target = nullptr;
  for (auto it = m_fibers.begin(); it != m_fibers.end(); ++it)
  {
    Fiber* fiber = it->get();
    if (fiber == self || fiber->context != context)
      continue;
    if (fiber->pc == state.pc && fiber->sp == state.gpr[1])
      target = fiber;
    else
    {
      // Its thread moved on without it (exited, or its context was rewritten).
      DeleteFiber(fiber->handle);
      m_fibers.erase(it);
    }
    break;
  }
  // Else the thread's first run on a fiber: from the dispatcher, at state.pc.
  if (!target && !(target = NewFiber(context)))
  {
    self->context = context;
    return;
  }
  SwitchTo(target);
#endif
}

void Cpu::DropFibers()
{
#ifdef _WIN32
  for (auto it = m_fibers.begin(); it != m_fibers.end();)
  {
    if (it->get() == m_fiber)
    {
      ++it;
      continue;
    }
    DeleteFiber((*it)->handle);
    it = m_fibers.erase(it);
  }
#endif
  if (m_fiber)
    m_fiber->context = 0;
}

void Cpu::RunTo(uint32_t pc)
{
  constexpr int MAX_DISPATCHES = 16;
  for (int i = 0; i < MAX_DISPATCHES && state.pc != pc && m_running; ++i)
  {
    if (state.downcount <= 0)
      m_scheduler.Advance();
    const int function = m_module ? Dispatchable(state.pc) : NO_FUNCTION;
    const uint32_t start = state.pc;
    const int64_t downcount_before = state.downcount;
    if (function >= 0)
      m_module->functions[function](&state);
    if (function < 0 || (state.pc == start && state.downcount == downcount_before && !state.exception))
      return;
    ++m_stats.dispatches;
    if (state.exception)
    {
      state.exception = 0;
      state.program_exception = 0;
      EnterException();
    }
  }
}

void Cpu::Run()
{
  m_running = true;
#ifdef _WIN32
  if (!m_home)
    m_home = ConvertThreadToFiber(nullptr);
  if (m_home && (m_fiber || (m_fiber = NewFiber(0))))
  {
    // Back here once the run loop stops, on whichever fiber.
    SwitchToFiber(m_fiber->handle);
    return;
  }
#endif
  RunLoop();
}

void Cpu::RunLoop()
{
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

uint32_t Cpu::CallGuest(uint32_t address, std::initializer_list<uint32_t> args)
{
  // A return address no guest code lives at.
  constexpr uint32_t CALL_RETURN = 0xFFFFFFF0u;
  // gpr through msr are contiguous in CPUState.
  constexpr size_t REGISTERS_BEGIN = offsetof(CPUState, gpr);
  constexpr size_t REGISTERS_END = offsetof(CPUState, msr) + sizeof(state.msr);
  uint8_t saved[REGISTERS_END - REGISTERS_BEGIN];
  uint8_t* registers = reinterpret_cast<uint8_t*>(&state) + REGISTERS_BEGIN;
  std::memcpy(saved, registers, sizeof(saved));
  const bool reserve_valid = state.reserve_valid;

  uint32_t reg = 3;
  for (uint32_t arg : args)
    state.gpr[reg++] = arg;
  state.lr = CALL_RETURN;
  state.pc = address;
  ++m_guest_calls;
  while (state.pc != CALL_RETURN)
  {
    if (state.downcount <= 0)
      m_scheduler.Advance();
    const int function = m_module ? Dispatchable(state.pc) : NO_FUNCTION;
    if (function >= 0 && RunNative(function))
      continue;
    Charge(Step());
  }

  --m_guest_calls;
  const uint32_t result = state.gpr[3];
  std::memcpy(registers, saved, sizeof(saved));
  state.reserve_valid = reserve_valid;
  ++m_stats.guest_calls;
  return result;
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
  if (operation == PPC_CACHE_ICBI)
  {
    cpu.InvalidateICache(ea & ~31u, lines * 32u);
    return;
  }
  // A data-cache op only matters on lines that hold code: the code bitmap
  // passes over a word of 64 clear lines (2 KB) at a time.
  for (uint32_t i = 0; i < lines;)
  {
    const uint32_t line = (ea & ~31u) + i * 32u;
    const int index = LookupIndex(line);
    const size_t bit = static_cast<size_t>(index) >> 3;
    if (index < 0 || (bit >> 6) >= cpu.m_code_lines.size())
    {
      ++i;
      continue;
    }
    const uint64_t code = cpu.m_code_lines[bit >> 6] >> (bit & 63);
    if (!code)
    {
      i += 64 - static_cast<uint32_t>(bit & 63);
      continue;
    }
    if (code & 1)
      cpu.InvalidateICache(line, 32);
    ++i;
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
    DropFibers();
  }
  io.Marker(0xC0DE0001);
}
}  // namespace dualize
