#include "sdk/nights_2p_text.hpp"

#include "ui/game_assets.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace dualize::nights
{
namespace
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wc++26-extensions"
// Static cuts of OFL faces (scripts/lettering/font_2p.py), the closest to the
// game's lettering of those tried, judged by the texel error over its lines.
constexpr unsigned char GOTHIC_FONT[] = {
#embed "../resources/fonts/Dualize2PGothic.ttf"
};
constexpr unsigned char SERIF_FONT[] = {
#embed "../resources/fonts/Dualize2PSerif-Bold.ttf"
};
constexpr unsigned char SANS_FONT[] = {
#embed "../resources/fonts/Dualize2PSans.ttf"
};
constexpr unsigned char SANS_MEDIUM_FONT[] = {
#embed "../resources/fonts/Dualize2PSans-Medium.ttf"
};
#pragma clang diagnostic pop

// Descriptions; titles and buttons; Start, Quit, scores; Continue.
enum Style : uint8_t
{
  GOTHIC,
  SERIF,
  SANS,
  SANS_MEDIUM,
  STYLES
};
constexpr std::array<const unsigned char*, STYLES> FACES{GOTHIC_FONT, SERIF_FONT, SANS_FONT, SANS_MEDIUM_FONT};

constexpr std::array<std::string_view, 3> FILES{"files/csd/textures/mode2Pselect", "files/csd/textures/2p_continue",
                                                "files/csd/textures/pjn_2premocon"};
constexpr std::array<std::string_view, 5> LANGUAGES{"us", "fr", "ga", "it", "sp"};
constexpr uint32_t CMPR = 14;

// A line of text: rows [y0, y1) of image `image` of FILES[file] +
// LANGUAGES[language] + ".tls", in `style` at `em` texels to the em, `stretch`
// as wide, on a baseline `base` rows below y0. Its ink makes `marks` marks
// (connected strokes, those above one another merged) for `letters` letters,
// spaces left out, whose UTF-8 has CRC-32 `crc`; `groups` lists the marks
// that are not one letter each, as "group:marks:letters,...", group counting
// the line's groups of marks from 0. Fitted per line by measure; lines whose
// letters run together past reading (two, in fr and sp) are left out and keep
// the sharpened original.
struct Line
{
  uint8_t language, file, image, y0, y1;
  Style style;
  float em, stretch;
  uint8_t base, marks, letters;
  uint32_t crc;
  const char* groups;
};
// clang-format off
constexpr Line LINES[] = {
    {0, 0, 8, 2, 14, SANS, 13.5f, 1.05f, 11, 7, 7, 0xd3bcc790, ""},
    {0, 0, 8, 18, 30, SANS, 13.5f, 1.05f, 11, 7, 7, 0xe254dd0d, ""},
    {0, 0, 9, 9, 24, SANS, 17.5f, 0.95f, 14, 5, 5, 0x5eb87a8b, ""},
    {0, 0, 9, 41, 56, SANS, 18.0f, 1.00f, 14, 4, 4, 0xe39878ab, ""},
    {0, 0, 11, 2, 19, GOTHIC, 18.0f, 0.95f, 14, 30, 30, 0x94258d61, ""},
    {0, 0, 11, 24, 41, GOTHIC, 18.0f, 0.95f, 14, 27, 27, 0x5d3952ee, ""},
    {0, 0, 11, 46, 63, GOTHIC, 18.0f, 0.95f, 14, 31, 31, 0xcb69201a, ""},
    {0, 0, 11, 68, 85, GOTHIC, 18.0f, 0.95f, 14, 25, 25, 0x15632445, ""},
    {0, 0, 11, 108, 125, SERIF, 20.0f, 1.00f, 15, 21, 23, 0x1a148533, "12:1:2,17:1:2"},
    {0, 0, 13, 2, 19, GOTHIC, 18.0f, 0.95f, 14, 26, 26, 0xcf34e0ac, ""},
    {0, 0, 13, 24, 41, GOTHIC, 18.0f, 0.90f, 14, 26, 27, 0xa2f613d8, "19:1:2"},
    {0, 0, 13, 108, 127, SERIF, 20.0f, 1.00f, 15, 21, 24, 0x54145485, "2:1:2,7:1:2,18:1:2"},
    {0, 0, 14, 4, 22, SERIF, 20.0f, 1.05f, 14, 21, 23, 0xd91f870d, "10:1:2,15:1:2"},
    {0, 1, 5, 0, 19, SANS_MEDIUM, 16.0f, 1.00f, 14, 26, 25, 0xe69253f6, "4:2:1"},
    {0, 1, 5, 20, 39, SANS_MEDIUM, 16.0f, 1.00f, 14, 26, 30, 0xad5842b1, "3:1:2,7:1:3,11:1:2"},
    {0, 1, 5, 40, 55, SANS, 17.0f, 1.05f, 14, 4, 4, 0xe39878ab, ""},
    {0, 2, 4, 7, 25, SERIF, 22.0f, 1.10f, 16, 3, 2, 0xd736d92d, "0:2:1"},
    {0, 2, 5, 7, 25, SERIF, 22.0f, 1.10f, 16, 4, 4, 0x83746bed, ""},
    {1, 0, 8, 1, 14, SANS, 14.0f, 0.95f, 12, 11, 14, 0xe15454bb, "2:1:3,4:1:2"},
    {1, 0, 8, 17, 30, SANS, 14.0f, 0.95f, 12, 11, 14, 0xca790778, "2:1:3,4:1:2"},
    {1, 0, 9, 9, 25, SANS, 17.5f, 1.00f, 14, 9, 9, 0x30ed2a5c, ""},
    {1, 0, 9, 41, 56, SANS, 18.0f, 0.95f, 14, 6, 7, 0xc627cc86, "3:1:2"},
    {1, 0, 11, 2, 18, GOTHIC, 18.0f, 0.80f, 14, 33, 34, 0xa069b2b1, "26:1:2"},
    {1, 0, 11, 24, 41, GOTHIC, 18.0f, 0.80f, 14, 37, 41, 0xd13dd5af, "3:1:2,4:1:2,10:1:3"},
    {1, 0, 11, 46, 63, GOTHIC, 18.0f, 0.80f, 14, 34, 34, 0xf1a73ac7, ""},
    {1, 0, 11, 68, 85, GOTHIC, 18.0f, 0.80f, 14, 19, 23, 0x7808f83b, "1:1:2,3:1:2,10:1:2,13:1:2"},
    {1, 0, 11, 108, 125, SERIF, 20.5f, 0.95f, 15, 25, 28, 0x360c89dd, "2:1:2,9:1:2,21:1:2"},
    {1, 0, 13, 2, 19, GOTHIC, 18.0f, 0.85f, 14, 32, 34, 0x8215518f, "25:1:2,27:1:2"},
    {1, 0, 13, 24, 41, GOTHIC, 17.5f, 0.85f, 14, 11, 13, 0x9a403120, "4:1:2,9:1:2"},
    {1, 0, 14, 4, 20, SERIF, 19.5f, 1.05f, 14, 21, 24, 0x2698a23c, "3:1:2,17:1:2,18:1:2"},
    {1, 1, 5, 0, 19, SANS_MEDIUM, 16.0f, 0.90f, 14, 27, 28, 0x41c78007, "23:1:2"},
    {1, 1, 5, 20, 40, SANS_MEDIUM, 16.0f, 0.85f, 14, 33, 33, 0xd2db2805, "16:2:1,30:1:2"},
    {1, 1, 5, 40, 56, SANS, 16.5f, 1.05f, 14, 6, 7, 0xc627cc86, "3:1:2"},
    {1, 2, 4, 7, 25, SERIF, 22.0f, 1.10f, 16, 3, 2, 0xd736d92d, "0:2:1"},
    {1, 2, 5, 7, 25, SERIF, 22.0f, 1.00f, 16, 6, 4, 0x32051eed, "0:2:1,2:2:1"},
    {2, 0, 8, 1, 16, SANS, 13.5f, 1.15f, 11, 10, 10, 0xe94480f2, ""},
    {2, 0, 8, 17, 32, SANS, 13.5f, 1.15f, 11, 10, 10, 0xc269d331, ""},
    {2, 0, 9, 9, 25, SANS, 17.5f, 1.00f, 14, 6, 7, 0x9bfe4658, "3:1:2"},
    {2, 0, 9, 40, 56, SANS, 18.0f, 0.95f, 15, 9, 9, 0x4264d895, ""},
    {2, 0, 11, 2, 19, GOTHIC, 18.0f, 0.90f, 14, 30, 30, 0xb65756fb, ""},
    {2, 0, 11, 24, 41, GOTHIC, 18.0f, 0.90f, 14, 31, 31, 0x63d65d98, ""},
    {2, 0, 11, 46, 63, GOTHIC, 18.0f, 0.85f, 14, 24, 26, 0xc4f12f27, "18:1:2,20:1:2"},
    {2, 0, 11, 68, 85, GOTHIC, 18.0f, 0.90f, 14, 33, 34, 0x718be23a, "26:1:2"},
    {2, 0, 11, 108, 127, SERIF, 20.0f, 1.00f, 15, 20, 24, 0x118a88ec, "0:1:3,11:1:2,16:1:2"},
    {2, 0, 13, 2, 17, GOTHIC, 18.0f, 0.90f, 14, 21, 23, 0x5a854949, "7:1:3"},
    {2, 0, 13, 24, 39, GOTHIC, 18.0f, 0.90f, 14, 12, 12, 0x45a6f384, ""},
    {2, 0, 13, 108, 127, SERIF, 20.0f, 1.00f, 15, 22, 25, 0xed3fa48e, "0:2:1,1:1:2,10:1:2,15:1:3"},
    {2, 0, 14, 4, 22, SERIF, 20.5f, 1.00f, 14, 20, 22, 0x82939b50, "5:1:2,16:1:2"},
    {2, 1, 5, 0, 19, SANS_MEDIUM, 15.5f, 0.95f, 14, 29, 31, 0x2a09b3b0, "23:1:3"},
    {2, 1, 5, 20, 39, SANS_MEDIUM, 15.5f, 1.00f, 14, 26, 29, 0xad058816, "1:1:2,20:1:3"},
    {2, 1, 5, 40, 56, SANS, 16.0f, 1.05f, 14, 9, 9, 0x4264d895, ""},
    {2, 2, 4, 7, 25, SERIF, 22.0f, 1.10f, 16, 3, 2, 0xd736d92d, "0:2:1"},
    {2, 2, 5, 7, 28, SERIF, 23.5f, 0.85f, 17, 8, 8, 0xbd9d9ea7, ""},
    {3, 0, 8, 2, 14, SANS, 13.5f, 1.05f, 11, 10, 11, 0x8561430c, "2:1:2"},
    {3, 0, 8, 18, 30, SANS, 13.5f, 1.05f, 11, 10, 11, 0xae4c10cf, "2:1:2"},
    {3, 0, 9, 9, 24, SANS, 18.0f, 0.95f, 14, 4, 5, 0x3313ec66, "1:1:2"},
    {3, 0, 9, 40, 57, SANS, 17.5f, 1.05f, 15, 9, 9, 0x72a9179d, ""},
    {3, 0, 11, 2, 17, GOTHIC, 18.0f, 0.90f, 14, 29, 29, 0x36611b58, ""},
    {3, 0, 11, 24, 39, GOTHIC, 18.0f, 0.90f, 14, 37, 37, 0x4afbb07b, ""},
    {3, 0, 11, 46, 63, GOTHIC, 17.5f, 0.95f, 14, 32, 32, 0xf6a40c3d, ""},
    {3, 0, 11, 68, 85, GOTHIC, 18.0f, 0.95f, 14, 25, 25, 0xf495cc17, ""},
    {3, 0, 11, 108, 125, SERIF, 20.0f, 1.00f, 15, 20, 24, 0x71199830, "0:1:2,2:1:2,12:1:2,17:1:2"},
    {3, 0, 13, 2, 19, GOTHIC, 18.0f, 0.90f, 14, 34, 34, 0x5fd2ea9b, ""},
    {3, 0, 13, 24, 41, GOTHIC, 18.0f, 0.90f, 14, 34, 34, 0xa5da4c8a, ""},
    {3, 0, 13, 106, 125, SERIF, 19.5f, 1.05f, 17, 24, 28, 0x95d11010, "5:1:2,7:1:2,9:1:3"},
    {3, 0, 14, 4, 20, SERIF, 19.5f, 1.05f, 14, 17, 22, 0x88cbc193, "0:1:2,6:1:2,7:1:2,9:1:2,12:1:2"},
    {3, 1, 5, 0, 20, SANS_MEDIUM, 15.5f, 1.00f, 14, 25, 28, 0x49122a3a, "8:1:2,13:1:2,21:1:3,22:2:1"},
    {3, 1, 5, 20, 40, SANS_MEDIUM, 15.5f, 1.00f, 14, 27, 28, 0x6568bf57, "15:1:2,23:1:2,24:2:1"},
    {3, 1, 5, 40, 56, SANS, 15.5f, 1.10f, 14, 9, 9, 0x72a9179d, ""},
    {3, 2, 4, 7, 25, SERIF, 22.0f, 1.10f, 16, 3, 2, 0xec58f9e5, "0:2:1"},
    {3, 2, 5, 7, 25, SERIF, 22.0f, 1.05f, 16, 4, 4, 0x75c1b87d, ""},
    {4, 0, 8, 2, 14, SANS, 13.5f, 0.95f, 11, 10, 12, 0xe9d6168a, "2:1:3"},
    {4, 0, 8, 18, 30, SANS, 13.5f, 0.95f, 11, 10, 12, 0xc2fb4549, "2:1:3"},
    {4, 0, 9, 9, 28, SANS, 18.0f, 1.00f, 14, 7, 7, 0x5194d9aa, ""},
    {4, 0, 9, 40, 57, SANS, 18.0f, 0.95f, 15, 5, 5, 0x71f220da, ""},
    {4, 0, 11, 2, 19, GOTHIC, 18.0f, 0.95f, 14, 25, 25, 0xc3538274, ""},
    {4, 0, 11, 24, 41, GOTHIC, 18.0f, 0.95f, 14, 29, 29, 0x0e340386, ""},
    {4, 0, 11, 46, 63, GOTHIC, 18.0f, 0.95f, 14, 26, 27, 0xe4c9647d, "17:1:2"},
    {4, 0, 11, 68, 85, GOTHIC, 18.0f, 0.95f, 14, 33, 33, 0xe88dd6fa, ""},
    {4, 0, 11, 108, 128, SERIF, 20.0f, 1.00f, 15, 18, 22, 0x9e2c6f24, "1:1:2,10:1:2,11:1:2,14:1:2"},
    {4, 0, 13, 2, 19, GOTHIC, 18.0f, 0.90f, 14, 28, 28, 0xef251b93, ""},
    {4, 0, 13, 24, 41, GOTHIC, 18.0f, 0.90f, 14, 20, 20, 0x448f507b, ""},
    {4, 0, 14, 4, 22, SERIF, 20.0f, 1.00f, 14, 14, 16, 0x63f356d4, "8:1:2,12:1:2"},
    {4, 1, 5, 0, 19, SANS_MEDIUM, 15.5f, 0.95f, 14, 29, 28, 0x5c647585, "9:1:2,14:2:1,19:2:1"},
    {4, 1, 5, 20, 39, SANS_MEDIUM, 15.5f, 0.85f, 14, 33, 33, 0x5a8665da, ""},
    {4, 1, 5, 40, 56, SANS, 17.0f, 0.95f, 14, 5, 5, 0x71f220da, ""},
    {4, 2, 4, 7, 25, SERIF, 22.0f, 1.10f, 16, 3, 2, 0xd736d92d, "0:2:1"},
    {4, 2, 5, 7, 25, SERIF, 22.0f, 1.00f, 16, 6, 4, 0x32051eed, "0:2:1,2:2:1"},
};
// clang-format on

// What a mark can be: Latin-1's letters and signs, and the right quote.
constexpr std::u32string_view LETTERS =
    U"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
    U"ÀÁÂÄÇÈÉÊËÌÍÎÏÑÒÓÔÖÙÚÛÜßàáâäçèéêëìíîïñòóôöùúûü¡¿«»’";

constexpr uint32_t SCALE = 4, BAND = 8;
constexpr float INK = 0.3f;            // a mark's texels
constexpr float BLUR = 0.8f;           // both sides blurred before they are compared
constexpr size_t KEEP = 10;            // readings kept per group
constexpr unsigned READERS = 4;        // threads reading texts at disc load
constexpr size_t KEEP_MANY = 20;       // readings kept per group of a few letters
constexpr size_t TRIES = 300000;        // line readings hashed at most
constexpr float SIZE_SLACK = 3.0f;     // texels a letter's box may differ from its mark's,
constexpr float WIDTH_SLACK = 0.35f;   // or this much of its width if more
constexpr size_t CUTS = 6;             // ways a mark of a few letters is cut, cheapest first
constexpr std::array<size_t, 4> PIECE_KEEP{0, 0, 6, 4};  // readings per piece, by letters
constexpr std::array TRACKS{-0.5f, -1.0f, -1.5f};  // closer spacings tried for letters set together
constexpr std::array<size_t, 4> PLACE_KEEP{0, 0, 14, 10};  // letters tried per place, by letters

struct Grid
{
  int w = 0, h = 0;
  std::vector<float> v;

  Grid() = default;
  Grid(int width, int height) : w(width), h(height), v(static_cast<size_t>(width) * height, 0.0f) {}
  float& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
  float at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
  float Sum() const
  {
    float s = 0;
    for (float t : v)
      s += t;
    return s;
  }
  // Ink's mean column, texel centres at +0.5.
  float CentreX() const
  {
    double s = 0, m = 1e-9;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        s += (x + 0.5) * at(x, y);
        m += at(x, y);
      }
    return static_cast<float>(s / m);
  }
};

