// The in-game pause window's Options.

#include "sdk/nights_pause.hpp"

#include "core/cpu.h"
#include "ppc_natives.h"
#include "sdk/csd_layout.hpp"
#include "sdk/nights_options.hpp"
#include "sdk/nights_text.hpp"

#include "kernel/ios/disc.hpp"
#include "machine.hpp"
#include "ui/game_assets.hpp"
#include "ui/graphics_settings.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using dualize::GetMachine;
using dualize::GraphicsOption;
using dualize::Image;
using dualize::Machine;
using dualize::Memory;
using dualize::nights::CpuOf;
using dualize::nights::Language;
using dualize::nights::Layout;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wc++26-extensions"
// Dualize Pause Serif (SIL Open Font License 1.1, resources/fonts/
// DualizePauseSerif-OFL.txt): Tinos Bold Italic cut to Latin-1
// (scripts/lettering/font_pause.py).
constexpr unsigned char LABEL_FONT[] = {
#embed "../resources/fonts/DualizePauseSerif-BoldItalic.ttf"
};
#pragma clang diagnostic pop

// The window's labels: image 10 of each language's screen textures, a 128x64
// RGB5A3 of grey words on black, one per 20-row band (Continue, Retry, Quit;
// the rest black). Each now has 16 rows, the fourth band Options'; the
// labels' texture places follow (NewV). The sharp copy letters all four. The
// Japanese file lays its labels out otherwise and keeps them.
struct LabelsFile
{
  Language language;
  const char* file;
  std::array<std::string_view, 3> words;  // the game's, as its texture has them
  std::string_view options;
};
constexpr std::array<LabelsFile, 5> LABELS_FILES{{
    {Language::ENGLISH, "files/csd/textures/pjn_screen_allus.tls", {"Continue", "Retry", "Quit"}, "Options"},
    {Language::FRENCH, "files/csd/textures/pjn_screen_allfr.tls", {"Continuer", "Réessayer", "Quitter"}, "Options"},
    {Language::GERMAN, "files/csd/textures/pjn_screen_allga.tls", {"Fortfahren", "Erneut versuchen", "Verlassen"},
     "Optionen"},
    {Language::ITALIAN, "files/csd/textures/pjn_screen_allit.tls", {"Continua", "Riprova", "Abbandona"}, "Opzioni"},
    {Language::SPANISH, "files/csd/textures/pjn_screen_allsp.tls", {"Continuar", "Reintentar", "Salir"}, "Opciones"},
}};
constexpr uint32_t LABELS_IMAGE = 10, LABELS_WIDTH = 128, LABELS_HEIGHT = 64, RGB5A3 = 5;
constexpr uint32_t OLD_BAND = 20, BAND = 16, GAME_LABELS = 3, OPTIONS_BAND = 3;
constexpr uint32_t SHARP_SCALE = 4, LABELS_BYTES = LABELS_WIDTH * LABELS_HEIGHT * 2;

// The lettering as the game's (fitted to its three words, texel error over
// their bands): capitals 13 texels high on a baseline 16 down the band, the
// face 0.8 as wide, letters 1.25 texels further apart, slanted 0.1 more and
// 1/8 texel bolder; each texel row as bright as the game's words' at that
// height (their lines run brighter and darker down the letters).
constexpr float CAP = 13.0f, BASELINE = 16.0f, NARROW = 0.8f, TRACKING = 1.25f, SLANT = 0.1f;
constexpr uint32_t SUB = 8;  // the word drawn this many times finer, boldened by one

struct Kept
{
  Language language;
  aurora::texture::TextureSourceKey key;
  Image original;  // the game's
  const LabelsFile* labels;
  std::array<float, OLD_BAND> shade{};  // the game's words' brightness per row (Shades)
  float centre = 0;  // where the game centres its words
  std::vector<uint8_t> encoded;  // the texture as the disc serves it
  std::vector<uint8_t> game;     // the texture as the game's disc had it
  bool provided = false;
};
std::vector<Kept> g_kept;

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint8_t& Grey(Image& image, uint32_t x, uint32_t y)
{
  return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
}

// Rows [from, from + rows) of `source` into `count` rows of `image` from
// `to`, each the average of the source it covers.
void PressRows(const Image& source, uint32_t from, uint32_t rows, Image& image, uint32_t to, uint32_t count)
{
  const double step = static_cast<double>(rows) / count;
  for (uint32_t r = 0; r < count; ++r)
  {
    const double y0 = r * step, y1 = y0 + step;
    for (uint32_t x = 0; x < image.width; ++x)
    {
      std::array<double, 4> sum{};
      for (auto y = static_cast<uint32_t>(y0); y < rows && y < y1; ++y)
      {
        const double weight = std::min<double>(y + 1, y1) - std::max<double>(y, y0);
        const uint8_t* p = &source.rgba[(static_cast<size_t>(from + y) * source.width + x) * 4];
        for (int c = 0; c < 4; ++c)
          sum[c] += p[c] * weight;
      }
      uint8_t* q = &image.rgba[(static_cast<size_t>(to + r) * image.width + x) * 4];
      for (int c = 0; c < 4; ++c)
        q[c] = static_cast<uint8_t>(std::lround(sum[c] / step));
    }
  }
}

// How bright the game's words are at each row of a band (the brightest of
// their texels there; a row only edges touch takes the nearest full one's).
std::array<float, OLD_BAND> Shades(const Image& game)
{
  std::array<float, OLD_BAND> shade{};
  for (uint32_t y = 0; y < OLD_BAND; ++y)
    for (uint32_t band = 0; band < GAME_LABELS; ++band)
      for (uint32_t x = 0; x < game.width; ++x)
        shade[y] = std::max<float>(shade[y], game.rgba[(static_cast<size_t>(band * OLD_BAND + y) * game.width + x) * 4]);
  const std::array<float, OLD_BAND> measured = shade;
  for (uint32_t y = 0; y < OLD_BAND; ++y)
  {
    int best = -1;
    for (uint32_t z = 0; z < OLD_BAND; ++z)
      if (measured[z] >= 128 && (best < 0 || std::abs(static_cast<int>(z) - static_cast<int>(y)) <
                                                 std::abs(best - static_cast<int>(y))))
        best = static_cast<int>(z);
    if (measured[y] < 128)
      shade[y] = best >= 0 ? measured[static_cast<size_t>(best)] : 255.0f;
  }
  return shade;
}

