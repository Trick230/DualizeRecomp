#pragma once

// The game's own interface art, read from the disc at run time for the
// runtime's menus to look like the game's: GX texture files (TPL) and the
// game's bitmap font. Nothing of the game's is kept in the runtime.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dualize
{
struct Image
{
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
};

// One image of a TPL file, decoded to RGBA8; empty on a missing file or an
// unknown format.
std::optional<Image> LoadTplImage(const std::filesystem::path& path, uint32_t index);

// The same from the file's bytes, with where the image's data starts and its
// GX format; with `pixels` false only the size is set (rgba stays empty).
struct TplImage
{
  Image image;
  uint32_t format = 0, offset = 0;
};
std::optional<TplImage> DecodeTplImage(std::span<const uint8_t> data, uint32_t index, bool pixels = true);

// Encodes the rows [y0, y1) of an opaque grey image (rounded out to whole
// blocks) as CMPR into `data`, the image's encoded bytes.
void EncodeGreyCmpr(const Image& image, uint32_t y0, uint32_t y1, uint8_t* data);

// A grey mask (text, shapes) drawn `scale` times larger with sharp edges: the
// coverage interpolated and smoothed, then cut near half coverage with a soft
// edge of even width, so letters keep their shape and weight; each band of `band` rows
// is cut at half its own brightest grey (dimmed lines stay dimmed). Opaque grey.
Image SharpenGreyMask(const Image& image, uint32_t scale, uint32_t band);

// An opaque grey image's texels and its smaller mips (2x2 averages) after
// them, down to one texel, as RGBA8; `mips` counts the levels.
std::vector<uint8_t> GreyMipChain(const Image& image, uint32_t& mips);

// A picture (logo, icon, panel) drawn `scale` times larger with nothing made
// up: its premultiplied colour interpolated (Catmull-Rom, kept within each
// spot's four nearest texels: no halos), and where its alpha has a real edge
// (a cut-out's outline, not a soft shadow or glow) that edge redrawn one
// output texel wide at the same place. Transparent texels take their
// neighbours' colour, for filtering not to darken the outline.
Image UpscaleImage(const Image& image, uint32_t scale);

// An image's texels and its smaller mips (premultiplied 2x2 averages) after
// them, down to one texel, as RGBA8; `mips` counts the levels.
std::vector<uint8_t> MipChain(const Image& image, uint32_t& mips);

// Encodes the rows [y0, y1) of an image (rounded out to whole blocks) as
// RGB5A3 into `data`, the image's encoded bytes.
void EncodeRgb5a3(const Image& image, uint32_t y0, uint32_t y1, uint8_t* data);

// The game's bitmap font: an atlas of 2-bit glyphs and, per character, its
// box in the atlas.
class BitmapFont
{
public:
  struct Glyph
  {
    float x0, y0, x1, y1;  // atlas pixels
  };

  // From <base>.inf and <base>.bin; false when they are missing or malformed.
  bool Load(const std::filesystem::path& base);

  // White, with the glyphs' coverage in alpha.
  const Image& Atlas() const { return m_atlas; }
  float CellSize() const { return m_cell; }
  const Glyph* Find(char32_t c) const;

private:
  Image m_atlas;
  float m_cell = 0;
  std::unordered_map<char32_t, Glyph> m_glyphs;
};
}  // namespace dualize
