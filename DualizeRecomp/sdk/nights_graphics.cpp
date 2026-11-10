// The Options list's graphics page. A on its Graphics Settings row (the
// "Check Aspect Ratio" row, renamed) turns the list over to the graphics
// settings and B turns it back: the list leaves as B makes it leave and
// opens again with the other page's rows, with the screen's own animations,
// sounds, cursor and help line.
//
// The page has more options than the list has rows, so up and down scroll
// past its ends. Each row's label and value are drawn into the labels
// texture's line for that row, in guest RAM (the renderer hashes textures by
// content, so it takes the change); the value boxes are hidden, the second
// row having none. Left and right, or A, change the value. The help line is
// one of the top level's messages, unused offline, rewritten.
//
// Hooks and the game's calls in config/nights.sigs (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"
#include "sdk/nights_options.hpp"

#include "cpu/cpu.hpp"
#include "machine.hpp"
#include "ui/game_assets.hpp"
#include "ui/graphics_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#pragma clang diagnostic pop

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::GraphicsOption;
using dualize::Image;
using dualize::Machine;
using dualize::Memory;
using dualize::nights::CpuOf;

// Nunito Sans Light (SIL Open Font License 1.1, resources/fonts/
// NunitoSans-OFL.txt), cut to Latin-1: the free face closest to the labels'.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr unsigned char LABEL_FONT[] = {
#embed "../resources/fonts/NunitoSans-Light.ttf"
};
#pragma clang diagnostic pop

// The screen (ModeOption): its state, the row its cursor is on and the
// cursor itself (its first word the row), its layout's holder.
constexpr uint32_t SCREEN_LAYOUT = 0x74, SCREEN_CURSOR = 0xA4, SCREEN_STATE = 0x70, SCREEN_ROW = 0xF0;
constexpr uint32_t STATE_OPEN = 0;  // plays the list's opening, then shows its rows
constexpr int ROWS = 4;
constexpr uint32_t GRAPHICS_ROW = 1;  // the main page's row that opens the graphics page
// The row nodes' table: a name each 16 bytes (the highlight's pattern 0 on, 1 off).
constexpr uint32_t ROW_NODE_STRIDE = 0x10;
// The big icon's patterns are the main page's rows plus one.
constexpr uint32_t GRAPHICS_ICON = GRAPHICS_ROW + 1;

// The game's sounds and input (a 64-bit mask, low word: the directions; the
// game's cursors read 0x10-0x80, previous and next whichever way a list lies).
constexpr uint32_t SOUND_CURSOR = 0, SOUND_DECIDE = 1;
constexpr uint32_t INPUT_UP = 0x1, INPUT_DOWN = 0x2, INPUT_LEFT = 0x4, INPUT_RIGHT = 0x8, INPUT_A = 0x100,
                   INPUT_B = 0x200;

// The labels texture: a 24-texel line per row, rows 0-3 the list's (lines 4
// to 7 the network list's, then the value boxes' words). CMPR encodes 8x8
// tiles, 32 bytes each.
constexpr uint32_t LINE = 24, WIDTH = 256, TILE_ROW_BYTES = WIDTH / 8 * 32;
constexpr uint32_t LIST_BYTES = ROWS * LINE / 8 * TILE_ROW_BYTES;
// The value boxes' words (lines 8-10), never redrawn: found in guest RAM.
constexpr uint32_t WORDS_FIRST_TILE_ROW = 8 * LINE / 8, WORDS_TILE_ROWS = 32 - WORDS_FIRST_TILE_ROW;

// The lettering: capitals 14 texels high on a baseline at 19, as the game's.
constexpr float CAP_HEIGHT = 14.0f, BASELINE = 19.0f;
constexpr float LABEL_X = 4.0f, VALUE_RIGHT = 250.0f, VALUE_GAP = 14.0f;

class LabelFont
{
public:
  LabelFont()
  {
    stbtt_InitFont(&m_font, LABEL_FONT, 0);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBox(&m_font, 'H', &x0, &y0, &x1, &y1);
    m_scale = CAP_HEIGHT / static_cast<float>(y1);
  }

