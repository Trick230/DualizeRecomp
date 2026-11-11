#include "sdk/nights_message_font.hpp"

#include <aurora/texture.hpp>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dualize::nights
{
namespace
{
// Dualize Sans SemiBold (resources/fonts/DualizeSans-OFL.txt) for the Latin,
// Dualize Gothic JP Bold (IBM Plex Sans JP cut to the atlas's characters,
// resources/fonts/DualizeGothicJP-OFL.txt) for the rest: of the free faces,
// the closest to the atlas's, at the em sizes that fit it best.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc23-extensions"
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr unsigned char LATIN_FACE[] = {
#embed "../resources/fonts/DualizeSans-SemiBold.ttf"
};
constexpr unsigned char JAPANESE_FACE[] = {
#embed "../resources/fonts/DualizeGothicJP-Bold.ttf"
};
#pragma clang diagnostic pop
constexpr float LATIN_EM = 16.0f, JAPANESE_EM = 19.25f;  // texels
constexpr char32_t LAST_LATIN = 0x24F;
constexpr char32_t FIRST_ICON = 0x391, LAST_ICON = 0x39D;  // Greek capitals the font draws pad buttons at

constexpr uint32_t CELL = 24, SCALE = 4, BIG = CELL * SCALE;
// The glyph cache: C4 with a palette, CACHE_COLUMNS by CACHE_ROWS cells.
constexpr uint32_t CACHE_FORMAT = 8, CACHE_WIDTH = 512, CACHE_HEIGHT = 256;
constexpr uint32_t CACHE_COLUMNS = CACHE_WIDTH / CELL, CACHE_ROWS = CACHE_HEIGHT / CELL;
constexpr uint32_t MIPS = 3;  // the sharp copy's levels: SCALE, SCALE / 2, SCALE / 4 times larger
constexpr std::array<uint8_t, 4> LEVELS{0x00, 0x62, 0xBD, 0xFF};
// A face's glyph off the atlas's by more (summed over the cell, over the
// atlas's ink) is not that glyph (an icon on its code point): the atlas's.
constexpr float MATCH_ERROR = 0.9f;
constexpr float FIT = 0.3f;    // a glyph narrowed (lowered) to its window by at most this
constexpr uint8_t INK = 8;     // coverage counted as ink when fitting
constexpr float CUT = 4.0f;    // error per texel of ink placed where the game cuts it
constexpr int REACH = 3;  // the registration's search, BIG texels each way
// Copies of the cache kept with the renderer (each new text makes another).
constexpr size_t KEPT = 3;

using Cell = std::array<uint8_t, CELL * CELL>;  // levels 0-3
// The part of a cell the game draws: (half_w + 1) texels either side of the
// middle across, (half_h + 1) up and down; the font's .inf half sizes.
struct Window
{
  int half_w, half_h;
};
using Coverage = std::vector<uint8_t>;          // BIG x BIG

std::string CellKey(const Cell& cell)
{
  std::string key(CELL * CELL / 4, '\0');
  for (size_t i = 0; i < cell.size(); ++i)
    key[i / 4] = static_cast<char>(key[i / 4] | cell[i] << (i % 4 * 2));
  return key;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

class MessageFont
{
public:
  bool Load(const std::filesystem::path& game)
  {
    const std::vector<uint8_t> inf = ReadFile(game / "files" / "fontJapan.inf");
    const std::vector<uint8_t> bin = ReadFile(game / "files" / "fontJapan.bin");
    if (inf.empty() || bin.empty())
      return false;
    const std::string_view text(reinterpret_cast<const char*>(inf.data()), inf.size());
    int width = 0, height = 0;
    if (const size_t at = text.find("bitmapSize :"); at != std::string_view::npos)
      std::sscanf(std::string(text.substr(at + 12, 32)).c_str(), "%d %d", &width, &height);
    if (width <= 0 || height <= 0 || bin.size() * 4 < static_cast<size_t>(width) * height)
      return false;
    const auto level = [&](int x, int y) {
      const size_t i = static_cast<size_t>(y) * width + x;
      return static_cast<uint8_t>(bin[i / 4] >> (i % 4 * 2) & 3);
    };
    size_t at = 0;
    while ((at = text.find("index :", at)) != std::string_view::npos)
    {
      const size_t end = text.find('\n', at);
      const std::string line(text.substr(at + 7, end == std::string_view::npos ? 64 : end - at - 7));
      at = end == std::string_view::npos ? text.size() : end;
      int half_w = 0, half_h = 0, cx = 0, cy = 0;
      unsigned code = 0;
      const size_t code_at = line.find("unicode 0x");
      if (std::sscanf(line.c_str(), "%d %d %d %d", &half_w, &half_h, &cx, &cy) != 4 || code_at == std::string::npos ||
          std::sscanf(line.c_str() + code_at + 10, "%x", &code) != 1 || code == 0)
        continue;
      const int x0 = cx - static_cast<int>(CELL / 2), y0 = cy - static_cast<int>(CELL / 2);
      if (x0 < 0 || y0 < 0 || x0 + static_cast<int>(CELL) > width || y0 + static_cast<int>(CELL) > height)
        continue;
      Cell cell{};
      for (uint32_t y = 0; y < CELL; ++y)
        for (uint32_t x = 0; x < CELL; ++x)
          cell[y * CELL + x] = level(x0 + static_cast<int>(x), y0 + static_cast<int>(y));
      m_by_cell.emplace(CellKey(cell), static_cast<char32_t>(code));
      m_cells.emplace(static_cast<char32_t>(code), cell);
      m_windows.emplace(static_cast<char32_t>(code), Window{half_w, half_h});
    }
    if (m_cells.empty())
      return false;
    stbtt_InitFont(&m_latin, LATIN_FACE, 0);
    stbtt_InitFont(&m_japanese, JAPANESE_FACE, 0);
    m_latin_scale = stbtt_ScaleForMappingEmToPixels(&m_latin, LATIN_EM * SCALE);
    m_japanese_scale = stbtt_ScaleForMappingEmToPixels(&m_japanese, JAPANESE_EM * SCALE);
    m_warm = std::jthread([this](std::stop_token stop) {
      for (const auto& [code, cell] : m_cells)
      {
        if (stop.stop_requested())
          return;
        Glyph(code);
      }
    });
    return true;
  }

  void Provide(const aurora::texture::TextureSource& source)
  {
    if (source.format != CACHE_FORMAT || source.width != CACHE_WIDTH || source.height != CACHE_HEIGHT ||
        source.data == nullptr || source.tlut == nullptr || source.tlutEntries < 16)
      return;
    const auto palette = Palette(source);
    std::array<uint8_t, 16> level{};
    for (size_t i = 0; i < 16; ++i)
    {
      const uint8_t alpha = palette[i][3];
      level[i] = static_cast<uint8_t>(std::min_element(LEVELS.begin(), LEVELS.end(), [&](uint8_t a, uint8_t b) {
                                        return std::abs(a - alpha) < std::abs(b - alpha);
                                      }) -
                                      LEVELS.begin());
    }
    // The cache's cells, as atlas levels.
    const auto* data = static_cast<const uint8_t*>(source.data);
    std::vector<uint8_t> index(static_cast<size_t>(CACHE_WIDTH) * CACHE_HEIGHT);
    for (uint32_t by = 0; by < CACHE_HEIGHT / 8; ++by)
      for (uint32_t bx = 0; bx < CACHE_WIDTH / 8; ++bx)
      {
        const uint8_t* block = data + (static_cast<size_t>(by) * (CACHE_WIDTH / 8) + bx) * 32;
        for (uint32_t y = 0; y < 8; ++y)
          for (uint32_t x = 0; x < 8; ++x)
          {
            const uint8_t byte = block[y * 4 + x / 2];
            index[static_cast<size_t>(by * 8 + y) * CACHE_WIDTH + bx * 8 + x] = x % 2 == 0 ? byte >> 4 : byte & 15;
          }
      }
    std::vector<std::optional<Cell>> cells(CACHE_COLUMNS * CACHE_ROWS);
    size_t inked = 0, known = 0;
    for (uint32_t r = 0; r < CACHE_ROWS; ++r)
      for (uint32_t c = 0; c < CACHE_COLUMNS; ++c)
      {
        Cell cell{};
        bool ink = false;
        for (uint32_t y = 0; y < CELL; ++y)
          for (uint32_t x = 0; x < CELL; ++x)
          {
            const uint8_t l = level[index[static_cast<size_t>(r * CELL + y) * CACHE_WIDTH + c * CELL + x]];
            cell[y * CELL + x] = l;
            ink |= l != 0;
          }
        if (!ink)
          continue;
        ++inked;
        known += m_by_cell.contains(CellKey(cell));
        cells[r * CACHE_COLUMNS + c] = cell;
      }
    if (inked == 0 || known * 5 < inked * 4)  // not the message font's cache
      return;

    // Its sharp copy, the cells that changed redrawn.
    const auto lowest = std::min_element(palette.begin(), palette.end(), [](auto& a, auto& b) { return a[3] < b[3]; });
    const auto highest = std::max_element(palette.begin(), palette.end(), [](auto& a, auto& b) { return a[3] < b[3]; });
    const std::array<uint8_t, 4> ink_color = *highest, paper = *lowest;
    if (m_copy.empty() || ink_color != m_ink || paper != m_paper)
    {
      m_copy.assign(ChainBytes(), 0);
      m_drawn.assign(cells.size(), std::nullopt);
      m_ink = ink_color;
      m_paper = paper;
      Fill(paper);
    }
    for (size_t i = 0; i < cells.size(); ++i)
    {
      if (cells[i] == m_drawn[i])
        continue;
      const uint32_t col = static_cast<uint32_t>(i % CACHE_COLUMNS), row = static_cast<uint32_t>(i / CACHE_COLUMNS);
      if (!cells[i])
        DrawCell(col, row, nullptr);
      else
      {
        const auto found = m_by_cell.find(CellKey(*cells[i]));
        const auto coverage = found != m_by_cell.end() ? Glyph(found->second) : Upscale(*cells[i]);
        DrawCell(col, row, coverage.get());
      }
      m_drawn[i] = cells[i];
    }
    aurora::texture::set_memory_replacement(source.key, CACHE_WIDTH * SCALE, CACHE_HEIGHT * SCALE, MIPS, m_copy);
    m_kept.push_back(source.key);
    if (m_kept.size() > KEPT)
    {
      aurora::texture::remove_memory_replacement(m_kept.front());
      m_kept.erase(m_kept.begin());
    }
  }

private:
  static std::array<std::array<uint8_t, 4>, 16> Palette(const aurora::texture::TextureSource& source)
  {
    std::array<std::array<uint8_t, 4>, 16> out{};
    const auto* p = static_cast<const uint8_t*>(source.tlut);
    for (size_t i = 0; i < out.size(); ++i)
    {
      const uint16_t v = static_cast<uint16_t>(p[i * 2] << 8 | p[i * 2 + 1]);
      switch (source.tlutFormat)
      {
      case 0:  // IA8: alpha, then intensity
        out[i] = {p[i * 2 + 1], p[i * 2 + 1], p[i * 2 + 1], p[i * 2]};
        break;
      case 1:  // RGB565
        out[i] = {static_cast<uint8_t>((v >> 11) * 255 / 31), static_cast<uint8_t>((v >> 5 & 63) * 255 / 63),
                  static_cast<uint8_t>((v & 31) * 255 / 31), 255};
        break;
      default:  // RGB5A3
        if (v & 0x8000)
          out[i] = {static_cast<uint8_t>((v >> 10 & 31) * 255 / 31), static_cast<uint8_t>((v >> 5 & 31) * 255 / 31),
                    static_cast<uint8_t>((v & 31) * 255 / 31), 255};
        else
          out[i] = {static_cast<uint8_t>((v >> 8 & 15) * 17), static_cast<uint8_t>((v >> 4 & 15) * 17),
                    static_cast<uint8_t>((v & 15) * 17), static_cast<uint8_t>((v >> 12 & 7) * 255 / 7)};
      }
    }
    return out;
  }

  static size_t ChainBytes()
  {
    size_t bytes = 0;
    for (uint32_t m = 0; m < MIPS; ++m)
      bytes += static_cast<size_t>(CACHE_WIDTH * SCALE >> m) * (CACHE_HEIGHT * SCALE >> m) * 4;
    return bytes;
  }

  void Fill(const std::array<uint8_t, 4>& color)
  {
    for (size_t i = 0; i < m_copy.size(); i += 4)
      std::memcpy(&m_copy[i], color.data(), 4);
  }

  // A cell of every level of the copy: `coverage` (BIG x BIG) between the
  // palette's paper and ink, averaged down for the smaller levels.
  void DrawCell(uint32_t col, uint32_t row, const Coverage* coverage)
  {
    size_t level_at = 0;
    std::vector<float> cover(static_cast<size_t>(BIG) * BIG, 0.0f);
    if (coverage != nullptr)
      for (size_t i = 0; i < cover.size(); ++i)
        cover[i] = (*coverage)[i] / 255.0f;
    uint32_t size = BIG;
    for (uint32_t m = 0; m < MIPS; ++m)
    {
      const uint32_t width = CACHE_WIDTH * SCALE >> m;
      for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
          const float c = cover[static_cast<size_t>(y) * size + x];
          uint8_t* p = &m_copy[level_at + (static_cast<size_t>(row * size + y) * width + col * size + x) * 4];
          for (int k = 0; k < 4; ++k)
            p[k] = static_cast<uint8_t>(std::lround(m_paper[k] + (m_ink[k] - m_paper[k]) * c));
        }
      level_at += static_cast<size_t>(width) * (CACHE_HEIGHT * SCALE >> m) * 4;
      if (m + 1 < MIPS)
      {
        const uint32_t half = size / 2;
        std::vector<float> next(static_cast<size_t>(half) * half);
        for (uint32_t y = 0; y < half; ++y)
          for (uint32_t x = 0; x < half; ++x)
            next[static_cast<size_t>(y) * half + x] =
                0.25f * (cover[(2 * y) * size + 2 * x] + cover[(2 * y) * size + 2 * x + 1] +
                         cover[(2 * y + 1) * size + 2 * x] + cover[(2 * y + 1) * size + 2 * x + 1]);
        cover = std::move(next);
        size = half;
      }
    }
  }

  // The glyph of `code` at BIG x BIG, placed as the atlas has it: drawn by
  // its face, else the atlas's own enlarged. Made once.
  std::shared_ptr<const Coverage> Glyph(char32_t code)
  {
    {
      std::lock_guard lock(m_mutex);
      if (const auto it = m_glyphs.find(code); it != m_glyphs.end())
        return it->second;
    }
    const Cell& cell = m_cells.at(code);
    std::shared_ptr<const Coverage> made = Draw(code, cell, m_windows.at(code));
    if (!made)
    if (!made)
      made = Upscale(cell);
    std::lock_guard lock(m_mutex);
    return m_glyphs.emplace(code, std::move(made)).first->second;
  }

  std::shared_ptr<const Coverage> Draw(char32_t code, const Cell& cell, const Window& window) const
  {
    if (code >= FIRST_ICON && code <= LAST_ICON)
      return nullptr;
    const bool latin = code <= LAST_LATIN;
    const stbtt_fontinfo& face = latin ? m_latin : m_japanese;
    const float scale = latin ? m_latin_scale : m_japanese_scale;
    if (stbtt_FindGlyphIndex(&face, static_cast<int>(code)) == 0)
      return nullptr;
    constexpr int PAD = static_cast<int>(BIG);
    const int side = static_cast<int>(BIG) + 2 * PAD;
    std::vector<uint8_t> canvas(static_cast<size_t>(side) * side);
    // The glyph centred on a canvas with room to slide a cell's window over,
    // `kx` and `ky` times as wide and high.
    const auto render = [&](float kx, float ky) {
      std::fill(canvas.begin(), canvas.end(), 0);
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      stbtt_GetCodepointBitmapBox(&face, static_cast<int>(code), scale * kx, scale * ky, &x0, &y0, &x1, &y1);
      const int w = x1 - x0, h = y1 - y0;
      if (w <= 0 || h <= 0 || w > side || h > side)
        return false;
      stbtt_MakeCodepointBitmap(&face, &canvas[static_cast<size_t>((side - h) / 2) * side + (side - w) / 2], w, h, side,
                                scale * kx, scale * ky, static_cast<int>(code));
      return true;
    };
    // The ink's span across and down, in atlas texels.
    const auto span = [&](bool across) {
      int first = side, last = -1;
      for (int i = 0; i < side; ++i)
        for (int j = 0; j < side; ++j)
          if ((across ? canvas[static_cast<size_t>(j) * side + i] : canvas[static_cast<size_t>(i) * side + j]) > INK)
          {
            first = std::min(first, i);
            last = i;
            break;
          }
      return last < first ? 0.0f : static_cast<float>(last - first + 1) / SCALE;
    };
    const int left = static_cast<int>(CELL / 2) - 1 - window.half_w, right = static_cast<int>(CELL / 2) + 1 + window.half_w;
    const int top = static_cast<int>(CELL / 2) - 1 - window.half_h, bottom = static_cast<int>(CELL / 2) + 1 + window.half_h;
    // A glyph wider (higher) than the part of its cell the game draws
    // narrowed (lowered) to fit, the face's outlines otherwise kept.
    if (!render(1, 1))
      return nullptr;
    const float kx = std::clamp(static_cast<float>(right - left) / std::max(0.5f, span(true)), 1 - FIT, 1.0f);
    const float ky = std::clamp(static_cast<float>(bottom - top) / std::max(0.5f, span(false)), 1 - FIT, 1.0f);
    if ((kx < 1 || ky < 1) && !render(kx, ky))
      return nullptr;
    // Summed area, for the window's texels at the atlas's size.
    std::vector<uint32_t> sum(static_cast<size_t>(side + 1) * (side + 1), 0);
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x)
        sum[static_cast<size_t>(y + 1) * (side + 1) + x + 1] = canvas[static_cast<size_t>(y) * side + x] +
                                                                 sum[static_cast<size_t>(y) * (side + 1) + x + 1] +
                                                                 sum[static_cast<size_t>(y + 1) * (side + 1) + x] -
                                                                 sum[static_cast<size_t>(y) * (side + 1) + x];
    const auto box = [&](int x, int y) {  // a texel at the atlas's size, the window's corner at (x, y)
      const auto at = [&](int xx, int yy) { return static_cast<int64_t>(sum[static_cast<size_t>(yy) * (side + 1) + xx]); };
      return static_cast<float>(at(x + SCALE, y + SCALE) - at(x, y + SCALE) - at(x + SCALE, y) + at(x, y)) /
             (255.0f * SCALE * SCALE);
    };
    // Where the window goes: the ink's middles over each other, then the
    // nearest fit around.
    double ax = 0, ay = 0, am = 0, cx = 0, cy = 0, cm = 0;
    for (uint32_t y = 0; y < CELL; ++y)
      for (uint32_t x = 0; x < CELL; ++x)
      {
        const double v = LEVELS[cell[y * CELL + x]] / 255.0;
        ax += v * (x + 0.5);
        ay += v * (y + 0.5);
        am += v;
      }
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x)
      {
        const double v = canvas[static_cast<size_t>(y) * side + x];
        cx += v * (x + 0.5);
        cy += v * (y + 0.5);
        cm += v;
      }
    if (am <= 0 || cm <= 0)
      return nullptr;
    const int gx = static_cast<int>(std::lround(cx / cm - ax / am * SCALE));
    const int gy = static_cast<int>(std::lround(cy / cm - ay / am * SCALE));
    float best = 0;
    int bx = gx, by = gy;
    bool first = true;
    for (int dy = -REACH; dy <= REACH; ++dy)
      for (int dx = -REACH; dx <= REACH; ++dx)
      {
        const int ox = gx + dx, oy = gy + dy;
        if (ox < 0 || oy < 0 || ox + static_cast<int>(BIG) > side || oy + static_cast<int>(BIG) > side)
          continue;
        float error = 0;
        for (uint32_t y = 0; y < CELL; ++y)
          for (uint32_t x = 0; x < CELL; ++x)
          {
            const float ours = box(ox + static_cast<int>(x * SCALE), oy + static_cast<int>(y * SCALE));
            const bool drawn = static_cast<int>(x) >= left && static_cast<int>(x) < right && static_cast<int>(y) >= top &&
                               static_cast<int>(y) < bottom;
            error += drawn ? std::abs(ours - LEVELS[cell[y * CELL + x]] / 255.0f) : CUT * ours;
          }
        if (first || error < best)
        {
          best = error;
          bx = ox;
          by = oy;
          first = false;
        }
      }
    if (first || best > MATCH_ERROR * static_cast<float>(am))
      return nullptr;
    auto out = std::make_shared<Coverage>(static_cast<size_t>(BIG) * BIG);
    for (uint32_t y = 0; y < BIG; ++y)
      std::memcpy(&(*out)[static_cast<size_t>(y) * BIG], &canvas[static_cast<size_t>(by + static_cast<int>(y)) * side + bx], BIG);
    return out;
  }

  // The atlas's own cell, enlarged smoothly (Catmull-Rom).
  static std::shared_ptr<const Coverage> Upscale(const Cell& cell)
  {
    const auto at = [&](int x, int y) {
      x = std::clamp(x, 0, static_cast<int>(CELL) - 1);
      y = std::clamp(y, 0, static_cast<int>(CELL) - 1);
      return LEVELS[cell[static_cast<size_t>(y) * CELL + x]] / 255.0f;
    };
    const auto weights = [](float t) {
      return std::array<float, 4>{((-0.5f * t + 1.0f) * t - 0.5f) * t, (1.5f * t - 2.5f) * t * t + 1.0f,
                                  ((-1.5f * t + 2.0f) * t + 0.5f) * t, (0.5f * t - 0.5f) * t * t};
    };
    auto out = std::make_shared<Coverage>(static_cast<size_t>(BIG) * BIG);
    for (uint32_t y = 0; y < BIG; ++y)
    {
      const float v = (y + 0.5f) / SCALE - 0.5f;
      const int vy = static_cast<int>(std::floor(v));
      const auto wy = weights(v - static_cast<float>(vy));
      for (uint32_t x = 0; x < BIG; ++x)
      {
        const float u = (x + 0.5f) / SCALE - 0.5f;
        const int ux = static_cast<int>(std::floor(u));
        const auto wx = weights(u - static_cast<float>(ux));
        float c = 0;
        for (int j = 0; j < 4; ++j)
          for (int i = 0; i < 4; ++i)
            c += wy[j] * wx[i] * at(ux - 1 + i, vy - 1 + j);
        (*out)[static_cast<size_t>(y) * BIG + x] = static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255));
      }
    }
    return out;
  }

  std::unordered_map<std::string, char32_t> m_by_cell;
  std::unordered_map<char32_t, Cell> m_cells;
  std::unordered_map<char32_t, Window> m_windows;
  stbtt_fontinfo m_latin{}, m_japanese{};
  float m_latin_scale = 1, m_japanese_scale = 1;
  std::mutex m_mutex;
  std::unordered_map<char32_t, std::shared_ptr<const Coverage>> m_glyphs;
  // The sharp copy (render thread only): every level, the cells it has.
  std::vector<uint8_t> m_copy;
  std::vector<std::optional<Cell>> m_drawn;
  std::array<uint8_t, 4> m_ink{}, m_paper{};
  std::vector<aurora::texture::TextureSourceKey> m_kept;
  std::jthread m_warm;  // last: stops before the rest goes
};

MessageFont g_font;
}  // namespace

void LoadMessageFont(const std::filesystem::path& game)
{
  if (!g_font.Load(game))
    return;
  aurora::texture::set_replacement_provider(
      [](const aurora::texture::TextureSource& source, void*) { g_font.Provide(source); }, nullptr);
}
}  // namespace dualize::nights
