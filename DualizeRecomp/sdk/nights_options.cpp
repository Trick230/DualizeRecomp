// NiGHTS' Options screen (the title module's ModeOption): its "Check Aspect
// Ratio" row opens the runtime's graphics settings instead. The window keeps
// the console's aspect itself, so the test screen behind that row has nothing
// left to check.
//
// Found by their code in config/nights.sigs; ModeOption's update and where it
// leaves come from the defines there (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "machine.hpp"

#include <cstring>
#include <string>
#include <string_view>

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Memory;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// One English line, UTF-16BE in the game's message data, swapped for another
// no longer than it. A message may go on after the line (a newline, then the
// screen's button guide): the rest moves up behind the new line and the room
// left at the end is padded with terminators.
struct Message
{
  std::u16string_view from, to;
};
constexpr Message MESSAGES[] = {
    {u"Check Aspect Ratio", u"Graphics Settings"},
    {u"Check the height/width ratio of your screen", u"Change the resolution and image quality"},
};

std::u16string BigEndian(std::u16string_view text)
{
  std::u16string out;
  for (char16_t c : text)
    out.push_back(static_cast<char16_t>((c >> 8) | (c << 8)));
  return out;
}

// Replaces every copy of each message in MEM2, where the game keeps its
// message data.
void RenameMessages(const Memory& memory)
{
  uint8_t* const data = memory.Effective(0x90000000u, Memory::MEM2_SIZE);
  if (data == nullptr)
    return;
  uint8_t* const end = data + Memory::MEM2_SIZE;
  for (const Message& message : MESSAGES)
  {
    const std::u16string from = BigEndian(message.from);
    const std::u16string to = BigEndian(message.to);
    const auto* needle = reinterpret_cast<const uint8_t*>(from.data());
    const size_t bytes = from.size() * 2;
    uint8_t* at = data;
    while (static_cast<size_t>(end - at) >= bytes + 2)
    {
      at = static_cast<uint8_t*>(std::memchr(at, needle[0], static_cast<size_t>(end - at) - bytes - 1));
      if (at == nullptr)
        break;
      // Messages are 2-byte aligned; the line ends at a terminator or a newline.
      const bool line_end = at[bytes] == 0 && (at[bytes + 1] == 0 || at[bytes + 1] == 0x0A);
      if ((reinterpret_cast<uintptr_t>(at) & 1) != 0 || !line_end || std::memcmp(at, needle, bytes) != 0)
      {
        ++at;
        continue;
      }
      uint8_t* tail = at + bytes;
      uint8_t* stop = tail;
      while (stop + 1 < end && (stop[0] | stop[1]) != 0)
        stop += 2;
      const size_t tail_bytes = static_cast<size_t>(stop - tail);
      const size_t to_bytes = to.size() * 2;
      std::memcpy(at, to.data(), to_bytes);
      std::memmove(at + to_bytes, tail, tail_bytes);
      std::memset(at + to_bytes + tail_bytes, 0, bytes - to_bytes);
      at = stop;
    }
  }
}
}  // namespace

// ModeOption's constructor: the screen's help line is copied from the
// messages when the cursor moves, so renaming them here reaches it. The rows'
// labels were built before this, from the messages as they were (still to do).
// Declines: the game's code goes on.
extern "C" bool nights_ModeOptionCtor(CPUState* ctx)
{
  RenameMessages(GetMachine(CpuOf(ctx)).memory);
  return false;
}

// ModeOption's update, where A on the second row (the aspect test) begins the
// test screen: the graphics menu opens instead and the update ends as it does
// when nothing was chosen. The decide sound has already played.
extern "C" bool nights_ModeOptionAspect(CPUState* ctx)
{
  GetMachine(CpuOf(ctx)).OpenGraphicsMenu();
  ctx->pc = NIGHTS_MODE_OPTION_UPDATE_DONE;
  return true;
}
