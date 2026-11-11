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

// The same from the file's bytes, with where the image's data starts and its GX format.
struct TplImage
{
  Image image;
  uint32_t format = 0, offset = 0;
};
std::optional<TplImage> DecodeTplImage(std::span<const uint8_t> data, uint32_t index);

// Encodes the rows [y0, y1) of an opaque grey image (rounded out to whole
// blocks) as CMPR into `data`, the image's encoded bytes.
void EncodeGreyCmpr(const Image& image, uint32_t y0, uint32_t y1, uint8_t* data);

// A grey mask (text, shapes) drawn `scale` times larger with sharp edges: the
// coverage interpolated smoothly, then cut at half coverage with a one-texel
// soft edge, so letters keep their shape and weight; each band of `band` rows
// is cut at half its own brightest grey (dimmed lines stay dimmed). Opaque
// grey RGBA8, the full mip chain after the base level.
struct SharpMask
{
  uint32_t width = 0, height = 0, mips = 0;
  std::vector<uint8_t> rgba;
};
SharpMask SharpenGreyMask(const Image& image, uint32_t scale, uint32_t band);

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
