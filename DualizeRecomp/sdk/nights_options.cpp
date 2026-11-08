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
#include <bit>
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
    {u"Change Nintendo Wi-Fi Connection and settings", u"Change the game's settings"},
    {u"Turn the control window display on/off", u"Turn the controller's rumble on/off"},
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
      // Keyed on the first character's low byte: the high byte of big-endian
      // UTF-16 text is 0, as is most of MEM2, so a search for it stops everywhere.
      uint8_t* low = static_cast<uint8_t*>(std::memchr(at + 1, needle[1], static_cast<size_t>(end - at) - bytes - 2));
      if (low == nullptr)
        break;
      at = low - 1;
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

// The controller window row's line (the third), whose setting now turns the
// controller's rumble on and off: "Enable Rumble", the word Enable as the
// Weather Link row has it.
constexpr uint32_t RUMBLE_LINE = 2;
constexpr Cut CUT_ENABLE{3, 3, 69};
constexpr std::array<Cut, 6> CUT_RUMBLE{{
    {4, 2, 12},     // R, of Register
    {0, 54, 63},    // u, of Language
    {7, 136, 152},  // m, of Chime
    {3, 39, 50},    // b, of Enable
    {3, 53, 55},    // l, of Enable
    {3, 57, 68},    // e, of Enable
}};

// The 2P Race window's description: image 13 of the 2P select textures, a
// 512x128 CMPR of grey text on black. Its last two lines (rows 47-82) invite
// play over Nintendo Wi-Fi Connection; the rows from 40 to 88 hold only them.
constexpr const char* MODE_2P_TEXTURES = "files/csd/textures/mode2Pselectus.tls";
constexpr uint32_t RACE_TEXT_IMAGE = 13, RACE_WIFI_Y0 = 40, RACE_WIFI_Y1 = 88;

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

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

  // A label line being composed left to right.
  struct Line
  {
    std::array<std::array<uint8_t, 256>, LINE> rows{};
    int pen = 3;

    void Put(const Glyph& glyph, int width)
    {
      for (uint32_t y = 0; y < LINE; ++y)
      {
        for (int x = 0; x < width && pen + x < 256; ++x)
          rows[y][pen + x] = glyph[y][x];
      }
      pen += width;
    }
  };

  // A cut wider than a glyph (a whole word), in glyph-sized parts.
  void PutWide(Line& line, const Cut& cut)
  {
    int width = 0;
    for (int x = cut.x0; x < cut.x1; x += 32)
      line.Put(Take({cut.line, x, std::min(x + 32, cut.x1)}, &width), width);
  }

  void Store(uint32_t index, const Line& line)
  {
    for (uint32_t y = 0; y < LINE; ++y)
    {
      for (uint32_t x = 0; x < 256; ++x)
      {
        uint8_t* p = &image.rgba[(static_cast<size_t>(index * LINE + y) * image.width + x) * 4];
        p[0] = p[1] = p[2] = line.rows[y][x];
      }
    }
  }

  void Redraw()
  {
    Line line;
    int width = 0;
    line.Put(MakeG(&width), width);
    for (const Cut& cut : CUT_RAPHICS)
    {
      line.pen += 2;
      line.Put(Take(cut, &width), width);
    }
    line.pen += 8;
    PutWide(line, CUT_SETTINGS);
    Store(ASPECT_LINE, line);
  }

  // Reads only lines the aspect redraw leaves alone (0, 3, 4 and 7).
  void RedrawRumble()
  {
    Line line;
    int width = 0;
    PutWide(line, CUT_ENABLE);
    line.pen += 6;
    for (const Cut& cut : CUT_RUMBLE)
    {
      line.Put(Take(cut, &width), width);
      line.pen += 2;
    }
    Store(RUMBLE_LINE, line);
  }
};
}  // namespace

namespace dualize
{
void PatchNightsDisc(ios::Disc& disc, const std::filesystem::path& game)
{
  {
    const std::filesystem::path path = game / OPTION_TEXTURES;
    std::vector<uint8_t> bytes = ReadFile(path);
    auto labels = DecodeTplImage(bytes, LABELS_IMAGE);
    if (labels && labels->format == 14 && labels->image.width == 256 && labels->image.height >= 8 * LINE)
    {
      Labels{labels->image}.Redraw();
      Labels{labels->image}.RedrawRumble();
      static_assert(RUMBLE_LINE == ASPECT_LINE + 1);
      EncodeGreyCmpr(labels->image, ASPECT_LINE * LINE, (RUMBLE_LINE + 1) * LINE, bytes.data() + labels->offset);
      disc.OverrideFile(path, std::move(bytes));
    }
  }
  {
    const std::filesystem::path path = game / MODE_2P_TEXTURES;
    std::vector<uint8_t> bytes = ReadFile(path);
    auto text = DecodeTplImage(bytes, RACE_TEXT_IMAGE);
    if (text && text->format == 14 && text->image.width == 512 && text->image.height == 128)
    {
      for (uint32_t y = RACE_WIFI_Y0; y < RACE_WIFI_Y1; ++y)
        std::fill_n(&text->image.rgba[static_cast<size_t>(y) * text->image.width * 4], text->image.width * 4, 0);
      EncodeGreyCmpr(text->image, RACE_WIFI_Y0, RACE_WIFI_Y1, bytes.data() + text->offset);
      disc.OverrideFile(path, std::move(bytes));
    }
  }
}
}  // namespace dualize

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Machine;
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

