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
// in, so it starts halfway. Then the instruction hooked (addi r3, r1, 0x138).
// A screen's CSD layout through the game's own calls, made from a native on
// scratch room below the stack pointer (a node handle, a name, and a member
// pointing at a cast for the cast methods).
class Layout
{
public:
  Layout(Cpu& cpu, CPUState* ctx, uint32_t holder)
      : m_cpu(cpu), m_memory(GetMachine(cpu).memory), m_ctx(ctx), m_sp(ctx->gpr[1])
  {
    ctx->gpr[1] = m_sp - SCRATCH;
    m_scene = cpu.CallGuest(CSD_SCENE_NODE, {holder});
  }
  ~Layout() { m_ctx->gpr[1] = m_sp; }
  Layout(const Layout&) = delete;
  Layout& operator=(const Layout&) = delete;

  // The cast named `name` (its runtime object), 0 if the scene has none.
  uint32_t Cast(std::string_view name)
  {
    const uint32_t handle = m_sp - HANDLE, text = m_sp - NAME;
    uint8_t* bytes = m_memory.Effective(text, static_cast<uint32_t>(name.size()) + 1);
    std::memcpy(bytes, name.data(), name.size());
    bytes[name.size()] = 0;
    m_cpu.CallGuest(CSD_FIND_NODE, {handle, m_scene, text});
    uint32_t cast = 0;
    if (const uint32_t node = m_cpu.CallGuest(CSD_SCENE_NODE, {handle}))
      cast = Load(Load(node + NODE_MEMBER) + MEMBER_CAST);
    m_cpu.CallGuest(CSD_RELEASE_HANDLE, {handle, 0});
    return cast;
  }
  uint32_t Parent(uint32_t cast) const { return cast != 0 ? Load(cast + CAST_PARENT) : 0; }
  float Y(uint32_t cast) const { return std::bit_cast<float>(Load(cast + CAST_Y)); }
  float X(uint32_t cast) const { return std::bit_cast<float>(Load(cast + CAST_X)); }

  void Hide(uint32_t cast) { m_cpu.CallGuest(CSD_CAST_SET_HIDE, {Member(cast), 1}); }
  void Move(uint32_t cast, float x, float y)
  {
    m_ctx->fpr[1] = x;
    m_ctx->fpr[2] = y;
    m_cpu.CallGuest(CSD_CAST_SET_POSITION, {Member(cast)});
  }

private:
  static constexpr uint32_t SCRATCH = 0x100, HANDLE = 0x80, NAME = 0x60, MEMBER = 0x30;
  // A node holds a member, the member its cast; a cast its current position
  // and its parent in the layout's tree.
  static constexpr uint32_t NODE_MEMBER = 0x34, MEMBER_CAST = 0xC, CAST_X = 0x2C, CAST_Y = 0x30, CAST_PARENT = 0x80;

  uint32_t Load(uint32_t address) const { return dualize::Load32(m_memory.Effective(address, 4)); }
  uint32_t Member(uint32_t cast)
  {
    const uint32_t member = m_sp - MEMBER;
    dualize::Store32(m_memory.Effective(member + MEMBER_CAST, 4), cast);
    return member;
  }

  Cpu& m_cpu;
  Memory& m_memory;
  CPUState* m_ctx;
  uint32_t m_sp, m_scene = 0;
};

// The settings list's rows: the controller window's (the third) is hidden, its
// option being off for good, and the weather link's takes its place.
constexpr uint32_t CONTROLLER_ROW = 2;

extern "C" bool nights_ModeOptionListOpen(CPUState* ctx)
{
  SeekAnimation(ctx, ctx->gpr[30] + 0x74, 0.5f, 0.001f);
  ctx->gpr[3] = ctx->gpr[1] + 0x138;
  ctx->pc = NIGHTS_MODE_OPTION_LIST_OPEN_NEXT;
  return true;
}

// The list's value boxes, set each frame it is up (r3 the screen): the layout
// changes go with them. Then the hooked stwu r1, -0x40(r1).
extern "C" bool nights_ModeOptionValues(CPUState* ctx)
{
  Cpu& cpu = CpuOf(ctx);
  {
    // Each row's buttons hang off its own group (the names the groups have
    // are the network list's too, so they are found through a button).
    Layout layout(cpu, ctx, ctx->gpr[3] + 0x74);
    const uint32_t controller = layout.Parent(layout.Cast("setting_btn02"));
    const uint32_t weather = layout.Parent(layout.Cast("setting_btn03"));
    if (controller != 0 && weather != 0)
    {
      layout.Hide(controller);
      layout.Move(weather, layout.X(controller), layout.Y(controller));
    }
  }
  dualize::Store32(GetMachine(cpu).memory.Effective(ctx->gpr[1] - 0x40, 4), ctx->gpr[1]);
  ctx->gpr[1] -= 0x40;
  ctx->pc = NIGHTS_MODE_OPTION_VALUES_BODY;
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

// The settings list's cursor, moved (r30 the screen): it steps over the hidden
// controller window row, on in the direction it came from. Then the hooked
// lwz r0, 0xa4(r30) (the row it is on).
extern "C" bool nights_ModeOptionCursor(CPUState* ctx)
{
  uint8_t* screen = GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[30], 0xF4);
  uint32_t row = dualize::Load32(screen + 0xA4);
  if (row == CONTROLLER_ROW)
  {
    row = dualize::Load32(screen + 0xF0) > CONTROLLER_ROW ? CONTROLLER_ROW - 1 : CONTROLLER_ROW + 1;
    dualize::Store32(screen + 0xA4, row);
  }
  ctx->gpr[0] = row;
  ctx->pc = NIGHTS_MODE_OPTION_CURSOR_NEXT;
  return true;
}

// Play's reads of the controller window option (lbz r3 and lbz r0, 0xb0 of the
// options): the window is off for good, whatever the save holds.
extern "C" bool nights_ControllerWindowShown(CPUState* ctx)
{
  ctx->gpr[3] = 0;
  ctx->pc = NIGHTS_CONTROLLER_WINDOW_SHOWN_NEXT;
  return true;
}

extern "C" bool nights_ControllerWindowUpdate(CPUState* ctx)
{
  ctx->gpr[0] = 0;
  ctx->pc = NIGHTS_CONTROLLER_WINDOW_UPDATE_NEXT;
  return true;
}