// Where the game centres its words (the middle of their inked columns, on average).
float Centre(const Image& game)
{
  float sum = 0;
  for (uint32_t band = 0; band < GAME_LABELS; ++band)
  {
    uint32_t x0 = game.width, x1 = 0;
    for (uint32_t y = band * OLD_BAND; y < (band + 1) * OLD_BAND; ++y)
      for (uint32_t x = 0; x < game.width; ++x)
        if (game.rgba[(static_cast<size_t>(y) * game.width + x) * 4] != 0)
          x0 = std::min(x0, x), x1 = std::max(x1, x + 1);
    sum += x0 < x1 ? (x0 + x1) / 2.0f : game.width / 2.0f;
  }
  return sum / GAME_LABELS;
}

const stbtt_fontinfo& Face()
{
  static const stbtt_fontinfo font = [] {
    stbtt_fontinfo info{};
    stbtt_InitFont(&info, LABEL_FONT, 0);
    return info;
  }();
  return font;
}

// A line of lettering SUB times finer than the texture, boldened by one, on
// a canvas the game's band high (OLD_BAND texels), with where its ink lies.
struct Lettering
{
  int width = 0, height = 0;
  std::vector<uint8_t> ink;
  int ink0 = 0, ink1 = 0;
  float cap = CAP, baseline = BASELINE;
  float Width() const { return static_cast<float>(std::max(0, ink1 - ink0)) / SUB; }
};

// `text` with capitals `cap` texels high on `baseline`, the face `narrow` as
// wide (letters TRACKING apart at the game's width, closer as it narrows).
Lettering Letter(std::u32string_view text, float cap, float baseline, float narrow)
{
  const stbtt_fontinfo& font = Face();
  int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  stbtt_GetCodepointBox(&font, 'H', &bx0, &by0, &bx1, &by1);
  const float em = cap / static_cast<float>(by1) * SUB, sx = em * narrow, tracking = TRACKING * SUB * narrow / NARROW;
  const auto advance = [&](size_t i) {
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&font, static_cast<int>(text[i]), &advance, &bearing);
    if (i + 1 < text.size())
      advance += stbtt_GetCodepointKernAdvance(&font, static_cast<int>(text[i]), static_cast<int>(text[i + 1]));
    return static_cast<float>(advance) * sx + tracking;
  };
  float length = 0;
  for (size_t i = 0; i < text.size(); ++i)
    length += advance(i);
  const int margin = static_cast<int>(4 * SUB);
  Lettering l;
  l.cap = cap;
  l.baseline = baseline;
  l.width = static_cast<int>(std::ceil(length)) + 2 * margin;
  l.height = static_cast<int>(OLD_BAND * SUB);
  const float base = baseline * SUB;
  std::vector<uint8_t> canvas(static_cast<size_t>(l.width) * l.height);
  std::vector<uint8_t> glyph;
  float pen = static_cast<float>(margin);
  for (size_t i = 0; i < text.size(); ++i)
  {
    const int c = static_cast<int>(text[i]);
    const float shift = pen - std::floor(pen);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBoxSubpixel(&font, c, sx, em, shift, 0, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0)
    {
      glyph.assign(static_cast<size_t>(w) * h, 0);
      stbtt_MakeCodepointBitmapSubpixel(&font, glyph.data(), w, h, w, sx, em, shift, 0, c);
      for (int gy = 0; gy < h; ++gy)
      {
        const int y = static_cast<int>(base) + y0 + gy;
        if (y < 0 || y >= l.height)
          continue;
        const int left = static_cast<int>(std::floor(pen)) + x0 + static_cast<int>(std::lround(SLANT * (base - y)));
        for (int gx = 0; gx < w; ++gx)
        {
          const int x = left + gx;
          if (x >= 0 && x < l.width)
            canvas[static_cast<size_t>(y) * l.width + x] =
                std::max(canvas[static_cast<size_t>(y) * l.width + x], glyph[static_cast<size_t>(gy) * w + gx]);
        }
      }
    }
    pen += advance(i);
  }
  l.ink.assign(canvas.size(), 0);
  l.ink0 = l.width;
  for (int y = 0; y < l.height; ++y)
    for (int x = 0; x < l.width; ++x)
    {
      uint8_t v = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          if (y + dy >= 0 && y + dy < l.height && x + dx >= 0 && x + dx < l.width)
            v = std::max(v, canvas[static_cast<size_t>(y + dy) * l.width + x + dx]);
      l.ink[static_cast<size_t>(y) * l.width + x] = v;
      if (v != 0)
        l.ink0 = std::min(l.ink0, x), l.ink1 = std::max(l.ink1, x + 1);
    }
  return l;
}

// `l` into `band` of `image` (`scale` times the texture's size), its ink at
// `anchor` texels (its left at `align` 0, middle 0.5, right 1), over what the
// band has: each texel the average of what it covers (SUB / scale columns,
// the game's rows pressed into the band's), as bright as the game's words at
// that height of the letters, times `level`.
void Place(Image& image, uint32_t scale, uint32_t band, const Lettering& l, float anchor, float align,
           const std::array<float, OLD_BAND>& shade, float level = 1.0f)
{
  if (l.ink0 >= l.ink1)
    return;
  const int offset = static_cast<int>(std::lround(anchor * SUB - (l.ink0 + (l.ink1 - l.ink0) * align)));
  const uint32_t columns = SUB / scale;
  const double rows = static_cast<double>(l.height) / (BAND * scale);
  for (uint32_t r = 0; r < BAND * scale; ++r)
  {
    const double y0 = r * rows, y1 = y0 + rows;
    for (uint32_t tx = 0; tx < LABELS_WIDTH * scale; ++tx)
    {
      double sum = 0;
      for (auto y = static_cast<int>(y0); y < l.height && y < y1; ++y)
      {
        const double weight = std::min<double>(y + 1, y1) - std::max<double>(y, y0);
        // The game's words' height for this one of the letters'.
        const float game_y = BASELINE * SUB - (l.baseline * SUB - static_cast<float>(y)) * CAP / l.cap;
        const auto at = static_cast<size_t>(std::clamp(game_y, 0.0f, OLD_BAND * SUB - 1.0f)) / SUB;
        const float bright = shade[at] / 255.0f * level;
        for (uint32_t k = 0; k < columns; ++k)
        {
          const int x = static_cast<int>(tx * columns + k) - offset;
          if (x >= 0 && x < l.width)
            sum += l.ink[static_cast<size_t>(y) * l.width + x] * bright * weight;
        }
      }
      uint8_t* p = &image.rgba[(static_cast<size_t>(band * BAND * scale + r) * image.width + tx) * 4];
      p[0] = p[1] = p[2] = std::max(p[0], static_cast<uint8_t>(std::lround(std::min(255.0, sum / (rows * columns)))));
      p[3] = 255;
    }
  }
}

