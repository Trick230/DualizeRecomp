#pragma once

#include <aurora/texture.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace dualize::nights
{
// The 2P menus' pre-drawn text (grey CMPR masks in the mode select, continue
// and remote textures of each Latin language) drawn sharp: each line is read
// back from the disc's own image, letter by letter against the face fitted to
// it, and redrawn four times larger with that face where the game has each
// letter. No game text is kept: a reading is taken only when it hashes as the
// line does; else the line keeps the sharpened original.

// A text texture file as the disc serves it (`file` under the game's root).
void Keep2PText(std::string_view file, std::span<const uint8_t> bytes);
// Reads the files not kept yet from the game's folder, then the lines on a worker.
void Load2PText(const std::filesystem::path& game);
// For the replacement provider: the sharp copy of a kept image, as it is
// first drawn; whether it registered one.
bool Provide2PText(const aurora::texture::TextureSource& source);
}  // namespace dualize::nights
