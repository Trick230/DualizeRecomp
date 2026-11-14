#include "ui/game_assets.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

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

std::optional<TplImage> DecodeTplImage(std::span<const uint8_t> data, uint32_t index, bool pixels)
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
  tpl.format = format;
  tpl.offset = offset;
  if (!pixels)
    return tpl;
  image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
  const uint8_t* src = &data[offset];
  for (uint32_t y = 0; y < blocks_y; ++y)
  {
    for (uint32_t x = 0; x < blocks_x; ++x, src += f->block_bytes)
      DecodeBlock(*f, src, image, x * f->block_w, y * f->block_h);
  }
  return tpl;
}

namespace
{
// fn(y0, y1) over [0, rows) in bands on a few threads (the calling one
// among them): a menu builds its sharp copies while the game waits.
template <typename Fn>
void ForRows(uint32_t rows, Fn&& fn)
{
  const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  const uint32_t step = (rows + threads - 1) / threads;
  std::vector<std::thread> pool;
  for (uint32_t y0 = step; y0 < rows; y0 += step)
    pool.emplace_back([&fn, y0, y1 = std::min(rows, y0 + step)] { fn(y0, y1); });
  fn(0u, std::min(rows, step));
  for (std::thread& thread : pool)
    thread.join();
}
}  // namespace

