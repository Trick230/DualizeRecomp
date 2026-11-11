#pragma once

// The Options labels' face as the game itself draws it: letters cut out of
// the game's own lines of it (its Options labels in every language and a few
// other screens' lines), read from the disc at boot, the few it never prints
// made from others (O from C, q from p, accents moved over). Two sizes, as
// the game sets them: its English labels, and the other languages' labels
// and every value word, smaller and narrower.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dualize::nights
{
enum class LetteringSize : uint8_t
{
  LABEL,
  SMALL,
};

// A letter's coverage (24 rows, its baseline on row 19, `width` texels a
// row) and the room the face keeps either side of it, in texels.
struct Letter
{
  uint32_t width = 0;
  std::vector<float> coverage;
  float left = 0, right = 0;
};

// Cuts the letters' sources from the game's files (before anything is drawn
// over them).
void LoadLettering(const std::filesystem::path& game);

// The letter `c` at `size`, pressed to `squeeze` of its width; nullptr when
// the game has no such letter (draw it in another face).
const Letter* GameLetter(char32_t c, LetteringSize size, float squeeze);

// A size's capitals' height, its letters' press against the English labels',
// the texels added between letters and a space's width (unsqueezed).
float LetteringCap(LetteringSize size);
float LetteringPress(LetteringSize size);
float LetteringTracking(LetteringSize size);
float LetteringSpace(LetteringSize size);
}  // namespace dualize::nights