std::u32string Utf32(std::string_view text)
{
  std::u32string out;
  for (size_t i = 0; i < text.size();)
  {
    const auto c = static_cast<unsigned char>(text[i]);
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    char32_t code = extra == 0 ? c : c & (0x3F >> extra);
    for (int k = 1; k <= extra && i + k < text.size(); ++k)
      code = code << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    out.push_back(code);
    i += 1 + extra;
  }
  return out;
}

// `word` into `band` of `image`, as the game would have set it there.
void DrawWord(Image& image, uint32_t scale, uint32_t band, std::string_view word,
              const std::array<float, OLD_BAND>& shade, float centre)
{
  const std::u32string letters = Utf32(word);
  Lettering l = Letter(letters, CAP, BASELINE, NARROW);
  // Pressed narrower if it would run off the texture.
  const float room = 2.0f * std::min(centre, static_cast<float>(LABELS_WIDTH) - centre) - 2.0f;
  if (l.Width() > room)
    l = Letter(letters, CAP, BASELINE, NARROW * room / l.Width());
  Place(image, scale, band, l, centre, 0.5f, shade);
}

// The labels texture as it now is: at its own size the game's words pressed
// into the new bands (the texels the disc serves), `scale` times larger all
// four lettered.
Image Relabel(const Image& game, uint32_t scale, const LabelsFile& labels, const std::array<float, OLD_BAND>& shade,
              float centre)
{
  Image image{LABELS_WIDTH * scale, LABELS_HEIGHT * scale,
              std::vector<uint8_t>(size_t{LABELS_WIDTH} * LABELS_HEIGHT * scale * scale * 4, 0)};
  for (size_t i = 3; i < image.rgba.size(); i += 4)
    image.rgba[i] = 255;
  for (uint32_t band = 0; band < GAME_LABELS; ++band)
  {
    if (scale == 1)
      PressRows(game, band * OLD_BAND, OLD_BAND, image, band * BAND, BAND);
    else
      DrawWord(image, scale, band, labels.words[band], shade, centre);
  }
  DrawWord(image, scale, OPTIONS_BAND, labels.options, shade, centre);
  return image;
}

constexpr uint32_t PAUSE_SCENE = 0x54, PAUSE_VARIANT = 0x60;
// Set when the scene updated this frame.
constexpr uint32_t PAUSE_UPDATED = 0x64;
// The stage's pause controller: its actions (action, name: 8 bytes each), the
// chosen action, the cursor.
constexpr uint32_t PAUSE_ACTIONS = 0x20, PAUSE_ACTION = 0x24, PAUSE_CURSOR = 0x2C;
constexpr uint32_t CAST_SIZE = 0x144, CAST_PATTERN = 0x40, CAST_GROUP = 0x84;
// Its scale as animated (before the overrides).
constexpr uint32_t CAST_SCALE_X = 0x38, CAST_SCALE_Y = 0x3C;
// A cast's pattern before its first update.
constexpr float UNSET_PATTERN = -100000.0f;

uint32_t Load(const Memory& memory, uint32_t address)
{
  return dualize::Load32(memory.Effective(address, 4));
}

float LoadFloat(const Memory& memory, uint32_t address)
{
  return std::bit_cast<float>(Load(memory, address));
}

void StoreFloat(const Memory& memory, uint32_t address, float value)
{
  dualize::Store32(memory.Effective(address, 4), std::bit_cast<uint32_t>(value));
}

// Whether the game's language has its labels texture relabelled.
bool Relabelled(const Memory& memory)
{
  const Language language = dualize::nights::GameLanguage(memory);
  return std::ranges::any_of(g_kept, [&](const Kept& k) { return k.language == language; });
}

// A CSD scene's sub-images (texture, u0, v0, u1, v1), which a cast's
// patterns index (count at 0x3C, indices at 0x40 of its info): the game's
// labels, three bands of 20 rows of the labels image, set to the relabelled
// texture's bands of 16 rows, once in each scene that has them.
constexpr uint32_t SCENE_SUBIMAGES = 0x20, SUBIMAGE = 0x14, INFO_SPRITE = 4, INFO_PATTERNS = 0x3C,
                   INFO_INDICES = 0x40;

bool IsBand(const Memory& memory, uint32_t subimage, uint32_t band, float rows)
{
  const float top = band * rows / LABELS_HEIGHT, bottom = (band + 1) * rows / LABELS_HEIGHT;
  return Load(memory, subimage) == LABELS_IMAGE && LoadFloat(memory, subimage + 4) == 0.0f &&
         LoadFloat(memory, subimage + 8) == top && LoadFloat(memory, subimage + 12) == 1.0f &&
         LoadFloat(memory, subimage + 16) == bottom;
}

void SetBand(const Memory& memory, uint32_t subimage, uint32_t band)
{
  StoreFloat(memory, subimage + 8, static_cast<float>(band * BAND) / LABELS_HEIGHT);
  StoreFloat(memory, subimage + 16, static_cast<float>((band + 1) * BAND) / LABELS_HEIGHT);
}