Image SharpenGreyMask(const Image& image, uint32_t scale, uint32_t band)
{
  const uint32_t w = image.width, h = image.height, W = w * scale, H = h * scale;
  Image out{W, H, {}};
  if (w == 0 || h == 0 || scale == 0 || band == 0)
    return out;
  // Each band at its own full brightness.
  std::vector<float> grey(static_cast<size_t>(w) * h), peak(h, 1.0f);
  for (uint32_t y0 = 0; y0 < h; y0 += band)
  {
    float most = 0.0f;
    for (uint32_t y = y0; y < std::min(h, y0 + band); ++y)
      for (uint32_t x = 0; x < w; ++x)
        most = std::max(most, image.rgba[(static_cast<size_t>(y) * w + x) * 4] / 255.0f);
    for (uint32_t y = y0; y < std::min(h, y0 + band); ++y)
    {
      peak[y] = most > 0.25f ? most : 1.0f;
      for (uint32_t x = 0; x < w; ++x)
        grey[static_cast<size_t>(y) * w + x] = image.rgba[(static_cast<size_t>(y) * w + x) * 4] / 255.0f / peak[y];
    }
  }
  // Catmull-Rom weights at each output texel's centre, then a blur (sigma
  // half a source texel) that hides the 4-level CMPR steps and evens the
  // outlines.
  const auto weights = [scale](uint32_t i, uint32_t n, std::array<int, 4>& at, std::array<float, 4>& wt) {
    const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(scale) - 0.5f;
    const int base = static_cast<int>(std::floor(u));
    const float t = u - static_cast<float>(base);
    wt = {((-0.5f * t + 1.0f) * t - 0.5f) * t, (1.5f * t - 2.5f) * t * t + 1.0f,
          ((-1.5f * t + 2.0f) * t + 0.5f) * t, (0.5f * t - 0.5f) * t * t};
    for (int k = 0; k < 4; ++k)
      at[k] = std::clamp(base - 1 + k, 0, static_cast<int>(n) - 1);
  };
  std::vector<float> wide(static_cast<size_t>(W) * h), up(static_cast<size_t>(W) * H);
  std::vector<std::array<int, 4>> column_at(W);
  std::vector<std::array<float, 4>> column_wt(W);
  for (uint32_t x = 0; x < W; ++x)
    weights(x, w, column_at[x], column_wt[x]);
  ForRows(h, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y)
    {
      const float* row = &grey[static_cast<size_t>(y) * w];
      for (uint32_t x = 0; x < W; ++x)
      {
        const std::array<int, 4>& at = column_at[x];
        const std::array<float, 4>& wt = column_wt[x];
        wide[static_cast<size_t>(y) * W + x] = wt[0] * row[at[0]] + wt[1] * row[at[1]] + wt[2] * row[at[2]] + wt[3] * row[at[3]];
      }
    }
  });
  ForRows(H, [&](uint32_t y0, uint32_t y1) {
    std::array<int, 4> at;
    std::array<float, 4> wt;
    for (uint32_t y = y0; y < y1; ++y)
    {
      weights(y, h, at, wt);
      for (uint32_t x = 0; x < W; ++x)
        up[static_cast<size_t>(y) * W + x] = wt[0] * wide[static_cast<size_t>(at[0]) * W + x] +
                                             wt[1] * wide[static_cast<size_t>(at[1]) * W + x] +
                                             wt[2] * wide[static_cast<size_t>(at[2]) * W + x] +
                                             wt[3] * wide[static_cast<size_t>(at[3]) * W + x];
    }
  });
  const float sigma = static_cast<float>(scale) / 2.0f;
  const int reach = static_cast<int>(std::ceil(sigma * 3.0f));
  std::vector<float> kernel(static_cast<size_t>(2 * reach + 1));
  float sum = 0.0f;
  for (int k = -reach; k <= reach; ++k)
    sum += kernel[static_cast<size_t>(k + reach)] = std::exp(-0.5f * static_cast<float>(k * k) / (sigma * sigma));
  for (float& k : kernel)
    k /= sum;
  // Both passes add their taps in order a whole row at a time (the rows
  // vectorize; only the edges clamp).
  std::vector<float> blur(up.size());
  const int last_x = static_cast<int>(W) - 1, last_y = static_cast<int>(H) - 1;
  ForRows(H, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y)
    {
      const float* from = &up[static_cast<size_t>(y) * W];
      float* to = &blur[static_cast<size_t>(y) * W];
      std::fill_n(to, W, 0.0f);
      for (int k = -reach; k <= reach; ++k)
      {
        const float weight = kernel[static_cast<size_t>(k + reach)];
        const int lo = std::clamp(-k, 0, last_x + 1), hi = std::clamp(last_x + 1 - k, lo, last_x + 1);
        for (int x = 0; x < lo; ++x)
          to[x] += weight * from[std::clamp(x + k, 0, last_x)];
        for (int x = lo; x < hi; ++x)
          to[x] += weight * from[x + k];
        for (int x = hi; x <= last_x; ++x)
          to[x] += weight * from[std::clamp(x + k, 0, last_x)];
      }
    }
  });
  ForRows(H, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y)
    {
      float* to = &up[static_cast<size_t>(y) * W];
      std::fill_n(to, W, 0.0f);
      for (int k = -reach; k <= reach; ++k)
      {
        const float weight = kernel[static_cast<size_t>(k + reach)];
        const float* from = &blur[static_cast<size_t>(std::clamp(static_cast<int>(y) + k, 0, last_y)) * W];
        for (uint32_t x = 0; x < W; ++x)
          to[x] += weight * from[x];
      }
    }
  });
  // Cut just under half coverage (the blur thins strokes a little), the soft
  // edge EDGE output texels wide everywhere: the field over its own slope.
  constexpr float CUT = 0.44f, EDGE = 1.6f;
  out.rgba.resize(static_cast<size_t>(W) * H * 4);
  ForRows(H, [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y)
    {
      const int iy = static_cast<int>(y);
      const float* row = &up[static_cast<size_t>(y) * W];
      const float* above = &up[static_cast<size_t>(std::max(iy - 1, 0)) * W];
      const float* below = &up[static_cast<size_t>(std::min(iy + 1, last_y)) * W];
      const float level = peak[y / scale];
      uint8_t* to = &out.rgba[static_cast<size_t>(y) * W * 4];
      for (int x = 0; x <= last_x; ++x, to += 4)
      {
        const float dx = row[std::min(x + 1, last_x)] - row[std::max(x - 1, 0)], dy = below[x] - above[x];
        const float slope = std::sqrt(dx * dx + dy * dy) / 2 + 1e-4f;
        const float v = std::clamp((row[x] - CUT) / (slope * EDGE) + 0.5f, 0.0f, 1.0f) * level;
        to[0] = to[1] = to[2] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        to[3] = 255;
      }
    }
  });
  return out;
}

