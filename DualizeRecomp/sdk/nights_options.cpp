// NiGHTS' Options screen (the title module's ModeOption): its "Check Aspect
// Ratio" row opens a page of graphics settings (nights_graphics.cpp). The
// window keeps the console's aspect itself, so the test screen behind that
// row has nothing left to check.
//
// Found by their code in config/nights.sigs; ModeOption's update and where it
// leaves come from the defines there (ppc_natives.h).
//
// The row's label is no message but a line of a texture with all the
// list's labels; the disc serves that texture with the line redrawn.

#include "core/cpu.h"
#include "ppc_natives.h"
#include "sdk/nights_options.hpp"
#include "sdk/nights_2p_text.hpp"
#include "sdk/nights_message_font.hpp"
#include "sdk/nights_text.hpp"

#include "cpu/cpu.hpp"
#include "kernel/ios/disc.hpp"
#include "machine.hpp"
#include "ui/game_assets.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <bit>
#include <array>
#include <cstring>
#include <span>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Memory;

using dualize::nights::CpuOf;

// One line in a language, UTF-16BE in the game's message data, swapped for
// another no longer than it. A message may go on after the line (a newline,
// then the screen's button guide): the rest moves up behind the new line and
// the room left at the end is padded with terminators. Japanese lines carry
// their readings (<rN,kana> over the N characters before).
using dualize::nights::Language;
using dualize::nights::LANGUAGE_COUNT;
struct Message
{
  Language language;
  std::u16string_view from, to;
};
constexpr Message MESSAGES[] = {
    // The aspect row's label, the row's help, the main menu's Options help
    // (offline, no Nintendo WFC) and the controller window row's help.
    {Language::ENGLISH, u"Check Aspect Ratio", u"Graphics Settings"},
    {Language::ENGLISH, u"Check the height/width ratio of your screen", u"Change the resolution and image quality"},
    {Language::ENGLISH, u"Change Nintendo Wi-Fi Connection and settings", u"Change the game's settings"},
    {Language::ENGLISH, u"Turn the control window display on/off", u"Turn the controller's rumble on/off"},
    {Language::JAPANESE, u"画面比率確認", u"画質設定"},
    {Language::JAPANESE, u"ＴＶ画面<r2,がめん>のタテ/ヨコ比<r1,ひ>を確認<r2,かくにん>できます",
     u"解像度<r3,かいぞうど>と画質<r2,がしつ>を変更<r2,へんこう>できます"},
    {Language::JAPANESE, u"ネットワーク機能<r2,きのう>や、ゲームの設定<r2,せってい>ができます",
     u"ゲームの設定<r2,せってい>ができます"},
    {Language::JAPANESE, u"操作<r2,そうさ>ウィンドウの表示<r2,ひょうじ>を変更<r2,へんこう>できます",
     u"振動<r2,しんどう>のオン/オフを変更<r2,へんこう>できます"},
    {Language::FRENCH, u"Vérifier le format de l'image", u"Paramètres graphiques"},
    {Language::FRENCH, u"Vérifier la largeur et la hauteur de l'écran", u"Modifier la résolution et la qualité d'image"},
    {Language::FRENCH, u"Modifier paramètres de la CWF Nintendo et du jeu", u"Modifier les paramètres du jeu"},
    {Language::FRENCH, u"Afficher/masquer la fenêtre des commandes", u"Activer/désactiver les vibrations"},
    {Language::SPANISH, u"Comprobar proporción de imagen", u"Configuración gráfica"},
    {Language::SPANISH, u"Comprobar ancho/largo de pantalla", u"Cambiar resolución y calidad"},
    {Language::SPANISH, u"Cambiar opciones de la CWF de Nintendo y del juego", u"Cambiar las opciones del juego"},
    {Language::SPANISH, u"Activar/Desactivar ventana de controles", u"Activar/Desactivar la vibración"},
    {Language::GERMAN, u"Seitenverhältnis überprüfen", u"Grafikeinstellungen"},
    {Language::GERMAN, u"Höhen-/Breitenverhältnis des Bildschirms überprüfen", u"Auflösung und Bildqualität ändern"},
    {Language::GERMAN, u"Nintendo WFC- und Spieleinstellungen ändern", u"Spieleinstellungen ändern"},
    {Language::GERMAN, u"Controller-Fenster ein-/ausschalten", u"Vibration ein-/ausschalten"},
    {Language::ITALIAN, u"Controlla proporzioni immagini", u"Impostazioni grafiche"},
    // Italian's first copy of these two ends in a space, the others don't.
    {Language::ITALIAN, u"Regola rapporto larghezza/altezza del tuo schermo ", u"Cambia risoluzione e qualità dell'immagine"},
    {Language::ITALIAN, u"Regola rapporto larghezza/altezza del tuo schermo", u"Cambia risoluzione e qualità dell'immagine"},
    {Language::ITALIAN, u"Cambia impostazioni di gioco e della Nintendo WFC ", u"Cambia le impostazioni di gioco"},
    {Language::ITALIAN, u"Cambia impostazioni di gioco e della Nintendo WFC", u"Cambia le impostazioni di gioco"},
    {Language::ITALIAN, u"Attiva/disattiva la finestra dei comandi", u"Attiva/disattiva la vibrazione"},
};
static_assert(std::ranges::all_of(MESSAGES, [](const Message& m) { return m.to.size() <= m.from.size(); }));