// A screen's layout animation (its holder at `holder`): the animation the
// holder points to, its current frame (two copies) and its last frame.
constexpr uint32_t ANIMATION_FRAME = 0x90, ANIMATION_PREVIOUS_FRAME = 0x94, ANIMATION_END = 0xA0;

// Moves a just begun animation to `fraction` of its length and poses the
// layout there as starting it did (its virtual update, +0x20, by `step`, the
// game's own tiny step in the direction it plays), so no frame shows its start.
constexpr uint32_t ANIMATION_UPDATE = 0x20;

void SeekAnimation(CPUState* ctx, uint32_t holder, float fraction, float step)
{
  Cpu& cpu = CpuOf(ctx);
  Memory& memory = GetMachine(cpu).memory;
  const uint32_t pointer = dualize::Load32(memory.Effective(holder + 4, 4));
  if (pointer == 0)
    return;
  const uint32_t address = dualize::Load32(memory.Effective(pointer, 4));
  uint8_t* animation = memory.Effective(address, ANIMATION_END + 4);
  const float end = std::bit_cast<float>(dualize::Load32(animation + ANIMATION_END));
  const uint32_t frame = std::bit_cast<uint32_t>(end * fraction);
  dualize::Store32(animation + ANIMATION_FRAME, frame);
  dualize::Store32(animation + ANIMATION_PREVIOUS_FRAME, frame);
  const uint32_t table = dualize::Load32(animation);
  ctx->fpr[1] = step;
  cpu.CallGuest(dualize::Load32(memory.Effective(table + ANIMATION_UPDATE, 4)), {address});
}

// The settings list's state 0 (r30 the screen), its "setting_open" begun:
// its first half moves the top level's icons out, the second fades the list
// in, so it starts halfway. The big icon still showed the top level's (the
// Wii) until the animation ended and the list set the row's; it is set now.
// Then the instruction hooked (addi r3, r1, 0x138).
extern "C" bool nights_ModeOptionListOpen(CPUState* ctx)
{
  SeekAnimation(ctx, ctx->gpr[30] + 0x74, 0.5f, 0.001f);
  CpuOf(ctx).CallGuest(NIGHTS_MODE_OPTION_ROW_ICON, {ctx->gpr[30]});
  ctx->gpr[3] = ctx->gpr[1] + 0x138;
  ctx->pc = NIGHTS_MODE_OPTION_LIST_OPEN_NEXT;
  return true;
}

// The top level's cancel path (r31 the screen), its icons' "select_In" begun
// in reverse: the list has faded out already, so the animation starts at its
// end, icons hidden, and the screen leaves. Then the instruction hooked
// (addi r3, r1, 0x40).
extern "C" bool nights_ModeOptionTopClose(CPUState* ctx)
{
  SeekAnimation(ctx, ctx->gpr[31] + 0x74, 0.0f, -0.001f);
  ctx->gpr[3] = ctx->gpr[1] + 0x40;
  ctx->pc = NIGHTS_MODE_OPTION_TOP_CLOSE_NEXT;
  return true;
}

// The top level's cancel path, its cancel sound (li r3, 2 then the call):
// skipped, B on the list having played it.
extern "C" bool nights_ModeOptionTopCancelSound(CPUState* ctx)
{
  ctx->pc = NIGHTS_MODE_OPTION_TOP_CANCEL_SOUND_NEXT;
  return true;
}

// ModeOption's top level, entered (r3 the screen): the Wi-Fi globe has no use
// here, so the first time (its state 0, cursor on the globe) the screen goes
// to the Wii settings list as A on its icon does (cursor on it, submode 1,
// its state 0) and runs the list's update in its place, so no frame shows the
// icons. Otherwise the hooked stwu r1, -0x80(r1) and the top level go on.
extern "C" bool nights_ModeOptionTopEnter(CPUState* ctx)
{
  Memory& memory = GetMachine(CpuOf(ctx)).memory;
  uint8_t* screen = memory.Effective(ctx->gpr[3], 0xF0);
  if (dualize::Load32(screen + 0x70) == 0 && dualize::Load32(screen + 0xEC) == 0)
  {
    dualize::Store32(screen + 0xEC, 1);
    dualize::Store32(screen + 0x6C, 1);
    ctx->pc = NIGHTS_MODE_OPTION_LIST_UPDATE;
    return true;
  }
  dualize::Store32(memory.Effective(ctx->gpr[1] - 0x80, 4), ctx->gpr[1]);
  ctx->gpr[1] -= 0x80;
  ctx->pc = NIGHTS_MODE_OPTION_TOP_BODY;
  return true;
}

