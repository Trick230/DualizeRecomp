#pragma once

#include "kernel/ios/ios.hpp"
#include "kernel/ios/disc.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dualize::ios
{
struct Config
{
  // Host directory holding the emulated NAND (the Dolphin user dir's Wii/).
  std::filesystem::path nand_root;
  // The disc's data partition; DI reads come from here.
  std::shared_ptr<const Disc> disc;
  // Seconds since the Unix epoch on the emulated clock.
  int64_t (*emulated_time)(void* user) = nullptr;
  // Asks the host to power off (STM shutdown/idle).
  void (*shutdown)(void* user) = nullptr;
  // DVDLowOpenPartition: copies the partition's TMD to tmd_address and returns
  // ES's verdict on it. Null rejects the command.
  int32_t (*open_partition)(void* user, uint64_t partition, uint32_t tmd_address) = nullptr;
};

// NAND access modes per owner, group and others: bit 0 read, bit 1 write.
struct NandModes
{
  uint8_t owner, group, other;
};
constexpr uint8_t NAND_NONE = 0, NAND_READ = 1, NAND_WRITE = 2, NAND_RW = 3;

// /dev/fs, plus the file system calls IOS modules make themselves (ES, the
// boot). Those return IPC codes, run as the given uid/gid and take no time.
class Nand : public Device
{
public:
  using Device::Device;
  // The uid/gid /dev/fs handles opened by the PPC run as (ES sets them).
  virtual void SetPpcIds(uint32_t uid, uint16_t gid) = 0;
  virtual int32_t CreateFile(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attribute,
                             NandModes modes) = 0;
  virtual int32_t CreateDirectory(uint32_t uid, uint16_t gid, const std::string& path,
                                  uint8_t attribute, NandModes modes) = 0;
  // Creates every missing parent directory of path.
  virtual int32_t CreateFullPath(uint32_t uid, uint16_t gid, const std::string& path,
                                 uint8_t attribute, NandModes modes) = 0;
  virtual int32_t SetMetadata(uint32_t caller, const std::string& path, uint32_t uid, uint16_t gid,
                              uint8_t attribute, NandModes modes) = 0;
  virtual int32_t Delete(uint32_t uid, uint16_t gid, const std::string& path) = 0;
  virtual int32_t Rename(uint32_t uid, uint16_t gid, const std::string& from,
                         const std::string& to) = 0;
  virtual bool Exists(const std::string& path) = 0;
  virtual int32_t ReadFile(const std::string& path, std::vector<uint8_t>* data) = 0;
  // Opens path (creating it with modes if missing) and writes data at offset;
  // offset UINT32_MAX appends.
  virtual int32_t WriteFile(uint32_t uid, uint16_t gid, const std::string& path, NandModes modes,
                            uint32_t offset, const void* data, uint32_t size) = 0;
};
std::unique_ptr<Nand> CreateFs(const Host& host, const Config& config);

// /dev/es plus the title setup the boot asks of ES.
class Es : public Device
{
public:
  using Device::Device;
  // ES_DIVerify: makes the disc title current, installs its TMD if the NAND
  // lacks one, and sets the PPC's uid/gid; an IPC code.
  virtual int32_t DiVerify(const std::vector<uint8_t>& tmd, const std::vector<uint8_t>& ticket) = 0;
  virtual int32_t CreateTitleDirectories(uint64_t title_id, uint16_t gid) = 0;
};
std::unique_ptr<Es> CreateEs(const Host& host, Nand& nand);

// Lists the four Wii Remote slots and the Balance Board's as paired in
// SYSCONF's BT.DINF, which WPAD assigns channels by.
void PairWiimotes(Nand& nand);

// The native device of that name, or null when the game's IPC stays on the
// host's own implementation. `id` is what the device passes to Host::schedule.
std::unique_ptr<Device> CreateDevice(const Host& host, const Config& config, std::string_view name,
                                     uint32_t id);

// Completes /dev/di's current command from the host's drive (a read the host
// started before the native driver took over).
void FinishHostDiCommand(Device& di, bool success);

// /dev/stm/eventhook's pending hook, completed by the console buttons.
void PressResetButton();
void PressPowerButton();
}  // namespace dualize::ios