using dualize::nights::BigEndian;
}  // namespace

namespace dualize::nights
{
// Messages are 2-byte aligned; the line ends at a terminator or a newline.
std::vector<uint8_t*> FindMessageLines(const Memory& memory, std::u16string_view line)
{
  std::vector<uint8_t*> found;
  uint8_t* const data = memory.Effective(0x90000000u, Memory::MEM2_SIZE);
  if (data == nullptr || line.empty())
    return found;
  uint8_t* const end = data + Memory::MEM2_SIZE;
  const std::u16string text = BigEndian(line);
  const auto* needle = reinterpret_cast<const uint8_t*>(text.data());
  const size_t bytes = text.size() * 2;
  uint8_t* at = data;
  while (static_cast<size_t>(end - at) >= bytes + 2)
  {
    // Keyed on the first character's low byte: the high byte of big-endian
    // UTF-16 text is 0, as is most of MEM2, so a search for it stops everywhere.
    uint8_t* low = static_cast<uint8_t*>(std::memchr(at + 1, needle[1], static_cast<size_t>(end - at) - bytes - 2));
    if (low == nullptr)
      break;
    at = low - 1;
    const bool line_end = at[bytes] == 0 && (at[bytes + 1] == 0 || at[bytes + 1] == 0x0A);
    if ((reinterpret_cast<uintptr_t>(at) & 1) == 0 && line_end && std::memcmp(at, needle, bytes) == 0)
    {
      found.push_back(at);
      at += bytes;
    }
    else
      ++at;
  }
  return found;
}
}  // namespace dualize::nights

