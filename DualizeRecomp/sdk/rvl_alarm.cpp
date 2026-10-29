// Alarms at the SDK boundary: OSSetAlarm, OSSetPeriodicAlarm, OSCancelAlarm
// and the decrementer interrupt's handler keep the OS's alarm queue in host
// code. The queue stays in guest memory, as the SDK lays it out (savestates
// hold it and Wud::AfterLoad walks it); its timer is a scheduler event that
// raises the decrementer interrupt, and the handler due runs as the game's
// code.
//
// The natives are pinned to their SDK bytes (R7EE8P.natives); the addresses
// and offsets below belong to that build.

#include "core/cpu.h"

#include "dolrecomp.hpp"
#include "machine.hpp"

namespace dualize
{
namespace
{
constexpr int32_t QUEUE_SDA_OFFSET = -27664;  // head, then tail
constexpr uint32_t QUEUE_TAIL = 4;

// OSAlarm
constexpr uint32_t ALARM_HANDLER = 0;  // null while not queued
constexpr uint32_t ALARM_FIRE = 8;     // system time, s64
constexpr uint32_t ALARM_PREV = 16;
constexpr uint32_t ALARM_NEXT = 20;
constexpr uint32_t ALARM_PERIOD = 24;  // s64, 0 for a one-shot alarm
constexpr uint32_t ALARM_START = 32;   // s64
constexpr uint32_t ALARM_SIZE = 40;

// OSGetSystemTime is the timebase plus this.
constexpr uint32_t SYSTEM_TIME_OFFSET = 0x800030D8;
constexpr uint32_t OS_LOAD_CONTEXT = 0x8044F578;
// DecrementerExceptionCallback from where it runs the alarm's handler on a
// temporary context, then reschedules and loads the interrupted one; it wants
// its frame, the handler in r30, the alarm in r31 and the context in r29.
constexpr uint32_t ALARM_HANDLER_TAIL = 0x8044E234;
constexpr uint32_t ALARM_HANDLER_FRAME = 736;

class Alarms
{
public:
  explicit Alarms(CPUState* ctx)
      : m_cpu(*static_cast<Cpu*>(ctx->external_user_data)), m_memory(GetMachine(m_cpu).memory),
        m_queue(m_memory.Effective(ctx->gpr[13] + QUEUE_SDA_OFFSET, 8))
  {
  }

  bool Valid() const { return m_queue != nullptr; }
  uint8_t* Bytes(uint32_t address, uint32_t size) const { return m_memory.Effective(address, size); }
  uint8_t* At(uint32_t alarm) const { return Bytes(alarm, ALARM_SIZE); }

  uint32_t Head() const { return Load32(m_queue); }
  uint32_t Tail() const { return Load32(m_queue + QUEUE_TAIL); }
  void SetHead(uint32_t alarm) { Store32(m_queue, alarm); }
  void SetTail(uint32_t alarm) { Store32(m_queue + QUEUE_TAIL, alarm); }

  // OSTimeToSystemTime
  int64_t TimeToSystemTime(int64_t time) const
  {
    const uint8_t* offset = Bytes(SYSTEM_TIME_OFFSET, 8);
    return time + static_cast<int64_t>(offset ? Load64(offset) : 0);
  }
  int64_t SystemTime() const { return TimeToSystemTime(static_cast<int64_t>(m_cpu.Timebase())); }

  // SetTimer: the decrementer counts down to the alarm's fire time.
  void SetTimer(uint32_t alarm)
  {
    const int64_t delta = static_cast<int64_t>(Load64(At(alarm) + ALARM_FIRE)) - SystemTime();
    m_cpu.SetDecrementer(delta < 0 ? 0 : delta < 0x80000000 ? static_cast<uint32_t>(delta) : 0x7FFFFFFF);
  }

  // InsertAlarm: a periodic alarm fires at its next period past now; the
  // queue keeps fire order, and a new head sets the timer.
  void Insert(uint32_t alarm, int64_t fire, uint32_t handler)
  {
    uint8_t* a = At(alarm);
    const int64_t period = static_cast<int64_t>(Load64(a + ALARM_PERIOD));
    if (period > 0)
    {
      const int64_t time = SystemTime();
      const int64_t start = static_cast<int64_t>(Load64(a + ALARM_START));
      fire = start;
      if (start < time)
        fire += period * ((time - start) / period + 1);
    }
    Store32(a + ALARM_HANDLER, handler);
    Store64(a + ALARM_FIRE, static_cast<uint64_t>(fire));
    for (uint32_t next = Head(); next;)
    {
      uint8_t* n = At(next);
      if (fire >= static_cast<int64_t>(Load64(n + ALARM_FIRE)))
      {
        next = Load32(n + ALARM_NEXT);
        continue;
      }
      const uint32_t prev = Load32(n + ALARM_PREV);
      Store32(a + ALARM_PREV, prev);
      Store32(n + ALARM_PREV, alarm);
      Store32(a + ALARM_NEXT, next);
      if (prev)
      {
        Store32(At(prev) + ALARM_NEXT, alarm);
        return;
      }
      SetHead(alarm);
      SetTimer(alarm);
      return;
    }
    const uint32_t tail = Tail();
    Store32(a + ALARM_NEXT, 0);
    Store32(a + ALARM_PREV, tail);
    SetTail(alarm);
    if (tail)
    {
      Store32(At(tail) + ALARM_NEXT, alarm);
      return;
    }
    SetHead(alarm);
    SetTimer(alarm);
  }

