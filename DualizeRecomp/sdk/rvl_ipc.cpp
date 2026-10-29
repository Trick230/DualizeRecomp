// The Revolution SDK's IPC client at its boundary with IOS. The SDK queues
// each request in a ring, rings the Hollywood doorbell, waits for IOS to
// acknowledge it and takes the reply from the mailbox in its IPC interrupt
// handler. Here the request goes straight to the runtime's IOS kernel, and
// the handler takes replies from it; what the SDK does around the transport
// (the waiting thread's queue, pointer fix-ups, callbacks, freeing) is kept,
// partly by running the SDK's own code.
//
// Both natives are found by their SDK code (config/sdk.sigs); the addresses
// and offsets below are NiGHTS' (defines, were it shared).

#include "core/cpu.h"

#include "dolrecomp.hpp"
#include "machine.hpp"

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Load32;
using dualize::Store32;

// IOS request block (as the SDK lays it out)
constexpr uint32_t REQUEST_COMMAND = 0;  // the command IOS answered, after the reply
constexpr uint32_t REQUEST_RESULT = 4;
constexpr uint32_t REQUEST_FD = 8;
constexpr uint32_t REQUEST_ARG0 = 12;
constexpr uint32_t REQUEST_ARG1 = 16;
constexpr uint32_t REQUEST_ARG2 = 20;
constexpr uint32_t REQUEST_ARG3 = 24;
constexpr uint32_t REQUEST_CALLBACK = 32;
constexpr uint32_t REQUEST_CALLBACK_DATA = 36;
constexpr uint32_t REQUEST_THREAD_QUEUE = 44;
constexpr uint32_t REQUEST_SIZE = 52;

constexpr uint32_t IPC_CMD_READ = 3;
constexpr uint32_t IPC_CMD_IOCTL = 6;
constexpr uint32_t IPC_CMD_IOCTLV = 7;

// SDK functions
constexpr uint32_t OS_SET_CURRENT_CONTEXT = 0x8044F490;
constexpr uint32_t OS_CLEAR_CONTEXT = 0x8044F6B8;
constexpr uint32_t OS_WAKEUP_THREAD = 0x804570DC;
constexpr uint32_t IOS_FREE = 0x80480D14;          // iosFree(heap, block)
constexpr uint32_t IPC_PROF_QUEUE_REQ = 0x80480FB8;
constexpr uint32_t IPC_PROF_REPLY = 0x80481048;
// ios_Ipc2 after the transport: sleeps on the request when synchronous,
// restores interrupts, reads the result, frees the request and returns.
constexpr uint32_t IOS_IPC2_WAIT = 0x8047F9B4;
// The IPC client's heap handle, a small-data global.
constexpr int32_t IPC_HEAP_SDA_OFFSET = -29412;

constexpr uint32_t OS_CONTEXT_SIZE = 0x2C8;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// The IOS kernel takes and gives physical addresses; the SDK keeps cached
// virtual ones (MEM1 0x8..., MEM2 0x9...).
uint32_t Physical(uint32_t address)
{
  return address - 0x80000000u;
}
uint32_t Virtual(uint32_t physical)
{
  return physical ? physical + 0x80000000u : 0;
}

uint32_t Load(const uint8_t* block, uint32_t offset)
{
  return Load32(block + offset);
}
void Store(uint8_t* block, uint32_t offset, uint32_t value)
{
  Store32(block + offset, value);
}