// Ink placed: texel (0, 0) at (x, y).
struct Ink
{
  int x = 0, y = 0;
  Grid a;
};

// Cut to the texels over `floor`, one empty texel around.
Ink Trim(const Ink& ink, float floor)
{
  int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
  for (int y = 0; y < ink.a.h; ++y)
    for (int x = 0; x < ink.a.w; ++x)
      if (ink.a.at(x, y) > floor)
      {
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      }
  if (x1 < 0)
    return ink;
  Ink out{ink.x + x0 - 1, ink.y + y0 - 1, Grid(x1 - x0 + 3, y1 - y0 + 3)};
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      out.a.at(x - x0 + 1, y - y0 + 1) = ink.a.at(x, y);
  return out;
}

// Inks over one another, each texel the most of them.
Ink Combine(std::span<const Ink> inks)
{
  int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
  for (const Ink& ink : inks)
  {
    x0 = std::min(x0, ink.x), y0 = std::min(y0, ink.y);
    x1 = std::max(x1, ink.x + ink.a.w), y1 = std::max(y1, ink.y + ink.a.h);
  }
  Ink out{x0, y0, Grid(x1 - x0, y1 - y0)};
  for (const Ink& ink : inks)
    for (int y = 0; y < ink.a.h; ++y)
      for (int x = 0; x < ink.a.w; ++x)
      {
        float& t = out.a.at(ink.x - x0 + x, ink.y - y0 + y);
        t = std::max(t, ink.a.at(x, y));
      }
  return out;
}

