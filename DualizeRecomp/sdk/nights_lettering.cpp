#include "sdk/nights_lettering.hpp"

#include "ui/game_assets.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

namespace
{
using dualize::nights::Letter;
using dualize::nights::LetteringSize;

// A line of the face in a texture: a window of 24 rows from `top` (rows above
// the texture are empty), its first rows cleared when the line above's
// descenders reach in; its capitals' height, baseline row and press, and how
// much its strokes are thinned (coverage (c - thin) / (1 - thin)) to the
// labels' weight, other screens drawing the face heavier.
struct LetteringSource
{
  const char* file;
  uint32_t image;
  int32_t top;
  bool clear_top;
  float cap, baseline, press, thin;
};

enum class Make : uint8_t
{
  CUT,             // the cell [x0, x1) of a source line
  O_FROM_C,        // the C's left half and its mirror
  Q_FROM_P,        // the p mirrored
  G_FROM_C,        // the C, the O's lower right quarter, a bar in at the middle
  U_FROM_U,        // the u made even, its stems drawn up to the capitals
  TIMES_FROM_X,    // the x centred on the capitals' middle
  ACCENT_SMALL,    // a small letter (an i without its dot) and another's accent
  ACCENT_CAPITAL,  // a capital and its small letter's accent, raised
};

struct LetteringGlyph
{
  char32_t c;
  LetteringSize size;
  Make make;
  uint8_t source;
  uint16_t x0, x1;
  char32_t base, donor;
  float left, right;  // side bearings per cap texel
};

#include "sdk/nights_lettering_table.inc"

constexpr int ROWS = 24, BASELINE = 19;
constexpr float INK = 0.12f, SOLID = 0.3f, COMPONENT = 0.25f;

struct Window
{
  int width = 0;
  std::vector<float> ink;      // ROWS x width
  std::vector<int> component;  // 0: none, else 1 + the component (in raster order of their first texel)
  float At(int y, int x) const { return ink[static_cast<size_t>(y) * width + x]; }
};

// A letter image of any height (rows x width).
struct Bitmap
{
  int width = 0, rows = ROWS;
  std::vector<float> ink;
  float At(int y, int x) const { return ink[static_cast<size_t>(y) * width + x]; }
  float& At(int y, int x) { return ink[static_cast<size_t>(y) * width + x]; }
  static Bitmap Empty(int width, int rows = ROWS)
  {
    Bitmap b;
    b.width = width;
    b.rows = rows;
    b.ink.assign(static_cast<size_t>(width) * rows, 0.0f);
    return b;
  }
  float RowMax(int y) const
  {
    float m = 0;
    for (int x = 0; x < width; ++x)
      m = std::max(m, At(y, x));
    return m;
  }
  float ColumnMax(int x) const
  {
    float m = 0;
    for (int y = 0; y < rows; ++y)
      m = std::max(m, At(y, x));
    return m;
  }
  int FirstRow(float level) const
  {
    for (int y = 0; y < rows; ++y)
      if (RowMax(y) > level)
        return y;
    return -1;
  }
  int LastRow(float level) const
  {
    for (int y = rows - 1; y >= 0; --y)
      if (RowMax(y) > level)
        return y;
    return -1;
  }
  Bitmap Columns(int from, int to) const
  {
    Bitmap b = Empty(to - from, rows);
    for (int y = 0; y < rows; ++y)
      for (int x = from; x < to; ++x)
        b.At(y, x - from) = At(y, x);
    return b;
  }
  Bitmap Mirrored() const
  {
    Bitmap b = Empty(width, rows);
    for (int y = 0; y < rows; ++y)
      for (int x = 0; x < width; ++x)
        b.At(y, x) = At(y, width - 1 - x);
    return b;
  }
};

std::vector<Window> g_windows;  // one per SOURCES entry; empty when its file is missing
std::map<std::tuple<char32_t, LetteringSize, int>, std::unique_ptr<Letter>> g_letters;

std::vector<uint8_t> ReadAll(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

// Components of the window's ink above COMPONENT, 8-connected, numbered in
// raster order of their first texel.
void Label(Window& window)
{
  window.component.assign(window.ink.size(), 0);
  int next = 0;
  std::vector<int> stack;
  for (int y = 0; y < ROWS; ++y)
  {
    for (int x = 0; x < window.width; ++x)
    {
      const size_t at = static_cast<size_t>(y) * window.width + x;
      if (window.component[at] != 0 || window.ink[at] <= COMPONENT)
        continue;
      window.component[at] = ++next;
      stack.assign(1, static_cast<int>(at));
      while (!stack.empty())
      {
        const int p = stack.back();
        stack.pop_back();
        const int py = p / window.width, px = p % window.width;
        for (int dy = -1; dy <= 1; ++dy)
        {
          for (int dx = -1; dx <= 1; ++dx)
          {
            const int ny = py + dy, nx = px + dx;
            if (ny < 0 || ny >= ROWS || nx < 0 || nx >= window.width)
              continue;
            const size_t n = static_cast<size_t>(ny) * window.width + nx;
            if (window.component[n] == 0 && window.ink[n] > COMPONENT)
            {
              window.component[n] = next;
              stack.push_back(static_cast<int>(n));
            }
          }
        }
      }
    }
  }
}

const LetteringGlyph* Find(char32_t c, LetteringSize size)
{
  for (const LetteringGlyph& glyph : GLYPHS)
    if (glyph.c == c && glyph.size == size)
      return &glyph;
  return nullptr;
}

// The letter in cell [x0, x1): the cell's columns, less the ink of components
// centred outside it (a neighbour reaching in), plus its own components'
// reaching out (a kerned overhang); a component spanning several letters
// keeps to the cell. Trimmed to its ink.
std::optional<Bitmap> Cut(const Window& window, int x0, int x1)
{
  const int w = window.width;
  std::vector<uint8_t> keep(window.ink.size(), 0);
  for (int y = 0; y < ROWS; ++y)
    for (int x = std::max(0, x0); x < std::min(w, x1); ++x)
      keep[static_cast<size_t>(y) * w + x] = 1;
  const int lo = std::max(0, x0 - 8), hi = std::min(w, x1 + 8);
  std::vector<int> ids;
  for (int y = 0; y < ROWS; ++y)
    for (int x = lo; x < hi; ++x)
      if (const int id = window.component[static_cast<size_t>(y) * w + x])
        ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (const int id : ids)
  {
    int min_x = w, max_x = -1;
    double sum = 0, weighted = 0;
    for (int y = 0; y < ROWS; ++y)
    {
      for (int x = 0; x < w; ++x)
      {
        if (window.component[static_cast<size_t>(y) * w + x] != id)
          continue;
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        sum += window.At(y, x);
        weighted += window.At(y, x) * x;
      }
    }
    if (static_cast<float>(max_x - min_x + 1) > static_cast<float>(x1 - x0) * 1.4f + 3.0f)
      continue;
    const double cx = weighted / sum;
    const bool inside = cx >= x0 && cx < x1;
    for (int y = 0; y < ROWS; ++y)
    {
      for (int x = 0; x < w; ++x)
      {
        if (inside)
        {
          if (window.component[static_cast<size_t>(y) * w + x] == id)
            keep[static_cast<size_t>(y) * w + x] = 1;
          continue;
        }
        // Clear the component grown by a texel.
        bool near = false;
        for (int dy = -1; dy <= 1 && !near; ++dy)
          for (int dx = -1; dx <= 1 && !near; ++dx)
          {
            const int ny = y + dy, nx = x + dx;
            near = ny >= 0 && ny < ROWS && nx >= 0 && nx < w && window.component[static_cast<size_t>(ny) * w + nx] == id;
          }
        if (near)
          keep[static_cast<size_t>(y) * w + x] = 0;
      }
    }
  }
  int a = w, b = -1;
  for (int x = 0; x < w; ++x)
  {
    for (int y = 0; y < ROWS; ++y)
    {
      if (keep[static_cast<size_t>(y) * w + x] && window.At(y, x) > INK)
      {
        a = std::min(a, x);
        b = std::max(b, x);
      }
    }
  }
  if (b < a)
    return std::nullopt;
  Bitmap out = Bitmap::Empty(b + 1 - a);
  for (int y = 0; y < ROWS; ++y)
    for (int x = a; x <= b; ++x)
      out.At(y, x - a) = keep[static_cast<size_t>(y) * w + x] ? window.At(y, x) : 0.0f;
  return out;
}

// Bicubic (a = -0.5) resampling, widened when shrinking, as image tools do.
std::vector<float> Weights(int in, int out, std::vector<int>& first, std::vector<int>& count)
{
  const double scale = static_cast<double>(out) / in;
  const double support = 2.0 * std::max(1.0, 1.0 / scale), stretch = std::max(1.0, 1.0 / scale);
  const auto cubic = [](double t) {
    t = std::abs(t);
    constexpr double A = -0.5;
    if (t < 1)
      return ((A + 2) * t - (A + 3)) * t * t + 1;
    if (t < 2)
      return ((A * t - 5 * A) * t + 8 * A) * t - 4 * A;
    return 0.0;
  };
  const int taps = static_cast<int>(std::ceil(support)) * 2 + 1;
  std::vector<float> weights(static_cast<size_t>(out) * taps, 0.0f);
  first.assign(out, 0);
  count.assign(out, 0);
  for (int o = 0; o < out; ++o)
  {
    const double center = (o + 0.5) / scale;
    const int lo = std::max(0, static_cast<int>(std::floor(center - support))),
              hi = std::min(in, static_cast<int>(std::ceil(center + support)));
    double total = 0;
    first[o] = lo;
    count[o] = std::min(taps, hi - lo);
    for (int k = 0; k < count[o]; ++k)
    {
      const double w = cubic((lo + k + 0.5 - center) / stretch);
      weights[static_cast<size_t>(o) * taps + k] = static_cast<float>(w);
      total += w;
    }
    for (int k = 0; k < count[o] && total != 0; ++k)
      weights[static_cast<size_t>(o) * taps + k] /= static_cast<float>(total);
  }
  return weights;
}

Bitmap Resample(const Bitmap& in, int width, int rows)
{
  std::vector<int> fx, cx, fy, cy;
  const std::vector<float> wx = Weights(in.width, width, fx, cx), wy = Weights(in.rows, rows, fy, cy);
  const size_t tx = wx.size() / width, ty = wy.size() / rows;
  Bitmap across = Bitmap::Empty(width, in.rows);
  for (int y = 0; y < in.rows; ++y)
    for (int x = 0; x < width; ++x)
    {
      float v = 0;
      for (int k = 0; k < cx[x]; ++k)
        v += wx[x * tx + k] * in.At(y, fx[x] + k);
      across.At(y, x) = v;
    }
  Bitmap out = Bitmap::Empty(width, rows);
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < width; ++x)
    {
      float v = 0;
      for (int k = 0; k < cy[y]; ++k)
        v += wy[y * ty + k] * across.At(fy[y] + k, x);
      out.At(y, x) = std::clamp(v, 0.0f, 1.0f);
    }
  return out;
}

// A cut of `source` at `cap` and `press`, its baseline moved to row 19.
Bitmap Fit(const Bitmap& image, const LetteringSource& source, float cap, float press)
{
  const float k = cap / source.cap, sx = k * press / source.press;
  Bitmap out;
  if (std::abs(k - 1) > 0.02f || std::abs(sx - 1) > 0.02f)
  {
    const int rows = static_cast<int>(std::lround(ROWS * k));
    const int width = std::max(1, static_cast<int>(std::lround(static_cast<float>(image.width) * sx)));
    const Bitmap big = Resample(image, width, rows);
    const int base = static_cast<int>(std::lround(source.baseline * k));
    out = Bitmap::Empty(width);
    for (int y = 0; y < ROWS; ++y)
    {
      const int from = y - BASELINE + base;
      if (from >= 0 && from < rows)
        for (int x = 0; x < width; ++x)
          out.At(y, x) = big.At(from, x);
    }
    return out;
  }
  const int shift = static_cast<int>(std::lround(BASELINE - source.baseline));
  out = Bitmap::Empty(image.width);
  for (int y = 0; y < ROWS; ++y)
  {
    const int from = y - shift;
    if (from >= 0 && from < ROWS)
      for (int x = 0; x < image.width; ++x)
        out.At(y, x) = image.At(from, x);
  }
  return out;
}

std::optional<Bitmap> Build(char32_t c, LetteringSize size, float cap, float press);

// The cut a CUT glyph names, unfitted, with its source.
std::optional<std::pair<Bitmap, const LetteringSource*>> RawCut(char32_t c, LetteringSize size)
{
  const LetteringGlyph* glyph = Find(c, size);
  if (glyph == nullptr || glyph->make != Make::CUT || glyph->source >= g_windows.size() ||
      g_windows[glyph->source].width == 0)
    return std::nullopt;
  auto cut = Cut(g_windows[glyph->source], glyph->x0, glyph->x1);
  if (!cut)
    return std::nullopt;
  if (const float thin = SOURCES[glyph->source].thin; thin > 0)
    for (float& ink : cut->ink)
      ink = std::clamp((ink - thin) / (1 - thin), 0.0f, 1.0f);
  return std::pair{std::move(*cut), &SOURCES[glyph->source]};
}

Bitmap EvenHalves(const Bitmap& image)
{
  const Bitmap half = image.Columns(0, (image.width + 1) / 2), mirror = half.Mirrored();
  Bitmap out = Bitmap::Empty(image.width, image.rows);
  for (int y = 0; y < image.rows; ++y)
  {
    for (int x = 0; x < half.width; ++x)
      out.At(y, x) = half.At(y, x);
    for (int x = image.width % 2; x < mirror.width; ++x)
      out.At(y, half.width + x - image.width % 2) = mirror.At(y, x);
  }
  return out;
}

std::optional<Bitmap> Build(char32_t c, LetteringSize size, float cap, float press)
{
  const LetteringGlyph* glyph = Find(c, size);
  if (glyph == nullptr)
    return std::nullopt;
  switch (glyph->make)
  {
  case Make::CUT:
  {
    auto cut = RawCut(c, size);
    return cut ? std::optional(Fit(cut->first, *cut->second, cap, press)) : std::nullopt;
  }
  case Make::O_FROM_C:
  {
    auto cut = RawCut(U'C', size);
    return cut ? std::optional(Fit(EvenHalves(cut->first), *cut->second, cap, press)) : std::nullopt;
  }
  case Make::Q_FROM_P:
  {
    auto cut = RawCut(U'p', size);
    return cut ? std::optional(Fit(cut->first.Mirrored(), *cut->second, cap, press)) : std::nullopt;
  }
  case Make::G_FROM_C:
  {
    const auto C = Build(U'C', size, cap, press), O = Build(U'O', size, cap, press), l = Build(U'l', size, cap, press);
    if (!C || !O || !l)
      return std::nullopt;
    Bitmap out = Bitmap::Empty(std::max(C->width, O->width));
    for (int y = 0; y < ROWS; ++y)
      for (int x = 0; x < C->width; ++x)
        out.At(y, x) = C->At(y, x);
    const int top = O->FirstRow(SOLID), bottom = O->LastRow(SOLID) + 1, mid = (top + bottom) / 2;
    for (int y = mid; y < ROWS; ++y)
      for (int x = O->width / 2; x < O->width; ++x)
        out.At(y, x) = std::max(out.At(y, x), O->At(y, x));
    // The bar: one row across the l's stem, turned.
    std::vector<float> profile;
    for (int x = 0; x < l->width; ++x)
      if (l->ColumnMax(x) > INK)
        profile.push_back(l->At(top + 3, x));
    int right = 0;
    for (int x = 0; x < O->width; ++x)
      if (O->At(mid + 1, x) > SOLID)
        right = x + 1;
    const int left = static_cast<int>(std::lround(O->width * 0.52f));
    for (size_t i = 0; i < profile.size(); ++i)
    {
      const int y = mid - static_cast<int>(profile.size()) / 2 + static_cast<int>(i);
      if (y >= 0 && y < ROWS)
        for (int x = left; x < right; ++x)
          out.At(y, x) = std::max(out.At(y, x), profile[i]);
    }
    return out;
  }
  case Make::U_FROM_U:
  {
    const auto u = Build(U'u', size, cap, press), E = Build(U'E', size, cap, press);
    if (!u || !E)
      return std::nullopt;
    Bitmap out = EvenHalves(*u);
    const int utop = out.FirstRow(SOLID), etop = E->FirstRow(SOLID);
    const Bitmap stem = out.Columns(0, out.width);
    for (int y = etop; y < utop + 2; ++y)
      for (int x = 0; x < out.width; ++x)
        out.At(y, x) = stem.At(utop + 2, x);
    return out;
  }
  case Make::TIMES_FROM_X:
  {
    const auto x = Build(U'x', size, cap, press), E = Build(U'E', size, cap, press);
    if (!x || !E)
      return std::nullopt;
    const float shift = (x->FirstRow(SOLID) + x->LastRow(SOLID)) / 2.0f - (E->FirstRow(SOLID) + E->LastRow(SOLID)) / 2.0f;
    const int s = static_cast<int>(std::lround(shift));
    Bitmap out = Bitmap::Empty(x->width);
    for (int y = 0; y < ROWS; ++y)
      if (y + s >= 0 && y + s < ROWS)
        for (int i = 0; i < x->width; ++i)
          out.At(y, i) = x->At(y + s, i);
    return out;
  }
  case Make::ACCENT_SMALL:
  case Make::ACCENT_CAPITAL:
  {
    const bool capital = glyph->make == Make::ACCENT_CAPITAL;
    const auto base = Build(glyph->base, size, cap, press), donor = Build(glyph->donor, size, cap, press),
               o = Build(U'o', size, cap, press);
    if (!base || !donor || !o)
      return std::nullopt;
    const int xtop = o->FirstRow(SOLID);
    Bitmap out = *base;
    if (!capital && (glyph->base == U'i' || glyph->base == U'j'))
      for (int y = 0; y < xtop - 1; ++y)
        for (int x = 0; x < out.width; ++x)
          out.At(y, x) = 0;
    // The donor's accent: its rows above the small letters, trimmed to ink.
    int a = donor->width, b = -1;
    for (int y = 0; y < xtop - 1; ++y)
      for (int x = 0; x < donor->width; ++x)
        if (donor->At(y, x) > INK)
          a = std::min(a, x), b = std::max(b, x);
    if (b < a)
      return out;
    const int aw = b + 1 - a;
    int x0 = 0, lift = 0;
    if (capital)
    {
      x0 = static_cast<int>(std::floor((out.width - aw) / 2.0f));
      lift = xtop - out.FirstRow(SOLID);
    }
    else
    {
      int bl = out.width, br = -1;
      for (int x = 0; x < out.width; ++x)
        if (out.ColumnMax(x) > INK)
          bl = std::min(bl, x), br = std::max(br, x);
      x0 = static_cast<int>(std::lround((bl + br + 1) / 2.0f - aw / 2.0f));
      const int pad_left = std::max(0, -x0);
      x0 += pad_left;
      const int pad_right = std::max(0, x0 + aw - (out.width + pad_left));
      if (pad_left > 0 || pad_right > 0)
      {
        Bitmap wider = Bitmap::Empty(out.width + pad_left + pad_right);
        for (int y = 0; y < ROWS; ++y)
          for (int x = 0; x < out.width; ++x)
            wider.At(y, x + pad_left) = out.At(y, x);
        out = std::move(wider);
      }
    }
    for (int y = 0; y < xtop - 1; ++y)
    {
      const int to = y - lift;
      if (to < 0 || to >= ROWS)
        continue;
      for (int x = 0; x < aw; ++x)
      {
        const int at = x0 + x;
        if (at >= 0 && at < out.width)
          out.At(to, at) = std::max(out.At(to, at), donor->At(y, a + x));
      }
    }
    return out;
  }
  }
  return std::nullopt;
}
}  // namespace

