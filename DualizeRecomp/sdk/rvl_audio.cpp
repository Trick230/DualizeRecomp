// The Revolution SDK's audio libraries at their boundary with the hardware: AI
// DMA and the DSP mailbox go straight to the native DSP, and the interrupt
// handlers acknowledge it there. AX stays the game's code over them, but its
// ucode's boot and the mails of each frame are calls to the host's mixer. What
// runs once at boot (__OSInitAudioSystem, AIInit, DSPInit) keeps the interface
// block's registers.
//
// The natives are found by their SDK code (config/sdk.sigs); the addresses
// and small-data offsets that depend on the game's link come from the
// natives list's defines (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "apu/native_dsp.hpp"
#include "cpu/cpu.hpp"
#include "machine.hpp"

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Load32;
using dualize::Machine;
using dualize::Store32;
using dualize::apu::DspCore;

// The interface block's registers.
constexpr uint32_t MAIL_TO_DSP_HI = 0x5000, MAIL_TO_DSP_LO = 0x5002;
constexpr uint32_t MAIL_FROM_DSP_HI = 0x5004, MAIL_FROM_DSP_LO = 0x5006;
constexpr uint32_t CONTROL = 0x500A;
constexpr uint32_t AUDIO_DMA_START_HI = 0x5030, AUDIO_DMA_START_LO = 0x5032;
constexpr uint32_t AUDIO_DMA_CONTROL_LEN = 0x5036, AUDIO_DMA_BLOCKS_LEFT = 0x503A;
// In the control register: its interrupts, written with 1 to clear.
constexpr uint16_t CONTROL_AI_INTERRUPT = 0x08, CONTROL_ARAM_INTERRUPT = 0x20;
constexpr uint16_t CONTROL_DSP_INTERRUPT = 0x80;
constexpr uint16_t AUDIO_DMA_ENABLE = 0x8000;

// Both handlers' frames: the back chain and saved lr, r31, an OSContext.
constexpr uint32_t HANDLER_FRAME = 736;
constexpr uint32_t HANDLER_LR = HANDLER_FRAME + 4;
constexpr uint32_t HANDLER_R31 = 732;
constexpr uint32_t HANDLER_CONTEXT = 8;

// In a DSPTaskInfo.
constexpr uint32_t TASK_FLAGS = 8, TASK_IRAM = 12, TASK_IRAM_SIZE = 16;
constexpr uint32_t TASK_RESUME_CALLBACK = 44, TASK_NEXT = 56;
constexpr uint32_t TASK_FLAG_CANCEL = 2;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

DspCore& DspOf(CPUState* ctx)
{
  return *GetMachine(CpuOf(ctx)).dsp;
}

// Clears one interrupt of the control register; the others stay pending.
void Acknowledge(DspCore& dsp, uint16_t interrupt)
{
  const uint16_t others = (CONTROL_AI_INTERRUPT | CONTROL_ARAM_INTERRUPT | CONTROL_DSP_INTERRUPT) & ~interrupt;
  dsp.Write(CONTROL, static_cast<uint16_t>((dsp.Read(CONTROL) & ~others) | interrupt));
}

uint32_t ReadWord(Machine& machine, uint32_t address)
{
  const uint8_t* bytes = machine.memory.Effective(address, 4);
  return bytes ? Load32(bytes) : 0;
}

void WriteWord(Machine& machine, uint32_t address, uint32_t value)
{
  if (uint8_t* bytes = machine.memory.Effective(address, 4))
    Store32(bytes, value);
}

// The mail waiting is a mix's end and __DSPHandler would only tell the DSP to
// go on and resume the task that's running: returns that task with the mail
// taken, or 0 for the game's code to read it and switch tasks or finish.
uint32_t TakeFinishedFrame(CPUState* ctx, Machine& machine)
{
  DspCore& dsp = *machine.dsp;
  if (!dsp.HasAxYield())
    return 0;
  const uint32_t sda = ctx->gpr[13];
  const uint32_t task = ReadWord(machine, sda + DSP_CURRENT_TASK_SDA);
  if (!task || (ReadWord(machine, task + TASK_FLAGS) & TASK_FLAG_CANCEL))
    return 0;
  if (ReadWord(machine, sda + DSP_TASK_PENDING_SDA))
  {
    if (task != ReadWord(machine, sda + DSP_RUDE_TASK_SDA))
      return 0;
    WriteWord(machine, sda + DSP_RUDE_TASK_SDA, 0);
    WriteWord(machine, sda + DSP_TASK_PENDING_SDA, 0);
  }
  else if (ReadWord(machine, task + TASK_NEXT) || task != ReadWord(machine, sda + DSP_FIRST_TASK_SDA))
  {
    return 0;
  }
  dsp.TakeAxYield();
  return task;
}
}  // namespace