namespace
{
// Replaces every copy of each message of the game's language in MEM2, where
// the game keeps its message data.
void RenameMessages(const Memory& memory)
{
  uint8_t* const end = memory.Effective(0x90000000u, Memory::MEM2_SIZE) + Memory::MEM2_SIZE;
  const Language language = dualize::nights::GameLanguage(memory);
  for (const Message& message : MESSAGES)
  {
    if (message.language != language)
      continue;
    const std::u16string to = BigEndian(message.to);
    const size_t bytes = message.from.size() * 2;
    for (uint8_t* at : dualize::nights::FindMessageLines(memory, message.from))
    {
      uint8_t* tail = at + bytes;
      uint8_t* stop = tail;
      while (stop + 1 < end && (stop[0] | stop[1]) != 0)
        stop += 2;
      const size_t tail_bytes = static_cast<size_t>(stop - tail);
      const size_t to_bytes = to.size() * 2;
      std::memcpy(at, to.data(), to_bytes);
      std::memmove(at + to_bytes, tail, tail_bytes);
      std::memset(at + to_bytes + tail_bytes, 0, bytes - to_bytes);
    }
  }
}

// The game's message files (compressed archives), counted as they are read.
constexpr const char* MESSAGE_FOLDER = "files/message";
std::atomic<uint32_t> g_message_reads{0};

// The Options list's labels: image 6 of its textures, a 256x256 CMPR of grey
// text on black, a 24-texel line per label. The aspect row's line (the
// second) is redrawn as "Graphics Settings" from the other lines' letters,
// the G from a C.
constexpr const char* OPTION_TEXTURES = "files/csd/textures/option_allus.tls";
// The other languages' (the pages draw over each, the disc's left as it is).
constexpr std::array<std::pair<Language, const char*>, 5> OTHER_OPTION_TEXTURES{{
    {Language::JAPANESE, "files/csd/textures/option_all.tls"},
    {Language::FRENCH, "files/csd/textures/option_allfr.tls"},
    {Language::SPANISH, "files/csd/textures/option_allsp.tls"},
    {Language::GERMAN, "files/csd/textures/option_allga.tls"},
    {Language::ITALIAN, "files/csd/textures/option_allit.tls"},
}};

constexpr uint32_t LABELS_IMAGE = 6, LINE = 24, ASPECT_LINE = 1;
constexpr size_t LABELS_BYTES = 256 * 256 / 2;  // CMPR, 4 bits a texel

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
// controller's rumble on and off: "Rumble". The Weather Link row's (the
// fourth) loses its "Enable " as well.
constexpr uint32_t RUMBLE_LINE = 2, WEATHER_LINE = 3;
constexpr Cut CUT_WEATHER_LINK{WEATHER_LINE, 73, 201};
constexpr std::array<Cut, 6> CUT_RUMBLE{{
    {4, 2, 12},     // R, of Register
    {0, 54, 63},    // u, of Language
    {7, 136, 152},  // m, of Chime
    {3, 39, 50},    // b, of Enable
    {3, 53, 55},    // l, of Enable
    {3, 57, 68},    // e, of Enable
}};

// The 2P Race window's description: image 13 of each language's 2P select
// textures, a 512x128 CMPR of grey text on black. Its last two lines (rows
// 46-84) invite play over Nintendo Wi-Fi Connection; the rows from 40 to 88
// hold only them. In German and Spanish that sentence begins on the second
// line (rows 24-40), from "Du kannst" and "Juega".
struct RaceText
{
  const char* file;
  uint32_t second_line_x;  // where the sentence begins on the second line, else the width
};
constexpr std::array<RaceText, 6> RACE_TEXTS{{
    {"files/csd/textures/mode2Pselect.tls", 512},
    {"files/csd/textures/mode2Pselectus.tls", 512},
    {"files/csd/textures/mode2Pselectfr.tls", 512},
    {"files/csd/textures/mode2Pselectga.tls", 95},
    {"files/csd/textures/mode2Pselectit.tls", 512},
    {"files/csd/textures/mode2Pselectsp.tls", 186},
}};
constexpr uint32_t RACE_TEXT_IMAGE = 13, RACE_LINE2_Y0 = 24, RACE_WIFI_Y0 = 40, RACE_WIFI_Y1 = 88;

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

  // Reads only lines the aspect redraw leaves alone (0, 3, 4 and 7), so
  // before the Weather Link redraw.
  void RedrawRumble()
  {
    Line line;
    int width = 0;
    for (const Cut& cut : CUT_RUMBLE)
    {
      line.Put(Take(cut, &width), width);
      line.pen += 2;
    }
    Store(RUMBLE_LINE, line);
  }

  void RedrawWeatherLink()
  {
    Line line;
    line.pen = 3;  // where "Enable" began
    PutWide(line, CUT_WEATHER_LINK);
    Store(WEATHER_LINE, line);
  }
};