std::vector<uint8_t> GreyMipChain(const Image& image, uint32_t& mips)
{
  std::vector<uint8_t> out;
  mips = 0;
  uint32_t lw = image.width, lh = image.height;
  if (lw == 0 || lh == 0)
    return out;
  size_t total = 0;
  for (uint32_t w = lw, h = lh;; w = std::max(1u, w / 2), h = std::max(1u, h / 2))
  {
    total += static_cast<size_t>(w) * h * 4;
    if (w == 1 && h == 1)
      break;
  }
  out.resize(total);
  uint8_t* to = out.data();
  std::vector<float> level(static_cast<size_t>(lw) * lh);
  for (size_t i = 0; i < level.size(); ++i)
    level[i] = image.rgba[i * 4];
  while (true)
  {
    for (float v : level)
    {
      const auto g = static_cast<uint8_t>(v + 0.5f);
      to[0] = to[1] = to[2] = g;
      to[3] = 255;
      to += 4;
    }
    ++mips;
    if (lw == 1 && lh == 1)
      break;
    const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
    std::vector<float> next(static_cast<size_t>(nw) * nh);
    for (uint32_t y = 0; y < nh; ++y)
      for (uint32_t x = 0; x < nw; ++x)
      {
        const uint32_t x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1);
        const uint32_t y0 = std::min(2 * y, lh - 1), y1 = std::min(2 * y + 1, lh - 1);
        next[static_cast<size_t>(y) * nw + x] = 0.25f * (level[static_cast<size_t>(y0) * lw + x0] + level[static_cast<size_t>(y0) * lw + x1] +
                                                         level[static_cast<size_t>(y1) * lw + x0] + level[static_cast<size_t>(y1) * lw + x1]);
      }
    level = std::move(next);
    lw = nw;
    lh = nh;
  }
  return out;
}