// ModeOption's top level, its entrance (state 0): reached only when the
// settings list comes back (B), so the screen leaves through its cancel path.
extern "C" bool nights_ModeOptionTop(CPUState* ctx)
{
  ctx->pc = NIGHTS_MODE_OPTION_TOP_CANCEL;
  return true;
}

// The 2P menu's top update, each frame (r3 the menu): the main menu leaves
// with the screen fading to black over half a second, and the 2P menu starts
// drawing its stage before that ends, so the stage showed and dimmed before
// the menu faded in. While the menu is still starting (its state 0), it waits
// for the screen to be black. Otherwise the hooked stwu r1, -0x80(r1).
constexpr uint32_t MENU_STATE = 0x68, SCREEN_FADER = 0x44, FADER_VALUE = 0x20, FADER_TARGET = 0x24;

extern "C" bool nights_Mode2PTop(CPUState* ctx)
{
  Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const uint32_t manager = dualize::Load32(memory.Effective(NIGHTS_MODE_MANAGER, 4));
  const bool starting = dualize::Load32(memory.Effective(ctx->gpr[3] + MENU_STATE, 4)) == 0;
  const uint32_t fader = starting && manager != 0 ? dualize::Load32(memory.Effective(manager + SCREEN_FADER, 4)) : 0;
  if (fader != 0)
  {
    const uint8_t* state = memory.Effective(fader, FADER_TARGET + 4);
    const float value = std::bit_cast<float>(dualize::Load32(state + FADER_VALUE));
    if (std::bit_cast<float>(dualize::Load32(state + FADER_TARGET)) == 1.0f && value < 1.0f)
    {
      ctx->pc = ctx->lr;
      return true;
    }
  }
  dualize::Store32(memory.Effective(ctx->gpr[1] - 0x80, 4), ctx->gpr[1]);
  ctx->gpr[1] -= 0x80;
  ctx->pc = NIGHTS_MODE_2P_TOP_BODY;
  return true;
}

// Mode2P's update, Start chosen on the race or battle window: the opponent
// list in between offers only Nearby offline, so the update takes its Nearby
// path (which plays the decide sound) and the list never shows.
extern "C" bool nights_Mode2PStart(CPUState* ctx)
{
  ctx->pc = NIGHTS_MODE_2P_NEARBY;
  return true;
}

// Mode2P's Nearby path, past its switch to the controller screen (state 1):
// the window closes as Quit closes it, with the decide sound (r3, r4 its
// arguments) instead of Quit's cancel sound.
extern "C" bool nights_Mode2PNearby(CPUState* ctx)
{
  ctx->gpr[3] = 1;
  ctx->gpr[4] = 1;
  ctx->pc = NIGHTS_MODE_2P_CLOSE;
  return true;
}

// Mode2P's Quit path, its window closing (r29 the menu): Quit then waits in
// state 3 to leave the menu; after Nearby the menu keeps its state 1.
extern "C" bool nights_Mode2PClosing(CPUState* ctx)
{
  uint8_t* state = GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[29] + 0x6C, 4);
  if (dualize::Load32(state) != 1)
    dualize::Store32(state, 3);
  ctx->pc = NIGHTS_MODE_2P_DONE;
  return true;
}

// Mode2P's controller screen, B: the menu has switched back to its state 1,
// which opens the race or battle window; the list's return animation and
// the list itself are left out.
extern "C" bool nights_Mode2PControllersBack(CPUState* ctx)
{
  ctx->pc = NIGHTS_MODE_2P_CONTROLLERS_DONE;
  return true;
}

// Mode2P's resume (r31 the menu), back from a 2P game: instead of the
// opponent list the menu reopens the race or battle window, its state 1 (the
// window's opening animation, then its Start and Quit buttons).
extern "C" bool nights_Mode2PResume(CPUState* ctx)
{
  dualize::Store32(GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[31] + 0x6C, 4), 1);
  ctx->pc = NIGHTS_MODE_2P_RESUME_DONE;
  return true;
}

// Play's reads of the controller window option (lbz r3 and lbz r0, 0xb0 of the
// options), each frame: the option is the rumble's now (its row reads Enable
// Rumble), and the window is off for good.
extern "C" bool nights_ControllerWindowShown(CPUState* ctx)
{
  Machine& machine = GetMachine(CpuOf(ctx));
  machine.AllowRumble(*machine.memory.Effective(ctx->gpr[3] + 0xB0, 1) != 0);
  ctx->gpr[3] = 0;
  ctx->pc = NIGHTS_CONTROLLER_WINDOW_SHOWN_NEXT;
  return true;
}

extern "C" bool nights_ControllerWindowUpdate(CPUState* ctx)
{
  Machine& machine = GetMachine(CpuOf(ctx));
  machine.AllowRumble(*machine.memory.Effective(ctx->gpr[4] + 0xB0, 1) != 0);
  ctx->gpr[0] = 0;
  ctx->pc = NIGHTS_CONTROLLER_WINDOW_UPDATE_NEXT;
  return true;
}