// The Options title: image 0 of the same textures, RGB5A3 256x128, three
// 40-texel rows (Options, Sound Library, Concept Art; each language's in its
// own, the Japanese the English), each a calligraphic word over the same
// underline (from texel row 27 down). On the graphics page the first reads
// "Graphics Settings" or the language's own, made of the titles' letters:
// the Options rows', 2P Game Select's and Nightopia Select's (the 2P select
// textures' image 1), Check Aspect Ratio's (image 8), and the Italian and
// Spanish Options rows', pressed narrower to fit. The other languages' Check
// Aspect Ratio rows are lettered narrower, so none of their letters is taken.
constexpr uint32_t TITLE_IMAGE = 0, CHECK_TITLE_IMAGE = 8, TITLE_ROW = 40, TITLE_UNDERLINE = 27;
constexpr const char* SELECT_2P_TEXTURES = "files/csd/textures/2p_piaselectus.tls";
constexpr uint32_t SELECT_2P_TITLE_IMAGE = 1;
constexpr size_t TITLE_BYTES = 256 * TITLE_ROW * 2;  // RGB5A3, 2 bytes a texel
constexpr float TITLE_ROOM = 240, TITLE_SPACE = 14;
constexpr int TITLE_KERN = 1;  // texels each letter overlaps the one before

enum TitleSource : uint8_t
{
  OPTIONS_TITLES,
  SELECT_2P_TITLES,
  CHECK_TITLE,
  ITALIAN_TITLES,
  SPANISH_TITLES,
  TITLE_SOURCES
};
constexpr std::array<uint32_t, TITLE_SOURCES> TITLE_SOURCE_ROWS{3, 2, 1, 3, 3};

// A letter (or a word) cut from a source row, by the name the titles use.
struct TitleCut
{
  std::string_view name;
  TitleSource source;
  uint32_t row;
  uint32_t x0, x1;
};
constexpr std::array<TitleCut, 22> TITLE_CUTS{{
    {"G", SELECT_2P_TITLES, 0, 68, 98},    // of Game
    {"r", OPTIONS_TITLES, 1, 189, 200},    // of Library
    {"a", SELECT_2P_TITLES, 0, 99, 112},   // of Game
    {"p", OPTIONS_TITLES, 0, 96, 113},     // of Options
    {"h", CHECK_TITLE, 0, 36, 48},         // of Check
    {"i", OPTIONS_TITLES, 0, 128, 136},    // of Options
    {"c", CHECK_TITLE, 0, 57, 67},         // of Check
    {"s", OPTIONS_TITLES, 0, 171, 196},    // of Options
    {"S", SELECT_2P_TITLES, 0, 160, 187},  // of Select
    {"e", SELECT_2P_TITLES, 0, 188, 199},  // of Select
    {"t", SELECT_2P_TITLES, 0, 232, 246},  // of Select
    {"n", OPTIONS_TITLES, 0, 157, 171},    // of Options
    {"g", SELECT_2P_TITLES, 1, 49, 64},    // of Nightopia
    {"m", SELECT_2P_TITLES, 0, 113, 137},  // of Game
    {"A", OPTIONS_TITLES, 2, 160, 198},    // of Art
    {"z", ITALIAN_TITLES, 0, 117, 133},    // of Opzioni
    // Opzioni's n and last i: Options' n is cut short by its s, and its i
    // keeps bits of the t before.
    {"n2", ITALIAN_TITLES, 0, 163, 183},
    {"i2", ITALIAN_TITLES, 0, 183, 195},
    {"Opzioni", ITALIAN_TITLES, 0, 62, 195},
    {"Opciones", SPANISH_TITLES, 0, 53, 204},
    {"de", SPANISH_TITLES, 2, 117, 150},  // of Libreta de dibujos
}};
// Each language's graphics page title, its cuts by name ('|' between, ' '
// for a space).
constexpr std::array<std::string_view, LANGUAGE_COUNT> GRAPHICS_TITLES{
    "G|r|a|p|h|i|c|s| |S|e|t|t|i|n|g|s",  // Japanese: the English titles
    "G|r|a|p|h|i|c|s| |S|e|t|t|i|n|g|s",
    "G|r|a|p|h|i|s|m|e|s",
    "Opciones| |de| |i2|m|a|g|e|n2",
    "A|n2|z|e|i2|g|e",
    "Opzioni| |i2|m|m|a|g|i2|n2|e",
};

using Texel = std::array<float, 4>;  // premultiplied RGBA, 0-255

struct Title
{
  std::array<const dualize::Image*, TITLE_SOURCES> sources{};
  // Per source, its underline alone: per texel, the row's where it is least
  // covered (the words' strokes cross it in different places).
  std::array<std::vector<Texel>, TITLE_SOURCES> underlines;

