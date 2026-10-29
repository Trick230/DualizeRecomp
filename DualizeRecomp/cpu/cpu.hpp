#pragma once

// The Broadway CPU: recompiled code where it covers the pc and its function
// still holds the code it was built from (verified by hash on entry, reset by
// icache invalidation), the interpreter everywhere else (code the module does
// not know). Both run on one CPUState; this class owns the SPRs the module
// reaches through hooks, the timebase and decrementer, and exception delivery.
//
// Interrupts enter the OS's handlers natively when its vector stubs are in
// place, and guest threads run on host fibers keyed by their OSContext.

#include "core/guest_module.h"
#include "kernel/memory.hpp"
#include "kernel/scheduler.hpp"
#include "state.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace dualize
{
// What the CPU needs from the machine around it. Machine derives from CpuHost
// and defines these members (one per process, so the calls bind at link time
// and inline under LTO); a test can define its own.
class CpuHost
{
public:
  // A load or store outside RAM and the locked cache: the hardware window, at
  // its physical address.
  uint32_t MmioRead(uint32_t physical, uint32_t size, uint32_t pc);
  void MmioWrite(uint32_t physical, uint32_t value, uint32_t size, uint32_t pc);
  // Whole 32-byte bursts of the write gather pipe (0xCC008000).
  void GatherPipeBursts(const uint8_t* data, size_t size);
  // After the run loop ran the due events.
  void AfterEvents();
  // The same at a safepoint, with generated code on the stack: false if the
  // CPU must leave for the run loop (to run AfterEvents' remaining work).
  bool AfterEventsInPlace();
  // The CPU reached the idle loop and waits for the next event.
  void BeforeIdle();

protected:
  CpuHost() = default;
  ~CpuHost() = default;
};

class Cpu
{
public:
  Cpu(CpuHost& host, const Memory& memory, Scheduler& scheduler);
  ~Cpu();
  Cpu(const Cpu&) = delete;
  Cpu& operator=(const Cpu&) = delete;

  // Runs the recompiled code where it covers the pc; false (with error set)
  // if its tables are malformed. Without it everything runs on the interpreter.
  // idle_pc (0: none) is the OS idle thread's loop: the CPU waits there for
  // the next event instead of spinning.
  bool AttachModule(uint32_t idle_pc, std::string* error);
  bool HasModule() const { return m_module != nullptr; }

  // Runs until Stop() (called from the CPU thread, e.g. by an event).
  void Run();
  void Stop() { m_running = false; }
  bool Running() const { return m_running; }

  // Calls the guest function at address with args in r3.. and runs it to its
  // return, recompiled where possible; returns its r3. Every register comes
  // back as it was, so host code (a native, an OS handler) can call the SDK
  // in the middle of its own work. The callee must return to its caller: no
  // thread switch, which holds inside an OS interrupt handler (the scheduler
  // is off there). r1 must leave the callee room below it.
  uint32_t CallGuest(uint32_t address, std::initializer_list<uint32_t> args = {});

  CpuHost& Host() { return m_host; }

  // Asserted by the processor interface while any unmasked cause is set.
  void SetExternalInterrupt(bool asserted);

  // Code at [address, address + length) may have changed: by an icbi, a data
  // cache op or a DMA. True if it retired code that had run (verified).
  bool InvalidateICache(uint32_t address, uint32_t length);

  uint64_t Timebase() const;
  void SetTimebase(uint64_t value);
  uint32_t& Spr(uint32_t index) { return m_spr[index & 1023]; }
  // mtdec from host code (the OS's alarm timer): the decrementer interrupt
  // comes after value timebase ticks.
  void SetDecrementer(uint32_t value);

  void DoState(StateIO& io);
  // After loading a state: function verification starts over.
  void ResetCodeState();

  // Statistics for the shutdown report.
  struct Stats
  {
    uint64_t dispatches = 0;
    uint64_t interpreted = 0;
    uint64_t entry_misses = 0;
    uint64_t fallbacks = 0;
    uint64_t verifications = 0;
    uint64_t safepoints = 0;
    uint64_t native_exceptions = 0;
    uint64_t guest_calls = 0;
  };
  const Stats& GetStats() const { return m_stats; }

  // Gather pipe: the module appends short stores itself; a store that fills a
  // burst comes through HookExternalWrite.
  void GatherPipeWrite(uint64_t value, uint32_t size);
  void ResetGatherPipe() { m_gather_ptr = m_gather; }
  bool GatherPipeEmpty() const { return CurrentGatherSize() == 0; }

  alignas(64) CPUState state{};

  // The verified function holding pc, verifying it first if it never was;
  // NULL if none does or its guest text changed.
  GuestCode Lookup(uint32_t pc);
  // Generated code ran its slice out at a loop about to go to pc: runs the
  // events due there, and an interrupt in place when it can; true if it must
  // return to the run loop at state.pc instead of going on (a handler that
  // switched threads, the idle loop, a stop or other request).
  bool Safepoint(uint32_t pc);
  // A native loaded the OSContext at `context` (OSLoadContext or a thread
  // switch). Another thread's context parks this host fiber and resumes the
  // one that thread parked at the same point, if any: the call returns once
  // this fiber's own thread is loaded again.
  void ContextLoaded(uint32_t context);
  // Runs the code at state.pc, function by function, until it returns to pc:
  // a native that resumed its own thread finishes the guest call it was in,
  // so the caller's host frame goes on inline. Gives up (the caller then
  // leaves for the dispatcher) if it doesn't get there soon.
  void RunTo(uint32_t pc);

private:
  enum FunctionState : uint8_t
  {
    FUNCTION_UNVERIFIED,
    FUNCTION_VERIFIED,
    FUNCTION_FAILED,
  };

  // One instruction at state.pc on the interpreter; returns its cycles.
  int Step();
  int Execute(uint32_t raw, uint32_t cia);
  bool Fetch(uint32_t pc, uint32_t* raw) const;
  // An effective data address as the RAM helpers see it (real mode maps
  // physical memory through the uncached mirror).
  uint32_t DataAddress(uint32_t ea) const
  {
    return (state.msr & 0x10u) || ea >= 0x20000000u ? ea : ea | 0xC0000000u;
  }

  // Delivers a pending external or decrementer interrupt if MSR[EE] allows.
  void CheckInterrupts();
  // Ends the slice early if an interrupt is pending and unmasked.
  void RequestInterruptCheck();
  void TakeInterrupt(uint32_t vector);
  // At a safepoint with an interrupt to take: runs its handler nested there,
  // up to the context load that ends it. True if that resumed the code at the
  // safepoint, which then goes on in place; false leaves for the run loop,
  // which goes on wherever the handler left the CPU.
  bool InterruptInPlace(uint32_t pc);
  // The run loop proper, on the running fiber.
  void RunLoop();
  // Enters the OS handler of exception `number` (4 external, 7 FP
  // unavailable, 8 decrementer) natively when its vector holds the OS's stub;
  // else pc stays at the vector.
  void EnterOsHandler(uint32_t number);
  // The same for an exception the code just raised (pc at its vector).
  void EnterException();

  // The function holding pc, verified first if it never was, or NO_FUNCTION
  // if none does or it does not match guest RAM.
  int Dispatchable(uint32_t pc);
  // The same with the function's code, from the dispatch cache.
  // Published to generated code as dolrecomp_lookup_cache.
  using DispatchEntry = DolRecompLookupEntry;
  const DispatchEntry* Dispatch(uint32_t pc);
  int FunctionAt(uint32_t pc) const;
  void VerifyFunction(uint32_t index);
  void SetFunctionState(uint32_t index, FunctionState function_state);
  void FlushDispatchCache();
  bool LineMayHoldCode(uint32_t ea) const;
  void BuildFunctionTables();
  void PublishDirectCalls(bool enable);
  // Runs native code from function, which holds state.pc, until it leaves
  // verified code or the slice ends; false if it had no entry for the pc.
  bool RunNative(int function);
  void Charge(int64_t cycles);

  void DecrementerSet();
  uint32_t Decrementer() const;
  static void DecrementerEvent(void* user, uint64_t, int64_t);

  size_t CurrentGatherSize() const;
  void FlushGatherPipe();

  // CPUState hooks; state.external_user_data is the Cpu.
  static uint64_t HookRead(CPUState* cpu, uint32_t ea, uint8_t size);
  static void HookWrite(CPUState* cpu, uint32_t ea, uint64_t value, uint8_t size);
  static uint32_t HookRead32(CPUState* cpu, uint32_t ea, uint8_t rid);
  static void HookWrite32(CPUState* cpu, uint32_t ea, uint32_t value, uint8_t rid);
  static void* HookPointer(CPUState* cpu, uint32_t ea, uint32_t size);
  static uint32_t HookSprRead(CPUState* cpu, uint16_t spr, uint32_t cia);
  static void HookSprWrite(CPUState* cpu, uint16_t spr, uint32_t value, uint32_t cia);
  static void HookCacheControl(CPUState* cpu, uint8_t operation, uint32_t ea, uint32_t cia);
  static bool HookCacheRange(CPUState* cpu, uint8_t operation, uint32_t ea, uint32_t lines);
  static void HookFallback(CPUState* cpu, uint32_t raw, uint32_t cia);
  static uint64_t HookReadTimebase(CPUState* cpu);
  static void HookMsrWritten(CPUState* cpu);

  void LockedCacheDma();

  CpuHost& m_host;
  const Memory& m_memory;
  Scheduler& m_scheduler;
  bool m_running = false;
  bool m_external = false;
  bool m_decrementer = false;
  int m_dec_event = -1;

  uint32_t m_spr[1024] = {};
  // Timebase = m_tb_start_value + (Now - m_tb_start_ticks) / TIMER_RATIO.
  uint64_t m_tb_start_value = 0;
  uint64_t m_tb_start_ticks = 0;
  uint32_t m_dec_start_value = 0xFFFFFFFF;
  uint64_t m_dec_start_ticks = 0;

  // 128 bytes so a burst-completing store of any size still fits.
  alignas(32) uint8_t m_gather[128] = {};
  uint8_t* m_gather_ptr = m_gather;

  const GuestModule* m_module = nullptr;

  std::vector<uint8_t> m_function_state;
  std::vector<uint8_t> m_direct_call_ok;  // FUNCTION_VERIFIED, for generated code
  static constexpr uint32_t FUNCTION_LINE_SHIFT = 5;
  static constexpr int NO_FUNCTION = -1;
  // The resident functions, then each overlay's: functions [first, end), and
  // per 32-byte line from base to the end of their MEM1 code the first
  // function ending past the line's start, or NO_FUNCTION if none starts
  // before its end.
  struct FunctionGroup
  {
    uint32_t first;
    uint32_t end;
    uint32_t base;
    std::vector<int32_t> lines;
  };
  std::vector<FunctionGroup> m_groups;
  // Direct-mapped by pc, ahead of the line table. Entries only name verified
  // functions, so it empties whenever one stops being verified.
  static constexpr uint32_t DISPATCH_CACHE_SIZE = 4096;
  std::array<DispatchEntry, DISPATCH_CACHE_SIZE> m_dispatch_cache;
  // One bit per 32-byte line: set where the line overlaps module code.
  std::vector<uint64_t> m_code_lines;
  uint32_t m_idle_pc = 0;
  uint64_t m_context_loads = 0;
  // Inside InterruptInPlace: the handler's run ends at its context load.
  bool m_in_handler = false;
  uint64_t m_handler_loads = 0;
  uint32_t m_guest_calls = 0;  // CallGuest depth

  // Guest threads on host fibers (Windows; elsewhere every switch unwinds to
  // the dispatcher). Each fiber runs one guest thread, keyed by its OSContext
  // (0 until a switch names it); a parked one waits to be resumed where its
  // thread's saved context says it stopped.
  struct Fiber;
  Fiber* NewFiber(uint32_t context);
  void SwitchTo(Fiber* fiber);
  // After a state loads: the parked fibers' stacks belong to the old state.
  void DropFibers();
  static void FiberMain(Cpu& cpu);
  std::vector<std::unique_ptr<Fiber>> m_fibers;
  Fiber* m_fiber = nullptr;  // the running one, inside Run
  void* m_home = nullptr;    // the host thread's own fiber, which Run returns on

  Stats m_stats;
};
}  // namespace dualize
