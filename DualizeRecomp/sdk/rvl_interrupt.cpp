// The OS's interrupt dispatch at the SDK boundary: __OSDispatchInterrupt's
// causes come straight from the host's devices instead of the guest reading
// their registers, and the handler the OS picks runs as the game's code.
//
// The native is pinned to its SDK bytes (R7EE8P.natives); the addresses and
// offsets below belong to that build.

#include "core/cpu.h"

#include "apu/native_dsp.hpp"
#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <bit>

namespace dualize
{
namespace
{
constexpr uint32_t OS_LOAD_CONTEXT = 0x8044F578;
// __OSInterruptPrioTable: masks of OS interrupts, highest priority first; the
// last one takes every interrupt.
constexpr uint32_t PRIORITY_TABLE = 0x805621E8;
constexpr uint32_t PRIORITY_ENTRIES = 32;
constexpr int32_t HANDLER_TABLE_SDA_OFFSET = -27568;  // __OSInterrupt (*)[32]
// The last interrupt past the first five (the memory interface's), for the
// OS's error reports.
constexpr int32_t LAST_INTERRUPT_SDA_OFFSET = -27580;       // s16
constexpr int32_t LAST_INTERRUPT_TIME_SDA_OFFSET = -27576;  // OSTime
constexpr int32_t LAST_INTERRUPT_SRR0_SDA_OFFSET = -27584;
constexpr uint32_t CONTEXT_SRR0 = 408;
// The OS's own and the application's masks of disabled interrupts.
constexpr uint32_t OS_INTERRUPT_MASK = 0x800000C4, USER_INTERRUPT_MASK = 0x800000C8;
// __OSDispatchInterrupt from where it runs the handler and then reschedules
// and loads the interrupted context; it wants its frame, the interrupt in
// r29, the context in r30 and the handler in r31.
constexpr uint32_t DISPATCH_TAIL = 0x80453AFC;
constexpr uint32_t DISPATCH_FRAME = 32;

// Sets each OS interrupt bit whose device bit is set.
struct BitMap
{
  uint32_t device, os;
};

uint32_t MapBits(uint32_t value, std::initializer_list<BitMap> bits)
{
  uint32_t os = 0;
  for (const BitMap& bit : bits)
    if (value & bit.device)
      os |= bit.os;
  return os;
}

// The OS interrupts (__OS_INTERRUPT_*, bit 31 - n) behind the PI causes. The
// SI and EXI raise none: their SDK natives finish every transfer in the call.
uint32_t OsInterrupts(Machine& machine, uint32_t cause)
{
  uint32_t os = 0;
  if (cause & PI_INT_MEM)  // MI_INTSR: the four protected regions, address
    os |= MapBits(machine.gfx.ReadMi(0x1E), {{0x1, 0x80000000}, {0x2, 0x40000000}, {0x4, 0x20000000},
                                             {0x8, 0x10000000}, {0x10, 0x08000000}});
  if ((cause & PI_INT_DSP) && machine.dsp)  // DSP_CSR: AI DMA, ARAM DMA, DSP
    os |= MapBits(machine.dsp->Read(0x500A), {{0x08, 0x04000000}, {0x20, 0x02000000}, {0x80, 0x01000000}});
  if (cause & PI_INT_AI)  // AI_CR: the sample counter
    os |= MapBits(machine.ai.Read32(0x00), {{0x08, 0x00800000}});
  return os | MapBits(cause, {{PI_INT_HSP, 0x20},
                              {PI_INT_DEBUG, 0x40},
                              {PI_INT_PE_FINISH, 0x1000},
                              {PI_INT_PE_TOKEN, 0x2000},
                              {PI_INT_VI, 0x80},
                              {PI_INT_DI, 0x400},
                              {PI_INT_RSW, 0x200},
                              {PI_INT_CP, 0x4000},
                              {PI_INT_PI, 0x100},
                              {PI_INT_WII_IPC, 0x10}});
}
}  // namespace
}  // namespace dualize

// __OSDispatchInterrupt(__OSException, OSContext* context): the
// highest-priority interrupt that is pending and enabled goes to its
// handler; with none, the interrupted context resumes.
extern "C" bool rvl_OSDispatchInterrupt(CPUState* ctx)
{
  using namespace dualize;
  Cpu& cpu = *static_cast<Cpu*>(ctx->external_user_data);
  Machine& machine = cpu.GetMachine();
  const Memory& memory = machine.memory;
  const uint32_t context = ctx->gpr[4];
  const uint32_t sda = ctx->gpr[13];
  const uint8_t* masks = memory.Effective(OS_INTERRUPT_MASK, 8);
  const uint8_t* priorities = memory.Effective(PRIORITY_TABLE, PRIORITY_ENTRIES * 4);
  const uint8_t* table = memory.Effective(sda + HANDLER_TABLE_SDA_OFFSET, 4);
  uint8_t* last = memory.Effective(sda + LAST_INTERRUPT_SRR0_SDA_OFFSET, 16);
  const uint8_t* saved = memory.Effective(context + CONTEXT_SRR0, 4);
  uint8_t* frame = memory.Effective(ctx->gpr[1] - DISPATCH_FRAME, DISPATCH_FRAME + 8);
  const uint8_t* handlers = table ? memory.Effective(Load32(table), 32 * 4) : nullptr;
  if (!masks || !priorities || !handlers || !last || !saved || !frame)
    return false;
  ctx->downcount -= 60;

  const uint32_t cause = machine.pi.Read32(0x00) & ~uint32_t{PI_INT_RST_BUTTON};
  const uint32_t enabled = cause & machine.pi.Read32(0x04);
  const uint32_t pending =
      enabled ? OsInterrupts(machine, cause) & ~(Load32(masks) | Load32(masks + 4)) : 0;
  uint32_t handler = 0;
  int interrupt = 0;
  if (pending)
  {
    for (uint32_t i = 0; i < PRIORITY_ENTRIES; ++i)
      if (const uint32_t mine = pending & Load32(priorities + i * 4))
      {
        interrupt = std::countl_zero(mine);
        break;
      }
    handler = Load32(handlers + interrupt * 4);
  }
  if (!handler)
  {
    ctx->gpr[3] = context;
    ctx->pc = OS_LOAD_CONTEXT;
    return true;
  }

  if (interrupt > 4)
  {
    constexpr int32_t base = LAST_INTERRUPT_SRR0_SDA_OFFSET;
    Store16(last + (LAST_INTERRUPT_SDA_OFFSET - base), static_cast<uint16_t>(interrupt));
    Store64(last + (LAST_INTERRUPT_TIME_SDA_OFFSET - base), cpu.Timebase());  // OSGetTime
    Store32(last, Load32(saved));
  }
  // The dispatcher's frame (its back chain and saved lr), then its tail.
  Store32(frame, ctx->gpr[1]);
  Store32(frame + DISPATCH_FRAME + 4, ctx->lr);
  ctx->gpr[1] -= DISPATCH_FRAME;
  ctx->gpr[29] = static_cast<uint32_t>(interrupt);
  ctx->gpr[30] = context;
  ctx->gpr[31] = handler;
  ctx->pc = DISPATCH_TAIL;
  return true;
}