  static Texel Premultiplied(const dualize::Image& image, uint32_t x, uint32_t y)
  {
    const uint8_t* p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
    const float a = p[3] / 255.0f;
    return {p[0] * a, p[1] * a, p[2] * a, static_cast<float>(p[3])};
  }

  void FindUnderlines()
  {
    for (uint32_t s = 0; s < TITLE_SOURCES; ++s)
    {
      const dualize::Image& image = *sources[s];
      std::vector<Texel>& line = underlines[s];
      line.assign(static_cast<size_t>(image.width) * TITLE_ROW, Texel{});
      for (uint32_t y = 0; y < TITLE_ROW; ++y)
      {
        for (uint32_t x = 0; x < image.width; ++x)
        {
          Texel least = Premultiplied(image, x, y);
          for (uint32_t row = 1; row < TITLE_SOURCE_ROWS[s]; ++row)
          {
            const Texel t = Premultiplied(image, x, row * TITLE_ROW + y);
            if (t[3] < least[3])
              least = t;
          }
          // Above it only letters are left where every row has some.
          line[static_cast<size_t>(y) * image.width + x] = y >= TITLE_UNDERLINE ? least : Texel{};
        }
      }
    }
  }

  // The letter's texels; below the underline's top only where they cover more
  // than the underline does (the letter's tail).
  void Put(std::vector<Texel>& canvas, uint32_t width, uint32_t pen, const TitleCut& cut) const
  {
    const dualize::Image& image = *sources[cut.source];
    for (uint32_t y = 0; y < TITLE_ROW; ++y)
    {
      for (uint32_t x = cut.x0; x < cut.x1 && pen + x - cut.x0 < width; ++x)
      {
        const Texel t = Premultiplied(image, x, cut.row * TITLE_ROW + y);
        const Texel& line = underlines[cut.source][static_cast<size_t>(y) * image.width + x];
        if (y >= TITLE_UNDERLINE && t[3] <= line[3] + 64)
          continue;
        Texel& c = canvas[static_cast<size_t>(y) * width + pen + x - cut.x0];
        const float keep = 1 - t[3] / 255;
        for (int k = 0; k < 4; ++k)
          c[k] = t[k] + c[k] * keep;
      }
    }
  }