// u32 DSPCheckMailToDSP(void): the DSP hasn't taken the last mail.
extern "C" bool rvl_DSPCheckMailToDSP(CPUState* ctx)
{
  ctx->downcount -= 4;
  ctx->gpr[3] = (DspOf(ctx).Read(MAIL_TO_DSP_HI) >> 15) & 1;
  return true;
}

// u32 DSPCheckMailFromDSP(void): a mail is waiting.
extern "C" bool rvl_DSPCheckMailFromDSP(CPUState* ctx)
{
  ctx->downcount -= 4;
  ctx->gpr[3] = (DspOf(ctx).Read(MAIL_FROM_DSP_HI) >> 15) & 1;
  return true;
}

// u32 DSPReadMailFromDSP(void)
extern "C" bool rvl_DSPReadMailFromDSP(CPUState* ctx)
{
  ctx->downcount -= 5;
  DspCore& dsp = DspOf(ctx);
  const uint32_t high = dsp.Read(MAIL_FROM_DSP_HI);
  ctx->gpr[3] = high << 16 | dsp.Read(MAIL_FROM_DSP_LO);
  return true;
}

// void DSPSendMailToDSP(u32 mail)
extern "C" bool rvl_DSPSendMailToDSP(CPUState* ctx)
{
  ctx->downcount -= 5;
  DspCore& dsp = DspOf(ctx);
  const uint32_t mail = ctx->gpr[3];
  dsp.Write(MAIL_TO_DSP_HI, static_cast<uint16_t>(mail >> 16));
  dsp.Write(MAIL_TO_DSP_LO, static_cast<uint16_t>(mail));
  return true;
}

// void __DSP_boot_task(DSPTaskInfo* task): the AX ucode starts with no upload.
extern "C" bool rvl_DSPBootTask(CPUState* ctx)
{
  Machine& machine = GetMachine(CpuOf(ctx));
  const uint32_t task = ctx->gpr[3];
  if (!machine.dsp->StartAxTask(ReadWord(machine, task + TASK_IRAM), ReadWord(machine, task + TASK_IRAM_SIZE)))
    return false;
  ctx->downcount -= 100;
  return true;
}

// In __AXOutNewFrame, the mail with the command list's size (in r3, its
// address is in r29): the host's mixer takes the list here.
extern "C" bool rvl_AXSendCommandList(CPUState* ctx)
{
  if (!DspOf(ctx).SubmitAxFrame(ctx->gpr[29], ctx->gpr[3] & 0xFFFF))
    return false;
  ctx->downcount -= 20;
  ctx->pc = AX_LIST_SIZE_SENT;
  return true;
}

// In __AXOutNewFrame, the mail with the command list's address.
extern "C" bool rvl_AXCommandListSent(CPUState* ctx)
{
  if (DspOf(ctx).AxWaitsForListAddress())
    return false;
  ctx->downcount -= 5;
  ctx->pc = AX_LIST_ADDRESS_SENT;
  return true;
}

// void __DSPHandler(__OSInterrupt, OSContext* context): a mix's end resumes the
// task from here. Any other mail runs as the game's code, from where it waits
// for that mail.
extern "C" bool rvl_DSPHandler(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  const uint32_t context = ctx->gpr[4];
  const uint32_t sp = ctx->gpr[1];
  const uint32_t frame = sp - HANDLER_FRAME;
  uint8_t* bytes = GetMachine(cpu).memory.Effective(frame, HANDLER_LR + 4);
  if (!bytes)
    return false;
  ctx->downcount -= 40;
  Acknowledge(DspOf(ctx), CONTROL_DSP_INTERRUPT);
  Store32(bytes, sp);
  Store32(bytes + HANDLER_LR, ctx->lr);
  Store32(bytes + HANDLER_R31, ctx->gpr[31]);
  ctx->gpr[1] = frame;
  const uint32_t scratch = frame + HANDLER_CONTEXT;
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
  cpu.EnterHandlerContext();
  ctx->gpr[31] = context;
  ctx->pc = DSP_HANDLER_TAIL;
  if (const uint32_t task = TakeFinishedFrame(ctx, GetMachine(cpu)))
  {
    const uint32_t resume = ReadWord(GetMachine(cpu), task + TASK_RESUME_CALLBACK);
    ctx->gpr[3] = task;
    ctx->lr = DSP_HANDLER_DONE;
    ctx->pc = resume ? resume : DSP_HANDLER_DONE;
  }
  return true;
}