void Rebanded(const Memory& memory, uint32_t subimages, uint32_t index)
{
  for (uint32_t band = 0; band < GAME_LABELS && band <= index; ++band)
  {
    const uint32_t first = subimages + (index - band) * SUBIMAGE;
    bool labels = true;
    for (uint32_t k = 0; k < GAME_LABELS && labels; ++k)
      labels = IsBand(memory, first + k * SUBIMAGE, k, OLD_BAND);
    if (!labels)
      continue;
    for (uint32_t k = 0; k < GAME_LABELS; ++k)
      SetBand(memory, first + k * SUBIMAGE, k);
    return;
  }
}

// The sub-image's band among the labels' three in its scene (one of them
// may show Options), or -1 if it isn't one of them.
int NaturalBand(const Memory& memory, uint32_t subimage)
{
  for (uint32_t band = 0; band < GAME_LABELS; ++band)
  {
    if (subimage < band * SUBIMAGE)
      break;
    const uint32_t first = subimage - band * SUBIMAGE;
    bool labels = true;
    for (uint32_t k = 0; k < GAME_LABELS && labels; ++k)
      labels = k == band || IsBand(memory, first + k * SUBIMAGE, k, BAND);
    if (labels)
      return static_cast<int>(band);
  }
  return -1;
}

// The pause window as its scene last played: the game's own (its language not
// relabelled), or one of its two sizes with Options among the buttons. The
// small window takes the large one's first three buttons, Retry's label
// showing Options; the large one borrows the small one's Quit button (from
// the scene's second group) as Options between Retry and Quit.
enum class Window { Game, Small, Large };
Window g_window = Window::Game;
// Options' label cast as the scene last played.
uint32_t g_label = 0, g_label_group = 0;
// The large and small windows' roots (casts) as the scene last played.
uint32_t g_large_root = 0, g_small_root = 0;

uint32_t OptionsButton()
{
  return g_window == Window::Large ? 2 : 1;
}

// The Options page: the window's labelled buttons become rows "Name
// < Value >" of the graphics page's options, Subtitles and Rumble, as many at
// a time, scrolled; left and right change the cursor's (A too, going round), B
// goes back. The rows are drawn into the labels texture's bands the buttons'
// labels show, with a sharp copy, and the texture put back as the page goes.
constexpr int MAX_ROWS = 4, SUBTITLES = dualize::GRAPHICS_OPTION_COUNT, RUMBLE = SUBTITLES + 1,
              PAGE_OPTIONS = RUMBLE + 1;
// Each row's label cast (in the scene's group of the large window, 0, or the
// small one, 1) and the band it shows: the labels Continue, Retry or Options,
// Options, Quit.
struct RowLabel
{
  uint32_t group, cast, band;
};
constexpr std::array<RowLabel, 3> SMALL_ROWS{{{0, 0x32, 0}, {0, 0x38, OPTIONS_BAND}, {0, 0x3e, 2}}};
constexpr std::array<RowLabel, MAX_ROWS> LARGE_ROWS{
    {{0, 0x32, 0}, {0, 0x38, 1}, {1, 0x2a, OPTIONS_BAND}, {0, 0x3e, 2}}};
std::span<const RowLabel> Rows()
{
  if (g_window == Window::Large)
    return LARGE_ROWS;
  return SMALL_ROWS;
}
// The rows' lettering: smaller than the words', the label casts stretched
// across their buttons and the face pressed back as much; the name from the
// left, the value between arrows at the right; a locked row dimmer.
constexpr float STRETCH = 1.4f, ROW_CAP = 11.0f, ROW_BASELINE = 15.0f, NAME_LEFT = 3.0f, ARROWS_LEFT = 82.0f,
                ARROWS_RIGHT = 125.0f, ARROW_ROOM = 7.0f, ROW_GAP = 4.0f, LOCKED = 0.5f;
constexpr uint32_t SETTINGS_RUMBLE = 0xB0;
constexpr uint32_t SOUND_CURSOR = 0, SOUND_DECIDE = 1, SOUND_CANCEL = 2;
// The pad state's application buttons for the four directions alone (the
// game's menus use pairs that also turn sideways ones), and its device
// buttons pressed (the application buttons have A and B as one), whose bits
// differ by controller: the Classic Controller's A and B, then the GameCube pad's.
constexpr uint32_t PAD_UP = 0x1, PAD_DOWN = 0x2, PAD_LEFT = 0x4, PAD_RIGHT = 0x8;
constexpr uint32_t PAD_DEVICE_PRESSED = 0x34, DEVICE_A = 0x1 | 0x80000, DEVICE_B = 0x2 | 0x100000;
constexpr uint32_t PAUSE_PAD = 0x34;  // the controller's pad, its cursor first

struct PageRow
{
  std::string name, value;
  bool locked = false;
  bool operator==(const PageRow&) const = default;
};

struct PausePage
{
  bool open = false;
  int top = 0, row = 0;
  // The device buttons pressed as the input last ran: a press stays in the pad
  // state for two runs (the A that opened the page among them).
  uint32_t pressed = 0;
  const Kept* kept = nullptr;
  std::vector<uint8_t*> textures;  // the labels texture's copies in memory
  std::vector<uint8_t> written;    // the bytes last put in them
  std::optional<aurora::texture::TextureSourceKey> key;  // the drawn rows' sharp copy
  std::array<PageRow, MAX_ROWS> drawn;
};
PausePage g_page;
// The device button that closed the page, kept from the game's input while it
// stays pressed (the game takes B for A).
uint32_t g_held = 0;
uint32_t g_task = 0;  // the pause window's task, as its scene last played

PageRow RowOf(const Machine& machine, int option)
{
  const auto on_off = [](bool on) { return std::string(on ? "ON" : "OFF"); };
  if (option < dualize::GRAPHICS_OPTION_COUNT)
  {
    const auto which = static_cast<GraphicsOption>(option);
    return {dualize::OptionName(which), dualize::OptionValue(machine.Graphics(), which),
            dualize::OptionLocked(machine.Graphics(), which)};
  }
  if (option == SUBTITLES)
    return {"Subtitles", on_off(machine.Graphics().subtitles)};
  const uint32_t settings = Load(machine.memory, NIGHTS_SETTINGS);
  return {"Rumble", on_off(*machine.memory.Effective(settings + SETTINGS_RUMBLE, 1) != 0)};
}

