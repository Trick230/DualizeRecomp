#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dualize::ios
{
// Partition handle: the raw disc offset of a partition's header.
constexpr uint64_t PARTITION_NONE = UINT64_MAX - 1;

// The inserted disc as the drive reads it.
class Disc
{
public:
  virtual ~Disc() = default;

  // Reads size bytes at offset: decrypted partition data, or raw disc bytes
  // for PARTITION_NONE. False if any of them don't exist.
  virtual bool Read(uint64_t offset, uint32_t size, uint8_t* out, uint64_t partition) const = 0;

  // The partition the console boots the game from.
  virtual uint64_t GamePartition() const = 0;
  // Whether partitions carry hash blocks (0x7C00 data bytes per 0x8000 block).
  virtual bool HasWiiHashes() const = 0;
  // One past the last readable offset (the physical disc size).
  virtual uint64_t EndOffset() const = 0;

  // Serves a file of the disc (a path under the extracted disc) from `bytes`
  // instead, before the game reads it; the size must stay. False when the
  // disc can't (not one of its files, another size, an image).
  virtual bool OverrideFile(const std::filesystem::path& path, std::vector<uint8_t> bytes)
  {
    (void)path;
    (void)bytes;
    return false;
  }

  // Counts in `reads` every read of the files right in `folder` (a folder
  // under the extracted disc), for the game's data loaded from them to be
  // known reloaded. Nothing for a disc that can't.
  virtual void CountReads(const std::filesystem::path& folder, std::atomic<uint32_t>* reads)
  {
    (void)folder;
    (void)reads;
  }

  // Calls `watch` (on the reading thread: it must be quick) with the path of
  // each file right in `folder` as the game reads it, once per read.
  using ReadWatch = void (*)(const std::filesystem::path& file, void* user);
  virtual void WatchReads(const std::filesystem::path& folder, ReadWatch watch, void* user)
  {
    (void)folder;
    (void)watch;
    (void)user;
  }
};
}  // namespace dualize::ios
