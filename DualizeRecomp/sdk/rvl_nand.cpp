// The Revolution SDK's NAND at its boundary with IOS. NAND* and ISFS_* stay the
// game's code down to the IOS client calls; when those name the file system
// (/dev/fs or a file on the NAND) they go straight to the host's FS, which
// answers in the call. A synchronous call returns the answer. An asynchronous
// one takes a request block from the IPC heap only to bring the callback back
// through the IPC interrupt, in the SDK's own reply path, when the NAND would
// have finished. Calls to every other device stay IOS requests.
//
// The natives are found by their SDK code (config/sdk.sigs); the addresses
// and small-data offsets that depend on the game's link come from the
// natives list's defines (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::IosKernel;
using dualize::Load32;
using dualize::Machine;
using dualize::Store32;
namespace ios = dualize::ios;

// IOS request block (as the SDK lays it out)
constexpr uint32_t REQUEST_RESULT = 4;
constexpr uint32_t REQUEST_FD = 8;
constexpr uint32_t REQUEST_CALLBACK = 32;
constexpr uint32_t REQUEST_CALLBACK_DATA = 36;
constexpr uint32_t REQUEST_SIZE = 64;
constexpr uint32_t REQUEST_ALIGNMENT = 32;
constexpr uint32_t VECTOR_SIZE = 8;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

IosKernel& KernelOf(CPUState* ctx)
{
  return GetMachine(CpuOf(ctx)).ios;
}

uint32_t Physical(uint32_t address)
{
  return address ? address - 0x80000000u : 0;
}

bool ReturnNow(CPUState* ctx, const ios::Reply& reply)
{
  ctx->downcount -= 60;
  ctx->gpr[3] = static_cast<uint32_t>(reply.value);
  return true;
}

// An asynchronous call: the block its reply will come back in, 0 when the heap
// has none (the SDK's code then reports it) or there's no callback to bring
// back (the SDK's code then waits).
uint32_t BeginAsync(CPUState* ctx, uint32_t callback)
{
  if (!callback)
    return 0;
  const dualize::Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const uint8_t* heap = memory.Effective(ctx->gpr[13] + IPC_HEAP_SDA_OFFSET, 4);
  return heap ? CpuOf(ctx).CallGuest(IOS_ALLOC_ALIGNED, {Load32(heap), REQUEST_SIZE, REQUEST_ALIGNMENT}) : 0;
}

bool FinishAsync(CPUState* ctx, uint32_t block, int32_t fd, uint32_t callback, uint32_t data,
                 const ios::Reply& reply)
{
  Cpu& cpu = CpuOf(ctx);
  uint8_t* bytes = GetMachine(cpu).memory.Effective(block, REQUEST_SIZE);
  std::memset(bytes, 0, REQUEST_SIZE);
  Store32(bytes + REQUEST_RESULT, static_cast<uint32_t>(reply.value));
  Store32(bytes + REQUEST_CALLBACK, callback);
  Store32(bytes + REQUEST_CALLBACK_DATA, data);
  // The field holds the command in a reply, and 0 has no pointers to fix up.
  Store32(bytes + REQUEST_FD, 0);
  cpu.CallGuest(IPC_PROF_QUEUE_REQ, {block, static_cast<uint32_t>(fd)});
  KernelOf(ctx).PostReply(Physical(block), reply.delay);
  ctx->downcount -= 80;
  ctx->gpr[3] = 0;
  return true;
}

ios::Reply Ioctlv(CPUState* ctx, int32_t fd, uint32_t request, uint32_t in, uint32_t io, const uint8_t* table)
{
  const auto vector = [&](uint32_t i) {
    return ios::IOCtlVRequest::Vector{Physical(Load32(table + i * VECTOR_SIZE)),
                                      Load32(table + i * VECTOR_SIZE + 4)};
  };
  std::vector<ios::IOCtlVRequest::Vector> in_vectors, io_vectors;
  for (uint32_t i = 0; i < in; ++i)
    in_vectors.push_back(vector(i));
  for (uint32_t i = 0; i < io; ++i)
    io_vectors.push_back(vector(in + i));
  return KernelOf(ctx).HostIoctlv(fd, request, std::move(in_vectors), std::move(io_vectors));
}
}  // namespace

// s32 IOS_Open(const char* path, u32 flags)
extern "C" bool rvl_IOS_Open(CPUState* ctx)
{
  IosKernel& kernel = KernelOf(ctx);
  const std::string path = kernel.ReadPath(Physical(ctx->gpr[3]));
  if (!IosKernel::IsFsFile(path))
    return false;
  return ReturnNow(ctx, kernel.HostOpen(path, ctx->gpr[4]));
}

// s32 IOS_OpenAsync(const char* path, u32 flags, callback, void* data)
extern "C" bool rvl_IOS_OpenAsync(CPUState* ctx)
{
  IosKernel& kernel = KernelOf(ctx);
  const std::string path = kernel.ReadPath(Physical(ctx->gpr[3]));
  const uint32_t flags = ctx->gpr[4], callback = ctx->gpr[5], data = ctx->gpr[6];
  if (!IosKernel::IsFsFile(path))
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, -1, callback, data, kernel.HostOpen(path, flags));
}

