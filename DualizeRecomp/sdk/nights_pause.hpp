#pragma once

// The in-game pause window's Options (nights_pause.cpp): what the disc patch
// and the texture provider call.

#include <aurora/texture.hpp>

#include <filesystem>

namespace dualize::ios
{
class Disc;
}

namespace dualize::nights
{
// The window's labels texture (Continue, Retry, Quit) of each Latin language
// with a fourth label, Options: the game's three pressed into four bands, the
// fourth drawn with a fitted face; served by the disc in place of the game's.
void PatchPauseLabels(ios::Disc& disc, const std::filesystem::path& game);
// For the replacement provider: that texture's sharp copy, as it is first
// drawn; whether it registered one.
bool ProvidePauseLabels(const aurora::texture::TextureSource& source);
}  // namespace dualize::nights