void Change(Machine& machine, int option, int direction, bool wrap)
{
  dualize::GraphicsSettings settings = machine.Graphics();
  if (option < dualize::GRAPHICS_OPTION_COUNT)
    dualize::StepOption(settings, static_cast<GraphicsOption>(option), direction, wrap);
  else if (option == SUBTITLES)
    settings.subtitles = !settings.subtitles;
  else
  {
    uint8_t& rumble = *machine.memory.Effective(Load(machine.memory, NIGHTS_SETTINGS) + SETTINGS_RUMBLE, 1);
    rumble = rumble == 0 ? 1 : 0;
  }
  if (!(settings == machine.Graphics()))
    machine.SetGraphics(settings);
}

// `text` in the rows' lettering, pressed narrower if wider than `room`.
Lettering Fitted(std::string_view text, float room)
{
  const std::u32string letters = Utf32(text);
  Lettering l = Letter(letters, ROW_CAP, ROW_BASELINE, NARROW / STRETCH);
  if (l.Width() > room)
    l = Letter(letters, ROW_CAP, ROW_BASELINE, NARROW / STRETCH * room / l.Width());
  return l;
}

void DrawRow(Image& image, uint32_t scale, uint32_t band, const PageRow& row, Language language)
{
  if (row.name.empty())
    return;
  const auto& shade = g_page.kept->shade;
  const float level = row.locked ? LOCKED : 1.0f;
  const float name_room = ARROWS_LEFT - ROW_GAP - NAME_LEFT, value_room = ARROWS_RIGHT - ARROWS_LEFT - 2 * ARROW_ROOM;
  Place(image, scale, band, Fitted(dualize::nights::Translate(row.name, language), name_room), NAME_LEFT, 0.0f, shade,
        level);
  Place(image, scale, band, Fitted("<", ARROW_ROOM), ARROWS_LEFT, 0.0f, shade, level);
  Place(image, scale, band, Fitted(">", ARROW_ROOM), ARROWS_RIGHT, 1.0f, shade, level);
  Place(image, scale, band, Fitted(dualize::nights::Translate(row.value, language), value_room),
        (ARROWS_LEFT + ARROWS_RIGHT) / 2, 0.5f, shade, level);
}

Image Blank(uint32_t scale)
{
  Image image{LABELS_WIDTH * scale, LABELS_HEIGHT * scale,
              std::vector<uint8_t>(static_cast<size_t>(LABELS_WIDTH) * LABELS_HEIGHT * scale * scale * 4, 0)};
  for (size_t i = 3; i < image.rgba.size(); i += 4)
    image.rgba[i] = 255;
  return image;
}

// Only the copies still holding the bytes put there: memory may have been
// replaced under the page (a savestate loaded).
bool KeepOurs()
{
  std::erase_if(g_page.textures,
                [](const uint8_t* copy) { return std::memcmp(copy, g_page.written.data(), LABELS_BYTES) != 0; });
  return !g_page.textures.empty();
}

// The rows shown, drawn if they changed: the sharp copy registered before
// memory has the new bytes (the renderer runs alongside; see DrawRows).
void DrawPage(const Machine& machine)
{
  const auto labels = Rows();
  std::array<PageRow, MAX_ROWS> rows;
  for (size_t r = 0; r < labels.size(); ++r)
    rows[r] = RowOf(machine, g_page.top + static_cast<int>(r));
  if (g_page.key && rows == g_page.drawn)
    return;
  const Language language = dualize::nights::GameLanguage(machine.memory);
  Image image = Blank(1), sharp = Blank(SHARP_SCALE);
  for (size_t r = 0; r < labels.size(); ++r)
  {
    DrawRow(image, 1, labels[r].band, rows[r], language);
    DrawRow(sharp, SHARP_SCALE, labels[r].band, rows[r], language);
  }
  std::vector<uint8_t> next(LABELS_BYTES);
  EncodeRgb5a3(image, 0, LABELS_HEIGHT, next.data());
  const auto key = aurora::texture::source_key(RGB5A3, LABELS_WIDTH, LABELS_HEIGHT, next.data());
  std::optional<aurora::texture::TextureSourceKey> replaced;
  if (!g_page.key || !(*g_page.key == key))
  {
    replaced = g_page.key;
    uint32_t mips = 0;
    std::vector<uint8_t> texels = MipChain(sharp, mips);
    aurora::texture::set_memory_replacement(key, sharp.width, sharp.height, mips, std::move(texels));
    g_page.key = key;
  }
  for (uint8_t* copy : g_page.textures)
    std::memcpy(copy, next.data(), LABELS_BYTES);
  g_page.written = std::move(next);
  if (replaced)
    aurora::texture::remove_memory_replacement(*replaced);
  g_page.drawn = rows;
}

// Every copy of the labels texture in memory, found by its words' first rows
// (only those: a savestate may hold an earlier build's Options lettering).
std::vector<uint8_t*> FindTextures(const Memory& memory, const std::vector<uint8_t>& encoded)
{
  constexpr size_t KEY = 1024, KEY_BYTES = 1024;  // texel rows 4-7: Continue's
  constexpr ptrdiff_t TEXTURE_ALIGN = 32;
  std::vector<uint8_t*> found;
  const std::boyer_moore_horspool_searcher search(encoded.begin() + KEY, encoded.begin() + KEY + KEY_BYTES);
  for (const auto& [base, size] : {std::pair{0x90000000u, Memory::MEM2_SIZE}, std::pair{0x80000000u, Memory::MEM1_SIZE}})
  {
    uint8_t* data = memory.Effective(base, size);
    for (uint8_t* at = data; (at = std::search(at, data + size, search)) != data + size; ++at)
      if (static_cast<size_t>(at - data) >= KEY && (at - KEY - data) % TEXTURE_ALIGN == 0 &&
          static_cast<size_t>(data + size - (at - KEY)) >= encoded.size())
        found.push_back(at - KEY);
  }
  return found;
}

// The rows' label casts as wide as their buttons (or back).
void Stretch(CPUState* ctx, float scale)
{
  if (g_task == 0)
    return;
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  Layout layout(CpuOf(ctx), ctx, g_task + PAUSE_SCENE);
  const std::array<uint32_t, 2> roots{layout.Cast("Root_Pause_L"), layout.Cast("Root_Pause_S")};
  for (const RowLabel& row : Rows())
    if (roots[row.group] != 0)
      layout.SetScaleX(Load(memory, Load(memory, roots[row.group] + CAST_GROUP) + 4) + row.cast * CAST_SIZE, scale);
}

