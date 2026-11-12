// The Options list's graphics page. A on its Graphics Settings row (the
// "Check Aspect Ratio" row, renamed) turns the list over to the graphics
// settings and B turns it back: the list leaves as B makes it leave and
// opens again with the other page's rows, with the screen's own animations,
// sounds, cursor and help line.
//
// The page has more options than the list has rows, so up and down scroll
// past its ends. Each row's label is drawn into the labels texture's line for
// that row, in guest RAM (the renderer hashes textures by content, so it
// takes the change), and its value into a line of its own that the row's
// value box shows (the second row borrowing a box). Left and right, or A,
// change the value. The help line is one of the top level's messages, unused
// offline, rewritten.
//
// The main page is drawn the same way, with five rows: the game's four, whose
// A runs the game's own action for that row (its window), and Subtitles.
//
// Hooks and the game's calls in config/nights.sigs (ppc_natives.h).

#include "core/cpu.h"
#include "ppc_natives.h"
#include "sdk/nights_options.hpp"
#include "sdk/nights_text.hpp"

#include "cpu/cpu.hpp"
#include "machine.hpp"
#include "ui/game_assets.hpp"
#include "ui/graphics_settings.hpp"

#include <aurora/fifo.h>
#include <aurora/texture.hpp>

#include <algorithm>
#include <array>
#include <bit>
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
using dualize::nights::Language;