  float Width(std::u32string_view text, float squeeze = 1.0f) const
  {
    float width = 0;
    for (size_t i = 0; i < text.size(); ++i)
      width += Advance(text, i) * m_scale * squeeze;
    return width;
  }

  // Coverage over what the image has (its red, green and blue alike).
  void Draw(Image& image, uint32_t line, std::u32string_view text, float x, float squeeze = 1.0f) const
  {
    const float sx = m_scale * squeeze, sy = m_scale;
    float pen = x;
    std::vector<uint8_t> glyph;
    for (size_t i = 0; i < text.size(); ++i)
    {
      const int c = static_cast<int>(text[i]);
      const float shift = pen - std::floor(pen);
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      stbtt_GetCodepointBitmapBoxSubpixel(&m_font, c, sx, sy, shift, 0, &x0, &y0, &x1, &y1);
      const int w = x1 - x0, h = y1 - y0;
      if (w > 0 && h > 0)
      {
        glyph.assign(static_cast<size_t>(w) * h, 0);
        stbtt_MakeCodepointBitmapSubpixel(&m_font, glyph.data(), w, h, w, sx, sy, shift, 0, c);
        const int left = static_cast<int>(std::floor(pen)) + x0, top = static_cast<int>(BASELINE) + y0;
        for (int gy = 0; gy < h; ++gy)
        {
          const int y = top + gy;
          if (y < 0 || y >= static_cast<int>(LINE))
            continue;
          for (int gx = 0; gx < w; ++gx)
          {
            const int px = left + gx;
            if (px < 0 || px >= static_cast<int>(WIDTH))
              continue;
            uint8_t* p = &image.rgba[(static_cast<size_t>(line * LINE + y) * image.width + px) * 4];
            p[0] = p[1] = p[2] = std::max(p[0], glyph[static_cast<size_t>(gy) * w + gx]);
          }
        }
      }
      pen += Advance(text, i) * sx;
    }
  }

private:
  float Advance(std::u32string_view text, size_t i) const
  {
    int advance = 0, bearing = 0;
    const int c = static_cast<int>(text[i]);
    stbtt_GetCodepointHMetrics(&m_font, c, &advance, &bearing);
    if (i + 1 < text.size())
      advance += stbtt_GetCodepointKernAdvance(&m_font, c, static_cast<int>(text[i + 1]));
    return static_cast<float>(advance);
  }

  stbtt_fontinfo m_font{};
  float m_scale = 1.0f;
};

std::u32string Utf32(std::string_view text)
{
  std::u32string out;
  for (size_t i = 0; i < text.size();)
  {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80 || i + 1 >= text.size())
    {
      out.push_back(c);
      ++i;
    }
    else
    {
      out.push_back(static_cast<char32_t>((c & 0x1F) << 6 | (static_cast<unsigned char>(text[i + 1]) & 0x3F)));
      i += 2;
    }
  }
  return out;
}

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

  // The pattern of the cast named by the string at `name` (guest memory).
  void SetPattern(uint32_t name, uint32_t pattern)
  {
    WithNode(name, [&](uint32_t node) { m_cpu.CallGuest(CSD_SET_PATTERN, {node, pattern}); });
  }

  // The cast named `name` (its runtime object), 0 if the scene has none.
  uint32_t Cast(std::string_view name)
  {
    const uint32_t text = m_sp - NAME;
    uint8_t* bytes = m_memory.Effective(text, static_cast<uint32_t>(name.size()) + 1);
    std::memcpy(bytes, name.data(), name.size());
    bytes[name.size()] = 0;
    uint32_t cast = 0;
    WithNode(text, [&](uint32_t node) { cast = Load(Load(node + NODE_MEMBER) + MEMBER_CAST); });
    return cast;
  }
  uint32_t Parent(uint32_t cast) const { return cast != 0 ? Load(cast + CAST_PARENT) : 0; }
  void SetHidden(uint32_t cast, bool hidden)
  {
    if (cast == 0)
      return;
    const uint32_t member = m_sp - MEMBER;
    dualize::Store32(m_memory.Effective(member + MEMBER_CAST, 4), cast);
    m_cpu.CallGuest(CSD_CAST_SET_HIDE, {member, hidden ? 1u : 0u});
  }