uint32_t PadState(dualize::Cpu& cpu, const Memory& memory)
{
  return cpu.CallGuest(NIGHTS_PAD_STATE, {Load(memory, NIGHTS_PAD), 0});
}

void SetCursor(const Memory& memory, uint32_t controller, uint32_t cursor)
{
  dualize::Store32(memory.Effective(controller + PAUSE_CURSOR, 4), cursor);
  dualize::Store32(memory.Effective(controller + PAUSE_PAD, 4), cursor);
}

bool OpenPage(CPUState* ctx, uint32_t controller)
{
  const Machine& machine = GetMachine(CpuOf(ctx));
  const Language language = dualize::nights::GameLanguage(machine.memory);
  const auto kept = std::ranges::find(g_kept, language, &Kept::language);
  if (kept == g_kept.end())
    return false;
  g_page.textures = FindTextures(machine.memory, kept->encoded);
  if (g_page.textures.empty())
    return false;
  for (uint8_t* copy : g_page.textures)
    std::memcpy(copy, kept->encoded.data(), LABELS_BYTES);
  g_page.written = kept->encoded;
  g_page.kept = &*kept;
  g_page.open = true;
  g_page.top = g_page.row = 0;
  g_page.pressed = DEVICE_A;
  DrawPage(machine);
  Stretch(ctx, STRETCH);
  SetCursor(machine.memory, controller, 0);
  return true;
}

// The game's labels texture still in memory (a stage loaded before the disc
// served it relabelled: a savestate) relabelled there, as the bands are.
uint32_t g_state_loads = 0;  // the machine's savestate loads when last looked
void RelabelLoaded(const Memory& memory)
{
  const Language language = dualize::nights::GameLanguage(memory);
  const auto kept = std::ranges::find(g_kept, language, &Kept::language);
  if (kept == g_kept.end())
    return;
  for (uint8_t* copy : FindTextures(memory, kept->game))
    if (std::memcmp(copy, kept->game.data(), LABELS_BYTES) == 0)
      std::memcpy(copy, kept->encoded.data(), LABELS_BYTES);
}

void ClosePage(CPUState* ctx)
{
  KeepOurs();
  for (uint8_t* copy : g_page.textures)
    std::memcpy(copy, g_page.kept->encoded.data(), LABELS_BYTES);
  if (g_page.key)
    aurora::texture::remove_memory_replacement(*g_page.key);
  g_page = {};
  Stretch(ctx, 1.0f);
}
}  // namespace

namespace dualize::nights
{
void PatchPauseLabels(ios::Disc& disc, const std::filesystem::path& game)
{
  for (const LabelsFile& labels : LABELS_FILES)
  {
    const std::filesystem::path path = game / labels.file;
    std::vector<uint8_t> bytes = ReadFile(path);
    auto image = DecodeTplImage(bytes, LABELS_IMAGE);
    if (!image || image->format != RGB5A3 || image->image.width != LABELS_WIDTH ||
        image->image.height != LABELS_HEIGHT)
      continue;
    const auto shade = Shades(image->image);
    const float centre = Centre(image->image);
    const Image relabelled = Relabel(image->image, 1, labels, shade, centre);
    uint8_t* texels = bytes.data() + image->offset;
    std::vector<uint8_t> game(texels, texels + LABELS_BYTES);
    EncodeRgb5a3(relabelled, 0, LABELS_HEIGHT, texels);
    const auto key = aurora::texture::source_key(RGB5A3, LABELS_WIDTH, LABELS_HEIGHT, texels);
    std::vector<uint8_t> encoded(texels, texels + LABELS_BYTES);
    if (disc.OverrideFile(path, std::move(bytes)))
      g_kept.push_back(
          {labels.language, key, std::move(image->image), &labels, shade, centre, std::move(encoded), std::move(game)});
  }
}

bool ProvidePauseLabels(const aurora::texture::TextureSource& source)
{
  if (source.format != RGB5A3)
    return false;
  for (Kept& kept : g_kept)
  {
    if (kept.provided || !(kept.key == source.key))
      continue;
    kept.provided = true;
    const Image sharp = Relabel(kept.original, SHARP_SCALE, *kept.labels, kept.shade, kept.centre);
    uint32_t mips = 0;
    std::vector<uint8_t> texels = MipChain(sharp, mips);
    aurora::texture::set_memory_replacement(kept.key, sharp.width, sharp.height, mips, std::move(texels));
    kept.original = {};
    return true;
  }
  return false;
}
}  // namespace dualize::nights

// A cast's pattern turned into its quad's texture place (r3 the cast, r4 its
// info, r5 the scene).
extern "C" bool nights_CsdPatternQuad(CPUState* ctx)
{
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const uint32_t cast = ctx->gpr[3], info = ctx->gpr[4], scene = ctx->gpr[5];
  if (Load(memory, info + INFO_SPRITE) != 1 || !Relabelled(memory))
    return false;
  const uint32_t subimages = Load(memory, scene + SCENE_SUBIMAGES), patterns = Load(memory, info + INFO_PATTERNS),
                 indices = Load(memory, info + INFO_INDICES);
  for (uint32_t i = 0; i < patterns; ++i)
  {
    const int32_t index = static_cast<int32_t>(Load(memory, indices + i * 4));
    if (index < 0)
      continue;
    Rebanded(memory, subimages, static_cast<uint32_t>(index));
    // The pause window's Options label shows its band, put back for the
    // others sharing its sub-image (Retry's or Quit's); each quad keeps the
    // place it takes here.
    const uint32_t subimage = subimages + static_cast<uint32_t>(index) * SUBIMAGE;
    const bool options = cast == g_label && Load(memory, cast + CAST_GROUP) == g_label_group;
    if (options == IsBand(memory, subimage, OPTIONS_BAND, BAND))
      continue;
    if (const int band = NaturalBand(memory, subimage); band >= 0)
      SetBand(memory, subimage, options ? OPTIONS_BAND : static_cast<uint32_t>(band));
  }
  return false;
}