// Dualize Sans (SIL Open Font License 1.1, resources/fonts/DualizeSans-OFL.txt):
// Kumbh Sans, of the free faces the closest to the game's own labels, bent
// to their letters (widths, x-height, ascenders, descenders, side bearings)
// at the weight that inks their lines as darkly, cut to Latin-1
// (scripts/lettering/font_tune.py); set at whatever size the screen needs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr unsigned char LABEL_FONT[] = {
#embed "../resources/fonts/DualizeSans.ttf"
};
// Noto Sans JP Black (SIL Open Font License 1.1, resources/fonts/
// NotoSansJP-OFL.txt), cut to our Japanese texts' characters: the game's
// Japanese is a heavy gothic, its em 18.5 texels where Latin capitals are 14
// high, set 1.25 times wider than tall.
constexpr unsigned char JAPANESE_FONT[] = {
#embed "../resources/fonts/NotoSansJP-Black-subset.ttf"
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

// The main page's options: the game's four rows (A opens each one's window,
// its help the row's) with Subtitles, ours, second. Its icon is Language's.
enum class MainOption
{
  LANGUAGE,
  SUBTITLES,
  GRAPHICS,
  RUMBLE,
  WEATHER_LINK,
  COUNT,
};
constexpr int MAIN_OPTION_COUNT = static_cast<int>(MainOption::COUNT);
constexpr std::array<int, MAIN_OPTION_COUNT> GAME_ROW{0, -1, static_cast<int>(GRAPHICS_ROW), 2, 3};
constexpr std::string_view SUBTITLES_HELP = "Show the dialogue lines in cutscenes";
// The game's settings: the language (in its language window's order) and two
// switches.
constexpr uint32_t SETTINGS_LANGUAGE = 0xAC, SETTINGS_RUMBLE = 0xB0, SETTINGS_WEATHER_LINK = 0xB1;
constexpr std::array<std::string_view, 6> LANGUAGES{"Japanese", "English", "Français", "Español", "Deutsch", "Italiano"};

// The game's sounds and input (a 64-bit mask, low word: the directions; the
// game's cursors read 0x10-0x80, previous and next whichever way a list lies).
constexpr uint32_t SOUND_CURSOR = 0, SOUND_DECIDE = 1;
constexpr uint32_t INPUT_UP = 0x1, INPUT_DOWN = 0x2, INPUT_LEFT = 0x4, INPUT_RIGHT = 0x8, INPUT_A = 0x100,
                   INPUT_B = 0x200;

// The labels texture: a 24-texel line per row, rows 0-3 the list's (lines 4
// to 7 the network window's, hidden offline: the page's values), then the
// value boxes' words. CMPR encodes 8x8 tiles, 32 bytes each.
constexpr uint32_t LINE = 24, WIDTH = 256, TILE_ROW_BYTES = WIDTH / 8 * 32;
constexpr uint32_t VALUE_LINE = ROWS, DRAWN_LINES = 2 * ROWS;
constexpr uint32_t SHARP_SCALE = 4, CMPR_FORMAT = 0xE;  // GX_TF_CMPR
constexpr uint32_t DRAWN_BYTES = DRAWN_LINES * LINE / 8 * TILE_ROW_BYTES;
// The value boxes' words (lines 8-10), never redrawn: found in guest RAM.
constexpr uint32_t WORDS_FIRST_TILE_ROW = 8 * LINE / 8, WORDS_TILE_ROWS = 32 - WORDS_FIRST_TILE_ROW;

// The lettering, as the game's (measured on its labels texture; the face's
// weight and spacing fitted to its lines by scripts/lettering/font_fit.py):
// labels' capitals 14 texels high on a baseline at 19, letters 0.85 texel
// further apart than the face sets them, ending clear of the value box; values' 13
// high, as far apart for their size, centred in a cell 20 texels high (from row 2
// of their line) on a baseline 15 down it.
constexpr float LABEL_CAP = 14.0f, LABEL_TRACKING = 0.85f, LABEL_BASELINE = 19.0f, LABEL_X = 4.0f, LABEL_RIGHT = 204.0f;
constexpr float VALUE_CAP = 13.0f, VALUE_TRACKING = 0.8f, VALUE_TOP = 2.0f, VALUE_BASELINE = VALUE_TOP + 15.0f;
// A row the other settings lock, drawn dim.
constexpr uint8_t LOCKED_LEVEL = 110;
constexpr float VALUE_WIDTH = 64.0f, VALUE_HEIGHT = 20.0f, VALUE_ROOM = 60.0f;

constexpr float JAPANESE_EM = 18.5f, JAPANESE_STRETCH = 1.25f;

class LabelFont
{
public:
  LabelFont(float cap_height, float tracking) : m_tracking(tracking)
  {
    stbtt_InitFont(&m_faces[0].font, LABEL_FONT, 0);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBox(&m_faces[0].font, 'H', &x0, &y0, &x1, &y1);
    m_faces[0].scale_y = m_faces[0].scale_x = cap_height / static_cast<float>(y1);
    stbtt_InitFont(&m_faces[1].font, JAPANESE_FONT, 0);
    m_faces[1].scale_y = stbtt_ScaleForMappingEmToPixels(&m_faces[1].font, JAPANESE_EM * cap_height / LABEL_CAP);
    m_faces[1].scale_x = m_faces[1].scale_y * JAPANESE_STRETCH;
  }

  float Width(std::u32string_view text, float squeeze = 1.0f) const
  {
    float width = 0;
    for (size_t i = 0; i < text.size(); ++i)
      width += (Advance(text, i) + (i + 1 < text.size() ? Tracking(text[i]) : 0.0f)) * squeeze;
    return width;
  }

  float Tracking() const { return m_tracking; }

  // Coverage over what the image has (its red, green and blue alike).
  // At `scale` times the texture's size (an image that much larger), placed
  // as at its own.
  void Draw(Image& image, uint32_t line, float baseline, std::u32string_view text, float x, float squeeze = 1.0f,
            uint8_t level = 255, uint32_t scale = 1) const
  {
    const auto k = static_cast<float>(scale);
    const int line_rows = static_cast<int>(LINE * scale), width = static_cast<int>(WIDTH * scale);
    float pen = x * k;
    std::vector<uint8_t> glyph;
    for (size_t i = 0; i < text.size(); ++i)
    {
      const int c = static_cast<int>(text[i]);
      const Face& face = FaceOf(c);
      const float sx = face.scale_x * squeeze * k, sy = face.scale_y * k;
      const float shift = pen - std::floor(pen);
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      stbtt_GetCodepointBitmapBoxSubpixel(&face.font, c, sx, sy, shift, 0, &x0, &y0, &x1, &y1);
      const int w = x1 - x0, h = y1 - y0;
      if (w > 0 && h > 0)
      {
        glyph.assign(static_cast<size_t>(w) * h, 0);
        stbtt_MakeCodepointBitmapSubpixel(&face.font, glyph.data(), w, h, w, sx, sy, shift, 0, c);
        const int left = static_cast<int>(std::floor(pen)) + x0, top = static_cast<int>(baseline * k) + y0;
        for (int gy = 0; gy < h; ++gy)
        {
          const int y = top + gy;
          if (y < 0 || y >= line_rows)
            continue;
          for (int gx = 0; gx < w; ++gx)
          {
            const int px = left + gx;
            if (px < 0 || px >= width)
              continue;
            uint8_t* p = &image.rgba[(static_cast<size_t>(line * line_rows + y) * image.width + px) * 4];
            const auto ink = static_cast<uint8_t>(glyph[static_cast<size_t>(gy) * w + gx] * level / 255);
            p[0] = p[1] = p[2] = std::max(p[0], ink);
          }
        }
      }
      pen += (Advance(text, i) + Tracking(text[i])) * squeeze * k;
    }
  }

private:
  struct Face
  {
    stbtt_fontinfo font{};
    float scale_x = 1.0f, scale_y = 1.0f;
  };

  // The Latin face, or the Japanese one for what it lacks.
  const Face& FaceOf(int c) const
  {
    return stbtt_FindGlyphIndex(&m_faces[0].font, c) != 0 ? m_faces[0] : m_faces[1];
  }

  // The game sets its Japanese as the face does, its Latin letters apart.
  float Tracking(char32_t c) const { return &FaceOf(static_cast<int>(c)) == m_faces.data() ? m_tracking : 0.0f; }

  // In texels, unsqueezed.
  float Advance(std::u32string_view text, size_t i) const
  {
    int advance = 0, bearing = 0;
    const int c = static_cast<int>(text[i]);
    const Face& face = FaceOf(c);
    stbtt_GetCodepointHMetrics(&face.font, c, &advance, &bearing);
    if (i + 1 < text.size() && &FaceOf(static_cast<int>(text[i + 1])) == &face)
      advance += stbtt_GetCodepointKernAdvance(&face.font, c, static_cast<int>(text[i + 1]));
    return static_cast<float>(advance) * face.scale_x;
  }

  std::array<Face, 2> m_faces{};
  float m_tracking;  // texels added between letters
};

std::u32string Utf32(std::string_view text)
{
  std::u32string out;
  for (size_t i = 0; i < text.size();)
  {
    const auto c = static_cast<unsigned char>(text[i]);
    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    char32_t code = length == 1 ? c : length == 2 ? c & 0x1F : length == 3 ? c & 0x0F : c & 0x07;
    for (size_t k = 1; k < length && i + k < text.size(); ++k)
      code = code << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    out.push_back(code);
    i += length;
  }
  return out;
}

// A layout's casts: one array, in drawing order, each cast with its own
// scale and colour (its animation's), its parent, and the quad it draws:
// its own quad (four vertices: x, y, colour, u, v) placed and coloured as
// its parents and it are, then its texture.
constexpr uint32_t CAST_SIZE = 0x144, CAST_OWN_SCALE_X = 0x38, CAST_OWN_SCALE_Y = 0x3C, CAST_OWN_COLOR = 0x44,
                   CAST_OVERRIDES = 0x74, CAST_PARENT = 0x80, CAST_GROUP = 0x84, CAST_QUAD = 0xE8, VERTEX = 20, VERTICES = 4,
                   QUAD_BYTES = VERTICES * VERTEX + 4;

// A cast's file data: its flags, one blending it by its texture's alpha.
constexpr uint32_t CAST_DATA_FLAGS = 0x38, CAST_DATA_TEXTURE_ALPHA = 0x1;

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
  // A colour (RGBA) over the one the cast's animation gives it.
  void SetColor(uint32_t cast, uint32_t color)
  {
    const uint32_t member = m_sp - MEMBER, value = m_sp - COLOR;
    dualize::Store32(m_memory.Effective(member + MEMBER_CAST, 4), cast);
    dualize::Store32(m_memory.Effective(value, 4), color);
    m_cpu.CallGuest(CSD_CAST_SET_COLOR, {member, value});
  }

private:
  static constexpr uint32_t SCRATCH = 0x100, HANDLE = 0x80, NAME = 0x60, COLOR = 0x40, MEMBER = 0x30;
  // A node holds a member, the member its cast.
  static constexpr uint32_t NODE_MEMBER = 0x34, MEMBER_CAST = 0xC;

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

// A row's value box: a group (its word's parent) holding the box's two
// halves, the casts after it, then the word. The second row has none, so it
// borrows the network window's (hidden offline): one of its box groups, moved
// to the row, with casts drawn after the list's (the layout draws in order)
// for the halves and the word, their own halves hidden.
struct ValueBox
{
  std::array<uint32_t, 2> halves{};
  uint32_t word = 0;
};
constexpr std::array<std::string_view, ROWS> VALUE_WORDS{"Cast_0104", "selectnode_n0", "Cast_0106", "Cast_0107"};
constexpr std::array<std::string_view, 2> BORROWED_HALVES{"Cast_0108", "Cast_0109"};
constexpr uint32_t BORROWED_ROW = 1;
// A cast's own quad (as for its local place), then the drawn one and its texture.
constexpr uint32_t CAST_LOCAL_QUAD = 0x98, CAST_QUADS_BYTES = CAST_QUAD + QUAD_BYTES - CAST_LOCAL_QUAD;

// One of the top level's help messages, each copy of it (one per controller
// kind, its button guide after the line) rewritten in place: the line gives
// way to the page's, the guide stays.
struct HelpMessage
{
  uint32_t id;
  std::array<std::u16string_view, dualize::nights::LANGUAGE_COUNT> lines;  // in each language
  // A language's other spelling of it in some copies (none: empty).
  std::array<std::u16string_view, dualize::nights::LANGUAGE_COUNT> others;
  struct Copy
  {
    uint8_t* at;
    std::vector<uint8_t> original;  // up to the terminator
    size_t line_bytes;
  };
  std::vector<Copy> copies;

  void Find(const Memory& memory, Language language)
  {
    copies.clear();
    for (const std::u16string_view line : {lines[static_cast<size_t>(language)], others[static_cast<size_t>(language)]})
    {
      for (uint8_t* at : dualize::nights::FindMessageLines(memory, line))
      {
        uint8_t* end = at;
        while ((end[0] | end[1]) != 0)
          end += 2;
        copies.push_back({at, {at, end}, line.size() * 2});
      }
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

// A row as drawn: its label and value (none: the row has no value box).
struct Row
{
  std::string label, value;
  bool locked = false;
  bool operator==(const Row&) const = default;
};

struct Page
{
  bool graphics = false;
  bool taken = false;  // the main page's rows drawn by the page too
  enum class Switch
  {
    NONE,
    ENTER,
    LEAVE,
    EXIT,  // the main page leaving the list for the top level
  } pending = Switch::NONE;
  int top = 0;  // the option in the first row
  int row = 0;  // the row the cursor is on
  int main_top = 0, main_row = 0;  // the main page's, while the graphics page is up
  int switch_option = -1;  // the option the ON/OFF window is up for
  std::array<Row, ROWS> drawn_rows;

  // The value boxes, found as the page first opens (none if the layout
  // differs), and the second row's group.
  std::optional<std::array<ValueBox, ROWS>> boxes;
  uint32_t second_row = 0;
  uint32_t borrowed_group = 0;
  std::array<uint32_t, 2> borrowed_group_halves{};
  uint32_t labels_texture = 0;  // the layout's index for the labels texture
  // While the page is up: the casts it changed, as they were.
  std::vector<std::pair<uint32_t, uint32_t>> undo;  // words of the layout's: address, value
  std::vector<std::pair<uint32_t, std::array<uint8_t, CAST_QUADS_BYTES>>> quads;

  // The labels texture in each language as the disc serves it, the game's
  // language as the page opened, and where the game has its texture.
  std::array<std::optional<Image>, dualize::nights::LANGUAGE_COUNT> labels;
  std::array<std::vector<uint8_t>, dualize::nights::LANGUAGE_COUNT> labels_encoded;
  Language language = Language::ENGLISH;
  uint8_t* labels_in_ram = nullptr;
  // The drawn labels texture's sharp copy in the renderer (see DrawRows).
  std::optional<aurora::texture::TextureSourceKey> sharp_key;
  std::array<uint32_t, 2> drawn_window{};  // the window's size the rows were drawn with
  // The title's first row per language, as the disc has it and as the page
  // shows it; where the page put its own (every copy the screen loaded).
  std::array<std::vector<uint8_t>, dualize::nights::LANGUAGE_COUNT> title_original, title_graphics;
  std::vector<uint8_t*> title_in_ram;
  Language title_language = Language::ENGLISH;
  // Where the textures were last found: the screen loads them there again
  // as a rule, so that place is checked before searching memory. Another
  // language's menus hold two copies of the labels texture (the list draws
  // with the second), each drawn over alike; a new language's are searched.
  std::vector<uint8_t*> labels_found;
  Language labels_found_language = Language::COUNT;
  std::vector<uint8_t*> title_found;
  Language title_found_language = Language::COUNT;

  std::array<HelpMessage, 2> help{{
      {static_cast<uint32_t>(NIGHTS_MODE_OPTION_TOP_HELP),
       {u"ネットワークに接続<r2,せつぞく>し、友<r1,とも>だち登録<r2,とうろく>や機能設定<r4,きのうせってい>をします",
        u"Register Friends or change Nintendo WFC settings", u"Inscrire un ami ou modifier la CWF Nintendo",
        u"Registrar amigos o cambiar la CWF de Nintendo", u"Freunde registrieren / Nintendo WFC ändern",
        u"Registra amici o modifica Nintendo WFC"},
       {u"", u"", u"", u"", u"", u"Registra amici o modifica la Nintendo WFC"},
       {}},
      {static_cast<uint32_t>(NIGHTS_MODE_OPTION_TOP_HELP) + 1,
       {u"言語<r2,げんご>選択<r2,せんたく>や、ゲーム画面<r2,がめん>を設定<r2,せってい>します",
        u"Change language or screen display settings", u"Modifier la langue ou les paramètres d'affichage",
        u"Cambiar configuración de idioma o de visualización", u"Sprache ändern / Bildschirmeinstellungen anpassen",
        u"Cambia lingua o cambia impostazioni visualizzazione"},
       {},
       {}},
  }};
  int help_shown = -1;  // the message the help line has, -1 before the first
  std::string help_text;
};
Page g_page;

const LabelFont& LabelLettering()
{
  static const LabelFont font(LABEL_CAP, LABEL_TRACKING);
  return font;
}

const LabelFont& ValueLettering()
{
  static const LabelFont font(VALUE_CAP, VALUE_TRACKING);
  return font;
}

// The labels texture of the page's language where the screen loaded it (its
// first copy; labels_found has them all).
uint8_t* FindLabels(const Memory& memory)
{
  const auto language = static_cast<size_t>(g_page.language);
  if (!g_page.labels[language])
    return nullptr;
  const size_t before = WORDS_FIRST_TILE_ROW * TILE_ROW_BYTES;
  const uint8_t* words = g_page.labels_encoded[language].data() + before;
  const size_t words_bytes = WORDS_TILE_ROWS * TILE_ROW_BYTES;
  if (g_page.labels_found_language == g_page.language && !g_page.labels_found.empty() && std::ranges::all_of(g_page.labels_found, [&](const uint8_t* copy) {
        return std::memcmp(copy + before, words, words_bytes) == 0;
      }))
    return g_page.labels_found.front();
  g_page.labels_found.clear();
  g_page.labels_found_language = g_page.language;
  const std::boyer_moore_horspool_searcher search(words, words + words_bytes);
  for (const auto& [base, size] : {std::pair{0x90000000u, Memory::MEM2_SIZE}, std::pair{0x80000000u, Memory::MEM1_SIZE}})
  {
    uint8_t* data = memory.Effective(base, size);
    for (uint8_t* found = std::search(data, data + size, search); found != data + size;
         found = std::search(found + 1, data + size, search))
    {
      if (static_cast<size_t>(found - data) >= before)
        g_page.labels_found.push_back(found - before);
    }
  }
  return g_page.labels_found.empty() ? nullptr : g_page.labels_found.front();
}

// The title's first row (Options) in the page's language, every copy the
// screen loaded.
const std::vector<uint8_t*>& FindTitle(const Memory& memory)
{
  std::vector<uint8_t*>& found = g_page.title_found;
  const std::vector<uint8_t>& title = g_page.title_original[static_cast<size_t>(g_page.language)];
  // Searched by a piece of the word's middle (RGB5A3 4x4 tiles, 32 bytes
  // each): the rows around it are mostly transparent, zeros, as much of
  // memory is, which makes the whole row a seconds-long search.
  constexpr size_t TILE_BYTES = 32, TILE_ROW_BYTES = 256 / 4 * TILE_BYTES;
  constexpr size_t KEY = 4 * TILE_ROW_BYTES + 20 * TILE_BYTES, KEY_BYTES = 24 * TILE_BYTES;
  if (title.size() < KEY + KEY_BYTES)
  {
    found.clear();
    return found;
  }
  if (g_page.title_found_language == g_page.language && !found.empty() &&
      std::ranges::all_of(found, [&](const uint8_t* copy) { return std::memcmp(copy, title.data(), title.size()) == 0; }))
    return found;
  found.clear();
  g_page.title_found_language = g_page.language;
  const std::boyer_moore_horspool_searcher search(title.begin() + KEY, title.begin() + KEY + KEY_BYTES);
  for (const auto& [base, size] : {std::pair{0x90000000u, Memory::MEM2_SIZE}, std::pair{0x80000000u, Memory::MEM1_SIZE}})
  {
    uint8_t* data = memory.Effective(base, size);
    for (uint8_t* at = data; (at = std::search(at, data + size, search)) != data + size; ++at)
    {
      if (static_cast<size_t>(at - data) >= KEY && static_cast<size_t>(data + size - (at - KEY)) >= title.size() &&
          std::memcmp(at - KEY, title.data(), title.size()) == 0)
        found.push_back(at - KEY);
    }
  }
  return found;
}

// The page's title in for Options, or back.
void ShowTitle(const Memory& memory, bool graphics)
{
  if (graphics)
  {
    g_page.title_in_ram = FindTitle(memory);
    g_page.title_language = g_page.language;
  }
  const auto language = static_cast<size_t>(g_page.title_language);
  const std::vector<uint8_t>& title = graphics ? g_page.title_graphics[language] : g_page.title_original[language];
  for (uint8_t* copy : g_page.title_in_ram)
    std::memcpy(copy, title.data(), title.size());
  if (!graphics)
    g_page.title_in_ram.clear();
}

int OptionCount()
{
  return g_page.graphics ? dualize::GRAPHICS_OPTION_COUNT : MAIN_OPTION_COUNT;
}

Row MainRow(const Machine& machine, MainOption option)
{
  const Memory& memory = machine.memory;
  const uint32_t settings = dualize::Load32(memory.Effective(NIGHTS_SETTINGS, 4));
  const auto on_off = [&](bool on) { return std::string(on ? "ON" : "OFF"); };
  const auto set = [&](uint32_t offset) { return *memory.Effective(settings + offset, 1) != 0; };
  switch (option)
  {
  case MainOption::LANGUAGE:
  {
    const uint32_t language = dualize::Load32(memory.Effective(settings + SETTINGS_LANGUAGE, 4));
    return {"Language Settings", std::string(language < LANGUAGES.size() ? LANGUAGES[language] : "")};
  }
  case MainOption::SUBTITLES:
    return {"Subtitles", on_off(machine.Graphics().subtitles)};
  case MainOption::GRAPHICS:
    return {"Graphics Settings", ""};
  case MainOption::RUMBLE:
    return {"Rumble", on_off(set(SETTINGS_RUMBLE))};
  case MainOption::WEATHER_LINK:
    return {"Weather Link", on_off(set(SETTINGS_WEATHER_LINK))};
  case MainOption::COUNT:
    break;
  }
  return {};
}

// The page's option `option` as its row shows it (none past the page's end).
Row RowOf(const Machine& machine, int option)
{
  if (option >= OptionCount())
    return {};
  if (!g_page.graphics)
    return MainRow(machine, static_cast<MainOption>(option));
  const auto which = static_cast<GraphicsOption>(option);
  return {dualize::OptionName(which), dualize::OptionValue(machine.Graphics(), which),
          dualize::OptionLocked(machine.Graphics(), which)};
}

// The page's option `option` as an ON/OFF switch, null if it isn't one (or
// is locked).
bool* SwitchOf(dualize::GraphicsSettings& settings, int option)
{
  if (!g_page.graphics)
    return option == static_cast<int>(MainOption::SUBTITLES) ? &settings.subtitles : nullptr;
  const auto which = static_cast<GraphicsOption>(option);
  return dualize::OptionLocked(settings, which) ? nullptr : dualize::OptionSwitch(settings, which);
}

std::array<Row, ROWS> RowsShown(const Machine& machine)
{
  std::array<Row, ROWS> rows;
  for (int row = 0; row < ROWS; ++row)
    rows[row] = RowOf(machine, g_page.top + row);
  return rows;
}

// The game's own lettering in its labels texture, used where our rows say
// the same: the language row's label (its first line) and the value words, in
// cells 20 texels high from rows 193, 213 and 233 (ON, OFF and Japanese's
// name in the first; English, French, Spanish and German in the second;
// Italian last in the third), each word a run of ink with gaps under 7 texels.
constexpr uint32_t LANGUAGE_LABEL_LINE = 0;
constexpr std::array<uint32_t, 3> WORD_TOPS{193, 213, 233};
constexpr uint32_t WORD_HEIGHT = 20, WORD_GAP = 7;
constexpr uint8_t INK = 38;
struct Word
{
  size_t band;
  int index;  // -1: the band's last
};
std::optional<Word> GameWord(std::string_view value)
{
  if (value == "ON")
    return Word{0, 0};
  if (value == "OFF")
    return Word{0, 1};
  constexpr std::array<Word, LANGUAGES.size()> NAMES{{{0, 2}, {1, 0}, {1, 1}, {1, 2}, {1, 3}, {2, -1}}};
  for (size_t i = 0; i < LANGUAGES.size(); ++i)
  {
    if (value == LANGUAGES[i])
      return NAMES[i];
  }
  return std::nullopt;
}

// The word's columns in `image`, if it has it.
std::optional<std::pair<uint32_t, uint32_t>> WordColumns(const Image& image, Word word)
{
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  const uint32_t top = WORD_TOPS[word.band];
  const auto inked = [&](uint32_t x) {
    for (uint32_t y = top; y < top + WORD_HEIGHT && y < image.height; ++y)
    {
      if (image.rgba[(static_cast<size_t>(y) * image.width + x) * 4] > INK)
        return true;
    }
    return false;
  };
  for (uint32_t x = 0; x < WIDTH;)
  {
    if (!inked(x))
    {
      ++x;
      continue;
    }
    const uint32_t from = x;
    uint32_t last = x;
    while (x < WIDTH && x - last < WORD_GAP)
    {
      if (inked(x))
        last = x;
      ++x;
    }
    runs.emplace_back(from, last + 1);
  }
  if (runs.empty())
    return std::nullopt;
  if (word.index < 0)
    return runs.back();
  if (static_cast<size_t>(word.index) < runs.size())
    return runs[static_cast<size_t>(word.index)];
  return std::nullopt;
}

// Rows [from_y, from_y + rows) of `source`'s columns [x0, x1) into `image` at
// (x, y), pressed to `room` texels when wider, at `level`.
void Copy(Image& image, const Image& source, uint32_t x0, uint32_t x1, uint32_t from_y, uint32_t rows, uint32_t x,
          uint32_t y, float room, uint8_t level)
{
  const float width = static_cast<float>(x1 - x0), drawn = std::min(width, room), step = width / drawn;
  for (uint32_t r = 0; r < rows; ++r)
  {
    for (uint32_t c = 0; static_cast<float>(c) < drawn && x + c < WIDTH; ++c)
    {
      const auto sx = std::min(x1 - 1, x0 + static_cast<uint32_t>(static_cast<float>(c) * step));
      const uint8_t ink = source.rgba[(static_cast<size_t>(from_y + r) * source.width + sx) * 4];
      uint8_t* p = &image.rgba[(static_cast<size_t>(y + r) * image.width + x + c) * 4];
      p[0] = p[1] = p[2] = static_cast<uint8_t>(ink * level / 255);
    }
  }
}

// The game's ON and OFF words in each language (the texture's own lettering).
constexpr std::array<std::array<std::string_view, 2>, dualize::nights::LANGUAGE_COUNT> SWITCH_WORDS{{
    {"", ""}, {"ON", "OFF"}, {"ACTIVÉ", "DÉSACTIVÉ"}, {"SÍ", "NO"}, {"EIN", "AUS"}, {"SÌ", "NO"}}};
// The languages' names as the game writes them.
constexpr std::array<std::string_view, 6> LANGUAGE_NAMES{"日本語", "English", "Français", "Español", "Deutsch", "Italiano"};

// The list's four lines, from option `top` on: each label (pressed narrower
// when too long), and its value in its value line, centred in the cell its
// box's word shows (pressed narrower when too long), both in the game's
// language. The texture gets the game's own lettering where it has the same
// words; the renderer draws it from a copy SHARP_SCALE times larger, every
// word set by the face at that size (the Japanese ON and OFF, which it lacks,
// from the texture's, sharpened). A row with no value has its box hidden.
void DrawRows(CPUState* ctx, uint32_t screen)
{
  if (g_page.labels_in_ram == nullptr)
    return;
  const Machine& machine = GetMachine(CpuOf(ctx));
  const Language language = g_page.language;
  const auto index = static_cast<size_t>(language);
  const Image& source = *g_page.labels[index];
  Image image = source;
  std::fill_n(image.rgba.begin(), static_cast<size_t>(DRAWN_LINES) * LINE * WIDTH * 4, 0);
  const std::array<Row, ROWS> rows = RowsShown(machine);
  const auto draw_label = [&](Image& target, uint32_t scale, int row, uint8_t level) {
    const std::u32string label = Utf32(dualize::nights::Translate(rows[row].label, language));
    const float width = LabelLettering().Width(label), room = LABEL_RIGHT - LABEL_X;
    LabelLettering().Draw(target, static_cast<uint32_t>(row), LABEL_BASELINE, label, LABEL_X,
                          width > room ? room / width : 1.0f, level, scale);
  };
  const auto draw_value = [&](Image& target, uint32_t scale, int row, std::u32string_view value, uint8_t level) {
    const float squeeze = std::min(1.0f, VALUE_ROOM / ValueLettering().Width(value));
    ValueLettering().Draw(target, VALUE_LINE + static_cast<uint32_t>(row), VALUE_BASELINE, value,
                          (VALUE_WIDTH - ValueLettering().Width(value, squeeze)) / 2, squeeze, level, scale);
  };
  // A value's words as the face sets them; none for a word only the texture has.
  const auto value_text = [&](std::string_view value) -> std::optional<std::u32string> {
    if (value == "ON" || value == "OFF")
    {
      const std::string_view word = SWITCH_WORDS[index][value == "ON" ? 0 : 1];
      return word.empty() ? std::nullopt : std::optional(Utf32(word));
    }
    if (const auto it = std::ranges::find(LANGUAGES, value); it != LANGUAGES.end())
      return Utf32(LANGUAGE_NAMES[static_cast<size_t>(it - LANGUAGES.begin())]);
    return Utf32(dualize::nights::Translate(value, language));
  };
  for (int row = 0; row < ROWS; ++row)
  {
    if (rows[row].label.empty())
      continue;
    const uint8_t level = rows[row].locked ? LOCKED_LEVEL : 255;
    if (rows[row].label == "Language Settings")
    {
      Copy(image, source, 0, WIDTH, LANGUAGE_LABEL_LINE * LINE, LINE, 0, static_cast<uint32_t>(row) * LINE,
           static_cast<float>(WIDTH), level);
    }
    else
      draw_label(image, 1, row, level);
    if (rows[row].value.empty())
      continue;
    const uint32_t value_y = (VALUE_LINE + static_cast<uint32_t>(row)) * LINE;
    if (const auto word = GameWord(rows[row].value))
    {
      if (const auto columns = WordColumns(source, *word))
      {
        const auto [x0, x1] = *columns;
        const float width = std::min(static_cast<float>(x1 - x0), VALUE_ROOM);
        Copy(image, source, x0, x1, WORD_TOPS[word->band], WORD_HEIGHT,
             static_cast<uint32_t>(std::floor((VALUE_WIDTH - width) / 2)), value_y + static_cast<uint32_t>(VALUE_TOP),
             VALUE_ROOM, level);
        continue;
      }
    }
    if (const auto value = value_text(rows[row].value))
      draw_value(image, 1, row, *value, level);
  }
  dualize::EncodeGreyCmpr(image, 0, DRAWN_LINES * LINE, g_page.labels_in_ram);
  for (uint8_t* copy : g_page.labels_found)
  {
    if (copy != g_page.labels_in_ram)
      std::memcpy(copy, g_page.labels_in_ram, DRAWN_BYTES);
  }
  // The sharp copy (every copy of the texture has the same bytes).
  const auto key = aurora::texture::source_key(CMPR_FORMAT, image.width, image.height, g_page.labels_in_ram);
  if (!g_page.sharp_key || !(*g_page.sharp_key == key))
  {
    if (g_page.sharp_key)
      aurora::texture::remove_memory_replacement(*g_page.sharp_key);
    Image sharp = dualize::SharpenGreyMask(image, SHARP_SCALE, LINE);
    const auto clear = [&](uint32_t line) {
      const size_t from = static_cast<size_t>(line) * LINE * SHARP_SCALE * sharp.width;
      for (size_t i = from; i < from + static_cast<size_t>(LINE) * SHARP_SCALE * sharp.width; ++i)
        sharp.rgba[i * 4] = sharp.rgba[i * 4 + 1] = sharp.rgba[i * 4 + 2] = 0;
    };
    for (int row = 0; row < ROWS; ++row)
    {
      if (rows[row].label.empty())
        continue;
      const uint8_t level = rows[row].locked ? LOCKED_LEVEL : 255;
      clear(static_cast<uint32_t>(row));
      draw_label(sharp, SHARP_SCALE, row, level);
      if (rows[row].value.empty())
        continue;
      if (const auto value = value_text(rows[row].value))
      {
        clear(VALUE_LINE + static_cast<uint32_t>(row));
        draw_value(sharp, SHARP_SCALE, row, *value, level);
      }
    }
    uint32_t mips = 0;
    const std::vector<uint8_t> texels = dualize::GreyMipChain(sharp, mips);
    aurora::texture::set_memory_replacement(key, sharp.width, sharp.height, mips, texels);
    g_page.sharp_key = key;
  }
  aurora_fifo_window_size(&g_page.drawn_window[0], &g_page.drawn_window[1]);
  if (g_page.boxes && !g_page.undo.empty())
  {
    Layout layout(CpuOf(ctx), ctx, screen + SCREEN_LAYOUT);
    for (int row = 0; row < ROWS; ++row)
      layout.SetHidden(layout.Parent((*g_page.boxes)[row].word), rows[row].value.empty());
  }
  g_page.drawn_rows = rows;
}

void RestoreRows()
{
  if (g_page.labels_in_ram != nullptr)
  {
    for (uint8_t* copy : g_page.labels_found)
      std::memcpy(copy, g_page.labels_encoded[static_cast<size_t>(g_page.language)].data(), DRAWN_BYTES);
  }
  if (g_page.sharp_key)
    aurora::texture::remove_memory_replacement(*g_page.sharp_key);
  g_page.sharp_key.reset();
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
    g_page.help[g_page.help_shown].Write(dualize::nights::Utf16(dualize::nights::Translate(text, g_page.language)));
    g_page.help_text = text;
  }
  const uint32_t help = dualize::Load32(GetMachine(cpu).memory.Effective(NIGHTS_MODE_OPTION_HELP, 4));
  cpu.CallGuest(NIGHTS_SET_HELP, {help, g_page.help[g_page.help_shown].id});
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
float LoadFloat(const Memory& memory, uint32_t address)
{
  return std::bit_cast<float>(Load(memory, address));
}

void StoreFloat(const Memory& memory, uint32_t address, float value)
{
  Store(memory, address, std::bit_cast<uint32_t>(value));
}

// The value boxes' casts, checked against the layout the page knows (each
// group's halves the two casts after it).
void FindValueBoxes(CPUState* ctx, uint32_t screen)
{
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  Layout layout(CpuOf(ctx), ctx, screen + SCREEN_LAYOUT);
  const auto halves_of = [&](uint32_t group) {
    std::array<uint32_t, 2> halves{};
    for (size_t i = 0; i < halves.size(); ++i)
    {
      const uint32_t half = group + CAST_SIZE * static_cast<uint32_t>(i + 1);
      halves[i] = group != 0 && layout.Parent(half) == group ? half : 0;
    }
    return halves;
  };
  std::array<ValueBox, ROWS> boxes;
  for (uint32_t row = 0; row < ROWS; ++row)
  {
    ValueBox& box = boxes[row];
    box.word = layout.Cast(VALUE_WORDS[row]);
    if (row == BORROWED_ROW)
    {
      for (size_t i = 0; i < box.halves.size(); ++i)
        box.halves[i] = layout.Cast(BORROWED_HALVES[i]);
    }
    else
    {
      box.halves = halves_of(layout.Parent(box.word));
    }
  }
  // The second row's group: the list's cast between the first and third rows'
  // (names repeat across the layout's windows).
  const uint32_t first_row = layout.Parent(layout.Parent(boxes[0].word));
  const uint32_t third_row = layout.Parent(layout.Parent(boxes[2].word));
  g_page.second_row = 0;
  for (uint32_t cast = first_row + CAST_SIZE; first_row != 0 && cast < third_row; cast += CAST_SIZE)
  {
    if (layout.Parent(cast) == layout.Parent(first_row))
    {
      g_page.second_row = cast;
      break;
    }
  }
  g_page.borrowed_group = layout.Parent(boxes[BORROWED_ROW].halves[0]);
  g_page.borrowed_group_halves = halves_of(g_page.borrowed_group);
  bool found = g_page.second_row != 0 && g_page.borrowed_group != 0;
  for (const ValueBox& box : boxes)
    found &= box.word != 0 && box.halves[0] != 0 && box.halves[1] != 0;
  for (uint32_t half : g_page.borrowed_group_halves)
    found &= half != 0;
  if (!found)
  {
    std::fprintf(stderr, "[options] the value boxes are not where the graphics page expects them\n");
    g_page.boxes.reset();
    return;
  }
  g_page.boxes = boxes;
  g_page.labels_texture = Load(memory, boxes[0].word + CAST_QUAD + VERTICES * VERTEX);
}

// A cast's quads as they are, to put back when the page goes.
void KeepQuads(const Memory& memory, uint32_t cast)
{
  auto& [at, bytes] = g_page.quads.emplace_back();
  at = cast;
  std::memcpy(bytes.data(), memory.Effective(cast + CAST_LOCAL_QUAD, CAST_QUADS_BYTES), CAST_QUADS_BYTES);
}

// The tree the layout places its casts by each frame (its file's: for each
// cast its first child's and next sibling's index, -1 for none), reached
// from a cast: its cast group (count, casts, ..., scene), the scene's file
// data, its cast groups (count, casts, root, tree).
class CastTree
{
public:
  CastTree(const Memory& memory, uint32_t cast) : m_memory(memory)
  {
    const uint32_t group = Load(memory, cast + CAST_GROUP);
    const uint32_t count = Load(memory, group), scene = Load(memory, group + 0xC);
    m_casts = Load(memory, group + 4);
    const uint32_t data = Load(memory, scene + 0x40);
    if (Load(memory, data + 0x48) != 1)
      return;  // the Options layout has one cast group
    const uint32_t file_group = Load(memory, data + 0x4C);
    if (Load(memory, file_group) == count)
    {
      m_data = Load(memory, file_group + 4);
      m_tree = Load(memory, file_group + 0xC);
    }
  }
  bool Valid() const { return m_tree != 0; }
  // The cast's file data.
  uint32_t Data(uint32_t cast) const { return Load(m_memory, m_data + static_cast<uint32_t>(Index(cast)) * 4); }

  // `cast` moved to the end of `parent`'s children.
  void Move(uint32_t cast, uint32_t parent)
  {
    const int32_t index = Index(cast), from = Index(Load(m_memory, cast + CAST_PARENT)), to = Index(parent);
    const uint32_t next = Load(m_memory, Next(index));
    if (static_cast<int32_t>(Load(m_memory, Child(from))) == index)
      Write(Child(from), next);
    else
      Write(LastBefore(Load(m_memory, Child(from)), index), next);
    Write(Next(index), NONE);
    const uint32_t first = Load(m_memory, Child(to));
    Write(first == NONE ? Child(to) : LastBefore(first, -1), static_cast<uint32_t>(index));
    Write(cast + CAST_PARENT, parent);
  }

private:
  static constexpr uint32_t NONE = 0xFFFFFFFF;
  int32_t Index(uint32_t cast) const { return static_cast<int32_t>((cast - m_casts) / CAST_SIZE); }
  uint32_t Child(int32_t index) const { return m_tree + static_cast<uint32_t>(index) * 8; }
  uint32_t Next(int32_t index) const { return Child(index) + 4; }
  // The next-sibling word, from `index` on, that holds `until`.
  uint32_t LastBefore(uint32_t index, int32_t until) const
  {
    while (static_cast<int32_t>(Load(m_memory, Next(static_cast<int32_t>(index)))) != until)
      index = Load(m_memory, Next(static_cast<int32_t>(index)));
    return Next(static_cast<int32_t>(index));
  }
  void Write(uint32_t address, uint32_t value)
  {
    g_page.undo.emplace_back(address, Load(m_memory, address));
    Store(m_memory, address, value);
  }

  const Memory& m_memory;
  uint32_t m_casts = 0, m_data = 0, m_tree = 0;
};

// A word of the layout's changed, to put back when the page goes.
void Change(const Memory& memory, uint32_t address, uint32_t value)
{
  g_page.undo.emplace_back(address, Load(memory, address));
  Store(memory, address, value);
}

// The texture place of row `row`'s value cell (top left, bottom left, top
// right, bottom right).
using Cell = std::array<std::array<float, 2>, VERTICES>;
Cell ValueCell(uint32_t row)
{
  const float v0 = (static_cast<float>((VALUE_LINE + row) * LINE) + VALUE_TOP) / WIDTH;
  const float v1 = v0 + VALUE_HEIGHT / WIDTH, u1 = VALUE_WIDTH / WIDTH;
  return {{{0, v0}, {0, v1}, {u1, v0}, {u1, v1}}};
}

// `to`'s own quad made `from`'s (its place scaled by the casts' own scales,
// so the layout draws it the same about `to`'s place), in `color`, with
// texture place `uv` (`from`'s when none) on `texture`. The layout sets a
// cast's texture place only as its pattern changes, so the drawn quad's too.
void CopyQuad(const Memory& memory, uint32_t from, uint32_t to, uint32_t color, const std::optional<Cell>& uv,
              uint32_t texture)
{
  const float sx = LoadFloat(memory, from + CAST_OWN_SCALE_X) / LoadFloat(memory, to + CAST_OWN_SCALE_X);
  const float sy = LoadFloat(memory, from + CAST_OWN_SCALE_Y) / LoadFloat(memory, to + CAST_OWN_SCALE_Y);
  for (uint32_t v = 0; v < VERTICES; ++v)
  {
    const uint32_t source = from + CAST_LOCAL_QUAD + v * VERTEX, own = to + CAST_LOCAL_QUAD + v * VERTEX;
    const auto [u, w] = uv ? (*uv)[v] : std::array{LoadFloat(memory, source + 12), LoadFloat(memory, source + 16)};
    StoreFloat(memory, own, LoadFloat(memory, source) * sx);
    StoreFloat(memory, own + 4, LoadFloat(memory, source + 4) * sy);
    Store(memory, own + 8, color);
    for (uint32_t at : {own, to + CAST_QUAD + v * VERTEX})
    {
      StoreFloat(memory, at + 12, u);
      StoreFloat(memory, at + 16, w);
    }
  }
  Store(memory, to + CAST_QUAD + VERTICES * VERTEX, texture);
}

// The borrowed casts in the first row's box's colours (its halves' and
// word's own, from its animation).
void ColorValueBox(const Memory& memory, Layout& layout)
{
  const std::array<ValueBox, ROWS>& boxes = *g_page.boxes;
  const ValueBox& borrowed = boxes[BORROWED_ROW];
  for (size_t i = 0; i < 2; ++i)
    layout.SetColor(borrowed.halves[i], Load(memory, boxes[0].halves[i] + CAST_OWN_COLOR));
  layout.SetColor(borrowed.word, Load(memory, boxes[0].word + CAST_OWN_COLOR));
}

// As the page opens: every word shows its row's value cell; the borrowed
// group moved to the second row, the casts borrowed for its halves and word
// into it, made the first row's, its own halves hidden. The layout then
// places, fades and animates them with the row.
void BorrowValueBox(CPUState* ctx, uint32_t screen)
{
  if (!g_page.boxes)
    return;
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const std::array<ValueBox, ROWS>& boxes = *g_page.boxes;
  const ValueBox& borrowed = boxes[BORROWED_ROW];
  CastTree tree(memory, g_page.borrowed_group);
  if (!tree.Valid())
  {
    std::fprintf(stderr, "[options] the layout's cast tree is not where the graphics page expects it\n");
    g_page.boxes.reset();
    return;
  }
  for (const ValueBox& box : boxes)
    KeepQuads(memory, box.word);
  for (uint32_t half : borrowed.halves)
    KeepQuads(memory, half);
  for (const auto& [cast, parent] : {std::pair{g_page.borrowed_group, g_page.second_row},
                                     std::pair{borrowed.halves[1], g_page.borrowed_group},
                                     std::pair{borrowed.word, g_page.borrowed_group}})
  {
    if (Load(memory, cast + CAST_PARENT) != parent)
      tree.Move(cast, parent);
  }
  for (size_t i = 0; i < 2; ++i)
  {
    const uint32_t half = boxes[0].halves[i];
    CopyQuad(memory, half, borrowed.halves[i], Load(memory, half + CAST_OWN_COLOR), std::nullopt,
             Load(memory, half + CAST_QUAD + VERTICES * VERTEX));
  }
  for (uint32_t row = 0; row < ROWS; ++row)
  {
    const uint32_t word = boxes[row].word, from = row == BORROWED_ROW ? boxes[0].word : word;
    CopyQuad(memory, from, word, Load(memory, from + CAST_LOCAL_QUAD + 8), ValueCell(row), g_page.labels_texture);
  }
  // Their overrides' flags, as they were (the colours set over the network
  // window's animation, which has them unseen, go with them).
  for (uint32_t cast : {borrowed.halves[0], borrowed.halves[1], borrowed.word})
    Change(memory, cast + CAST_OVERRIDES, Load(memory, cast + CAST_OVERRIDES));
  // The word's blended by its texture's alpha, as the words are (its own
  // cast, a sparkle, isn't).
  const uint32_t word_data = tree.Data(borrowed.word);
  Change(memory, word_data + CAST_DATA_FLAGS, Load(memory, word_data + CAST_DATA_FLAGS) | CAST_DATA_TEXTURE_ALPHA);
  Layout layout(CpuOf(ctx), ctx, screen + SCREEN_LAYOUT);
  for (uint32_t half : g_page.borrowed_group_halves)
    layout.SetHidden(half, true);
  ColorValueBox(memory, layout);
}

// Off the graphics page: the casts it changed as they were (the layout
// places them again as its own).
void ReleaseValueBoxes(CPUState* ctx, uint32_t screen)
{
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  for (auto it = g_page.undo.rbegin(); it != g_page.undo.rend(); ++it)
    Store(memory, it->first, it->second);
  for (const auto& [cast, bytes] : g_page.quads)
    std::memcpy(memory.Effective(cast + CAST_LOCAL_QUAD, CAST_QUADS_BYTES), bytes.data(), CAST_QUADS_BYTES);
  if (!g_page.undo.empty() && g_page.boxes)
  {
    Layout layout(CpuOf(ctx), ctx, screen + SCREEN_LAYOUT);
    for (uint32_t half : g_page.borrowed_group_halves)
      layout.SetHidden(half, false);
  }
  g_page.undo.clear();
  g_page.quads.clear();
}
}  // namespace

namespace dualize::nights
{
void KeepOptionLabels(Language language, const Image& image, std::span<const uint8_t> encoded)
{
  g_page.labels[static_cast<size_t>(language)] = image;
  g_page.labels_encoded[static_cast<size_t>(language)].assign(encoded.begin(), encoded.end());
}

void KeepOptionTitle(Language language, std::span<const uint8_t> original, std::span<const uint8_t> graphics)
{
  g_page.title_original[static_cast<size_t>(language)].assign(original.begin(), original.end());
  g_page.title_graphics[static_cast<size_t>(language)].assign(graphics.begin(), graphics.end());
}

void TakeOverOptionList(CPUState* ctx, uint32_t screen)
{
  if (g_page.graphics || g_page.taken)
    return;
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  g_page.language = dualize::nights::GameLanguage(memory);
  g_page.labels_in_ram = FindLabels(memory);
  if (g_page.labels_in_ram == nullptr)
  {
    std::fprintf(stderr, "[options] the labels texture is not in memory; the list keeps the game's rows\n");
    return;
  }
  if (!g_page.boxes)
    FindValueBoxes(ctx, screen);
  if (g_page.undo.empty())
    BorrowValueBox(ctx, screen);
  if (!g_page.boxes)
  {
    RestoreRows();
    return;
  }
  for (HelpMessage& message : g_page.help)
    message.Find(memory, g_page.language);
  g_page.help_shown = -1;
  g_page.help_text.clear();
  g_page.taken = true;
  g_page.top = 0;
  g_page.row = 0;
  Store(memory, screen + SCREEN_ROW, 0);
  Store(memory, screen + SCREEN_CURSOR, 0);
  DrawRows(ctx, screen);
}

void ResetGraphicsPage()
{
  g_page.graphics = false;
  g_page.taken = false;
  g_page.top = g_page.row = g_page.main_top = g_page.main_row = 0;
  g_page.drawn_rows = {};
  g_page.pending = Page::Switch::NONE;
  g_page.boxes.reset();
  g_page.undo.clear();
  g_page.quads.clear();
  g_page.labels_in_ram = nullptr;
  g_page.title_in_ram.clear();
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
  const auto release = [&] {
    ReleaseValueBoxes(ctx, screen);
    RestoreRows();
    for (HelpMessage& message : g_page.help)
      message.Restore();
  };
  if (g_page.pending == Page::Switch::EXIT)
  {
    // The list goes on to the top level, as it was.
    release();
    g_page.pending = Page::Switch::NONE;
    g_page.taken = false;
    g_page.drawn_rows = {};
    return false;
  }
  g_page.graphics = g_page.pending == Page::Switch::ENTER;
  g_page.pending = Page::Switch::NONE;
  g_page.help_shown = -1;
  g_page.help_text.clear();
  if (g_page.graphics)
  {
    g_page.main_top = g_page.top;
    g_page.main_row = g_page.row;
    g_page.top = g_page.row = 0;
    if (g_page.labels_in_ram == nullptr)
    {
      g_page.language = dualize::nights::GameLanguage(machine.memory);
      g_page.labels_in_ram = FindLabels(machine.memory);
    }
    if (g_page.labels_in_ram == nullptr)
      std::fprintf(stderr, "[options] the labels texture is not in memory; the graphics page keeps its labels\n");
    ShowTitle(machine.memory, true);
    if (!g_page.taken)
    {
      for (HelpMessage& message : g_page.help)
        message.Find(machine.memory, g_page.language);
    }
    Store(machine.memory, screen + SCREEN_ROW, 0);
    if (!g_page.boxes)
      FindValueBoxes(ctx, screen);
    if (g_page.undo.empty())
      BorrowValueBox(ctx, screen);
    DrawRows(ctx, screen);
  }
  else
  {
    ShowTitle(machine.memory, false);
    if (g_page.taken)
    {
      g_page.top = g_page.main_top;
      g_page.row = g_page.main_row;
      DrawRows(ctx, screen);
      Store(machine.memory, screen + SCREEN_ROW, static_cast<uint32_t>(g_page.row));
    }
    else
    {
      release();
      Store(machine.memory, screen + SCREEN_ROW, GRAPHICS_ROW);
    }
  }
  Store(machine.memory, screen + SCREEN_STATE, STATE_OPEN);
  ctx->pc = NIGHTS_MODE_OPTION_UPDATE_DONE;
  return true;
}

// The value boxes' words, set as the list opens (r3 the screen): on the
// graphics page they keep the value cells the page gave them.
extern "C" bool nights_ModeOptionValues(CPUState* ctx)
{
  if (!g_page.graphics && !g_page.taken)
    return false;
  ctx->pc = ctx->lr;
  return true;
}

// The rows' highlight and the big icon (r3 the screen), from the row the
// cursor is on: on the graphics page the icon stays Graphics Settings'.
extern "C" bool nights_ModeOptionHighlight(CPUState* ctx)
{
  if (!g_page.graphics && !g_page.taken)
    return false;
  Cpu& cpu = CpuOf(ctx);
  const Memory& memory = GetMachine(cpu).memory;
  const uint32_t screen = ctx->gpr[3];
  const auto row = static_cast<uint32_t>(g_page.row);
  uint32_t icon = GRAPHICS_ICON;
  if (!g_page.graphics)
  {
    const int option = std::clamp(g_page.top + g_page.row, 0, MAIN_OPTION_COUNT - 1);
    icon = static_cast<uint32_t>(std::max(GAME_ROW[option], 0)) + 1;
  }
  {
    Layout layout(cpu, ctx, screen + SCREEN_LAYOUT);
    for (uint32_t r = 0; r < ROWS; ++r)
      layout.SetPattern(Load(memory, NIGHTS_MODE_OPTION_ROW_NODES + r * ROW_NODE_STRIDE), r == row ? 0 : 1);
    layout.SetPattern(NIGHTS_MODE_OPTION_ICON, icon);
    layout.SetPattern(NIGHTS_MODE_OPTION_ICON_GLOW, icon);
  }
  ctx->pc = ctx->lr;
  return true;
}

// The list's state 1 (r30 the screen), each frame: on the graphics page the
// page's help line and input instead of the main page's.
extern "C" bool nights_ModeOptionList(CPUState* ctx)
{
  if (!g_page.graphics && !g_page.taken)
    return false;
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const Memory& memory = machine.memory;
  const uint32_t screen = ctx->gpr[30];
  // Back from a window of the game's, opened with its own row: the page's.
  g_page.switch_option = -1;
  const int row = g_page.row;
  if (Load(memory, screen + SCREEN_ROW) != static_cast<uint32_t>(row))
  {
    Store(memory, screen + SCREEN_ROW, static_cast<uint32_t>(row));
    Store(memory, screen + SCREEN_CURSOR, static_cast<uint32_t>(row));
    cpu.CallGuest(NIGHTS_MODE_OPTION_ROW_ICON, {screen});
  }
  const int count = OptionCount();
  const int option = std::clamp(g_page.top + row, 0, count - 1);
  const int game_row = g_page.graphics ? -1 : GAME_ROW[option];
  if (g_page.graphics)
  {
    ShowHelp(ctx, dualize::OptionHelp(machine.Graphics(), static_cast<GraphicsOption>(option)));
    std::array<uint32_t, 2> window{};
    aurora_fifo_window_size(&window[0], &window[1]);
    if (window != g_page.drawn_window)
      DrawRows(ctx, screen);
  }
  else
  {
    if (game_row >= 0)
    {
      const uint32_t help = Load(memory, NIGHTS_MODE_OPTION_HELP);
      cpu.CallGuest(NIGHTS_SET_HELP, {help, static_cast<uint32_t>(NIGHTS_MODE_OPTION_ROW_HELP + game_row)});
    }
    else
    {
      ShowHelp(ctx, SUBTITLES_HELP);
    }
    // The game's windows change its settings.
    if (RowsShown(machine) != g_page.drawn_rows)
      DrawRows(ctx, screen);
  }
  ctx->pc = NIGHTS_MODE_OPTION_UPDATE_DONE;
  if (cpu.CallGuest(NIGHTS_MODE_OPTION_BUSY, {screen}) != 0)
    return true;

  const uint32_t controller = cpu.CallGuest(NIGHTS_INPUT_CONTROLLER, {Load(memory, NIGHTS_INPUT), 0});
  const auto repeated = [&](uint32_t mask) { return cpu.CallGuest(NIGHTS_INPUT_REPEATED, {controller, 0, 0, mask}) != 0; };
  const auto pressed = [&](uint32_t mask) { return cpu.CallGuest(NIGHTS_INPUT_PRESSED, {controller, 0, 0, mask}) != 0; };
  int move = repeated(INPUT_UP) ? -1 : repeated(INPUT_DOWN) ? 1 : 0;
  if (move != 0)
  {
    // Past the list's ends it scrolls; past the page's it goes round.
    const int to = (option + move + count) % count;
    g_page.top = std::clamp(g_page.top, to - (ROWS - 1), to);
    g_page.top = std::clamp(g_page.top, 0, std::max(0, count - ROWS));
    const bool scrolled = g_page.top != option - row;
    g_page.row = to - g_page.top;
    Store(memory, screen + SCREEN_ROW, static_cast<uint32_t>(g_page.row));
    Store(memory, screen + SCREEN_CURSOR, static_cast<uint32_t>(g_page.row));
    cpu.CallGuest(NIGHTS_MODE_OPTION_ROW_ICON, {screen});
    PlaySound(cpu, SOUND_CURSOR);
    if (scrolled)
      DrawRows(ctx, screen);
    return true;
  }
  const int change = repeated(INPUT_LEFT) ? -1 : repeated(INPUT_RIGHT) ? 1 : pressed(INPUT_A) ? 2 : 0;
  if (change == 2 && game_row >= 0)
  {
    // The game's row: its own action, as A on it makes it.
    Store(memory, screen + SCREEN_ROW, static_cast<uint32_t>(game_row));
    ctx->pc = NIGHTS_MODE_OPTION_DECIDE;
    return true;
  }
  dualize::GraphicsSettings settings = machine.Graphics();
  if (SwitchOf(settings, option) != nullptr && change != 0)
  {
    // A switch: A opens the game's ON/OFF window, as on its own switches
    // (left and right do nothing; B leaves below).
    if (change == 2)
    {
      g_page.switch_option = option;
      PlaySound(cpu, SOUND_DECIDE);
      ctx->pc = NIGHTS_MODE_OPTION_SWITCH_WINDOW;
    }
    return true;
  }
  if (change != 0 && g_page.graphics)
  {
    dualize::StepOption(settings, static_cast<GraphicsOption>(option), change > 0 ? 1 : -1, change == 2);
    if (!(settings == machine.Graphics()))
    {
      machine.SetGraphics(settings);
      DrawRows(ctx, screen);
      PlaySound(cpu, change == 2 ? SOUND_DECIDE : SOUND_CURSOR);
    }
    return true;
  }
  if (pressed(INPUT_B))
  {
    g_page.pending = g_page.graphics ? Page::Switch::LEAVE : Page::Switch::EXIT;
    ctx->pc = NIGHTS_MODE_OPTION_LEAVE;
  }
  return true;
}

// The ON/OFF window opening (cntlzw r0, r0 hooked, r0 the setting): for a
// switch of the page's, the cursor on its setting. Then the instruction.
extern "C" bool nights_ModeOptionSwitchOpen(CPUState* ctx)
{
  if (g_page.switch_option < 0)
    return false;
  dualize::GraphicsSettings settings = GetMachine(CpuOf(ctx)).Graphics();
  if (const bool* on = SwitchOf(settings, g_page.switch_option))
    ctx->gpr[0] = *on ? 1 : 0;
  return false;
}

// The ON/OFF window's help line (lwz r4, row hooked; the help is the first
// row's plus r4): for a switch of the page's, the help its row has.
extern "C" bool nights_ModeOptionSwitchHelp(CPUState* ctx)
{
  if (g_page.switch_option < 0 || g_page.help_shown < 0)
    return false;
  ctx->gpr[4] = g_page.help[static_cast<size_t>(g_page.help_shown)].id - static_cast<uint32_t>(NIGHTS_MODE_OPTION_ROW_HELP);
  ctx->pc += 4;
  return true;
}

// ON or OFF chosen in the window (lwz r0, row hooked; the window at r1+0x48,
// r30 the screen): for a switch of the page's, the setting from the choice
// (0 ON), then the window closes as for the game's rows.
extern "C" bool nights_ModeOptionSwitchChosen(CPUState* ctx)
{
  if (g_page.switch_option < 0)
    return false;
  Cpu& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const uint32_t window = Load(machine.memory, ctx->gpr[1] + 0x48);
  const bool on = cpu.CallGuest(NIGHTS_SWITCH_WINDOW_CHOICE, {window}) == 0;
  dualize::GraphicsSettings settings = machine.Graphics();
  if (bool* setting = SwitchOf(settings, g_page.switch_option); setting != nullptr && *setting != on)
  {
    *setting = on;
    machine.SetGraphics(settings);
    DrawRows(ctx, ctx->gpr[30]);
  }
  ctx->pc = NIGHTS_MODE_OPTION_SWITCH_CLOSE;
  return true;
}