private:
  static constexpr uint32_t SCRATCH = 0x100, HANDLE = 0x80, NAME = 0x60, MEMBER = 0x30;
  // A node holds a member, the member its cast; a cast its parent in the tree.
  static constexpr uint32_t NODE_MEMBER = 0x34, MEMBER_CAST = 0xC, CAST_PARENT = 0x80;

  uint32_t Load(uint32_t address) const { return dualize::Load32(m_memory.Effective(address, 4)); }
  void WithNode(uint32_t name, const std::function<void(uint32_t)>& use)
  {
    const uint32_t handle = m_sp - HANDLE;
    m_cpu.CallGuest(CSD_FIND_NODE, {handle, m_scene, name});
    if (const uint32_t node = m_cpu.CallGuest(CSD_SCENE_NODE, {handle}))
      use(node);
    m_cpu.CallGuest(CSD_RELEASE_HANDLE, {handle, 0});
  }

  Cpu& m_cpu;
  Memory& m_memory;
  CPUState* m_ctx;
  uint32_t m_sp, m_scene = 0;
};

// The value boxes' casts: their groups hold the box and its word.
constexpr std::array<std::string_view, 3> VALUE_CASTS{"Cast_0104", "Cast_0106", "Cast_0107"};

// One of the top level's help messages, each copy of it (one per controller
// kind, its button guide after the line) rewritten in place: the line gives
// way to the page's, the guide stays.
struct HelpMessage
{
  uint32_t id;
  std::u16string_view line;
  struct Copy
  {
    uint8_t* at;
    std::vector<uint8_t> original;  // up to the terminator
    size_t line_bytes;
  };
  std::vector<Copy> copies;

  void Find(const Memory& memory)
  {
    copies.clear();
    for (uint8_t* at : dualize::nights::FindMessageLines(memory, line))
    {
      uint8_t* end = at;
      while ((end[0] | end[1]) != 0)
        end += 2;
      copies.push_back({at, {at, end}, line.size() * 2});
    }
  }

  void Write(std::u16string_view text)
  {
    const std::u16string big = dualize::nights::BigEndian(text);
    for (Copy& copy : copies)
    {
      const size_t tail = copy.original.size() - copy.line_bytes;
      const size_t room = copy.original.size() - tail;
      const size_t bytes = std::min(big.size() * 2, room);
      std::memcpy(copy.at, big.data(), bytes);
      std::memcpy(copy.at + bytes, copy.original.data() + copy.line_bytes, tail);
      std::memset(copy.at + bytes + tail, 0, copy.original.size() - bytes - tail);
    }
  }

  void Restore()
  {
    for (const Copy& copy : copies)
      std::memcpy(copy.at, copy.original.data(), copy.original.size());
    copies.clear();
  }
};

struct Page
{
  bool graphics = false;
  enum class Switch
  {
    NONE,
    ENTER,
    LEAVE,
  } pending = Switch::NONE;
  int top = 0;  // the option in the first row
  bool boxes_hidden = false;

  // The labels texture as the disc serves it, and where the game has it.
  std::optional<Image> labels;
  std::vector<uint8_t> labels_encoded;
  uint8_t* labels_in_ram = nullptr;

  std::array<HelpMessage, 2> help{{
      {static_cast<uint32_t>(NIGHTS_MODE_OPTION_TOP_HELP), u"Register Friends or change Nintendo WFC settings", {}},
      {static_cast<uint32_t>(NIGHTS_MODE_OPTION_TOP_HELP) + 1, u"Change language or screen display settings", {}},
  }};
  int help_shown = -1;  // the message the help line has, -1 before the first
  std::string help_text;
};
Page g_page;

