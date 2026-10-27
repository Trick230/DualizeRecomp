#include "kernel/scheduler.hpp"

#include <algorithm>

namespace dualize
{
int Scheduler::Register(Callback callback, void* user)
{
  m_types.push_back({callback, user});
  return static_cast<int>(m_types.size() - 1);
}

void Scheduler::Push(const Event& event)
{
  m_heap.push_back(event);
  std::push_heap(m_heap.begin(), m_heap.end(), Later);
}

void Scheduler::Schedule(int64_t cycles_into_future, int type, uint64_t userdata)
{
  const uint64_t now = Now();
  const uint64_t time = cycles_into_future > 0 ? now + cycles_into_future : now;
  ScheduleAt(time, type, userdata);
}

void Scheduler::ScheduleAt(uint64_t tick, int type, uint64_t userdata)
{
  Push({tick, m_order++, userdata, type});
  // An event inside the running slice cuts it short.
  const uint64_t now = Now();
  ForceCheck(tick > now ? static_cast<int64_t>(tick - now) : 0);
}

void Scheduler::Remove(int type)
{
  const auto end = std::remove_if(m_heap.begin(), m_heap.end(),
                                  [type](const Event& e) { return e.type == type; });
  if (end == m_heap.end())
    return;
  m_heap.erase(end, m_heap.end());
  std::make_heap(m_heap.begin(), m_heap.end(), Later);
}

void Scheduler::Remove(int type, uint64_t userdata)
{
  const auto end = std::remove_if(m_heap.begin(), m_heap.end(), [&](const Event& e) {
    return e.type == type && e.userdata == userdata;
  });
  if (end == m_heap.end())
    return;
  m_heap.erase(end, m_heap.end());
  std::make_heap(m_heap.begin(), m_heap.end(), Later);
}

bool Scheduler::IsScheduled(int type) const
{
  return std::any_of(m_heap.begin(), m_heap.end(), [type](const Event& e) { return e.type == type; });
}

void Scheduler::ForceCheck(int64_t cycles)
{
  if (cycles < 0)
    cycles = 0;
  if (*m_downcount > cycles)
  {
    m_slice -= *m_downcount - cycles;
    *m_downcount = cycles;
  }
}

void Scheduler::StartSlice()
{
  m_slice_start = Now();
  int64_t slice = MAX_SLICE;
  if (!m_heap.empty())
  {
    const uint64_t next = m_heap.front().time;
    slice = next > m_slice_start ? std::min<int64_t>(slice, next - m_slice_start) : 0;
  }
  m_slice = slice;
  *m_downcount = slice;
}

void Scheduler::Advance()
{
  // Time runs on from where the CPU stopped, overshoot included.
  m_slice_start = Now();
  m_slice = 0;
  *m_downcount = 0;
  while (!m_heap.empty() && m_heap.front().time <= m_slice_start)
  {
    std::pop_heap(m_heap.begin(), m_heap.end(), Later);
    const Event event = m_heap.back();
    m_heap.pop_back();
    const Type& type = m_types[event.type];
    type.callback(type.user, event.userdata, static_cast<int64_t>(m_slice_start - event.time));
  }
  StartSlice();
}

void Scheduler::Idle()
{
  if (!m_heap.empty() && m_heap.front().time > Now())
  {
    m_slice_start = m_heap.front().time;
    m_slice = 0;
    *m_downcount = 0;
  }
}

void Scheduler::DoState(StateIO& io)
{
  uint64_t now = Now();
  io.Do(now);
  io.Do(m_order);
  uint32_t count = static_cast<uint32_t>(m_heap.size());
  io.Do(count);
  if (io.Reading())
    m_heap.resize(count);
  for (Event& event : m_heap)
  {
    io.Do(event.time);
    io.Do(event.order);
    io.Do(event.userdata);
    io.Do(event.type);
  }
  if (io.Reading())
  {
    std::make_heap(m_heap.begin(), m_heap.end(), Later);
    m_slice_start = now;
    m_slice = 0;
    *m_downcount = 0;
  }
}
}  // namespace dualize