  // Rows [0, TITLE_ROW) of `out` (Options' row): its underline with the
  // title (cut names, '|' between) over it, centred.
  bool Compose(std::string_view title, dualize::Image& out) const
  {
    constexpr uint32_t WIDE = 512;
    std::vector<Texel> canvas(static_cast<size_t>(WIDE) * TITLE_ROW, Texel{});
    uint32_t pen = 0;
    for (const auto part : std::views::split(title, '|'))
    {
      const std::string_view name(part.begin(), part.end());
      if (name == " ")
      {
        pen += static_cast<uint32_t>(TITLE_SPACE);
        continue;
      }
      const auto cut = std::ranges::find(TITLE_CUTS, name, &TitleCut::name);
      if (cut == TITLE_CUTS.end() || cut->x1 > sources[cut->source]->width)
        return false;
      if (pen + cut->x1 - cut->x0 > WIDE)
        return false;
      Put(canvas, WIDE, pen, *cut);
      pen += cut->x1 - cut->x0 - TITLE_KERN;
    }
    const float natural = static_cast<float>(pen + TITLE_KERN), fitted = std::min(TITLE_ROOM, natural);
    const float step = natural / fitted, left = std::floor((static_cast<float>(out.width) - fitted) / 2);
    for (uint32_t y = 0; y < TITLE_ROW; ++y)
    {
      for (uint32_t x = 0; x < out.width; ++x)
      {
        // The word pressed narrower: each texel the canvas span it covers.
        Texel word{};
        const float from = (static_cast<float>(x) - left) * step, to = from + step;
        if (from >= 0 && to <= natural)
        {
          for (auto cx = static_cast<uint32_t>(from); static_cast<float>(cx) < to; ++cx)
          {
            const float cover = std::min(to, cx + 1.0f) - std::max(from, static_cast<float>(cx));
            for (int k = 0; k < 4; ++k)
              word[k] += canvas[static_cast<size_t>(y) * WIDE + cx][k] * cover / step;
          }
        }
        const Texel& line = underlines[OPTIONS_TITLES][static_cast<size_t>(y) * out.width + x];
        Texel t;
        for (int k = 0; k < 4; ++k)
          t[k] = word[k] + line[k] * (1 - word[3] / 255);
        uint8_t* p = &out.rgba[(static_cast<size_t>(y) * out.width + x) * 4];
        const float a = t[3] / 255;
        for (int k = 0; k < 3; ++k)
          p[k] = static_cast<uint8_t>(a > 0 ? std::clamp(t[k] / a + 0.5f, 0.0f, 255.0f) : 0);
        p[3] = static_cast<uint8_t>(std::clamp(t[3] + 0.5f, 0.0f, 255.0f));
      }
    }
    return true;
  }
};
// Each language's graphics page title (its Options row redrawn) for the
// pages; `english` and `others` the languages' Options textures.
void KeepTitles(const std::filesystem::path& game, std::span<const uint8_t> english,
                const std::array<std::vector<uint8_t>, LANGUAGE_COUNT>& others)
{
  const auto titles_of = [&](Language language) {
    const auto& bytes = language == Language::ENGLISH ? english : std::span<const uint8_t>(others[static_cast<size_t>(language)]);
    auto titles = dualize::DecodeTplImage(bytes, TITLE_IMAGE);
    if (titles && (titles->format != 5 || titles->image.width != 256 ||
                   titles->image.height < TITLE_SOURCE_ROWS[OPTIONS_TITLES] * TITLE_ROW ||
                   titles->offset + TITLE_BYTES > bytes.size()))
      titles.reset();
    return titles;
  };
  auto english_titles = titles_of(Language::ENGLISH), italian = titles_of(Language::ITALIAN),
       spanish = titles_of(Language::SPANISH);
  auto check = dualize::DecodeTplImage(english, CHECK_TITLE_IMAGE);
  auto select = dualize::DecodeTplImage(ReadFile(game / SELECT_2P_TEXTURES), SELECT_2P_TITLE_IMAGE);
  if (!english_titles || !italian || !spanish || !check || !select ||
      select->image.height < TITLE_SOURCE_ROWS[SELECT_2P_TITLES] * TITLE_ROW || check->image.height < TITLE_ROW)
    return;
  Title title{{&english_titles->image, &select->image, &check->image, &italian->image, &spanish->image}};
  title.FindUnderlines();
  for (size_t l = 0; l < LANGUAGE_COUNT; ++l)
  {
    const auto language = static_cast<Language>(l);
    const auto titles = language == Language::ENGLISH ? english_titles : titles_of(language);
    if (!titles)
      continue;
    dualize::Image composed = titles->image;
    if (!title.Compose(GRAPHICS_TITLES[l], composed))
      continue;
    const auto& bytes = language == Language::ENGLISH ? english : std::span<const uint8_t>(others[l]);
    const std::span original = bytes.subspan(titles->offset, TITLE_BYTES);
    std::vector<uint8_t> encoded(original.begin(), original.end());
    dualize::EncodeRgb5a3(composed, 0, TITLE_ROW, encoded.data());
    dualize::nights::KeepOptionTitle(language, original, encoded);
  }
}
}  // namespace

