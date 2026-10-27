#pragma once

#include <cstdint>

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
};
}  // namespace dualize::ios