// The large window's buttons by their places in the game's names table
// (0-3 Continue, Restart, Quit, Owl; the small window's from 10, its Quit
// 11): its Options the small window's Quit. The small window has the first three of
// the game's.
constexpr std::array<uint32_t, 5> LARGE_NAMES{0, 1, 11, 2, 3};
constexpr uint32_t SMALL_BUTTONS = 3;

// The pad's buttons added: the window's.
extern "C" bool nights_PausePadCount(CPUState* ctx)
{
  if (g_window == Window::Game)
    return false;
  const uint32_t buttons = g_window == Window::Large ? LARGE_NAMES.size() : SMALL_BUTTONS;
  ctx->pc = ctx->gpr[30] < buttons ? NIGHTS_PAUSE_PAD_LOOP : NIGHTS_PAUSE_PAD_DONE;
  return true;
}

// A large window's button found by its name (r0 the name's place in the table).
bool LargeName(CPUState* ctx, uint32_t button)
{
  if (g_window == Window::Large && button < LARGE_NAMES.size())
    ctx->gpr[0] = LARGE_NAMES[button] * 4;
  return false;
}

extern "C" bool nights_PausePadName(CPUState* ctx)
{
  return LargeName(ctx, ctx->gpr[30]);
}

extern "C" bool nights_PauseHighlightName(CPUState* ctx)
{
  return LargeName(ctx, ctx->gpr[28]);
}

// The buttons lit or not: the large window's.
extern "C" bool nights_PauseHighlightCount(CPUState* ctx)
{
  if (g_window != Window::Large)
    return false;
  ctx->pc = ctx->gpr[28] < LARGE_NAMES.size() ? NIGHTS_PAUSE_HIGHLIGHT_LOOP : NIGHTS_PAUSE_HIGHLIGHT_DONE;
  return true;
}

// A on a button: Options opens its page; the others take the game's action,
// their place past Options moved back one (the game's table hasn't it).
extern "C" bool nights_PauseDecide(CPUState* ctx)
{
  if (g_window == Window::Game)
    return false;
  const Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const uint32_t controller = ctx->gpr[31], cursor = Load(memory, controller + PAUSE_CURSOR);
  const uint32_t options = OptionsButton();
  if (cursor == options)
  {
    // B decides too in the game's pause (its application buttons make A and B
    // one): only A opens the page, so the B that closed it doesn't open it again.
    if (Load(memory, PadState(CpuOf(ctx), memory) + PAD_DEVICE_PRESSED) & DEVICE_A)
      OpenPage(ctx, controller);
    ctx->gpr[3] = 0;
    ctx->pc = NIGHTS_PAUSE_INPUT_RETURN;
    return true;
  }
  const uint32_t action = Load(memory, Load(memory, controller + PAUSE_ACTIONS) + (cursor - (cursor > options)) * 8);
  dualize::Store32(memory.Effective(controller + PAUSE_ACTION, 4), action);
  ctx->gpr[3] = 1;
  ctx->pc = NIGHTS_PAUSE_INPUT_RETURN;
  return true;
}

// The pause window's input (r31 the controller) while the Options page is up:
// the page's, the game's skipped.
extern "C" bool nights_PausePage(CPUState* ctx)
{
  if (!g_page.open && g_held == 0)
    return false;
  auto& cpu = CpuOf(ctx);
  Machine& machine = GetMachine(cpu);
  const Memory& memory = machine.memory;
  const uint32_t controller = ctx->gpr[31];
  const uint32_t pad = PadState(cpu, memory);
  if (!g_page.open)
  {
    if (!(Load(memory, pad + PAD_DEVICE_PRESSED) & g_held))
    {
      g_held = 0;
      return false;
    }
    ctx->gpr[3] = 0;
    ctx->pc = NIGHTS_PAUSE_INPUT_NONE;
    return true;
  }
  if (!KeepOurs())
  {
    // Memory replaced under the page: nothing of it left to put back.
    if (g_page.key)
      aurora::texture::remove_memory_replacement(*g_page.key);
    g_page = {};
    return false;
  }
  ctx->gpr[3] = 0;
  ctx->pc = NIGHTS_PAUSE_INPUT_NONE;
  const auto repeated = [&](uint32_t mask) { return cpu.CallGuest(NIGHTS_PAD_REPEATED, {pad, 0, 0, mask}) != 0; };
  const auto sound = [&](uint32_t id) { cpu.CallGuest(NIGHTS_PAUSE_SOUND, {id, 1}); };
  const uint32_t now = Load(memory, pad + PAD_DEVICE_PRESSED), pressed = now & ~g_page.pressed;
  g_page.pressed = now;
  if (pressed & DEVICE_B)
  {
    ClosePage(ctx);
    g_held = DEVICE_B;
    SetCursor(memory, controller, OptionsButton());
    sound(SOUND_CANCEL);
    return true;
  }
  const int option = g_page.top + g_page.row;
  if (const int move = repeated(PAD_UP) ? -1 : repeated(PAD_DOWN) ? 1 : 0; move != 0)
  {
    // Past the rows' ends it scrolls; past the options' it goes round.
    const int to = (option + move + PAGE_OPTIONS) % PAGE_OPTIONS;
    const int rows = static_cast<int>(Rows().size());
    g_page.top = std::clamp(std::clamp(g_page.top, to - (rows - 1), to), 0, PAGE_OPTIONS - rows);
    g_page.row = to - g_page.top;
    SetCursor(memory, controller, static_cast<uint32_t>(g_page.row));
    sound(SOUND_CURSOR);
  }
  else if (const int change = repeated(PAD_LEFT) ? -1 : repeated(PAD_RIGHT) ? 1 : (pressed & DEVICE_A) ? 2 : 0;
           change != 0)
  {
    const PageRow before = RowOf(machine, option);
    Change(machine, option, change > 0 ? 1 : -1, change == 2);
    if (!(RowOf(machine, option) == before))
      sound(change == 2 ? SOUND_DECIDE : SOUND_CURSOR);
  }
  // Drawn again when a row changed, here or elsewhere (the window's size).
  DrawPage(machine);
  return true;
}

