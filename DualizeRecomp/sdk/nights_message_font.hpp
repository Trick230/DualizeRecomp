#pragma once

#include <filesystem>

namespace dualize::nights
{
// The game's message font (files/fontJapan: subtitles, dialogue, help lines)
// drawn sharp at any size: the game copies the glyphs a text needs from the
// font's 24-texel atlas into a glyph cache texture; the renderer is handed
// that texture SCALE times larger, each cell's glyph set by vector faces
// fitted to the atlas's (scripts/lettering/atlas_fit.py), placed where the
// atlas has it. Glyphs the faces don't match (icons) keep the atlas's.
// Starts rendering every glyph in the background.
void LoadMessageFont(const std::filesystem::path& game);
}  // namespace dualize::nights