Image UpscaleImage(const Image& image, uint32_t scale)
{
  const uint32_t w = image.width, h = image.height, ow = w * scale, oh = h * scale;
  Image out{ow, oh, std::vector<uint8_t>(static_cast<size_t>(ow) * oh * 4)};
  if (w == 0 || h == 0 || scale == 0)
    return out;
  // Premultiplied source.
  std::vector<std::array<float, 4>> src(static_cast<size_t>(w) * h);
  bool opaque = true;
  for (size_t i = 0; i < src.size(); ++i)
  {
    const uint8_t* p = &image.rgba[i * 4];
    const float a = p[3] / 255.0f;
    src[i] = {p[0] / 255.0f * a, p[1] / 255.0f * a, p[2] / 255.0f * a, a};
    opaque = opaque && p[3] == 255;
  }
  // Output texel o of an axis of n source texels samples (o + 0.5) / scale -
  // 0.5: Catmull-Rom over texels i0 - 1 .. i0 + 2 (clamped), its nearest two
  // i0, i0 + 1 bounding the result.
  struct Tap
  {
    std::array<uint32_t, 4> at;
    std::array<float, 4> w;
  };
  const auto taps = [scale](uint32_t n) {
    std::vector<Tap> t(static_cast<size_t>(n) * scale);
    for (uint32_t o = 0; o < t.size(); ++o)
    {
      const float s = (static_cast<float>(o) + 0.5f) / static_cast<float>(scale) - 0.5f;
      const float i0 = std::floor(s), f = s - i0;
      Tap& tap = t[o];
      for (int k = 0; k < 4; ++k)
        tap.at[k] = static_cast<uint32_t>(std::clamp(static_cast<int>(i0) - 1 + k, 0, static_cast<int>(n) - 1));
      const float f2 = f * f, f3 = f2 * f;
      tap.w = {-0.5f * f3 + f2 - 0.5f * f, 1.5f * f3 - 2.5f * f2 + 1.0f, -1.5f * f3 + 2.0f * f2 + 0.5f * f,
               0.5f * f3 - 0.5f * f2};
    }
    return t;
  };
  const std::vector<Tap> tx = taps(w), ty = taps(h);
  // Rows widened.
  std::vector<std::array<float, 4>> wide(static_cast<size_t>(h) * ow);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < ow; ++x)
    {
      const Tap& t = tx[x];
      std::array<float, 4> acc{};
      for (int k = 0; k < 4; ++k)
      {
        const auto& s = src[static_cast<size_t>(y) * w + t.at[k]];
        for (int c = 0; c < 4; ++c)
          acc[c] += s[c] * t.w[k];
      }
      wide[static_cast<size_t>(y) * ow + x] = acc;
    }
  // Columns heightened, each texel kept within its four nearest source
  // texels' range; straight colour out, alpha kept apart for its edges.
  std::vector<float> alpha(static_cast<size_t>(ow) * oh);
  for (uint32_t y = 0; y < oh; ++y)
  {
    const Tap& t = ty[y];
    for (uint32_t x = 0; x < ow; ++x)
    {
      std::array<float, 4> acc{};
      for (int k = 0; k < 4; ++k)
      {
        const auto& s = wide[static_cast<size_t>(t.at[k]) * ow + x];
        for (int c = 0; c < 4; ++c)
          acc[c] += s[c] * t.w[k];
      }
      const auto& n00 = src[static_cast<size_t>(t.at[1]) * w + tx[x].at[1]];
      const auto& n01 = src[static_cast<size_t>(t.at[1]) * w + tx[x].at[2]];
      const auto& n10 = src[static_cast<size_t>(t.at[2]) * w + tx[x].at[1]];
      const auto& n11 = src[static_cast<size_t>(t.at[2]) * w + tx[x].at[2]];
      for (int c = 0; c < 4; ++c)
        acc[c] = std::clamp(acc[c], std::min({n00[c], n01[c], n10[c], n11[c]}),
                            std::max({n00[c], n01[c], n10[c], n11[c]}));
      const size_t i = static_cast<size_t>(y) * ow + x;
      alpha[i] = acc[3];
      uint8_t* p = &out.rgba[i * 4];
      if (acc[3] > 1.0f / 1024)
        for (int c = 0; c < 3; ++c)
          p[c] = static_cast<uint8_t>(std::lround(std::min(acc[c] / acc[3], 1.0f) * 255));
      p[3] = static_cast<uint8_t>(std::lround(acc[3] * 255));
    }
  }
  if (opaque)
    return out;
  // A real edge rises across about one source texel (its slope `scale`
  // times an output texel's); there the ramp is cut to one output texel
  // around half coverage. Gentler slopes (shadows, glows) stay as they are.
  const auto at = [&](int x, int y) {
    return alpha[static_cast<size_t>(std::clamp(y, 0, static_cast<int>(oh) - 1)) * ow +
                 static_cast<size_t>(std::clamp(x, 0, static_cast<int>(ow) - 1))];
  };
  const auto k = static_cast<float>(scale);
  for (int y = 0; y < static_cast<int>(oh); ++y)
    for (int x = 0; x < static_cast<int>(ow); ++x)
    {
      const float a = at(x, y);
      const float gx = (at(x + 1, y) - at(x - 1, y)) * 0.5f, gy = (at(x, y + 1) - at(x, y - 1)) * 0.5f;
      const float edge = std::clamp((std::sqrt(gx * gx + gy * gy) * k - 0.35f) / 0.3f, 0.0f, 1.0f);
      const float cut = std::clamp((a - 0.5f) * k + 0.5f, 0.0f, 1.0f);
      out.rgba[(static_cast<size_t>(y) * ow + static_cast<size_t>(x)) * 4 + 3] =
          static_cast<uint8_t>(std::lround((a + (cut - a) * edge) * 255));
    }
  // Colourless texels (no coverage at all) take their covered neighbours' mean.
  for (int pass = 0; pass < 2; ++pass)
  {
    std::vector<size_t> filled;
    std::vector<std::array<uint8_t, 3>> colours;
    for (int y = 0; y < static_cast<int>(oh); ++y)
      for (int x = 0; x < static_cast<int>(ow); ++x)
      {
        const size_t i = static_cast<size_t>(y) * ow + static_cast<size_t>(x);
        if (alpha[i] > 1.0f / 1024)
          continue;
        std::array<uint32_t, 3> sum{};
        uint32_t count = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
          {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int>(ow) || ny >= static_cast<int>(oh))
              continue;
            const size_t j = static_cast<size_t>(ny) * ow + static_cast<size_t>(nx);
            if (alpha[j] <= 1.0f / 1024)
              continue;
            for (int c = 0; c < 3; ++c)
              sum[c] += out.rgba[j * 4 + c];
            ++count;
          }
        if (count == 0)
          continue;
        filled.push_back(i);
        colours.push_back({static_cast<uint8_t>(sum[0] / count), static_cast<uint8_t>(sum[1] / count),
                           static_cast<uint8_t>(sum[2] / count)});
      }
    for (size_t f = 0; f < filled.size(); ++f)
    {
      std::memcpy(&out.rgba[filled[f] * 4], colours[f].data(), 3);
      alpha[filled[f]] = 1.0f;  // coloured now (its stored alpha stays 0)
    }
  }
  return out;
}

