#pragma once

// The Broadway CPU: recompiled code where it covers the pc and its
// function still holds the code it was built from (verified by hash on entry,
// reset by icache invalidation), the interpreter everywhere else (exception
// vector stubs, the apploader, code the module does not know). Both run on
// one CPUState; this class owns the SPRs the module reaches through hooks,
// the timebase and decrementer, and exception delivery.

#include "cpu/guest_module.h"
#include "kernel/scheduler.hpp"
#include "state.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace dualize
{
class Machine;

class Cpu
{
public:
  explicit Cpu(Machine& machine);
  ~Cpu();
  Cpu(const Cpu&) = delete;
  Cpu& operator=(const Cpu&) = delete;

  // Runs the recompiled code where it covers the pc; false (with error set)
  // if its tables are malformed. Without it everything runs on the interpreter.
  bool AttachModule(std::string* error);
  bool HasModule() const { return m_module != nullptr; }

  // Runs until Stop() (called from the CPU thread, e.g. by an event).
  void Run();
  void Stop() { m_running = false; }
  bool Running() const { return m_running; }

  // Runs the function at address to its return (lr = 0) on the interpreter,
  // as the HLE boot calls the apploader.
  void RunFunction(uint32_t address);

  // Calls the guest function at address with args in r3.. and runs it to its
  // return, recompiled where possible; returns its r3. Every register comes
  // back as it was, so host code (a native, an OS handler) can call the SDK
  // in the middle of its own work. The callee must return to its caller: no
  // thread switch, which holds inside an OS interrupt handler (the scheduler
  // is off there). r1 must leave the callee room below it.
  uint32_t CallGuest(uint32_t address, std::initializer_list<uint32_t> args = {});

  Machine& GetMachine() { return m_machine; }

  // Asserted by the processor interface while any unmasked cause is set.
  void SetExternalInterrupt(bool asserted);

  // Code at [address, address + length) may have changed.
  void InvalidateICache(uint32_t address, uint32_t length);

  uint64_t Timebase() const;
  void SetTimebase(uint64_t value);
  uint32_t& Spr(uint32_t index) { return m_spr[index & 1023]; }

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
  // events due there; true if it must return to the run loop instead of
  // going on (an interrupt to take, the idle loop, a stop or other request).
  bool Safepoint(uint32_t pc);

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
  // Enters the OS handler of exception `number` (4 external, 8 decrementer)
  // natively when its vector holds the OS's stub; else pc stays at the vector.
  void EnterOsHandler(uint32_t number);

  // The function holding pc, verified first if it never was, or NO_FUNCTION
  // if none does or it does not match guest RAM.
  int Dispatchable(uint32_t pc);
  int FunctionAt(uint32_t pc) const;
  void VerifyFunction(uint32_t index);
  void SetFunctionState(uint32_t index, FunctionState function_state);
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
  static void HookCacheRange(CPUState* cpu, uint8_t operation, uint32_t ea, uint32_t lines);
  static void HookFallback(CPUState* cpu, uint32_t raw, uint32_t cia);
  static uint64_t HookReadTimebase(CPUState* cpu);
  static void HookMsrWritten(CPUState* cpu);

  void LockedCacheDma();

  Machine& m_machine;
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
  // Per 32-byte line of MEM1 up to the end of its code: the first function
  // ending past the line's start, or NO_FUNCTION if none starts before its end.
  static constexpr uint32_t FUNCTION_LINE_SHIFT = 5;
  static constexpr int NO_FUNCTION = -1;
  std::vector<int32_t> m_line_function;
  // One bit per 32-byte line: set where the line overlaps module code.
  std::vector<uint64_t> m_code_lines;
  uint32_t m_idle_pc = 0;

  Stats m_stats;
};
}  // namespace dualize