// `text` set with its pen from `fx` and its baseline `fy` below row 0's top
// (each in [0, 1)), `track` texels more between letters: its coverage, placed
// against the pen's start and the baseline.
Ink Set(const stbtt_fontinfo& font, float sx, float sy, std::u32string_view text, float fx, float fy,
        float track = 0)
{
  struct Box
  {
    int c, left, top, w, h;
    float shift;
  };
  std::vector<Box> boxes;
  int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
  float pen = fx;
  for (size_t i = 0; i < text.size(); ++i)
  {
    const int c = static_cast<int>(text[i]);
    const float shift = pen - std::floor(pen);
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    stbtt_GetCodepointBitmapBoxSubpixel(&font, c, sx, sy, shift, fy, &bx0, &by0, &bx1, &by1);
    if (bx1 > bx0 && by1 > by0)
    {
      const Box box{c, static_cast<int>(std::floor(pen)) + bx0, by0, bx1 - bx0, by1 - by0, shift};
      boxes.push_back(box);
      x0 = std::min(x0, box.left), y0 = std::min(y0, box.top);
      x1 = std::max(x1, box.left + box.w), y1 = std::max(y1, box.top + box.h);
    }
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&font, c, &advance, &bearing);
    if (i + 1 < text.size())
      advance += stbtt_GetCodepointKernAdvance(&font, c, static_cast<int>(text[i + 1]));
    pen += static_cast<float>(advance) * sx + track;
  }
  if (boxes.empty())
    return {};
  Ink out{x0, y0, Grid(x1 - x0, y1 - y0)};
  std::vector<uint8_t> glyph;
  for (const Box& box : boxes)
  {
    glyph.assign(static_cast<size_t>(box.w) * box.h, 0);
    stbtt_MakeCodepointBitmapSubpixel(&font, glyph.data(), box.w, box.h, box.w, sx, sy, box.shift, fy, box.c);
    for (int y = 0; y < box.h; ++y)
      for (int x = 0; x < box.w; ++x)
      {
        float& t = out.a.at(box.left - x0 + x, box.top - y0 + y);
        t = std::max(t, glyph[static_cast<size_t>(y) * box.w + x] / 255.0f);
      }
  }
  return out;
}