// s32 IOS_Close(s32 fd)
extern "C" bool rvl_IOS_Close(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  return ReturnNow(ctx, KernelOf(ctx).HostClose(fd));
}

// s32 IOS_CloseAsync(s32 fd, callback, void* data)
extern "C" bool rvl_IOS_CloseAsync(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const uint32_t callback = ctx->gpr[4], data = ctx->gpr[5];
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, fd, callback, data, KernelOf(ctx).HostClose(fd));
}

// s32 IOS_Read(s32 fd, void* buffer, u32 size)
extern "C" bool rvl_IOS_Read(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  return ReturnNow(ctx, KernelOf(ctx).HostReadWrite(fd, Physical(ctx->gpr[4]), ctx->gpr[5], false));
}

// s32 IOS_ReadAsync(s32 fd, void* buffer, u32 size, callback, void* data)
extern "C" bool rvl_IOS_ReadAsync(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const uint32_t buffer = ctx->gpr[4], size = ctx->gpr[5], callback = ctx->gpr[6], data = ctx->gpr[7];
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, fd, callback, data, KernelOf(ctx).HostReadWrite(fd, Physical(buffer), size, false));
}

// s32 IOS_Write(s32 fd, const void* buffer, u32 size)
extern "C" bool rvl_IOS_Write(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  return ReturnNow(ctx, KernelOf(ctx).HostReadWrite(fd, Physical(ctx->gpr[4]), ctx->gpr[5], true));
}

// s32 IOS_WriteAsync(s32 fd, const void* buffer, u32 size, callback, void* data)
extern "C" bool rvl_IOS_WriteAsync(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const uint32_t buffer = ctx->gpr[4], size = ctx->gpr[5], callback = ctx->gpr[6], data = ctx->gpr[7];
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, fd, callback, data, KernelOf(ctx).HostReadWrite(fd, Physical(buffer), size, true));
}

// s32 IOS_Seek(s32 fd, s32 offset, u32 mode)
extern "C" bool rvl_IOS_Seek(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  return ReturnNow(ctx, KernelOf(ctx).HostSeek(fd, static_cast<int32_t>(ctx->gpr[4]), ctx->gpr[5]));
}

// s32 IOS_SeekAsync(s32 fd, s32 offset, u32 mode, callback, void* data)
extern "C" bool rvl_IOS_SeekAsync(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const int32_t offset = static_cast<int32_t>(ctx->gpr[4]);
  const uint32_t mode = ctx->gpr[5], callback = ctx->gpr[6], data = ctx->gpr[7];
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, fd, callback, data, KernelOf(ctx).HostSeek(fd, offset, mode));
}

// s32 IOS_Ioctl(s32 fd, s32 request, void* in, u32 in_size, void* out, u32 out_size)
extern "C" bool rvl_IOS_Ioctl(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  return ReturnNow(ctx, KernelOf(ctx).HostIoctl(fd, ctx->gpr[4], Physical(ctx->gpr[5]), ctx->gpr[6],
                                                Physical(ctx->gpr[7]), ctx->gpr[8]));
}

// s32 IOS_IoctlAsync(s32 fd, s32 request, void* in, u32 in_size, void* out,
//                    u32 out_size, callback, void* data)
extern "C" bool rvl_IOS_IoctlAsync(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const uint32_t request = ctx->gpr[4], in = ctx->gpr[5], in_size = ctx->gpr[6];
  const uint32_t out = ctx->gpr[7], out_size = ctx->gpr[8], callback = ctx->gpr[9], data = ctx->gpr[10];
  if (!KernelOf(ctx).IsFsFd(fd))
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, fd, callback, data,
                     KernelOf(ctx).HostIoctl(fd, request, Physical(in), in_size, Physical(out), out_size));
}

// s32 IOS_Ioctlv(s32 fd, s32 request, u32 in, u32 io, IOVector* vectors)
extern "C" bool rvl_IOS_Ioctlv(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const uint32_t in = ctx->gpr[5], io = ctx->gpr[6];
  const uint8_t* table = GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[7], (in + io) * VECTOR_SIZE);
  if (!KernelOf(ctx).IsFsFd(fd) || !table)
    return false;
  return ReturnNow(ctx, Ioctlv(ctx, fd, ctx->gpr[4], in, io, table));
}

// s32 IOS_IoctlvAsync(s32 fd, s32 request, u32 in, u32 io, IOVector* vectors,
//                     callback, void* data)
extern "C" bool rvl_IOS_IoctlvAsync(CPUState* ctx)
{
  const int32_t fd = static_cast<int32_t>(ctx->gpr[3]);
  const uint32_t request = ctx->gpr[4], in = ctx->gpr[5], io = ctx->gpr[6], vectors = ctx->gpr[7];
  const uint32_t callback = ctx->gpr[8], data = ctx->gpr[9];
  const uint8_t* table = GetMachine(CpuOf(ctx)).memory.Effective(vectors, (in + io) * VECTOR_SIZE);
  if (!KernelOf(ctx).IsFsFd(fd) || !table)
    return false;
  const uint32_t block = BeginAsync(ctx, callback);
  if (!block)
    return false;
  return FinishAsync(ctx, block, fd, callback, data, Ioctlv(ctx, fd, request, in, io, table));
}