  void Cancel(uint32_t alarm)
  {
    uint8_t* a = At(alarm);
    if (!Load32(a + ALARM_HANDLER))
      return;
    const uint32_t next = Load32(a + ALARM_NEXT), prev = Load32(a + ALARM_PREV);
    if (next)
      Store32(At(next) + ALARM_PREV, prev);
    else
      SetTail(prev);
    if (prev)
      Store32(At(prev) + ALARM_NEXT, next);
    else
    {
      SetHead(next);
      if (next)
        SetTimer(next);
    }
    Store32(a + ALARM_HANDLER, 0);
  }

private:
  Cpu& m_cpu;
  const Memory& m_memory;
  uint8_t* m_queue;
};

int64_t Arg64(const CPUState* ctx, int reg)
{
  return static_cast<int64_t>(uint64_t{ctx->gpr[reg]} << 32 | ctx->gpr[reg + 1]);
}
}  // namespace
}  // namespace dualize

// void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
extern "C" bool rvl_OSSetAlarm(CPUState* ctx)
{
  using namespace dualize;
  Alarms alarms(ctx);
  const uint32_t alarm = ctx->gpr[3];
  uint8_t* a = alarms.At(alarm);
  if (!alarms.Valid() || !a)
    return false;
  Store64(a + ALARM_PERIOD, 0);
  alarms.Insert(alarm, alarms.SystemTime() + Arg64(ctx, 5), ctx->gpr[7]);
  ctx->downcount -= 60;
  return true;
}

// void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
//                         OSAlarmHandler handler)
extern "C" bool rvl_OSSetPeriodicAlarm(CPUState* ctx)
{
  using namespace dualize;
  Alarms alarms(ctx);
  const uint32_t alarm = ctx->gpr[3];
  uint8_t* a = alarms.At(alarm);
  if (!alarms.Valid() || !a)
    return false;
  Store64(a + ALARM_PERIOD, static_cast<uint64_t>(Arg64(ctx, 7)));
  Store64(a + ALARM_START, static_cast<uint64_t>(alarms.TimeToSystemTime(Arg64(ctx, 5))));
  alarms.Insert(alarm, 0, ctx->gpr[9]);
  ctx->downcount -= 80;
  return true;
}

// void OSCancelAlarm(OSAlarm* alarm)
extern "C" bool rvl_OSCancelAlarm(CPUState* ctx)
{
  using namespace dualize;
  Alarms alarms(ctx);
  if (!alarms.Valid() || !alarms.At(ctx->gpr[3]))
    return false;
  alarms.Cancel(ctx->gpr[3]);
  ctx->downcount -= 30;
  return true;
}

// DecrementerExceptionCallback(__OSException, OSContext* context): takes the
// alarm due off the queue (a periodic one goes back in), sets the timer for
// the next, and runs the handler as the SDK does; with none due, the
// interrupted context resumes.
extern "C" bool rvl_DecrementerExceptionCallback(CPUState* ctx)
{
  using namespace dualize;
  Alarms alarms(ctx);
  if (!alarms.Valid())
    return false;
  const uint32_t context = ctx->gpr[4];
  const uint32_t alarm = alarms.Head();
  uint8_t* a = alarm ? alarms.At(alarm) : nullptr;
  const uint32_t sp = ctx->gpr[1] - ALARM_HANDLER_FRAME;
  uint8_t* frame = alarms.Bytes(sp, ALARM_HANDLER_FRAME + 8);
  if (alarm && (!a || !frame))
    return false;
  ctx->downcount -= 60;
  if (!alarm || alarms.SystemTime() < static_cast<int64_t>(Load64(a + ALARM_FIRE)))
  {
    if (alarm)
      alarms.SetTimer(alarm);
    ctx->gpr[3] = context;
    ctx->pc = OS_LOAD_CONTEXT;
    return true;
  }

  const uint32_t next = Load32(a + ALARM_NEXT);
  alarms.SetHead(next);
  if (next)
    Store32(alarms.At(next) + ALARM_PREV, 0);
  else
    alarms.SetTail(0);
  const uint32_t handler = Load32(a + ALARM_HANDLER);
  Store32(a + ALARM_HANDLER, 0);
  if (static_cast<int64_t>(Load64(a + ALARM_PERIOD)) > 0)
    alarms.Insert(alarm, 0, handler);
  if (const uint32_t head = alarms.Head())
    alarms.SetTimer(head);

  // The callback's frame (its back chain and saved lr), then its tail.
  Store32(frame, ctx->gpr[1]);
  Store32(frame + ALARM_HANDLER_FRAME + 4, ctx->lr);
  ctx->gpr[1] = sp;
  ctx->gpr[29] = context;
  ctx->gpr[30] = handler;
  ctx->gpr[31] = alarm;
  ctx->pc = ALARM_HANDLER_TAIL;
  return true;
}
