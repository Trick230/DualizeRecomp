// The Revolution SDK's GX library at its boundary with the hardware. The
// renderer takes the FIFO's bytes as the CPU writes them (hw: the write gather
// pipe), so there is no FIFO to set up, no GP to watch and no register behind
// CP, PE or MI: GXInit, GXSetCPUFifo, GXSetGPFifo, the GXPoke family and the
// rest run as the game's code against windows that ignore writes and read
// idle (machine.cpp). What reaches the guest from the renderer is the draw
// done and the token, delivered here: their interrupt handlers acknowledge
// the host's signal and run what the SDK runs after them.
//
// The matrix and light loads are GX at the API: instead of the SDK encoding
// XF packets, they hand the renderer the call and its data as it sits in
// memory (aurora gx/recomp.hpp HostGxOpcode), in the stream where the SDK's
// packets would have gone. They touch no other guest state.
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

// aurora gx/recomp.hpp HostGxOpcode and HostGxCall.
constexpr uint8_t HOST_GX_OPCODE = 0x78;
enum : uint8_t
{
  HOST_GX_LOAD_POS_MTX,
  HOST_GX_LOAD_NRM_MTX,
  HOST_GX_LOAD_TEX_MTX34,
  HOST_GX_LOAD_TEX_MTX24,
  HOST_GX_LOAD_LIGHT,
};
constexpr uint32_t GX_PTTEXMTX0 = 64;

// Puts a host GX call in the stream: rows of words from guest memory, each
// row `stride` bytes after the last. Declines if the data isn't in RAM.
bool HostGx(CPUState* ctx, uint8_t call, uint32_t index, uint32_t source, uint32_t rows, uint32_t rowWords,
            uint32_t stride, int32_t cycles)
{
  Cpu& cpu = CpuOf(ctx);
  const uint8_t* data = GetMachine(cpu).memory.Effective(source, (rows - 1) * stride + rowWords * 4);
  if (!data || index > 0xFF)
    return false;
  uint8_t packet[3 + 13 * 4];
  packet[0] = HOST_GX_OPCODE;
  packet[1] = call;
  packet[2] = static_cast<uint8_t>(index);
  uint8_t* out = packet + 3;
  for (uint32_t row = 0; row < rows; ++row, out += rowWords * 4)
    std::memcpy(out, data + row * stride, rowWords * 4);
  cpu.GatherPipeAppend(packet, static_cast<size_t>(out - packet));
  ctx->downcount -= cycles;
  return true;
}
}  // namespace

// void GXLoadPosMtxImm(const Mtx mtx, u32 id)
extern "C" bool rvl_GXLoadPosMtxImm(CPUState* ctx)
{
  return HostGx(ctx, HOST_GX_LOAD_POS_MTX, ctx->gpr[4], ctx->gpr[3], 1, 12, 48, 20);
}

// void GXLoadNrmMtxImm(const Mtx mtx, u32 id): the upper-left 3x3.
extern "C" bool rvl_GXLoadNrmMtxImm(CPUState* ctx)
{
  return HostGx(ctx, HOST_GX_LOAD_NRM_MTX, ctx->gpr[4], ctx->gpr[3], 3, 3, 16, 22);
}

// void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, GXTexMtxType type): a
// post-transform matrix is always 3x4.
extern "C" bool rvl_GXLoadTexMtxImm(CPUState* ctx)
{
  const uint32_t id = ctx->gpr[4];
  const bool rows2 = ctx->gpr[5] == 1;  // GX_MTX2x4
  if (id >= GX_PTTEXMTX0 && rows2)
    return false;
  return HostGx(ctx, rows2 ? HOST_GX_LOAD_TEX_MTX24 : HOST_GX_LOAD_TEX_MTX34, id, ctx->gpr[3], 1, rows2 ? 8 : 12,
                48, 30);
}

// void GXLoadLightObjImm(const GXLightObj* obj, GXLightID light): the light
// a single bit; the object from its color on (after 3 reserved words).
extern "C" bool rvl_GXLoadLightObjImm(CPUState* ctx)
{
  const uint32_t light = ctx->gpr[4];
  if (light == 0 || (light & (light - 1)) != 0 || light > 0x80)
    return false;
  return HostGx(ctx, HOST_GX_LOAD_LIGHT, 31 - __builtin_clz(light), ctx->gpr[3] + 12, 1, 13, 52, 31);
}

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
