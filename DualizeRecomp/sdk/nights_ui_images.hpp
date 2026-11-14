#pragma once

#include "kernel/ios/disc.hpp"

#include <aurora/texture.hpp>

#include <filesystem>

namespace dualize::nights
{
// The interface's pictures (the CSD textures: logos, icons, panels, menu
// art) drawn larger with UpscaleImage: as the game reads one of their files,
// workers enlarge its images, and each copy replaces its image when the game
// first draws it. Nothing is kept beyond a budget of recent copies.

// Watches the game's reads of the CSD texture files on `disc`.
void WatchUiImages(ios::Disc& disc, const std::filesystem::path& game);
// `listener` also told of each load of one of those files, on the reading
// thread (set before the game runs).
void AlsoOnUiFileRead(void (*listener)(const std::filesystem::path& file, void* data), void* data);
// For the replacement provider: whether it registered the source's copy.
bool ProvideUiImage(const aurora::texture::TextureSource& source);
}  // namespace dualize::nights