namespace dualize
{
void PatchNightsDisc(ios::Disc& disc, const std::filesystem::path& game)
{
  disc.CountReads(game / MESSAGE_FOLDER, &g_message_reads);
  nights::LoadMessageFont(game);
  {
    const std::filesystem::path path = game / OPTION_TEXTURES;
    std::vector<uint8_t> bytes = ReadFile(path);
    auto labels = DecodeTplImage(bytes, LABELS_IMAGE);
    const bool relabel = labels && labels->format == 14 && labels->image.width == 256 && labels->image.height >= 8 * LINE;
    if (relabel)
    {
      Labels{labels->image}.Redraw();
      Labels{labels->image}.RedrawRumble();
      Labels{labels->image}.RedrawWeatherLink();
      static_assert(RUMBLE_LINE == ASPECT_LINE + 1 && WEATHER_LINE == RUMBLE_LINE + 1);
      EncodeGreyCmpr(labels->image, ASPECT_LINE * LINE, (WEATHER_LINE + 1) * LINE, bytes.data() + labels->offset);
      nights::KeepOptionLabels(Language::ENGLISH, labels->image, std::span(bytes).subspan(labels->offset, LABELS_BYTES));
    }
    std::array<std::vector<uint8_t>, LANGUAGE_COUNT> option_textures;
    for (const auto& [language, file] : OTHER_OPTION_TEXTURES)
    {
      std::vector<uint8_t>& other = option_textures[static_cast<size_t>(language)];
      other = ReadFile(game / file);
      const auto other_labels = DecodeTplImage(other, LABELS_IMAGE);
      if (other_labels && other_labels->format == 14 && other_labels->image.width == 256 &&
          other_labels->image.height == 256)
        nights::KeepOptionLabels(language, other_labels->image,
                                 std::span(other).subspan(other_labels->offset, LABELS_BYTES));
    }
    KeepTitles(game, bytes, option_textures);
    if (relabel)
      disc.OverrideFile(path, std::move(bytes));
  }
  for (const RaceText& race : RACE_TEXTS)
  {
    const std::filesystem::path path = game / race.file;
    std::vector<uint8_t> bytes = ReadFile(path);
    auto text = DecodeTplImage(bytes, RACE_TEXT_IMAGE);
    if (!text || text->format != 14 || text->image.width != 512 || text->image.height != 128)
      continue;
    const uint32_t width = text->image.width;
    const uint32_t y0 = race.second_line_x < width ? RACE_LINE2_Y0 : RACE_WIFI_Y0;
    for (uint32_t y = y0; y < RACE_WIFI_Y1; ++y)
    {
      const uint32_t x0 = y < RACE_WIFI_Y0 ? race.second_line_x : 0;
      std::fill_n(&text->image.rgba[(static_cast<size_t>(y) * width + x0) * 4], (width - x0) * 4, 0);
    }
    EncodeGreyCmpr(text->image, y0, RACE_WIFI_Y1, bytes.data() + text->offset);
    nights::Keep2PText(race.file, bytes);
    disc.OverrideFile(path, std::move(bytes));
  }
  nights::Load2PText(game);
}
}  // namespace dualize

namespace
{
using dualize::Cpu;
using dualize::GetMachine;
using dualize::Machine;
}  // namespace

// ModeOption's constructor. Declines: the game's code goes on.
extern "C" bool nights_ModeOptionCtor(CPUState*)
{
  dualize::nights::ResetGraphicsPage();
  return false;
}

// A help line set to a message (every screen's, the main menu's too): the
// messages are renamed first when a message file was read since the last time
// (the screens load theirs again as they open, the originals with them), and
// the first time (a savestate's memory has them as they were). Declines.
extern "C" bool nights_HelpSet(CPUState* ctx)
{
  static uint32_t renamed_after = UINT32_MAX;
  const uint32_t reads = g_message_reads.load(std::memory_order_relaxed);
  if (reads != renamed_after)
  {
    renamed_after = reads;
    RenameMessages(GetMachine(CpuOf(ctx)).memory);
  }
  return false;
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
  dualize::nights::TakeOverOptionList(ctx, ctx->gpr[30]);
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


// The boot's logo task choosing whether to show the strap reminder (r3 its
// flag): never, it ends at once and the logos follow.
extern "C" bool nights_StrapReminder(CPUState* ctx)
{
  ctx->pc = ctx->gpr[3] != 0 ? NIGHTS_STRAP_REMINDER_END : NIGHTS_STRAP_REMINDER_NONE;
  return true;
}

// A cutscene's message command, its kind in r0 (cmpwi r0, 1 hooked): with
// subtitles off a dialogue line (kind 1) is not shown. Otherwise declines.
extern "C" bool nights_EventSubtitle(CPUState* ctx)
{
  if (ctx->gpr[0] != 1 || GetMachine(CpuOf(ctx)).Graphics().subtitles)
    return false;
  ctx->pc = NIGHTS_EVENT_MESSAGE_COMMAND_DONE;
  return true;
}