Grid Blur(const Grid& g)
{
  constexpr int R = 3;
  std::array<float, 2 * R + 1> k{};
  float total = 0;
  for (int i = -R; i <= R; ++i)
    total += k[i + R] = std::exp(-static_cast<float>(i * i) / (2 * BLUR * BLUR));
  for (float& t : k)
    t /= total;
  Grid across(g.w, g.h), out(g.w, g.h);
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x)
    {
      float s = 0;
      for (int i = std::max(-R, -x); i <= std::min(R, g.w - 1 - x); ++i)
        s += k[i + R] * g.at(x + i, y);
      across.at(x, y) = s;
    }
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x)
    {
      float s = 0;
      for (int i = std::max(-R, -y); i <= std::min(R, g.h - 1 - y); ++i)
        s += k[i + R] * across.at(x, y + i);
      out.at(x, y) = s;
    }
  return out;
}

struct Match
{
  float error = 1e9f, dy = 0;
  float track = 0;  // texels between letters, for a text set together
};

// How unlike a mark and a letter set on the line are: both blurred, the
// letter's ink centred on the mark's, then the least relative difference
// over shifts of up to half a texel each way (the mark's ink, at least).
Match Compare(const Ink& mark, const Ink& letter)
{
  const int top = std::min(mark.y, letter.y) - 2;
  const int h = std::max(mark.y + mark.a.h, letter.y + letter.a.h) - top + 2;
  const int w = std::max(mark.a.w, letter.a.w) + 4;
  Grid g(w, h), r(w, h);
  for (int y = 0; y < mark.a.h; ++y)
    for (int x = 0; x < mark.a.w; ++x)
      g.at(x + 2, mark.y - top + y) = mark.a.at(x, y);
  for (int y = 0; y < letter.a.h; ++y)
    for (int x = 0; x < letter.a.w; ++x)
      r.at(x + 2, letter.y - top + y) = letter.a.at(x, y);
  const float centre = g.CentreX() - r.CentreX();
  g = Blur(g);
  r = Blur(r);
  const float g_sum = g.Sum();
  Match best;
  for (int j = -2; j <= 2; ++j)
    for (int i = -2; i <= 2; ++i)
    {
      const float dx = centre + 0.25f * static_cast<float>(i), dy = 0.25f * static_cast<float>(j);
      const int ix = static_cast<int>(std::floor(dx)), iy = static_cast<int>(std::floor(dy));
      const float fx = dx - static_cast<float>(ix), fy = dy - static_cast<float>(iy);
      float diff = 0, r_sum = 0;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
          // r shifted by (dx, dy), bilinear, nothing outside it
          const int sx = x - ix, sy = y - iy;
          const auto texel = [&](int u, int v) { return u >= 0 && v >= 0 && u < w && v < h ? r.at(u, v) : 0.0f; };
          const float t = (1 - fy) * ((1 - fx) * texel(sx, sy) + fx * texel(sx - 1, sy)) +
                          fy * ((1 - fx) * texel(sx, sy - 1) + fx * texel(sx - 1, sy - 1));
          diff += std::abs(t - g.at(x, y));
          r_sum += t;
        }
      const float error = diff / std::max({g_sum, r_sum, 1e-9f});
      if (error < best.error)
        best = {error, dy};
    }
  return best;
}

