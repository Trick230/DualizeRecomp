// The Revolution SDK's GX library at its boundary with the hardware. The
// renderer takes the FIFO's bytes as the CPU writes them (hw: the write gather
// pipe), so there is no FIFO to set up, no GP to watch and no register behind
// CP, PE or MI: GXInit, GXSetCPUFifo, GXSetGPFifo, the GXPoke family and the
// rest run as the game's code against windows that ignore writes and read
// idle (machine.cpp). What reaches the guest from the renderer is the draw
// done and the token, delivered here: their interrupt handlers acknowledge
// the host's signal and run what the SDK runs after them.
//
// The natives are found by their SDK code (config/sdk.sigs); the addresses
// and small-data offsets that depend on the game's link come from the
// natives list's defines (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Load32;
using dualize::Machine;

// Both handlers' frames, with an OSContext to run a callback on.
constexpr uint32_t HANDLER_FRAME = 736;
constexpr uint32_t HANDLER_CONTEXT = 8;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// Calls a callback the way an interrupt handler does: on a context of its own
// (a callback may use the FPU), back on the interrupted one afterwards.
void CallOnOwnContext(CPUState* ctx, uint32_t callback, uint32_t context, uint32_t token)
{
  Cpu& cpu = CpuOf(ctx);
  const uint32_t sp = ctx->gpr[1];
  const uint32_t frame = sp - HANDLER_FRAME;
  const uint32_t scratch = frame + HANDLER_CONTEXT;
  ctx->gpr[1] = frame;
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
  cpu.EnterHandlerContext();
  cpu.CallGuest(callback, {token});
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {context});
  ctx->gpr[1] = sp;
}
}  // namespace

// void GXFinishInterruptHandler(__OSInterrupt, OSContext* context): a draw
// done. Marks it for GXWaitDrawDone, runs GXSetDrawDoneCallback's callback and
// wakes the threads waiting.
extern "C" bool rvl_GXFinishInterruptHandler(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const uint32_t sda = ctx->gpr[13];
  uint8_t* finished = machine.memory.Effective(sda + GX_DRAW_DONE_FLAG_SDA, 1);
  const uint8_t* callback = machine.memory.Effective(sda + GX_DRAW_DONE_CALLBACK_SDA, 4);
  if (!finished || !callback)
    return false;
  ctx->downcount -= 40;
  machine.gfx.AckFinish();
  *finished = 1;
  if (const uint32_t function = Load32(callback))
    CallOnOwnContext(ctx, function, ctx->gpr[4], 0);
  cpu.CallGuest(OS_WAKEUP_THREAD, {sda + GX_FINISH_QUEUE_SDA});
  return true;
}

// void GXTokenInterruptHandler(__OSInterrupt, OSContext* context): a draw
// sync. Runs GXSetDrawSyncCallback's callback with the token.
extern "C" bool rvl_GXTokenInterruptHandler(CPUState* ctx)
{
  Machine& machine = GetMachine(CpuOf(ctx));
  const uint8_t* callback = machine.memory.Effective(ctx->gpr[13] + GX_DRAW_SYNC_CALLBACK_SDA, 4);
  if (!callback)
    return false;
  ctx->downcount -= 40;
  machine.SyncGpu();
  const uint32_t token = machine.gfx.Token();
  if (const uint32_t function = Load32(callback))
    CallOnOwnContext(ctx, function, ctx->gpr[4], token);
  machine.gfx.AckToken();
  return true;
}