extern "C" bool nights_PauseVariant(CPUState* ctx)
{
  Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const uint32_t task = ctx->gpr[29];
  // Without the Options label (a language not relabelled) the game's own window.
  const uint32_t variant = Load(memory, task + PAUSE_VARIANT);
  g_window = !Relabelled(memory) ? Window::Game
             : variant == 1      ? Window::Small
             : variant == 0      ? Window::Large
                                 : Window::Game;
  if (g_window == Window::Small)
    dualize::Store32(memory.Effective(task + PAUSE_VARIANT, 4), 0);
  return false;
}

// After the pause scene's update: the small window's root (holding the
// borrowed Options button) takes the large root's animated scale, so the
// button opens and closes with the window (a frame late: the scene's next
// update applies it).
extern "C" bool nights_PauseFollow(CPUState* ctx)
{
  const uint32_t task = ctx->gpr[31];
  if (g_window != Window::Large || task != g_task || g_large_root == 0)
    return false;
  auto& cpu = CpuOf(ctx);
  // A savestate loaded since the scene played: its casts may be elsewhere.
  if (GetMachine(cpu).StateLoads() != g_state_loads)
    return false;
  const Memory& memory = GetMachine(cpu).memory;
  if (memory.Effective(task + PAUSE_UPDATED, 1)[0] == 0)
    return false;
  Layout layout(cpu, ctx, task + PAUSE_SCENE);
  layout.SetScaleX(g_small_root, LoadFloat(memory, g_large_root + CAST_SCALE_X));
  layout.SetScaleY(g_small_root, LoadFloat(memory, g_large_root + CAST_SCALE_Y));
  return false;
}

extern "C" bool nights_PauseScenePlay(CPUState* ctx)
{
  auto& cpu = CpuOf(ctx);
  const Memory& memory = GetMachine(cpu).memory;
  const uint32_t task = ctx->gpr[29];
  g_task = task;
  g_held = 0;
  if (g_page.open)
    ClosePage(ctx);
  // Only a savestate brings the game's texture back (~50 ms to look).
  if (const uint32_t loads = GetMachine(cpu).StateLoads(); loads != g_state_loads)
  {
    g_state_loads = loads;
    RelabelLoaded(memory);
  }
  g_label = g_label_group = 0;
  g_large_root = g_small_root = 0;
  if (g_window == Window::Game)
    return false;
  Layout layout(cpu, ctx, task + PAUSE_SCENE);
  const std::array<uint32_t, 2> roots{layout.Cast("Root_Pause_L"), layout.Cast("Root_Pause_S")};
  if (roots[0] == 0 || roots[1] == 0)
  {
    g_window = Window::Game;
    return false;
  }
  const auto casts_of = [&](uint32_t root) { return Load(memory, Load(memory, root + CAST_GROUP) + 4); };
  const uint32_t root = roots[0], count = Load(memory, Load(memory, root + CAST_GROUP)), casts = casts_of(root);
  auto at = [&](uint32_t i) { return casts + i * CAST_SIZE; };
  // The frame's bottom `lift` pixels lower (its sides stretched as much), the
  // window kept centred; the root's place returned.
  const auto resize = [&](float lift)
  {
    const float centre = lift / 2, k = (320.0f + lift) / 320.0f;
    for (uint32_t i : {0xeu, 0xfu, 0x10u, 0x20u})
      layout.SetY(at(i), (160.0f + lift) / 480.0f);
    for (uint32_t i : {0x1au, 0x1bu, 0x1du})
      layout.SetY(at(i), (152.0f + lift) / 480.0f);
    for (uint32_t i : {2u, 0x16u, 0x17u, 0x1eu, 0x1fu})
      layout.SetY(at(i), centre / 480.0f);
    layout.SetScaleY(at(2), 23.4f * k);
    layout.SetScaleX(at(0x16), 19.0f * k);
    layout.SetScaleX(at(0x17), 19.0f * k);
    layout.SetScaleY(at(0x1e), 9.0f * k);
    layout.SetScaleY(at(0x1f), 9.0f * k);
    const float y = (240.0f - centre) / 480.0f;
    layout.SetY(root, y);
    return y;
  };
  if (g_window == Window::Small)
  {
    auto mission = [](uint32_t i) { return (i >= 4 && i <= 9) || (i >= 0x24 && i <= 0x2c && i != 0x27) || i >= 0x3f; };
    for (uint32_t i = 0; i < count; ++i)
      layout.SetHidden(at(i), mission(i));
    resize(-81.0f);
  }
  else
  {
    // A button lower: Quit and the mission below it (their places in the
    // game's layout, in pixels from the window's centre).
    constexpr float BUTTON = 57.0f;
    const float y = resize(BUTTON);
    for (const auto& [i, from] : {std::pair{0x39u, 33.0f}, std::pair{4u, 82.0f}, std::pair{0x24u, 116.0f},
                                  std::pair{0x25u, 100.0f}, std::pair{0x26u, 109.0f}, std::pair{0x28u, 72.0f},
                                  std::pair{0x29u, 0.0f}, std::pair{0x3fu, 116.0f}})
      layout.SetY(at(i), (from + BUTTON) / 480.0f);
    // The small window's Quit button alone of its group, as Options where
    // Quit was.
    const uint32_t small = roots[1], small_count = Load(memory, Load(memory, small + CAST_GROUP)),
                   small_casts = casts_of(small);
    // Its root shown too: the large window's motion hides it.
    for (uint32_t i = 0; i < small_count; ++i)
      layout.SetHidden(small_casts + i * CAST_SIZE, i != 0 && (i < 0x25 || i > 0x2a));
    layout.SetY(small, y);
    g_large_root = root;
    g_small_root = small;
    layout.SetY(small_casts + 0x25 * CAST_SIZE, 33.0f / 480.0f);
  }
  // The labels' patterns read again on the next update, which takes their bands.
  for (const RowLabel& row : Rows())
    StoreFloat(memory, casts_of(roots[row.group]) + row.cast * CAST_SIZE + CAST_PATTERN, UNSET_PATTERN);
  const RowLabel& options = Rows()[OptionsButton()];
  g_label_group = Load(memory, roots[options.group] + CAST_GROUP);
  g_label = casts_of(roots[options.group]) + options.cast * CAST_SIZE;
  return false;
}