// What IpcReplyHandler does with one reply, minus the mailbox: buffer
// pointers back to virtual, then the callback (under a scratch OS context, so
// a wakeup it causes waits for the reschedule after the interrupt) and the
// free, or the waiting thread's wakeup.
void Complete(Cpu& cpu, uint32_t request, uint32_t interrupted_context)
{
  CPUState& s = cpu.state;
  const dualize::Memory& memory = GetMachine(cpu).memory;
  uint8_t* block = memory.Effective(request, REQUEST_SIZE);
  if (!block)
    return;
  switch (Load(block, REQUEST_FD))
  {
  case IPC_CMD_READ:
    Store(block, REQUEST_ARG0, Virtual(Load(block, REQUEST_ARG0)));
    break;
  case IPC_CMD_IOCTL:
    Store(block, REQUEST_ARG3, Virtual(Load(block, REQUEST_ARG3)));
    break;
  case IPC_CMD_IOCTLV:
  {
    const uint32_t vectors = Virtual(Load(block, REQUEST_ARG3));
    Store(block, REQUEST_ARG3, vectors);
    const uint32_t count = Load(block, REQUEST_ARG1) + Load(block, REQUEST_ARG2);
    if (uint8_t* vector = memory.Effective(vectors, count * 8))
      for (uint32_t i = 0; i < count; ++i)
        Store(vector, i * 8, Virtual(Load(vector, i * 8)));
    break;
  }
  default:
    break;
  }

  if (const uint32_t callback = Load(block, REQUEST_CALLBACK))
  {
    // The scratch context sits in a frame below the handler's stack.
    const uint32_t sp = s.gpr[1];
    const uint32_t scratch = (sp - OS_CONTEXT_SIZE - 8) & ~7u;
    s.gpr[1] = scratch - 16;
    cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
    cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
    cpu.CallGuest(callback, {Load(block, REQUEST_RESULT), Load(block, REQUEST_CALLBACK_DATA)});
    cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
    cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {interrupted_context});
    const uint8_t* heap = memory.Effective(s.gpr[13] + IPC_HEAP_SDA_OFFSET, 4);
    cpu.CallGuest(IOS_FREE, {heap ? Load32(heap) : 0, request});
    s.gpr[1] = sp;
  }
  else
  {
    cpu.CallGuest(OS_WAKEUP_THREAD, {request + REQUEST_THREAD_QUEUE});
  }
  cpu.CallGuest(IPC_PROF_REPLY, {request, Load(block, REQUEST_FD)});
}
}  // namespace

// s32 ios_Ipc2(IOSRequest* request, IOSCallback callback)
// Jump native: runs the prologue and the transport, then continues in the
// SDK's own code where it waits for a synchronous reply.
extern "C" bool rvl_ios_Ipc2(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  dualize::Machine& machine = GetMachine(cpu);
  const uint32_t request = ctx->gpr[3];
  const uint32_t callback = ctx->gpr[4];
  uint8_t* block = request ? machine.memory.Effective(request, REQUEST_SIZE) : nullptr;
  const uint32_t sp = ctx->gpr[1] - 32;
  uint8_t* frame = machine.memory.Effective(sp, 40);
  if (!block || !frame)
    return false;

  // stwu r1, -32(r1) and the saves the epilogue at IOS_IPC2_WAIT undoes.
  Store(frame, 0, ctx->gpr[1]);
  Store(frame, 36, ctx->lr);
  Store(frame, 28, ctx->gpr[31]);
  Store(frame, 24, ctx->gpr[30]);
  Store(frame, 20, ctx->gpr[29]);
  Store(frame, 16, ctx->gpr[28]);
  ctx->gpr[1] = sp;
  if (!callback)
  {
    // OSInitThreadQueue(&request->queue)
    Store(block, REQUEST_THREAD_QUEUE, 0);
    Store(block, REQUEST_THREAD_QUEUE + 4, 0);
  }
  // OSDisableInterrupts(): the reply can't land before the caller sleeps.
  const uint32_t level = (ctx->msr >> 15) & 1u;
  ctx->msr &= ~PPC_MSR_EE;

  machine.ios.Submit(Physical(request));
  cpu.CallGuest(IPC_PROF_QUEUE_REQ, {request, Load(block, REQUEST_FD)});

  ctx->gpr[28] = request;
  ctx->gpr[29] = callback;
  ctx->gpr[30] = level;
  ctx->gpr[31] = 0;
  ctx->pc = IOS_IPC2_WAIT;
  ctx->downcount -= 40;
  return true;
}

// void IPCInterruptHandler(__OSInterrupt interrupt, OSContext* context)
// Runs inside OSDispatchInterrupt, with the scheduler off.
extern "C" bool rvl_IPCInterruptHandler(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  const uint32_t context = ctx->gpr[4];
  uint32_t reply;
  while (GetMachine(cpu).ios.PopReply(&reply))
    Complete(cpu, Virtual(reply), context);
  ctx->downcount -= 20;
  return true;
}
