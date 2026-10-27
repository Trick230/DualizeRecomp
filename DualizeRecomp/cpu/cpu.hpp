#pragma once

// The Broadway CPU: recompiled code where it covers the pc and its
// chunk still holds the code it was built from (verified by hash on entry,
// reset by icache invalidation), the interpreter everywhere else (exception
// vector stubs, the apploader, code the module does not know). Both run on
// one CPUState; this class owns the SPRs the module reaches through hooks,
// the timebase and decrementer, and exception delivery.

#include "cpu/guest_module.h"
#include "kernel/scheduler.hpp"
#include "state.hpp"

#include <cstdint>
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

  // Asserted by the processor interface while any unmasked cause is set.
  void SetExternalInterrupt(bool asserted) { m_external = asserted; }

  // Code at [address, address + length) may have changed.
  void InvalidateICache(uint32_t address, uint32_t length);

  uint64_t Timebase() const;
  void SetTimebase(uint64_t value);
  uint32_t& Spr(uint32_t index) { return m_spr[index & 1023]; }

  void DoState(StateIO& io);
  // After loading a state: chunk verification starts over.
  void ResetCodeState();

  // Statistics for the shutdown report.
  struct Stats
  {
    uint64_t dispatches = 0;
    uint64_t interpreted = 0;
    uint64_t entry_misses = 0;
    uint64_t fallbacks = 0;
    uint64_t verifications = 0;
  };
  const Stats& GetStats() const { return m_stats; }

  // Gather pipe: the module appends short stores itself; a store that fills a
  // burst comes through HookExternalWrite.
  void GatherPipeWrite(uint64_t value, uint32_t size);
  void ResetGatherPipe() { m_gather_ptr = m_gather; }
  bool GatherPipeEmpty() const { return CurrentGatherSize() == 0; }

  alignas(64) CPUState state{};

private:
  enum ChunkState : uint8_t
  {
    CHUNK_UNVERIFIED,
    CHUNK_VERIFIED,
    CHUNK_FAILED,
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
  void TakeInterrupt(uint32_t vector);

  bool Dispatchable(uint32_t pc);
  bool FastDispatchable(uint32_t pc) const
  {
    const int chunk = ChunkAt(pc);
    return chunk >= 0 && m_chunk_state[chunk] == CHUNK_VERIFIED;
  }
  int ChunkAt(uint32_t pc) const;
  int ChunkAtSplitLine(uint32_t pc) const;
  void VerifyChunk(uint32_t index);
  void SetChunkState(uint32_t index, ChunkState chunk_state);
  bool LineMayHoldCode(uint32_t ea) const;
  void BuildChunkTables();
  void PublishDirectCalls(bool enable);
  // Runs native code from state.pc until it leaves verified code or the slice
  // ends; false if the module had no entry for the pc.
  bool RunNative();
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
  uint64_t m_tb_remainder = 0;  // ticks since the last whole timebase step
  uint32_t m_dec_start_value = 0xFFFFFFFF;
  uint64_t m_dec_start_ticks = 0;

  // 128 bytes so a burst-completing store of any size still fits.
  alignas(32) uint8_t m_gather[128] = {};
  uint8_t* m_gather_ptr = m_gather;

  const GuestModule* m_module = nullptr;

  std::vector<uint8_t> m_chunk_state;
  std::vector<uint8_t> m_direct_call_ok;
  static constexpr uint32_t DIRECT_LINE_SHIFT = GUEST_DIRECT_LINE_SHIFT;
  std::vector<uint8_t> m_direct_lines;
  // Chunk of each 128-byte line of MEM1+MEM2: the index when one chunk covers
  // the whole line, NO_CHUNK when none touches it, SPLIT_LINE otherwise.
  static constexpr uint32_t CHUNK_LINE_SHIFT = 7;
  static constexpr int16_t NO_CHUNK = -1;
  static constexpr int16_t SPLIT_LINE = -2;
  std::vector<int16_t> m_chunk_lines;
  // One bit per 32-byte line: set where the line overlaps module code.
  std::vector<uint64_t> m_code_lines;
  uint32_t m_idle_pc = 0;

  Stats m_stats;
};
}  // namespace dualize
