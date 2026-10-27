#pragma once

#include "kernel/ios/disc.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace dualize::ios
{
// A Wii disc extracted to a directory (sys/, files/, disc/, ticket.bin,
// tmd.bin, cert.bin, h3.bin), served decrypted with the layout Dolphin's
// DirectoryBlob gives it: game partition at 0xF800000, apploader, DOL and a
// rebuilt FST in the partition's data, files 32 KiB aligned in FST order.
std::shared_ptr<Disc> OpenDirectoryDisc(const std::filesystem::path& root, std::string* error);
}  // namespace dualize::ios
