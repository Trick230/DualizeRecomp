#pragma once

// The game's own interface art, read from the disc at run time for the
// runtime's menus to look like the game's: GX texture files (TPL) and the
// game's bitmap font. Nothing of the game's is kept in the runtime.

#include <cstdint>
#include <filesystem>
#include <optional>
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
