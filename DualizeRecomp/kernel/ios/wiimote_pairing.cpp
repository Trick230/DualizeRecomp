// The remotes' pairing: SYSCONF's BT.DINF lists the devices the console
// knows, and WPAD gives each its channel by that list.

#include "kernel/ios/devices.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dualize::ios
{
namespace
{
constexpr int REMOTES = 5;  // four Wii Remotes and the Balance Board slot
constexpr int BALANCE_BOARD = 4;

uint16_t Be16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
}  // namespace

void PairWiimotes(Nand& nand)
{
  constexpr const char* PATH = "/shared2/sys/SYSCONF";
  std::vector<uint8_t> sysconf;
  if (nand.ReadFile(PATH, &sysconf) != IPC_SUCCESS || sysconf.size() != 0x4000)
  {
    std::fprintf(stderr, "[native-wud] no SYSCONF; remotes stay unpaired\n");
    return;
  }
  // ConfPads: count, 10 registered, 5 active, 1 unknown, each address +
  // 64-byte name.
  constexpr size_t PAD = 6 + 0x40, SIZE = 1 + PAD * (10 + REMOTES + 1);
  std::array<uint8_t, SIZE> pads{};
  pads[0] = REMOTES;
  for (int i = 0; i < REMOTES; ++i)
  {
    const char* name = i == BALANCE_BOARD ? "Nintendo RVL-WBC-01" : "Nintendo RVL-CNT-01";
    for (uint8_t* pad : {&pads[1 + PAD * i], &pads[1 + PAD * (10 + i)]})
    {
      const std::array<uint8_t, 6> bd = {0x11, 0x02, 0x19, 0x79, 0, static_cast<uint8_t>(i)};
      std::reverse_copy(bd.begin(), bd.end(), pad);
      std::memcpy(pad + 6, name, 20);
    }
  }
  const uint16_t entries = Be16(&sysconf[4]);
  for (uint16_t e = 0; e < entries && 6 + 2 * e + 2 <= sysconf.size(); ++e)
  {
    const uint16_t offset = Be16(&sysconf[6 + 2 * e]);
    if (offset + 1 + 7 + 2 + SIZE > sysconf.size())
      continue;
    const uint8_t description = sysconf[offset];
    const size_t name_length = (description & 0x1F) + 1;
    // A big array (type 1) named BT.DINF of exactly the struct's size.
    if (description >> 5 != 1 || name_length != 7 ||
        std::memcmp(&sysconf[offset + 1], "BT.DINF", 7) != 0 ||
        Be16(&sysconf[offset + 8]) + 1u != SIZE)
      continue;
    const uint32_t data = offset + 10;
    if (std::memcmp(&sysconf[data], pads.data(), SIZE) == 0)
      return;
    if (nand.WriteFile(0, 0, PATH, {NAND_RW, NAND_RW, NAND_RW}, data, pads.data(), SIZE) !=
        IPC_SUCCESS)
      std::fprintf(stderr, "[native-wud] can't write BT.DINF\n");
    return;
  }
  std::fprintf(stderr, "[native-wud] SYSCONF has no BT.DINF; remotes stay unpaired\n");
}
}  // namespace dualize::ios