const LabelFont& Font()
{
  static const LabelFont font;
  return font;
}

uint8_t* FindLabels(const Memory& memory)
{
  if (!g_page.labels)
    return nullptr;
  const uint8_t* words = g_page.labels_encoded.data() + WORDS_FIRST_TILE_ROW * TILE_ROW_BYTES;
  const size_t words_bytes = WORDS_TILE_ROWS * TILE_ROW_BYTES;
  const std::boyer_moore_horspool_searcher search(words, words + words_bytes);
  for (const auto& [base, size] : {std::pair{0x90000000u, Memory::MEM2_SIZE}, std::pair{0x80000000u, Memory::MEM1_SIZE}})
  {
    uint8_t* data = memory.Effective(base, size);
    uint8_t* found = std::search(data, data + size, search);
    const size_t before = WORDS_FIRST_TILE_ROW * TILE_ROW_BYTES;
    if (found != data + size && static_cast<size_t>(found - data) >= before)
      return found - before;
  }
  return nullptr;
}

// The list's four lines, from option `top` on: the label at the left, its
// value at the right, a label too long for both pressed narrower.
void DrawRows(const Machine& machine)
{
  if (g_page.labels_in_ram == nullptr)
    return;
  Image image = *g_page.labels;
  const LabelFont& font = Font();
  for (int row = 0; row < ROWS; ++row)
  {
    std::fill_n(&image.rgba[static_cast<size_t>(row) * LINE * WIDTH * 4], LINE * WIDTH * 4, 0);
    const int option = g_page.top + row;
    if (option >= dualize::GRAPHICS_OPTION_COUNT)
      continue;
    const auto which = static_cast<GraphicsOption>(option);
    const std::u32string label = Utf32(dualize::OptionName(which));
    const std::u32string value = Utf32(dualize::OptionValue(machine.Graphics(), which));
    const float value_width = font.Width(value);
    const float room = VALUE_RIGHT - value_width - VALUE_GAP - LABEL_X;
    const float label_width = font.Width(label);
    font.Draw(image, row, label, LABEL_X, label_width > room ? room / label_width : 1.0f);
    font.Draw(image, row, value, VALUE_RIGHT - value_width);
  }
  dualize::EncodeGreyCmpr(image, 0, ROWS * LINE, g_page.labels_in_ram);
}

void RestoreRows()
{
  if (g_page.labels_in_ram != nullptr)
    std::memcpy(g_page.labels_in_ram, g_page.labels_encoded.data(), LIST_BYTES);
  g_page.labels_in_ram = nullptr;
}

// The help line, rewritten into the message the line doesn't show (the
// game sets a message's text once, when the line changes to it).
void ShowHelp(CPUState* ctx, std::string_view text)
{
  Cpu& cpu = CpuOf(ctx);
  if (g_page.help[0].copies.empty() || g_page.help[1].copies.empty())
    return;
  if (text != g_page.help_text || g_page.help_shown < 0)
  {
    g_page.help_shown = g_page.help_shown == 0 ? 1 : 0;
    std::u16string line;
    for (char c : text)
      line.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
    g_page.help[g_page.help_shown].Write(line);
    g_page.help_text = text;
  }
  const uint32_t help = dualize::Load32(GetMachine(cpu).memory.Effective(NIGHTS_MODE_OPTION_HELP, 4));
  cpu.CallGuest(NIGHTS_SET_HELP, {help, g_page.help[g_page.help_shown].id});
}

void HideValueBoxes(CPUState* ctx, uint32_t screen, bool hidden)
{
  if (g_page.boxes_hidden == hidden)
    return;
  g_page.boxes_hidden = hidden;
  Layout layout(CpuOf(ctx), ctx, screen + SCREEN_LAYOUT);
  for (std::string_view name : VALUE_CASTS)
    layout.SetHidden(layout.Parent(layout.Cast(name)), hidden);
}

void PlaySound(Cpu& cpu, uint32_t sound)
{
  cpu.CallGuest(NIGHTS_PLAY_SOUND, {sound, 1});
}

