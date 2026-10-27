#pragma once

// Guest time: one tick is one CPU cycle (729 MHz). The CPU runs in slices that
// end at the next event; events fire in time order, ties in scheduling order.

#include "state.hpp"

#include <cstdint>
#include <vector>

namespace dualize
{
class Scheduler
{
public:
  using Callback = void (*)(void* user, uint64_t userdata, int64_t late);
  static constexpr uint32_t CPU_CLOCK = 729000000;
  // Timebase and decrementer count once per this many CPU ticks.
  static constexpr uint32_t TIMER_RATIO = 12;

  // Event types are saved by index, so they must be registered in the same
  // order on every run.
  int Register(Callback callback, void* user);

  void Schedule(int64_t cycles_into_future, int type, uint64_t userdata = 0);
  // Schedules at an absolute tick.
  void ScheduleAt(uint64_t tick, int type, uint64_t userdata = 0);
  void Remove(int type);
  void Remove(int type, uint64_t userdata);
  bool IsScheduled(int type) const;

  // The current tick, counting what the CPU has run of its slice.
  uint64_t Now() const { return m_slice_start + static_cast<uint64_t>(m_slice - downcount); }
  // Makes the running slice end within cycles ticks.
  void ForceCheck(int64_t cycles);

  // Ends the slice, runs every due event and starts the next slice.
  void Advance();
  // Moves time to the next event (the CPU has nothing to do until then).
  void Idle();

  void DoState(StateIO& io);

  // Ticks left in the slice; the CPU counts it down.
  int64_t downcount = 0;

private:
  struct Event
  {
    uint64_t time;
    uint64_t order;
    uint64_t userdata;
    int type;
  };
  struct Type
  {
    Callback callback;
    void* user;
  };
  static bool Later(const Event& a, const Event& b)
  {
    return a.time != b.time ? a.time > b.time : a.order > b.order;
  }
  void Push(const Event& event);
  void StartSlice();

  static constexpr int64_t MAX_SLICE = 20000;

  std::vector<Type> m_types;
  std::vector<Event> m_heap;  // min-heap on (time, order)
  uint64_t m_order = 0;
  uint64_t m_slice_start = 0;
  int64_t m_slice = 0;
};
}  // namespace dualize