// A line's face at its size and the letters set in it (top at the baseline's row).
class Letters
{
public:
  // Descenders are cut where the line's rows end, as its marks' are (accents
  // above are kept: cut, they would read as their bare letters).
  Letters(const Line& line) : m_line(line), m_rows(line.y1 - line.y0)
  {
    stbtt_InitFont(&m_font, FACES[line.style], 0);
    m_sy = stbtt_ScaleForMappingEmToPixels(&m_font, line.em);
    m_sx = m_sy * line.stretch;
    for (char32_t c : LETTERS)
      m_set.push_back(Trim(Set(m_font, m_sx, m_sy, std::u32string_view(&c, 1), 0, 0), 0.02f));
  }

  struct Reading
  {
    Match match;
    std::u32string text;
  };

  // The `keep` likeliest letters of a mark, likeliest first.
  std::vector<Reading> Read(const Ink& mark, size_t keep) const
  {
    std::vector<Reading> out;
    const float width_slack = std::max(SIZE_SLACK, WIDTH_SLACK * static_cast<float>(mark.a.w));
    for (size_t i = 0; i < LETTERS.size(); ++i)
    {
      const Ink letter = Shifted(m_set[i], mark.x);
      if (std::abs(static_cast<float>(letter.a.h - mark.a.h)) > SIZE_SLACK ||
          std::abs(static_cast<float>(letter.a.w - mark.a.w)) > width_slack)  // both one texel around
        continue;
      out.push_back({Compare(mark, letter), std::u32string(1, LETTERS[i])});
    }
    Keep(out, keep);
    return out;
  }

  // How a text of a few letters matches a mark, by the letters set together
  // (letters run into one mark where the game set them tighter than the face:
  // closed up when wider than the mark).
  Match Whole(const Ink& mark, std::u32string_view text) const
  {
    const Ink set = Trim(Set(m_font, m_sx, m_sy, text, 0, 0), 0.02f);
    Match best = Compare(mark, Shifted(set, mark.x));
    if (set.a.w - mark.a.w < 1)
      return best;
    for (const float track : TRACKS)
    {
      Match m = Compare(mark, Shifted(Trim(Set(m_font, m_sx, m_sy, text, 0, 0, track), 0.02f), mark.x));
      m.track = track;
      if (m.error < best.error)
        best = m;
    }
    return best;
  }

  static void Keep(std::vector<Reading>& readings, size_t keep)
  {
    std::ranges::sort(readings, [](const Reading& a, const Reading& b) { return a.match.error < b.match.error; });
    if (readings.size() > keep)
      readings.resize(keep);
  }

private:
  // On the line: its top row at the baseline's plus its own, cut at the line's bottom.
  Ink Shifted(const Ink& letter, int x) const
  {
    Ink out = letter;
    out.x = x;
    out.y += m_line.base;
    if (out.y + out.a.h - 1 <= m_rows)
      return out;
    for (int y = 0; y < out.a.h; ++y)
      if (out.y + y >= m_rows)
        std::fill_n(&out.a.at(0, y), out.a.w, 0.0f);
    return Trim(out, 0.02f);
  }

  const Line& m_line;
  int m_rows;
  stbtt_fontinfo m_font{};
  float m_sx = 1, m_sy = 1;
  std::vector<Ink> m_set;
};

// The marks of a line (rows [y0, y1) of a coverage grid) from left to right:
// each connected stroke (eight ways) of texels over INK, those above one
// another merged, with one texel around them (and its faint texels).
std::vector<Ink> Marks(const Grid& m, int y0, int y1)
{
  const int w = m.w, h = y1 - y0;
  std::vector<int> label(static_cast<size_t>(w) * h, 0);
  struct Box
  {
    int x0, x1, y0, y1;
    std::vector<int> labels;
  };
  std::vector<Box> boxes;
  std::vector<std::pair<int, int>> stack;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      if (label[static_cast<size_t>(y) * w + x] != 0 || m.at(x, y0 + y) <= INK)
        continue;
      const int n = static_cast<int>(boxes.size()) + 1;
      Box box{x, x + 1, y, y + 1, {n}};
      label[static_cast<size_t>(y) * w + x] = n;
      stack.emplace_back(x, y);
      while (!stack.empty())
      {
        const auto [px, py] = stack.back();
        stack.pop_back();
        box.x0 = std::min(box.x0, px), box.x1 = std::max(box.x1, px + 1);
        box.y0 = std::min(box.y0, py), box.y1 = std::max(box.y1, py + 1);
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
          {
            const int qx = px + dx, qy = py + dy;
            if (qx < 0 || qy < 0 || qx >= w || qy >= h)
              continue;
            int& l = label[static_cast<size_t>(qy) * w + qx];
            if (l == 0 && m.at(qx, y0 + qy) > INK)
            {
              l = n;
              stack.emplace_back(qx, qy);
            }
          }
      }
      boxes.push_back(std::move(box));
    }
  std::ranges::sort(boxes, [](const Box& a, const Box& b) {
    return std::tie(a.x0, a.x1, a.y0, a.y1, a.labels) < std::tie(b.x0, b.x1, b.y0, b.y1, b.labels);
  });
  std::vector<Box> merged;
  for (Box& box : boxes)
  {
    if (!merged.empty())
    {
      Box& last = merged.back();
      const int overlap = std::min(last.x1, box.x1) - std::max(last.x0, box.x0);
      if (2 * overlap > std::min(last.x1 - last.x0, box.x1 - box.x0))
      {
        last.x0 = std::min(last.x0, box.x0), last.x1 = std::max(last.x1, box.x1);
        last.y0 = std::min(last.y0, box.y0), last.y1 = std::max(last.y1, box.y1);
        last.labels.insert(last.labels.end(), box.labels.begin(), box.labels.end());
        continue;
      }
    }
    merged.push_back(std::move(box));
  }
  std::vector<Ink> out;
  for (const Box& box : merged)
  {
    const int x0 = std::max(0, box.x0 - 1), x1 = std::min(w, box.x1 + 1);
    const int by0 = std::max(0, box.y0 - 1), by1 = std::min(h, box.y1 + 1);
    Ink ink{x0, by0, Grid(x1 - x0, by1 - by0)};
    for (int y = by0; y < by1; ++y)
      for (int x = x0; x < x1; ++x)
      {
        const int l = label[static_cast<size_t>(y) * w + x];
        if (l == 0 || std::ranges::find(box.labels, l) != box.labels.end())
          ink.a.at(x - x0, y - by0) = m.at(x, y0 + y);
      }
    out.push_back(std::move(ink));
  }
  return out;
}