uint32_t Load(const Memory& memory, uint32_t address)
{
  return dualize::Load32(memory.Effective(address, 4));
}

void Store(const Memory& memory, uint32_t address, uint32_t value)
{
  dualize::Store32(memory.Effective(address, 4), value);
}
}  // namespace

namespace dualize::nights
{
void KeepOptionLabels(const Image& image, std::span<const uint8_t> encoded)
{
  g_page.labels = image;
  g_page.labels_encoded.assign(encoded.begin(), encoded.end());
}

void ResetGraphicsPage()
{
  g_page.graphics = false;
  g_page.pending = Page::Switch::NONE;
  g_page.boxes_hidden = false;
  g_page.labels_in_ram = nullptr;
  for (HelpMessage& message : g_page.help)
    message.copies.clear();
  g_page.help_shown = -1;
}
}  // namespace dualize::nights

// A on the main page's Graphics Settings row, its decide sound played: the
// list leaves as B makes it, for the graphics page.
extern "C" bool nights_ModeOptionAspect(CPUState* ctx)
{
  g_page.pending = Page::Switch::ENTER;
  ctx->pc = NIGHTS_MODE_OPTION_LEAVE;
  return true;
}

// The list leaving, its cancel sound (li r3, 2 then the call): not when it
// leaves for the graphics page, A having played the decide sound.
extern "C" bool nights_ModeOptionLeaveSound(CPUState* ctx)
{
  if (g_page.pending != Page::Switch::ENTER)
    return false;
  ctx->pc = NIGHTS_MODE_OPTION_LEAVE_SOUND_NEXT;
  return true;
}

// The list has left (state 2's end, r30 the screen; the screen would go back
// to its top level): when it left for the other page, it opens again on it
// (state 0), the cursor on the graphics page's first row or, back, on the
// Graphics Settings row.
extern "C" bool nights_ModeOptionLeft(CPUState* ctx)
{
  if (g_page.pending == Page::Switch::NONE)
    return false;
  Machine& machine = GetMachine(CpuOf(ctx));
  const uint32_t screen = ctx->gpr[30];
  g_page.graphics = g_page.pending == Page::Switch::ENTER;
  g_page.pending = Page::Switch::NONE;
  if (g_page.graphics)
  {
    g_page.top = 0;
    g_page.labels_in_ram = FindLabels(machine.memory);
    if (g_page.labels_in_ram == nullptr)
      std::fprintf(stderr, "[options] the labels texture is not in memory; the graphics page keeps its labels\n");
    DrawRows(machine);
    for (HelpMessage& message : g_page.help)
      message.Find(machine.memory);
    g_page.help_shown = -1;
    g_page.help_text.clear();
    Store(machine.memory, screen + SCREEN_ROW, 0);
  }
  else
  {
    RestoreRows();
    for (HelpMessage& message : g_page.help)
      message.Restore();
    Store(machine.memory, screen + SCREEN_ROW, GRAPHICS_ROW);
  }
  Store(machine.memory, screen + SCREEN_STATE, STATE_OPEN);
  ctx->pc = NIGHTS_MODE_OPTION_UPDATE_DONE;
  return true;
}

// The value boxes, set as the list opens and each frame it is up (r3 the
// screen): hidden on the graphics page, whose rows draw their values.
extern "C" bool nights_ModeOptionValues(CPUState* ctx)
{
  HideValueBoxes(ctx, ctx->gpr[3], g_page.graphics);
  if (!g_page.graphics)
    return false;
  ctx->pc = ctx->lr;
  return true;
}