// void AIInitDMA(u32 start, u32 length)
extern "C" bool rvl_AIInitDMA(CPUState* ctx)
{
  ctx->downcount -= 30;
  DspCore& dsp = DspOf(ctx);
  const uint32_t start = ctx->gpr[3], length = ctx->gpr[4];
  dsp.Write(AUDIO_DMA_START_HI, static_cast<uint16_t>(start >> 16));
  dsp.Write(AUDIO_DMA_START_LO, static_cast<uint16_t>(start));
  dsp.Write(AUDIO_DMA_CONTROL_LEN, static_cast<uint16_t>((dsp.Read(AUDIO_DMA_CONTROL_LEN) & AUDIO_DMA_ENABLE) |
                                                         ((length >> 5) & 0x7FFF)));
  return true;
}

// void AIStartDMA(void)
extern "C" bool rvl_AIStartDMA(CPUState* ctx)
{
  ctx->downcount -= 5;
  DspCore& dsp = DspOf(ctx);
  dsp.Write(AUDIO_DMA_CONTROL_LEN, dsp.Read(AUDIO_DMA_CONTROL_LEN) | AUDIO_DMA_ENABLE);
  return true;
}

// void AIStopDMA(void)
extern "C" bool rvl_AIStopDMA(CPUState* ctx)
{
  ctx->downcount -= 5;
  DspCore& dsp = DspOf(ctx);
  dsp.Write(AUDIO_DMA_CONTROL_LEN, dsp.Read(AUDIO_DMA_CONTROL_LEN) & ~AUDIO_DMA_ENABLE);
  return true;
}

// u32 AIGetDMABytesLeft(void)
extern "C" bool rvl_AIGetDMABytesLeft(CPUState* ctx)
{
  ctx->downcount -= 4;
  ctx->gpr[3] = (uint32_t{DspOf(ctx).Read(AUDIO_DMA_BLOCKS_LEFT)} << 5) & 0x000FFFE0;
  return true;
}

// void __AIDHandler(__OSInterrupt, OSContext* context): AIRegisterDMACallback's
// callback runs as the SDK's own code does, on the SDK's stack if it was given
// one: from the handler's code it is an ordinary call, and it stays open to
// what a native's guest call holds back until its end.
extern "C" bool rvl_AIDHandler(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  const dualize::Memory& memory = GetMachine(cpu).memory;
  const uint32_t context = ctx->gpr[4];
  const uint32_t sp = ctx->gpr[1];
  const uint32_t sda = ctx->gpr[13];
  const uint32_t frame = sp - HANDLER_FRAME;
  const uint8_t* callback = memory.Effective(sda + AI_DMA_CALLBACK_SDA, 4);
  uint8_t* active = memory.Effective(sda + AI_DMA_CALLBACK_ACTIVE_SDA, 4);
  const uint8_t* stack = memory.Effective(sda + AI_CALLBACK_STACK_SDA, 4);
  uint8_t* bytes = memory.Effective(frame, HANDLER_LR + 4);
  if (!callback || !active || !stack || !bytes)
    return false;
  ctx->downcount -= 40;
  Acknowledge(DspOf(ctx), CONTROL_AI_INTERRUPT);
  Store32(bytes, sp);
  Store32(bytes + HANDLER_LR, ctx->lr);
  Store32(bytes + HANDLER_R31, ctx->gpr[31]);
  ctx->gpr[1] = frame;
  const uint32_t scratch = frame + HANDLER_CONTEXT;
  cpu.CallGuest(OS_CLEAR_CONTEXT, {scratch});
  cpu.CallGuest(OS_SET_CURRENT_CONTEXT, {scratch});
  cpu.EnterHandlerContext();
  ctx->gpr[31] = context;
  const uint32_t function = Load32(callback);
  if (!function || Load32(active))
  {
    ctx->pc = AI_HANDLER_TAIL;
    return true;
  }
  Store32(active, 1);
  ctx->gpr[3] = function;
  if (Load32(stack))
  {
    ctx->lr = AI_HANDLER_AFTER_CALLBACK;
    ctx->pc = AI_CALLBACK_STACK_SWITCH;
  }
  else
  {
    ctx->pc = AI_HANDLER_CALL_CALLBACK;
  }
  return true;
}