namespace dualize::nights
{
void LoadLettering(const std::filesystem::path& game)
{
  g_windows.assign(std::size(SOURCES), Window{});
  g_letters.clear();
  std::map<std::pair<std::string, uint32_t>, std::optional<Image>> images;
  for (size_t i = 0; i < std::size(SOURCES); ++i)
  {
    const LetteringSource& source = SOURCES[i];
    auto& image = images[{source.file, source.image}];
    if (!image)
    {
      if (auto decoded = DecodeTplImage(ReadAll(game / source.file), source.image))
        image = std::move(decoded->image);
    }
    if (!image || image->width == 0)
      continue;
    Window& window = g_windows[i];
    window.width = static_cast<int>(image->width);
    window.ink.assign(static_cast<size_t>(ROWS) * window.width, 0.0f);
    for (int y = source.clear_top ? 4 : 0; y < ROWS; ++y)
    {
      const int from = source.top + y;
      if (from < 0 || from >= static_cast<int>(image->height))
        continue;
      for (int x = 0; x < window.width; ++x)
        window.ink[static_cast<size_t>(y) * window.width + x] =
            image->rgba[(static_cast<size_t>(from) * image->width + x) * 4] / 255.0f;
    }
    Label(window);
  }
}

float LetteringCap(LetteringSize size) { return size == LetteringSize::LABEL ? 14.0f : 13.0f; }
float LetteringPress(LetteringSize size) { return size == LetteringSize::LABEL ? 1.0f : SMALL_PRESS; }
float LetteringTracking(LetteringSize size) { return size == LetteringSize::LABEL ? 0.0f : SMALL_TRACKING; }
float LetteringSpace(LetteringSize size) { return SPACE * LetteringCap(size) * LetteringPress(size); }

const Letter* GameLetter(char32_t c, LetteringSize size, float squeeze)
{
  const int key = static_cast<int>(std::lround(squeeze * 256));
  auto& slot = g_letters[{c, size, key}];
  if (!slot)
  {
    slot = std::make_unique<Letter>();
    const LetteringGlyph* glyph = Find(c, size);
    const float cap = LetteringCap(size), press = LetteringPress(size) * static_cast<float>(key) / 256;
    if (glyph != nullptr)
    {
      if (auto made = Build(c, size, cap, press))
      {
        slot->width = static_cast<uint32_t>(made->width);
        slot->coverage = std::move(made->ink);
        slot->left = glyph->left * cap * press;
        slot->right = glyph->right * cap * press;
      }
    }
  }
  return slot->width != 0 ? slot.get() : nullptr;
}
}  // namespace dualize::nights