// Columns [a, b] of a mark.
Ink Columns(const Ink& mark, int a, int b)
{
  Ink out{mark.x + a, mark.y, Grid(b - a + 1, mark.a.h)};
  for (int y = 0; y < mark.a.h; ++y)
    for (int x = a; x <= b; ++x)
      out.a.at(x - a, y) = mark.a.at(x, y);
  return Trim(out, 0.05f);
}

// The likeliest readings of a mark holding `count` letters: cut where its
// columns hold least ink into the cheapest pieces, each piece's letters
// combined and the combinations judged set together.
std::vector<Letters::Reading> ReadMany(const Letters& letters, const Ink& mark, int count)
{
  if (count == 1)
    return letters.Read(mark, KEEP);
  if (count >= static_cast<int>(PIECE_KEEP.size()))
    return {};
  const int w = mark.a.w;
  std::vector<float> column(static_cast<size_t>(w), 0.0f);
  for (int y = 0; y < mark.a.h; ++y)
    for (int x = 0; x < w; ++x)
      column[static_cast<size_t>(x)] += mark.a.at(x, y);
  std::vector<float> sorted = column;
  std::ranges::sort(sorted);
  const float median = w % 2 ? sorted[static_cast<size_t>(w / 2)] : (sorted[static_cast<size_t>(w / 2 - 1)] + sorted[static_cast<size_t>(w / 2)]) / 2;
  std::vector<int> cuts;
  for (int c = 3; c < w - 3; ++c)
    if (column[static_cast<size_t>(c)] <= median)
      cuts.push_back(c);
  std::map<std::pair<int, int>, std::vector<Letters::Reading>> pieces;
  const auto piece = [&](int a, int b) -> const std::vector<Letters::Reading>& {
    auto [it, fresh] = pieces.try_emplace({a, b});
    if (fresh)
      it->second = letters.Read(Columns(mark, a, b), KEEP);
    return it->second;
  };
  const auto cost = [&](int a, int b) {
    const auto& r = piece(a, b);
    return r.empty() ? 1e9f : r.front().match.error;
  };
  // the cheapest ways to cut it (one or two cuts)
  std::vector<std::pair<float, std::vector<int>>> ways;
  if (count == 2)
  {
    for (int c : cuts)
      ways.push_back({cost(0, c) + cost(c, w - 1), {c}});
  }
  else if (count == 3)
  {
    for (int c1 : cuts)
      for (int c2 : cuts)
        if (c2 - c1 >= 3)
          ways.push_back({cost(0, c1) + cost(c1, c2) + cost(c2, w - 1), {c1, c2}});
  }
  std::ranges::sort(ways, [](const auto& a, const auto& b) { return a.first < b.first; });
  if (ways.size() > CUTS)
    ways.resize(CUTS);
  // each place's likeliest letters over those cuts, then every combination
  std::vector<std::vector<std::pair<float, char32_t>>> places(static_cast<size_t>(count));
  for (const auto& [e, way] : ways)
  {
    if (e >= 1e9f)
      continue;
    std::vector<int> edges{0};
    edges.insert(edges.end(), way.begin(), way.end());
    edges.push_back(w - 1);
    for (size_t p = 0; p + 1 < edges.size(); ++p)
    {
      const auto& readings = piece(edges[p], edges[p + 1]);
      for (size_t k = 0; k < std::min(readings.size(), PIECE_KEEP[static_cast<size_t>(count)]); ++k)
      {
        auto& place = places[p];
        const char32_t c = readings[k].text.front();
        const auto it = std::ranges::find(place, c, &std::pair<float, char32_t>::second);
        if (it == place.end())
          place.push_back({readings[k].match.error, c});
        else
          it->first = std::min(it->first, readings[k].match.error);
      }
    }
  }
  std::vector<std::u32string> texts{U""};
  for (auto& place : places)
  {
    std::ranges::sort(place);
    if (place.size() > PLACE_KEEP[static_cast<size_t>(count)])
      place.resize(PLACE_KEEP[static_cast<size_t>(count)]);
    std::vector<std::u32string> next;
    for (const std::u32string& t : texts)
      for (const auto& [e, c] : place)
        next.push_back(t + c);
    texts = std::move(next);
  }
  std::vector<Letters::Reading> out;
  for (std::u32string& t : texts)
    out.push_back({letters.Whole(mark, t), std::move(t)});
  Letters::Keep(out, KEEP_MANY);
  return out;
}

