#include "ui/game_assets.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace dualize
{
namespace
{
std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint16_t Be16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

uint32_t Be32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

using Rgba = std::array<uint8_t, 4>;

Rgba Rgb565(uint16_t v)
{
  return {static_cast<uint8_t>((v >> 11 & 31) * 255 / 31), static_cast<uint8_t>((v >> 5 & 63) * 255 / 63),
          static_cast<uint8_t>((v & 31) * 255 / 31), 255};
}

Rgba Rgb5a3(uint16_t v)
{
  if (v & 0x8000)
    return {static_cast<uint8_t>((v >> 10 & 31) * 255 / 31), static_cast<uint8_t>((v >> 5 & 31) * 255 / 31),
            static_cast<uint8_t>((v & 31) * 255 / 31), 255};
  return {static_cast<uint8_t>((v >> 8 & 15) * 17), static_cast<uint8_t>((v >> 4 & 15) * 17),
          static_cast<uint8_t>((v & 15) * 17), static_cast<uint8_t>((v >> 12 & 7) * 255 / 7)};
}

// GX texture formats: their block size and bytes per block.
struct Format
{
  uint32_t id, block_w, block_h, block_bytes;
};
constexpr Format FORMATS[] = {
    {0, 8, 8, 32},   // I4
    {1, 8, 4, 32},   // I8
    {2, 8, 4, 32},   // IA4
    {3, 4, 4, 32},   // IA8
    {4, 4, 4, 32},   // RGB565
    {5, 4, 4, 32},   // RGB5A3
    {6, 4, 4, 64},   // RGBA8
    {14, 8, 8, 32},  // CMPR
};

// Decodes one block at `src` into the image at (bx, by).
void DecodeBlock(const Format& f, const uint8_t* src, Image& image, uint32_t bx, uint32_t by)
{
  const auto put = [&](uint32_t x, uint32_t y, const Rgba& c) {
    if (bx + x < image.width && by + y < image.height)
      std::memcpy(&image.rgba[((by + y) * image.width + bx + x) * 4], c.data(), 4);
  };
  switch (f.id)
  {
  case 0:
    for (uint32_t i = 0; i < 64; ++i)
    {
      const uint8_t v = static_cast<uint8_t>((src[i / 2] >> (i & 1 ? 0 : 4) & 15) * 17);
      put(i % 8, i / 8, {v, v, v, v});
    }
    break;
  case 1:
    for (uint32_t i = 0; i < 32; ++i)
      put(i % 8, i / 8, {src[i], src[i], src[i], src[i]});
    break;
  case 2:
    for (uint32_t i = 0; i < 32; ++i)
    {
      const uint8_t v = static_cast<uint8_t>((src[i] & 15) * 17);
      put(i % 8, i / 8, {v, v, v, static_cast<uint8_t>((src[i] >> 4) * 17)});
    }
    break;
  case 3:
    for (uint32_t i = 0; i < 16; ++i)
      put(i % 4, i / 4, {src[2 * i + 1], src[2 * i + 1], src[2 * i + 1], src[2 * i]});
    break;
  case 4:
    for (uint32_t i = 0; i < 16; ++i)
      put(i % 4, i / 4, Rgb565(Be16(src + 2 * i)));
    break;
  case 5:
    for (uint32_t i = 0; i < 16; ++i)
      put(i % 4, i / 4, Rgb5a3(Be16(src + 2 * i)));
    break;
  case 6:
    for (uint32_t i = 0; i < 16; ++i)
      put(i % 4, i / 4, {src[2 * i + 1], src[32 + 2 * i], src[32 + 2 * i + 1], src[2 * i]});
    break;
  case 14:
    for (uint32_t sub = 0; sub < 4; ++sub)
    {
      const uint8_t* s = src + sub * 8;
      const uint16_t c0 = Be16(s), c1 = Be16(s + 2);
      const Rgba a = Rgb565(c0), b = Rgb565(c1);
      std::array<Rgba, 4> palette{a, b};
      for (int k = 0; k < 3; ++k)
      {
        if (c0 > c1)
        {
          palette[2][k] = static_cast<uint8_t>((2 * a[k] + b[k]) / 3);
          palette[3][k] = static_cast<uint8_t>((a[k] + 2 * b[k]) / 3);
        }
        else
        {
          palette[2][k] = static_cast<uint8_t>((a[k] + b[k]) / 2);
          palette[3][k] = 0;
        }
      }
      palette[2][3] = 255;
      palette[3][3] = c0 > c1 ? 255 : 0;
      const uint32_t bits = Be32(s + 4);
      for (uint32_t i = 0; i < 16; ++i)
        put(sub % 2 * 4 + i % 4, sub / 2 * 4 + i / 4, palette[bits >> (30 - 2 * i) & 3]);
    }
    break;
  }
}

int ParseInt(std::string_view& text)
{
  while (!text.empty() && text.front() == ' ')
    text.remove_prefix(1);
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  text.remove_prefix(static_cast<size_t>(end - text.data()));
  return error == std::errc{} ? value : -1;
}
}  // namespace

std::optional<Image> LoadTplImage(const std::filesystem::path& path, uint32_t index)
{
  const std::vector<uint8_t> data = ReadFile(path);
  auto decoded = DecodeTplImage(data, index);
  if (!decoded)
    return std::nullopt;
  return std::move(decoded->image);
}

std::optional<TplImage> DecodeTplImage(std::span<const uint8_t> data, uint32_t index)
{
  if (data.size() < 12 || Be32(data.data()) != 0x0020AF30)
    return std::nullopt;
  const uint32_t count = Be32(&data[4]), table = Be32(&data[8]);
  if (index >= count || table + 8 * (index + 1) > data.size())
    return std::nullopt;
  const uint32_t header = Be32(&data[table + 8 * index]);
  if (header + 12 > data.size())
    return std::nullopt;
  TplImage tpl;
  Image& image = tpl.image;
  image.height = Be16(&data[header]);
  image.width = Be16(&data[header + 2]);
  const uint32_t format = Be32(&data[header + 4]), offset = Be32(&data[header + 8]);
  const Format* f = nullptr;
  for (const Format& candidate : FORMATS)
  {
    if (candidate.id == format)
      f = &candidate;
  }
  if (f == nullptr)
    return std::nullopt;
  const uint32_t blocks_x = (image.width + f->block_w - 1) / f->block_w;
  const uint32_t blocks_y = (image.height + f->block_h - 1) / f->block_h;
  if (offset + static_cast<size_t>(blocks_x) * blocks_y * f->block_bytes > data.size())
    return std::nullopt;
  image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
  const uint8_t* src = &data[offset];
  for (uint32_t y = 0; y < blocks_y; ++y)
  {
    for (uint32_t x = 0; x < blocks_x; ++x, src += f->block_bytes)
      DecodeBlock(*f, src, image, x * f->block_w, y * f->block_h);
  }
  tpl.format = format;
  tpl.offset = offset;
  return tpl;
}

void EncodeGreyCmpr(const Image& image, uint32_t y0, uint32_t y1, uint8_t* data)
{
  const auto grey565 = [](int v) {
    const int r = (v * 31 + 127) / 255, g = (v * 63 + 127) / 255;
    return static_cast<uint16_t>(r << 11 | g << 5 | r);
  };
  const uint32_t blocks_x = (image.width + 7) / 8;
  for (uint32_t by = y0 / 8; by < (y1 + 7) / 8 && by * 8 < image.height; ++by)
  {
    for (uint32_t bx = 0; bx < blocks_x; ++bx)
    {
      uint8_t* block = data + (static_cast<size_t>(by) * blocks_x + bx) * 32;
      for (uint32_t sub = 0; sub < 4; ++sub)
      {
        const uint32_t sx = bx * 8 + sub % 2 * 4, sy = by * 8 + sub / 2 * 4;
        std::array<int, 16> grey{};
        int lo = 255, hi = 0;
        for (uint32_t i = 0; i < 16; ++i)
        {
          const uint32_t x = std::min(sx + i % 4, image.width - 1), y = std::min(sy + i / 4, image.height - 1);
          grey[i] = image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
          lo = std::min(lo, grey[i]);
          hi = std::max(hi, grey[i]);
        }
        const uint16_t c0 = grey565(hi), c1 = grey565(lo);
        // The levels the GPU decodes them to (four when c0 > c1).
        const int a = Rgb565(c0)[0], b = Rgb565(c1)[0];
        const std::array<int, 4> levels{a, b, (2 * a + b) / 3, (a + 2 * b) / 3};
        uint32_t bits = 0;
        for (uint32_t i = 0; i < 16; ++i)
        {
          uint32_t best = 0;
          if (c0 > c1)
          {
            for (uint32_t k = 1; k < 4; ++k)
            {
              if (std::abs(levels[k] - grey[i]) < std::abs(levels[best] - grey[i]))
                best = k;
            }
          }
          bits |= best << (30 - 2 * i);
        }
        uint8_t* s = block + sub * 8;
        s[0] = static_cast<uint8_t>(c0 >> 8);
        s[1] = static_cast<uint8_t>(c0);
        s[2] = static_cast<uint8_t>(c1 >> 8);
        s[3] = static_cast<uint8_t>(c1);
        s[4] = static_cast<uint8_t>(bits >> 24);
        s[5] = static_cast<uint8_t>(bits >> 16);
        s[6] = static_cast<uint8_t>(bits >> 8);
        s[7] = static_cast<uint8_t>(bits);
      }
    }
  }
}

void EncodeRgb5a3(const Image& image, uint32_t y0, uint32_t y1, uint8_t* data)
{
  const auto scale = [](uint8_t v, int max) { return (v * max + 127) / 255; };
  const uint32_t blocks_x = (image.width + 3) / 4;
  for (uint32_t by = y0 / 4; by < (y1 + 3) / 4 && by * 4 < image.height; ++by)
  {
    for (uint32_t bx = 0; bx < blocks_x; ++bx)
    {
      uint8_t* block = data + (static_cast<size_t>(by) * blocks_x + bx) * 32;
      for (uint32_t i = 0; i < 16; ++i)
      {
        const uint32_t x = std::min(bx * 4 + i % 4, image.width - 1), y = std::min(by * 4 + i / 4, image.height - 1);
        const uint8_t* p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
        const int alpha = scale(p[3], 7);
        const uint16_t v = alpha == 7
                               ? static_cast<uint16_t>(0x8000 | scale(p[0], 31) << 10 | scale(p[1], 31) << 5 | scale(p[2], 31))
                               : static_cast<uint16_t>(alpha << 12 | scale(p[0], 15) << 8 | scale(p[1], 15) << 4 | scale(p[2], 15));
        block[2 * i] = static_cast<uint8_t>(v >> 8);
        block[2 * i + 1] = static_cast<uint8_t>(v);
      }
    }
  }
}

// <base>.inf is text: "bitmapSize : W H", "fontSize : N N", a palette of four
// grey levels and one "index : halfW halfH centerX centerY // ... unicode 0xXXXX"
// line per glyph. <base>.bin is the atlas, 2 bits a pixel, low bits first.
bool BitmapFont::Load(const std::filesystem::path& base)
{
  std::filesystem::path inf_path = base, bin_path = base;
  inf_path += ".inf";
  bin_path += ".bin";
  const std::vector<uint8_t> inf = ReadFile(inf_path);
  const std::vector<uint8_t> bin = ReadFile(bin_path);
  if (inf.empty() || bin.empty())
    return false;
  const std::string_view text(reinterpret_cast<const char*>(inf.data()), inf.size());
  const auto field = [&](std::string_view key) {
    const size_t at = text.find(key);
    std::string_view rest = at == std::string_view::npos ? std::string_view{} : text.substr(at + key.size());
    return rest;
  };
  std::string_view size = field("bitmapSize :");
  const int width = ParseInt(size), height = ParseInt(size);
  std::string_view cell = field("fontSize :");
  m_cell = static_cast<float>(ParseInt(cell));
  if (width <= 0 || height <= 0 || m_cell <= 0 || bin.size() * 4 < static_cast<size_t>(width) * height)
    return false;

  constexpr std::array<uint8_t, 4> LEVELS{0x00, 0x62, 0xBD, 0xFF};
  m_atlas.width = static_cast<uint32_t>(width);
  m_atlas.height = static_cast<uint32_t>(height);
  m_atlas.rgba.assign(static_cast<size_t>(width) * height * 4, 255);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
    m_atlas.rgba[i * 4 + 3] = LEVELS[bin[i / 4] >> (i % 4 * 2) & 3];

  m_glyphs.clear();
  size_t at = 0;
  while ((at = text.find("index :", at)) != std::string_view::npos)
  {
    const size_t end = text.find('\n', at);
    std::string_view line = text.substr(at + 7, end == std::string_view::npos ? std::string_view::npos : end - at - 7);
    at = end == std::string_view::npos ? text.size() : end;
    const int half_w = ParseInt(line), half_h = ParseInt(line), cx = ParseInt(line), cy = ParseInt(line);
    const size_t code_at = line.find("unicode 0x");
    if (half_w < 0 || half_h < 0 || code_at == std::string_view::npos)
      continue;
    uint32_t code = 0;
    const std::string_view hex = line.substr(code_at + 10);
    std::from_chars(hex.data(), hex.data() + hex.size(), code, 16);
    if (code == 0)
      continue;
    const float half_cell = m_cell * 0.5f;
    m_glyphs.emplace(static_cast<char32_t>(code),
                     Glyph{static_cast<float>(cx - half_w), static_cast<float>(cy) - half_cell,
                           static_cast<float>(cx + half_w), static_cast<float>(cy) + half_cell});
  }
  return !m_glyphs.empty();
}

const BitmapFont::Glyph* BitmapFont::Find(char32_t c) const
{
  const auto it = m_glyphs.find(c);
  return it == m_glyphs.end() ? nullptr : &it->second;
}
}  // namespace dualize
