// The Revolution SDK's DVD library at its boundary with the drive. DVDLowRead
// and DVDLowClearCoverInterrupt, nearly all of what a game asks of the drive,
// are served by the host's drive with no IOS request: the data is there at the
// call and the callback comes when the drive would have finished, through the
// IPC interrupt the SDK's replies use. The queue, retries and cancels above
// them stay the game's code, and the rarer DVDLow commands stay IOS requests
// to /dev/di.
//
// The natives are found by their SDK code (config/sdk.sigs); the addresses
// and small-data offsets that depend on the game's link come from the
// natives list's defines (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "kernel/ios/devices.hpp"
#include "machine.hpp"

namespace dualize
{
namespace
{
constexpr uint32_t OS_CONTEXT_SIZE = 0x2C8;
constexpr uint32_t CACHED_BASE = 0x80000000u;
// A completion is the callback's physical address with the drive's result (1
// or 2) in its low bits; IOS replies are request blocks, 32-byte aligned.
constexpr uint32_t COMPLETION_RESULT = 3;
constexpr uint32_t RESULT_TRANSFER_COMPLETE = 1;
constexpr uint32_t RESULT_BREAK = 8;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// What every command's start records for the SDK.
struct Globals
{
  uint8_t* busy;
  uint8_t* transfer_length;
};

Globals GlobalsOf(CPUState* ctx, const Memory& memory)
{
  return {memory.Effective(ctx->gpr[13] + DVD_LOW_BUSY_SDA, 1),
          memory.Effective(ctx->gpr[13] + DVD_TRANSFER_LENGTH_SDA, 4)};
}

void Post(Machine& machine, uint32_t callback, const ios::DiHostReply& reply)
{
  const uint32_t physical = callback ? callback - CACHED_BASE : 0;
  machine.ios.PostCompletion(physical | static_cast<uint32_t>(reply.result), reply.ticks);
}
}  // namespace

bool IsDvdCompletion(uint32_t reply)
{
  return (reply & COMPLETION_RESULT) != 0;
}

// What the SDK's transaction callback does, in the IPC interrupt handler: the
// drive is free, a pending break shows in the result, the callback runs on a
// context of its own.
void CompleteDvdCommand(Cpu& cpu, uint32_t completion, uint32_t interrupted_context)
{
  CPUState& s = cpu.state;
  const Memory& memory = GetMachine(cpu).memory;
  const uint32_t sda = s.gpr[13];
  uint8_t* busy = memory.Effective(sda + DVD_LOW_BUSY_SDA, 1);
  uint8_t* breaking = memory.Effective(sda + DVD_BREAK_SDA, 1);
  uint8_t* in_callback = memory.Effective(sda + DVD_IN_CALLBACK_SDA, 1);
  uint8_t* transfer_length = memory.Effective(sda + DVD_TRANSFER_LENGTH_SDA, 4);
  if (!busy || !breaking || !in_callback || !transfer_length)
    return;
  *busy = 0;
  const uint32_t physical = completion & ~COMPLETION_RESULT;
  if (!physical)
    return;
  uint32_t result = completion & COMPLETION_RESULT;
  *in_callback = 1;
  if (*breaking == 1)
  {
    *breaking = 0;
    result |= RESULT_BREAK;
  }
  if (result & RESULT_TRANSFER_COMPLETE)
    Store32(transfer_length, 0);
  // The scratch context sits in a frame below the handler's stack.
  const uint32_t sp = s.gpr[1];
  const uint32_t scratch = (sp - OS_CONTEXT_SIZE - 8) & ~7u;
  s.gpr[1] = scratch - 16;
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
  cpu.EnterHandlerContext();
  cpu.CallGuest(physical + CACHED_BASE, {result});
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {interrupted_context});
  s.gpr[1] = sp;
  *in_callback = 0;
}
}  // namespace dualize

// BOOL DVDLowRead(void* buffer, u32 length, u32 offset_in_words, callback)
extern "C" bool rvl_DVDLowRead(CPUState* ctx)
{
  using namespace dualize;
  Machine& machine = GetMachine(CpuOf(ctx));
  const Globals globals = GlobalsOf(ctx, machine.memory);
  const uint32_t buffer = ctx->gpr[3], length = ctx->gpr[4];
  // A misaligned buffer gets the SDK's own report.
  if (!globals.busy || !globals.transfer_length || (buffer & 0x1F))
    return false;
  const auto reply = ios::HostDiRead(machine.ios.Di(), buffer - CACHED_BASE, length, ctx->gpr[5]);
  if (!reply)
    return false;
  ctx->downcount -= 100;
  *globals.busy = 1;
  Store32(globals.transfer_length, length);
  Post(machine, ctx->gpr[6], *reply);
  ctx->gpr[3] = 1;
  return true;
}

// BOOL DVDLowClearCoverInterrupt(callback)
extern "C" bool rvl_DVDLowClearCoverInterrupt(CPUState* ctx)
{
  using namespace dualize;
  Machine& machine = GetMachine(CpuOf(ctx));
  const Globals globals = GlobalsOf(ctx, machine.memory);
  if (!globals.busy)
    return false;
  const auto reply = ios::HostDiClearCover(machine.ios.Di());
  if (!reply)
    return false;
  ctx->downcount -= 100;
  *globals.busy = 1;
  Post(machine, ctx->gpr[3], *reply);
  ctx->gpr[3] = 1;
  return true;
}