uint32_t Crc32(std::string_view bytes)
{
  uint32_t crc = 0xFFFFFFFF;
  for (char c : bytes)
  {
    crc ^= static_cast<uint8_t>(c);
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
  }
  return ~crc;
}

void AppendUtf8(std::string& out, char32_t c)
{
  if (c < 0x80)
    out += static_cast<char>(c);
  else if (c < 0x800)
  {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
  else
  {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// A group of marks read: its letters, set where its ink's centre is.
struct Placed
{
  std::u32string text;
  float centre = 0;  // texels from the image's left
  float track = 0;   // texels between its letters
};

// A line read: its letters, the baseline they sit on, and what to clear and how bright.
struct Read
{
  const Line* line = nullptr;
  std::vector<Placed> groups;
  float baseline = 0;  // rows from the image's top
  int x0 = 0, x1 = 0;
  uint8_t level = 255;
};

std::optional<Read> ReadLine(const Line& line, const Image& image, const Grid& m, std::stop_token stop)
{
  std::vector<Ink> marks = Marks(m, line.y0, line.y1);
  if (marks.size() != line.marks)
    return std::nullopt;
  // the groups: one mark per letter but where `groups` says otherwise
  std::vector<std::pair<int, int>> shape;  // marks, letters
  std::map<int, std::pair<int, int>> told;
  for (const char* p = line.groups; *p != '\0';)
  {
    int g = 0, n = 0, l = 0, used = 0;
    if (std::sscanf(p, "%d:%d:%d%n", &g, &n, &l, &used) != 3)
      return std::nullopt;
    told[g] = {n, l};
    p += used;
    if (*p == ',')
      ++p;
  }
  int marks_left = line.marks, letters_left = line.letters;
  while (marks_left > 0)
  {
    const auto it = told.find(static_cast<int>(shape.size()));
    const auto group = it != told.end() ? it->second : std::pair{1, 1};
    shape.push_back(group);
    marks_left -= group.first, letters_left -= group.second;
  }
  if (marks_left != 0 || letters_left != 0)
    return std::nullopt;
  const Letters letters(line);
  std::vector<std::vector<Letters::Reading>> options;
  std::vector<Ink> inks;
  size_t next = 0;
  for (const auto& [count, length] : shape)
  {
    Ink ink = Combine(std::span(marks).subspan(next, static_cast<size_t>(count)));
    next += static_cast<size_t>(count);
    std::vector<Letters::Reading> readings = ReadMany(letters, ink, length);
    if (readings.empty())
      return std::nullopt;
    options.push_back(std::move(readings));
    inks.push_back(std::move(ink));
  }
  // The readings of the whole line, likeliest first (each group's error by
  // its letters), until one hashes as the line does: each reading's next are
  // it with one group from its last changed onwards a step less likely, so
  // each is reached once.
  struct Try
  {
    float cost;
    size_t at;
    size_t pivot;
    bool operator>(const Try& o) const { return cost > o.cost; }
  };
  const size_t groups = options.size();
  const auto weight = [&](size_t g, size_t k) { return options[g][k].match.error * static_cast<float>(shape[g].second); };
  std::vector<uint8_t> picks(groups, 0);  // every try's picks, one after another
  std::priority_queue<Try, std::vector<Try>, std::greater<>> queue;
  float start = 0;
  for (size_t g = 0; g < groups; ++g)
    start += weight(g, 0);
  queue.push({start, 0, 0});
  std::string utf8;
  for (size_t tries = 0; !queue.empty() && tries < TRIES; ++tries)
  {
    if (tries % 4096 == 0 && stop.stop_requested())
      return std::nullopt;
    const Try t = queue.top();
    queue.pop();
    const uint8_t* pick = &picks[t.at * groups];
    utf8.clear();
    for (size_t g = 0; g < groups; ++g)
      for (char32_t c : options[g][pick[g]].text)
        AppendUtf8(utf8, c);
    if (Crc32(utf8) == line.crc)
    {
      Read read{&line};
      float dy = 0;
      read.x0 = INT_MAX, read.x1 = 0;
      for (size_t g = 0; g < groups; ++g)
      {
        const Ink& ink = inks[g];
        read.groups.push_back({options[g][pick[g]].text, static_cast<float>(ink.x) + ink.a.CentreX(),
                               options[g][pick[g]].match.track});
        dy += options[g][pick[g]].match.dy;
        read.x0 = std::min(read.x0, ink.x), read.x1 = std::max(read.x1, ink.x + ink.a.w);
      }
      read.baseline = line.y0 + line.base + dy / static_cast<float>(groups);
      uint8_t level = 0;
      for (int y = line.y0; y < line.y1; ++y)
        for (int x = read.x0; x < read.x1; ++x)
          level = std::max(level, image.rgba[(static_cast<size_t>(y) * image.width + x) * 4]);
      read.level = level;
      return read;
    }
    for (size_t g = t.pivot; g < groups; ++g)
    {
      if (pick[g] + 1u >= options[g].size())
        continue;
      const size_t at = picks.size() / groups;
      const float cost = t.cost - weight(g, pick[g]) + weight(g, pick[g] + 1u);
      picks.resize(picks.size() + groups);
      pick = &picks[t.at * groups];  // the resize may have moved it
      std::copy_n(pick, groups, &picks[at * groups]);
      ++picks[at * groups + g];
      queue.push({cost, at, g});
    }
  }
  return std::nullopt;
}

// A kept image: its key, texels and lines, and what reading them found.
struct Text
{
  aurora::texture::TextureSourceKey key;
  Image image;
  std::vector<const Line*> lines;
  std::vector<Read> reads;
  std::vector<uint8_t> texels;  // its sharp copy, drawn once read
  uint32_t mips = 0, width = 0, height = 0;
  bool provided = false;
};

std::vector<uint8_t> Draw(const Text& text, uint32_t& mips, uint32_t& width, uint32_t& height);

std::vector<Text> g_texts;
std::array<std::array<bool, LANGUAGES.size()>, FILES.size()> g_kept{};
std::atomic<bool> g_ready = false, g_read = false;
std::jthread g_reader;  // last: stops before the rest goes

// Reads every kept text's lines and draws its sharp copy, a few texts at a
// time (off the render thread: a menu's copies take tens of ms).
void ReadTexts(std::stop_token stop)
{
  std::atomic<size_t> next = 0, read = 0, lines = 0;
  {
    std::vector<std::jthread> workers;
    const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, READERS);
    for (unsigned w = 0; w < count; ++w)
      workers.emplace_back([&] {
        for (size_t t; !stop.stop_requested() && (t = next++) < g_texts.size();)
        {
          Text& text = g_texts[t];
          Grid m(static_cast<int>(text.image.width), static_cast<int>(text.image.height));
          for (size_t i = 0; i < m.v.size(); ++i)
            m.v[i] = std::clamp((text.image.rgba[i * 4] / 255.0f - 0.15f) / 0.85f, 0.0f, 1.0f);
          for (const Line* line : text.lines)
          {
            if (stop.stop_requested())
              break;
            ++lines;
            if (auto r = ReadLine(*line, text.image, m, stop))
            {
              text.reads.push_back(std::move(*r));
              ++read;
            }
          }
          if (stop.stop_requested())
            break;
          text.texels = Draw(text, text.mips, text.width, text.height);
          text.image = {};
        }
      });
  }
  if (read != lines && !stop.stop_requested())
    std::fprintf(stderr, "[2p text] %zu of %zu lines read; the rest keep the game's lettering\n", read.load(), lines.load());
  g_read = true;
  g_read.notify_all();
}

// The image four times larger, sharpened, each line read set anew.
std::vector<uint8_t> Draw(const Text& text, uint32_t& mips, uint32_t& width, uint32_t& height)
{
  Image sharp = SharpenGreyMask(text.image, SCALE, BAND);
  const auto k = static_cast<float>(SCALE);
  for (const Read& read : text.reads)
  {
    const Line& line = *read.line;
    for (uint32_t y = line.y0 * SCALE; y < line.y1 * SCALE; ++y)
      for (uint32_t x = static_cast<uint32_t>(read.x0) * SCALE; x < static_cast<uint32_t>(read.x1) * SCALE; ++x)
      {
        uint8_t* p = &sharp.rgba[(static_cast<size_t>(y) * sharp.width + x) * 4];
        p[0] = p[1] = p[2] = 0;
      }
    stbtt_fontinfo font{};
    stbtt_InitFont(&font, FACES[line.style], 0);
    const float sy = stbtt_ScaleForMappingEmToPixels(&font, line.em) * k, sx = sy * line.stretch;
    const float base = read.baseline * k;
    const int by = static_cast<int>(std::floor(base));
    const float fy = base - static_cast<float>(by);
    for (const Placed& group : read.groups)
    {
      const Ink first = Set(font, sx, sy, group.text, 0, fy, group.track * k);
      const float origin = group.centre * k - (static_cast<float>(first.x) + first.a.CentreX());
      const int ox = static_cast<int>(std::floor(origin));
      const Ink ink = Set(font, sx, sy, group.text, origin - static_cast<float>(ox), fy, group.track * k);
      for (int y = 0; y < ink.a.h; ++y)
        for (int x = 0; x < ink.a.w; ++x)
        {
          const int px = ox + ink.x + x, py = by + ink.y + y;
          if (px < 0 || py < 0 || px >= static_cast<int>(sharp.width) || py >= static_cast<int>(sharp.height))
            continue;
          uint8_t* p = &sharp.rgba[(static_cast<size_t>(py) * sharp.width + px) * 4];
          const auto t = static_cast<uint8_t>(std::lround(ink.a.at(x, y) * read.level));
          p[0] = p[1] = p[2] = std::max(p[0], t);
        }
    }
  }
  width = sharp.width, height = sharp.height;
  return GreyMipChain(sharp, mips);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}  // namespace

void Keep2PText(std::string_view file, std::span<const uint8_t> bytes)
{
  for (size_t f = 0; f < FILES.size(); ++f)
    for (size_t l = 0; l < LANGUAGES.size(); ++l)
    {
      if (g_kept[f][l] || file != std::string(FILES[f]) + std::string(LANGUAGES[l]) + ".tls")
        continue;
      g_kept[f][l] = true;
      std::map<uint8_t, std::vector<const Line*>> images;
      for (const Line& line : LINES)
        if (line.file == f && line.language == l)
          images[line.image].push_back(&line);
      for (auto& [index, lines] : images)
      {
        auto image = DecodeTplImage(bytes, index);
        if (!image || image->format != CMPR || image->offset + image->image.width * image->image.height / 2 > bytes.size() ||
            std::ranges::any_of(lines, [&](const Line* line) { return line->y1 > image->image.height; }))
          continue;
        const auto key = aurora::texture::source_key(CMPR, image->image.width, image->image.height,
                                                     bytes.data() + image->offset);
        g_texts.push_back({key, std::move(image->image), std::move(lines)});
      }
    }
}

void Load2PText(const std::filesystem::path& game)
{
  for (size_t f = 0; f < FILES.size(); ++f)
    for (size_t l = 0; l < LANGUAGES.size(); ++l)
      if (!g_kept[f][l])
      {
        const std::string file = std::string(FILES[f]) + std::string(LANGUAGES[l]) + ".tls";
        const std::vector<uint8_t> bytes = ReadFile(game / file);
        if (!bytes.empty())
          Keep2PText(file, bytes);
      }
  g_ready = true;
  g_reader = std::jthread(ReadTexts);
}

bool Provide2PText(const aurora::texture::TextureSource& source)
{
  if (source.format != CMPR || !g_ready)
    return false;
  for (Text& text : g_texts)
  {
    if (text.provided || !(text.key == source.key))
      continue;
    g_read.wait(false);
    text.provided = true;
    const bool drawn = !text.texels.empty();
    if (drawn)
      aurora::texture::set_memory_replacement(text.key, text.width, text.height, text.mips, text.texels);
    text.texels = {};
    return drawn;
  }
  return false;
}
}  // namespace dualize::nights
