// SYSCONF, the console settings the system menu keeps: a NAND without one
// gets the settings a USA console leaves its factory with, widescreen on.

#include "kernel/ios/devices.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

namespace dualize::ios
{
namespace
{
constexpr size_t SYSCONF_SIZE = 0x4000;
constexpr uint32_t SYSMENU_UID = 0x1000;
constexpr uint16_t SYSMENU_GID = 1;
constexpr NandModes RW_ALL{NAND_RW, NAND_RW, NAND_RW};

enum Type : uint8_t
{
  BIG_ARRAY = 1,
  SMALL_ARRAY = 2,
  BYTE = 3,
  LONG = 5,
  BYTE_BOOL = 7,
};

struct Entry
{
  Type type;
  std::string_view name;
  std::vector<uint8_t> bytes;
};

std::vector<Entry> DefaultEntries()
{
  std::vector<uint8_t> nick(22);
  constexpr std::string_view NICK = "Dualize";
  for (size_t i = 0; i < NICK.size(); ++i)
    nick[2 * i + 1] = static_cast<uint8_t>(NICK[i]);
  nick[21] = static_cast<uint8_t>(NICK.size());
  std::vector<uint8_t> country(0x1008);
  country[0] = 49;  // United States
  std::vector<uint8_t> parental(0x4A);
  parental[1] = 0x04;
  parental[2] = 0x14;
  return {
      {BIG_ARRAY, "BT.DINF", std::vector<uint8_t>(0x461)},
      {BIG_ARRAY, "BT.CDIF", std::vector<uint8_t>(0x205)},
      {LONG, "BT.SENS", {0, 0, 0, 3}},
      {BYTE, "BT.BAR", {1}},
      {BYTE, "BT.SPKV", {0x58}},
      {BYTE, "BT.MOT", {1}},
      {SMALL_ARRAY, "IPL.NIK", std::move(nick)},
      {BYTE, "IPL.LNG", {1}},  // English
      {BIG_ARRAY, "IPL.SADR", std::move(country)},
      {SMALL_ARRAY, "IPL.PC", std::move(parental)},
      {LONG, "IPL.CB", {0, 0, 0, 0}},
      {BYTE, "IPL.AR", {1}},  // 16:9
      {BYTE, "IPL.SSV", {1}},
      {BYTE_BOOL, "IPL.CD", {0}},
      {BYTE_BOOL, "IPL.CD2", {0}},
      {BYTE_BOOL, "IPL.EULA", {1}},
      {BYTE, "IPL.UPT", {2}},
      {BYTE, "IPL.PGS", {0}},
      {BYTE, "IPL.E60", {1}},
      {BYTE, "IPL.DH", {0}},
      {LONG, "IPL.INC", {0, 0, 0, 8}},
      {LONG, "IPL.FRC", {0, 0, 0, 0x28}},
      {SMALL_ARRAY, "IPL.IDL", {0, 1}},
      {LONG, "NET.WCFG", {0, 0, 0, 1}},
      {LONG, "NET.CTPC", {0, 0, 0, 0}},
      {BYTE, "WWW.RST", {0}},
      {BYTE_BOOL, "MPLS.MOVIE", {1}},
  };
}

// "SCv0", the entry count, each entry's offset and one past the last, the
// entries, and "SCed" at the end of the 16 KiB.
std::vector<uint8_t> Serialize(const std::vector<Entry>& entries)
{
  std::vector<uint8_t> out = {'S', 'C', 'v', '0'};
  auto be16 = [&](size_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
  };
  be16(entries.size());
  std::vector<uint8_t> body;
  const size_t begin = out.size() + 2 * (entries.size() + 1);
  for (const Entry& entry : entries)
  {
    be16(begin + body.size());
    body.push_back(static_cast<uint8_t>(entry.type << 5 | (entry.name.size() - 1)));
    body.insert(body.end(), entry.name.begin(), entry.name.end());
    const size_t length = entry.bytes.size() - 1;  // arrays store their length - 1
    if (entry.type == BIG_ARRAY)
      body.push_back(static_cast<uint8_t>(length >> 8));
    if (entry.type == BIG_ARRAY || entry.type == SMALL_ARRAY)
      body.push_back(static_cast<uint8_t>(length));
    body.insert(body.end(), entry.bytes.begin(), entry.bytes.end());
  }
  be16(begin + body.size());
  out.insert(out.end(), body.begin(), body.end());
  out.resize(SYSCONF_SIZE);
  constexpr std::array<uint8_t, 4> FOOTER = {'S', 'C', 'e', 'd'};
  std::copy(FOOTER.begin(), FOOTER.end(), out.end() - FOOTER.size());
  return out;
}
}  // namespace

void EnsureSysconf(Nand& nand)
{
  constexpr const char* PATH = "/shared2/sys/SYSCONF";
  std::vector<uint8_t> sysconf;
  if (nand.ReadFile(PATH, &sysconf) == IPC_SUCCESS && sysconf.size() == SYSCONF_SIZE)
    return;
  const std::vector<uint8_t> data = Serialize(DefaultEntries());
  nand.CreateDirectory(SYSMENU_UID, SYSMENU_GID, "/shared2/sys", 0, RW_ALL);
  nand.Delete(SYSMENU_UID, SYSMENU_GID, PATH);
  if (nand.WriteFile(SYSMENU_UID, SYSMENU_GID, PATH, RW_ALL, 0, data.data(),
                     static_cast<uint32_t>(data.size())) != IPC_SUCCESS)
    std::fprintf(stderr, "[ios] can't write SYSCONF\n");
}
}  // namespace dualize::ios