// The rows' highlight and the big icon (r3 the screen), from the row the
// cursor is on: on the graphics page the icon stays Graphics Settings'.
extern "C" bool nights_ModeOptionHighlight(CPUState* ctx)
{
  if (!g_page.graphics)
    return false;
  Cpu& cpu = CpuOf(ctx);
  const Memory& memory = GetMachine(cpu).memory;
  const uint32_t screen = ctx->gpr[3];
  const uint32_t row = Load(memory, screen + SCREEN_ROW);
  {
    Layout layout(cpu, ctx, screen + SCREEN_LAYOUT);
    for (uint32_t r = 0; r < ROWS; ++r)
      layout.SetPattern(Load(memory, NIGHTS_MODE_OPTION_ROW_NODES + r * ROW_NODE_STRIDE), r == row ? 0 : 1);
    layout.SetPattern(NIGHTS_MODE_OPTION_ICON, GRAPHICS_ICON);
    layout.SetPattern(NIGHTS_MODE_OPTION_ICON_GLOW, GRAPHICS_ICON);
  }
  ctx->pc = ctx->lr;
  return true;
}

// The list's state 1 (r30 the screen), each frame: on the graphics page the
// page's help line and input instead of the main page's.
extern "C" bool nights_ModeOptionList(CPUState* ctx)
{
  if (!g_page.graphics)
    return false;
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const Memory& memory = machine.memory;
  const uint32_t screen = ctx->gpr[30];
  const int row = static_cast<int>(Load(memory, screen + SCREEN_ROW));
  const int option = std::clamp(g_page.top + row, 0, dualize::GRAPHICS_OPTION_COUNT - 1);
  ShowHelp(ctx, machine.GraphicsPending() ? "Applies the next time the game starts"
                                          : dualize::OptionHelp(static_cast<GraphicsOption>(option)));
  ctx->pc = NIGHTS_MODE_OPTION_UPDATE_DONE;
  if (cpu.CallGuest(NIGHTS_MODE_OPTION_BUSY, {screen}) != 0)
    return true;

  const uint32_t controller = cpu.CallGuest(NIGHTS_INPUT_CONTROLLER, {Load(memory, NIGHTS_INPUT), 0});
  const auto repeated = [&](uint32_t mask) { return cpu.CallGuest(NIGHTS_INPUT_REPEATED, {controller, 0, 0, mask}) != 0; };
  const auto pressed = [&](uint32_t mask) { return cpu.CallGuest(NIGHTS_INPUT_PRESSED, {controller, 0, 0, mask}) != 0; };
  const int count = dualize::GRAPHICS_OPTION_COUNT;
  int move = repeated(INPUT_UP) ? -1 : repeated(INPUT_DOWN) ? 1 : 0;
  if (move != 0)
  {
    // Past the list's ends it scrolls; past the page's it goes round.
    const int to = (option + move + count) % count;
    g_page.top = std::clamp(g_page.top, to - (ROWS - 1), to);
    g_page.top = std::clamp(g_page.top, 0, std::max(0, count - ROWS));
    const bool scrolled = g_page.top != option - row;
    Store(memory, screen + SCREEN_ROW, static_cast<uint32_t>(to - g_page.top));
    Store(memory, screen + SCREEN_CURSOR, static_cast<uint32_t>(to - g_page.top));
    cpu.CallGuest(NIGHTS_MODE_OPTION_ROW_ICON, {screen});
    PlaySound(cpu, SOUND_CURSOR);
    if (scrolled)
      DrawRows(machine);
    return true;
  }
  const int change = repeated(INPUT_LEFT) ? -1 : repeated(INPUT_RIGHT) ? 1 : pressed(INPUT_A) ? 2 : 0;
  if (change != 0)
  {
    dualize::GraphicsSettings settings = machine.Graphics();
    dualize::StepOption(settings, static_cast<GraphicsOption>(option), change > 0 ? 1 : -1, change == 2);
    if (!(settings == machine.Graphics()))
    {
      machine.SetGraphics(settings);
      DrawRows(machine);
      PlaySound(cpu, change == 2 ? SOUND_DECIDE : SOUND_CURSOR);
    }
    return true;
  }
  if (pressed(INPUT_B))
  {
    g_page.pending = Page::Switch::LEAVE;
    ctx->pc = NIGHTS_MODE_OPTION_LEAVE;
  }
  return true;
}