std::vector<uint8_t> MipChain(const Image& image, uint32_t& mips)
{
  std::vector<uint8_t> out;
  mips = 0;
  uint32_t lw = image.width, lh = image.height;
  if (lw == 0 || lh == 0)
    return out;
  size_t total = 0;
  for (uint32_t mw = lw, mh = lh;; mw = std::max(1u, mw / 2), mh = std::max(1u, mh / 2))
  {
    total += static_cast<size_t>(mw) * mh * 4;
    if (mw == 1 && mh == 1)
      break;
  }
  out.reserve(total);
  out.insert(out.end(), image.rgba.begin(), image.rgba.end());
  mips = 1;
  size_t level = 0;  // where the current level starts in `out`
  while (lw > 1 || lh > 1)
  {
    const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
    const size_t next = out.size();
    out.resize(next + static_cast<size_t>(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y)
      for (uint32_t x = 0; x < nw; ++x)
      {
        std::array<float, 4> acc{};
        for (uint32_t s = 0; s < 4; ++s)
        {
          const uint32_t sx = std::min(2 * x + s % 2, lw - 1), sy = std::min(2 * y + s / 2, lh - 1);
          const uint8_t* p = &out[level + (static_cast<size_t>(sy) * lw + sx) * 4];
          const float a = p[3] / 255.0f;
          for (int c = 0; c < 3; ++c)
            acc[c] += p[c] * a;
          acc[3] += a;
        }
        uint8_t* q = &out[next + (static_cast<size_t>(y) * nw + x) * 4];
        if (acc[3] > 0)
          for (int c = 0; c < 3; ++c)
            q[c] = static_cast<uint8_t>(std::lround(std::min(acc[c] / acc[3], 255.0f)));
        else
          for (int c = 0; c < 3; ++c)
            q[c] = out[level + (static_cast<size_t>(std::min(2 * y, lh - 1)) * lw + std::min(2 * x, lw - 1)) * 4 + c];
        q[3] = static_cast<uint8_t>(std::lround(acc[3] / 4 * 255));
      }
    level = next;
    lw = nw;
    lh = nh;
    ++mips;
  }
  return out;
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
