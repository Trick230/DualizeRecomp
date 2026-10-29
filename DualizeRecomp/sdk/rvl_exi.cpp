// The Revolution SDK's EXI driver at its API. The SDK locks a channel,
// selects a device on it, shifts bytes through the channel's registers
// (immediate words or DMA) and waits for the transfer-complete interrupt.
// Here each call works on the runtime's expansion bus (hw/hw.hpp): a
// transfer finishes inside the call, so no EXI register or interrupt is
// left. NiGHTS reaches one device through it, the IPL chip on channel 0
// (the RTC, the SRAM and the debug UART behind OSReport).
//
// The natives are pinned to their SDK bytes (R7EE8P.natives); the addresses
// below belong to that build. No caller in it passes a transfer callback.

#include "core/cpu.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <algorithm>

namespace
{
using dualize::Cpu;
using dualize::ExpansionBus;
using dualize::Machine;

// EXIInit clears the OS globals that cache the channels' device IDs.
constexpr uint32_t OS_EXI_ID_CACHE = 0x800030C0;

Machine& MachineOf(CPUState* ctx)
{
  return static_cast<Cpu*>(ctx->external_user_data)->GetMachine();
}

ExpansionBus::Channel* ChannelOf(CPUState* ctx)
{
  const uint32_t channel = ctx->gpr[3];
  auto& channels = MachineOf(ctx).exi.channels;
  return channel < channels.size() ? &channels[channel] : nullptr;
}

void Return(CPUState* ctx, bool value, int cycles)
{
  ctx->gpr[3] = value ? 1 : 0;
  ctx->downcount -= cycles;
}

// EXIImm, EXIImmEx and EXIDma: (chan, buf, len, type); type 0 reads, 1
// writes, 2 does both.
bool Transfer(CPUState* ctx)
{
  ExpansionBus::Channel* channel = ChannelOf(ctx);
  const uint32_t length = ctx->gpr[5];
  const uint32_t type = ctx->gpr[6];
  Machine& machine = MachineOf(ctx);
  uint8_t* data = machine.memory.Effective(ctx->gpr[4], length);
  if (!channel || !channel->selected || !data || type > 2)
  {
    Return(ctx, false, 20);
    return true;
  }
  machine.exi.Transfer(static_cast<int>(ctx->gpr[3]), data, length, type != 1, type != 0);
  Return(ctx, true, 40 + static_cast<int>(length));
  return true;
}
}  // namespace

// void EXIInit(void)
extern "C" bool rvl_EXIInit(CPUState* ctx)
{
  uint8_t* cache = MachineOf(ctx).memory.Effective(OS_EXI_ID_CACHE, 8);
  if (!cache)
    return false;
  std::fill_n(cache, 8, 0);
  ctx->downcount -= 100;
  return true;
}

// BOOL EXILock(s32 chan, u32 dev, EXICallback unlockedCallback): a locked
// channel queues the callback (once) for its unlock.
extern "C" bool rvl_EXILock(CPUState* ctx)
{
  ExpansionBus::Channel* channel = ChannelOf(ctx);
  if (!channel)
  {
    Return(ctx, false, 20);
    return true;
  }
  if (channel->locked)
  {
    const uint32_t callback = ctx->gpr[5];
    const auto waiting = channel->callbacks.begin() + channel->waiting;
    if (callback && channel->waiting < channel->callbacks.size() &&
        std::find(channel->callbacks.begin(), waiting, callback) == waiting)
    {
      channel->callbacks[channel->waiting++] = callback;
    }
    Return(ctx, false, 30);
    return true;
  }
  channel->locked = true;
  channel->device = ctx->gpr[4];
  Return(ctx, true, 30);
  return true;
}

// BOOL EXIUnlock(s32 chan): the first waiting callback runs, as
// unlockedCallback(chan, NULL).
extern "C" bool rvl_EXIUnlock(CPUState* ctx)
{
  ExpansionBus::Channel* channel = ChannelOf(ctx);
  if (!channel || !channel->locked)
  {
    Return(ctx, false, 20);
    return true;
  }
  channel->locked = false;
  if (channel->waiting)
  {
    const uint32_t callback = channel->callbacks[0];
    std::copy(channel->callbacks.begin() + 1, channel->callbacks.end(), channel->callbacks.begin());
    --channel->waiting;
    static_cast<Cpu*>(ctx->external_user_data)->CallGuest(callback, {ctx->gpr[3], 0});
  }
  Return(ctx, true, 30);
  return true;
}

// BOOL EXISelect(s32 chan, u32 dev, u32 freq): the channel's locker selects
// its device. Device 0 of channels 0 and 1 is the memory card slot, empty.
extern "C" bool rvl_EXISelect(CPUState* ctx)
{
  ExpansionBus::Channel* channel = ChannelOf(ctx);
  const uint32_t device = ctx->gpr[4];
  if (!channel || channel->selected || !channel->locked || channel->device != device ||
      (ctx->gpr[3] != 2 && device == 0))
  {
    Return(ctx, false, 20);
    return true;
  }
  channel->selected = true;
  MachineOf(ctx).exi.Select(static_cast<int>(ctx->gpr[3]));
  Return(ctx, true, 30);
  return true;
}

// BOOL EXIDeselect(s32 chan)
extern "C" bool rvl_EXIDeselect(CPUState* ctx)
{
  ExpansionBus::Channel* channel = ChannelOf(ctx);
  const bool selected = channel && channel->selected;
  if (selected)
    channel->selected = false;
  Return(ctx, selected, 20);
  return true;
}

// BOOL EXIImm(s32 chan, void* buf, s32 len, u32 type, EXICallback callback)
extern "C" bool rvl_EXIImm(CPUState* ctx)
{
  return Transfer(ctx);
}

// BOOL EXIImmEx(s32 chan, void* buf, s32 len, u32 mode)
extern "C" bool rvl_EXIImmEx(CPUState* ctx)
{
  return Transfer(ctx);
}

// BOOL EXIDma(s32 chan, void* buf, s32 len, u32 type, EXICallback callback)
extern "C" bool rvl_EXIDma(CPUState* ctx)
{
  return Transfer(ctx);
}

// BOOL EXISync(s32 chan): the transfer finished in its call.
extern "C" bool rvl_EXISync(CPUState* ctx)
{
  const ExpansionBus::Channel* channel = ChannelOf(ctx);
  Return(ctx, channel && channel->selected, 20);
  return true;
}

// u32 WriteUARTN(const void* buf, u32 len): OSReport's text to the debug
// UART, which nothing reads; reports it sent (0).
extern "C" bool rvl_WriteUARTN(CPUState* ctx)
{
  ctx->gpr[3] = 0;
  ctx->downcount -= 20;
  return true;
}
