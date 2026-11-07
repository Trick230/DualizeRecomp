// NiGHTS' Options screen (the title module's ModeOption): its "Check Aspect
// Ratio" row opens the runtime's graphics settings instead. The window keeps
// the console's aspect itself, so the test screen behind that row has nothing
// left to check.
//
// Found by their code in config/nights.sigs; ModeOption's update and where it
// leaves come from the defines there (ppc_natives.h).
//
// The row's label is no message but a line of a texture with all the
// list's labels; the disc serves that texture with the line redrawn.

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "kernel/ios/disc.hpp"
#include "machine.hpp"
#include "ui/game_assets.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
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

// The Options list's labels: image 6 of its textures, a 256x256 CMPR of grey
// text on black, a 24-texel line per label. The aspect row's line (the
// second) is redrawn as "Graphics Settings" from the other lines' letters,
// the G from a C.
constexpr const char* OPTION_TEXTURES = "files/csd/textures/option_allus.tls";
constexpr uint32_t LABELS_IMAGE = 6, LINE = 24, ASPECT_LINE = 1;

struct Cut
{
  int line, x0, x1;  // texels
};
constexpr Cut CUT_C{1, 3, 18}, CUT_E{1, 32, 43}, CUT_SETTINGS{0, 111, 193};
constexpr std::array<Cut, 7> CUT_RAPHICS{{
    {2, 131, 137},  // r, of Controller
    {0, 15, 24},    // a, of Language
    {1, 99, 111},   // p, of Aspect
    {1, 20, 30},    // h, of Check
    {0, 152, 155},  // i, of Settings
    {1, 45, 55},    // c, of Check
    {1, 89, 97},    // s, of Aspect
}};

using Glyph = std::array<std::array<uint8_t, 32>, LINE>;  // rows of up to 32 texels

struct Labels
{
  dualize::Image& image;

  uint8_t& At(uint32_t x, uint32_t y) { return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4]; }

  Glyph Take(const Cut& cut, int* width)
  {
    Glyph glyph{};
    *width = cut.x1 - cut.x0;
    for (uint32_t y = 0; y < LINE; ++y)
    {
      for (int x = 0; x < *width; ++x)
        glyph[y][x] = At(cut.x0 + x, cut.line * LINE + y);
    }
    return glyph;
  }

  // A C with a G's bar (as thick as an e's) and the spur down its right end.
  Glyph MakeG(int* width)
  {
    int w = 0, e_w = 0;
    Glyph g = Take(CUT_C, &w);
    const Glyph e = Take(CUT_E, &e_w);
    int top = LINE, bottom = 0;
    for (int y = 0; y < static_cast<int>(LINE); ++y)
    {
      for (int x = 0; x < w; ++x)
      {
        if (g[y][x] > 128)
        {
          top = std::min(top, y);
          bottom = std::max(bottom, y);
        }
      }
    }
    const int mid = (top + bottom) / 2 + 1;
    int bar = 8, best = -1;
    for (int y = 8; y < 18; ++y)
    {
      int sum = 0;
      for (int x = 0; x < e_w; ++x)
        sum += e[y][x];
      if (sum > best)
      {
        best = sum;
        bar = y;
      }
    }
    for (int k = 0; k < 3; ++k)
    {
      uint8_t level = 0;
      for (int x = 2; x < e_w - 2; ++x)
        level = std::max(level, e[bar - 1 + k][x]);
      for (int x = w / 2 + 1; x < w; ++x)
        g[mid - 1 + k][x] = std::max(g[mid - 1 + k][x], level);
    }
    for (int j = 0; j < 4; ++j)
    {
      uint8_t level = 0;
      for (int y = mid; y <= bottom; ++y)
        level = std::max(level, g[y][w - 4 + j]);
      for (int y = mid; y <= bottom; ++y)
        g[y][w - 4 + j] = std::max(g[y][w - 4 + j], level);
    }
    *width = w + 1;
    return g;
  }

  void Redraw()
  {
    std::array<std::array<uint8_t, 256>, LINE> line{};
    int pen = 3;
    const auto put = [&](const Glyph& glyph, int width) {
      for (uint32_t y = 0; y < LINE; ++y)
      {
        for (int x = 0; x < width && pen + x < 256; ++x)
          line[y][pen + x] = glyph[y][x];
      }
      pen += width;
    };
    int width = 0;
    put(MakeG(&width), width);
    for (const Cut& cut : CUT_RAPHICS)
    {
      pen += 2;
      put(Take(cut, &width), width);
    }
    pen += 8;
    for (int x = CUT_SETTINGS.x0; x < CUT_SETTINGS.x1; x += 32)
    {
      const Cut part{CUT_SETTINGS.line, x, std::min(x + 32, CUT_SETTINGS.x1)};
      put(Take(part, &width), width);
    }
    for (uint32_t y = 0; y < LINE; ++y)
    {
      for (uint32_t x = 0; x < 256; ++x)
      {
        uint8_t* p = &image.rgba[(static_cast<size_t>(ASPECT_LINE * LINE + y) * image.width + x) * 4];
        p[0] = p[1] = p[2] = line[y][x];
      }
    }
  }
};
}  // namespace

namespace dualize
{
void PatchNightsDisc(ios::Disc& disc, const std::filesystem::path& game)
{
  const std::filesystem::path path = game / OPTION_TEXTURES;
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  auto labels = DecodeTplImage(bytes, LABELS_IMAGE);
  if (!labels || labels->format != 14 || labels->image.width != 256 || labels->image.height < 2 * LINE)
    return;
  Labels{labels->image}.Redraw();
  EncodeGreyCmpr(labels->image, ASPECT_LINE * LINE, (ASPECT_LINE + 1) * LINE, bytes.data() + labels->offset);
  disc.OverrideFile(path, std::move(bytes));
}
}  // namespace dualize

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
}  // namespace

// ModeOption's constructor: the screen's help line is copied from the
// messages when the cursor moves, so renaming them here reaches it.
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
