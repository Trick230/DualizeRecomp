// The Wii's boot of a disc title, as Dolphin's BS2 emulation does it: the
// system menu's NAND bookkeeping (setting.txt, state.dat, play_rec.dat), the
// low-memory globals of IOS and the system menu, the CPU's boot state, then
// what the disc's apploader does (in host code: it loads the DOL and the FST
// and hands over the DOL's entry point), and finally ES_DIVerify of the game
// partition.

#include "boot.hpp"

#include "machine.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace dualize
{
namespace
{
constexpr uint32_t SYSMENU_UID = 0x1000;
constexpr uint16_t SYSMENU_GID = 1;
constexpr uint64_t SYSMENU_TITLE = 0x0000000100000002;
constexpr const char* SYSMENU_DATA = "/title/00000001/00000002/data";
constexpr ios::NandModes RW_ALL{ios::NAND_RW, ios::NAND_RW, ios::NAND_RW};

// IOS's low-memory globals.
struct IosMemory
{
  uint32_t version, date;
  uint32_t mem2_end, mem2_arena_end, ipc_begin, ipc_end;
  uint32_t reserved_begin, reserved_end;
};
// The system menu's IOS, which sets up memory first, and IOS21, which NiGHTS
// reloads into.
constexpr IosMemory IOS80{0x501b20, 0x030310, 0x93600000, 0x935E0000,
                          0x935E0000, 0x93600000, 0x93600000, 0x93620000};
constexpr IosMemory IOS21{0x15040f, 0x030110, 0x93400000, 0x933E0000,
                          0x933E0000, 0x93400000, 0xDEADBEEF, 0xDEADBEEF};
constexpr uint32_t MEM1_SIZE = 0x01800000;
constexpr uint32_t MEM2_SIZE = 0x04000000;
constexpr uint32_t PLACEHOLDER = 0xDEADBEEF;

void WriteIosMemory(Memory& memory, const IosMemory& ios, bool reload)
{
  memory.Write32(0x3140, ios.version);
  memory.Write32(0x3118, MEM2_SIZE);
  memory.Write32(0x311C, MEM2_SIZE);
  memory.Write32(0x3120, ios.mem2_end);
  memory.Write32(0x3124, 0x90000800);
  memory.Write32(0x3128, ios.mem2_arena_end);
  memory.Write32(0x3130, ios.ipc_begin);
  memory.Write32(0x3134, ios.ipc_end);
  memory.Write32(0x3148, ios.reserved_begin);
  memory.Write32(0x314C, ios.reserved_end);
  if (reload)
    return;
  memory.Write32(0x3100, MEM1_SIZE);
  memory.Write32(0x3104, MEM1_SIZE);
  memory.Write32(0x3108, 0x81800000);
  memory.Write32(0x310C, 0);
  memory.Write32(0x3110, 0x81800000);
  memory.Write32(0x3114, PLACEHOLDER);
  memory.Write32(0x312C, PLACEHOLDER);
  memory.Write32(0x3138, 0x11);  // Hollywood revision
  memory.Write32(0x313C, PLACEHOLDER);
  memory.Write32(0x3144, ios.date);
  memory.Write32(0x3150, PLACEHOLDER);
  memory.Write32(0x3154, PLACEHOLDER);
  memory.Write32(0x3158, 0xFF01);  // RAM vendor
  memory.Write8(0x315C, 0xDE);
  memory.Write8(0x315D, 0xAD);
  memory.Write16(0x315E, 0xBEEF);
  memory.Write32(0x3160, 0);  // system menu sync
  memory.Write32(0x28, MEM1_SIZE);
  memory.Write32(0x30, 0);
  memory.Write32(0x34, 0x81800000);
  memory.Write32(0xF0, MEM1_SIZE);
}

// setting.txt: "KEY=value\r\n" lines XORed with a rotating key.
constexpr uint32_t SETTINGS_SEED = 0x73B5DBFA;
constexpr size_t SETTINGS_SIZE = 0x100;

std::string DecodeSettings(const std::vector<uint8_t>& data)
{
  std::string decoded;
  uint32_t key = SETTINGS_SEED;
  for (size_t i = 0; i < data.size() && i < SETTINGS_SIZE; ++i)
  {
    const char c = static_cast<char>(data[i] ^ key);
    if (c != '\r')
      decoded.push_back(c);
    key = key >> 31 | key << 1;
  }
  return decoded;
}

std::string SettingValue(const std::string& decoded, const std::string& key)
{
  size_t found = decoded.find("\n" + key + "=");
  size_t start;
  if (found != std::string::npos)
    start = found + key.size() + 2;
  else if (decoded.compare(0, key.size() + 1, key + "=") == 0)
    start = key.size() + 1;
  else
    return {};
  size_t end = decoded.find('\n', start);
  if (end == std::string::npos)
    end = decoded.size() - 1;
  return decoded.substr(start, end - start);
}

class SettingsWriter
{
public:
  void Add(const std::string& key, const std::string& value) { Line(key + "=" + value + "\r\n"); }
  const std::array<uint8_t, SETTINGS_SIZE>& Bytes() const { return m_buffer; }

private:
  void Line(const std::string& line)
  {
    const size_t position = m_position;
    const uint32_t key = m_key;
    for (char c : line)
      Byte(static_cast<uint8_t>(c));
    // Nintendo's decoder stops at an encoded zero; a line feed before the
    // line shifts the key so none appears.
    for (size_t i = position; i < m_position; ++i)
    {
      if (m_buffer[i] == 0)
      {
        m_position = position;
        m_key = key;
        Byte('\n');
        Line(line);
        return;
      }
    }
  }
  void Byte(uint8_t b)
  {
    if (m_position >= m_buffer.size())
      return;
    m_buffer[m_position++] = b ^ static_cast<uint8_t>(m_key);
    m_key = m_key >> 31 | m_key << 1;
  }

  std::array<uint8_t, SETTINGS_SIZE> m_buffer{};
  size_t m_position = 0;
  uint32_t m_key = SETTINGS_SEED;
};

bool SetupWiiMemory(Machine& machine, std::string* error)
{
  ios::Nand& nand = machine.ios.Nand();
  machine.ios.Es().CreateTitleDirectories(SYSMENU_TITLE, SYSMENU_GID);
  const std::string path = std::string(SYSMENU_DATA) + "/setting.txt";

  // NiGHTS is an NTSC-U disc.
  std::string area = "USA", video = "NTSC", game = "US", code = "LU";
  std::string model = "RVL-001(USA)", serno;
  std::vector<uint8_t> data;
  if (nand.ReadFile(path, &data) == ios::IPC_SUCCESS && data.size() >= SETTINGS_SIZE)
  {
    const std::string decoded = DecodeSettings(data);
    serno = SettingValue(decoded, "SERNO");
    model = SettingValue(decoded, "MODEL");
    const std::string stored_code = SettingValue(decoded, "CODE");
    if (stored_code.size() >= 2 && stored_code[1] == 'U')
    {
      area = SettingValue(decoded, "AREA");
      video = SettingValue(decoded, "VIDEO");
      game = SettingValue(decoded, "GAME");
      code = stored_code;
    }
    else if (const size_t paren = model.find('('); paren != std::string::npos)
    {
      model = model.substr(0, paren) + "(" + area + ")";
    }
  }
  nand.Delete(SYSMENU_UID, SYSMENU_GID, path);
  if (serno.empty() || serno == "000000000")
  {
    char generated[16];
    std::snprintf(generated, sizeof(generated), "%09lld",
                  static_cast<long long>(std::time(nullptr) % 1000000000));
    serno = generated;
  }

  SettingsWriter settings;
  settings.Add("AREA", area);
  settings.Add("MODEL", model);
  settings.Add("DVD", "0");
  settings.Add("MPCH", "0x7FFE");
  settings.Add("CODE", code);
  settings.Add("SERNO", serno);
  settings.Add("VIDEO", video);
  settings.Add("GAME", game);
  if (const int32_t result = nand.WriteFile(SYSMENU_UID, SYSMENU_GID, path, RW_ALL, 0,
                                            settings.Bytes().data(), SETTINGS_SIZE))
  {
    *error = "can't write setting.txt (" + std::to_string(result) + ")";
    return false;
  }

  Memory& memory = machine.memory;
  memory.CopyTo(0x3800, settings.Bytes().data(), SETTINGS_SIZE);
  memory.Write32(0x20, 0x0D15EA5E);
  memory.Write32(0x24, 1);
  memory.Write32(0x28, MEM1_SIZE);
  memory.Write32(0x2C, 0x23);  // retail board
  memory.Write32(0x30, 0);
  memory.Write32(0x34, 0x817FEC60);
  memory.Write32(0xE4, 0x8008F7B8);
  memory.Write32(0xF0, MEM1_SIZE);
  memory.Write32(0xF4, 0x8179B500);
  memory.Write32(0xF8, 0x0E7BE2C0);  // bus clock
  memory.Write32(0xFC, 0x2B73A840);  // CPU clock
  memory.Write16(0x30E6, 0);
  memory.Write32(0x30C0, 0);
  memory.Write32(0x30C4, 0);
  memory.Write32(0x30DC, 0);
  memory.Write32(0x30D8, 0xFFFFFFFF);
  memory.Write16(0x30E6, 0x8201);
  memory.Write32(0x30F0, 0);
  memory.Write16(0x315E, 0x0113);
  memory.Write8(0x315C, 0x80);
  memory.Write16(0x30E0, 0);
  memory.Write32(0x3184, 0x80000000);
  memory.Write32(0xCC, 0);  // NTSC
  for (uint32_t address = 0x3000; address <= 0x3038; address += 4)
    memory.Write32(address, 0);
  return true;
}

void WriteSystemMenuState(Machine& machine)
{
  ios::Nand& nand = machine.ios.Nand();
  ios::Es& es = machine.ios.Es();

  es.CreateTitleDirectories(SYSMENU_TITLE, SYSMENU_GID);
  const std::array<uint8_t, 0x80> record{};
  nand.WriteFile(SYSMENU_UID, SYSMENU_GID, std::string(SYSMENU_DATA) + "/play_rec.dat", RW_ALL, 0,
                 record.data(), record.size());

  // state.dat: checksum, flags, type, disc state, return-to, unknown[6]; the
  // checksum is the host-order sum of the words after it.
  es.CreateTitleDirectories(SYSMENU_TITLE, SYSMENU_GID);
  const std::string state_path = std::string(SYSMENU_DATA) + "/state.dat";
  std::vector<uint8_t> state(0x20);
  std::vector<uint8_t> stored;
  if (nand.ReadFile(state_path, &stored) == ios::IPC_SUCCESS && stored.size() == 0x20)
    state = stored;
  state[4] = 0xC1;
  state[5] = 0xFF;
  state[6] = 0x01;
  uint32_t checksum = 0;
  for (size_t i = 4; i < 0x20; i += 4)
  {
    uint32_t word;
    std::memcpy(&word, &state[i], 4);
    checksum += word;
  }
  std::memcpy(&state[0], &checksum, 4);
  nand.WriteFile(SYSMENU_UID, SYSMENU_GID, state_path, RW_ALL, 0, state.data(), 0x20);
}

bool DiscRead(Machine& machine, uint64_t offset, uint32_t address, uint32_t length,
              uint64_t partition)
{
  uint8_t* out = machine.memory.Physical(address & 0x1FFFFFFF, length);
  return out && machine.disc->Read(offset, length, out, partition);
}

uint32_t Read32(Machine& machine, uint32_t address)
{
  return machine.memory.Read32(address & 0x1FFFFFFF);
}

constexpr uint32_t RoundDown32(uint32_t value)
{
  return value & ~31u;
}

constexpr uint32_t RoundUp32(uint32_t value)
{
  return (value + 31u) & ~31u;
}

// What a retail Wii apploader does, in host code: it places bi2 and the FST
// below the top of MEM1, loads the DOL's sections and the FST, and sets the
// low-memory globals the OS reads. Disc offsets in the header are in words.
bool LoadGame(Machine& machine, uint64_t partition, std::string* error)
{
  Memory& memory = machine.memory;
  std::array<uint8_t, 0x100> dol;
  std::array<uint8_t, 0x20> header, bi2;
  if (!machine.disc->Read(0x420, header.size(), header.data(), partition) ||
      !machine.disc->Read(0x440, bi2.size(), bi2.data(), partition))
  {
    *error = "can't read the disc header";
    return false;
  }
  const auto be32 = [](const uint8_t* p) {
    return uint32_t{p[0]} << 24 | p[1] << 16 | p[2] << 8 | p[3];
  };
  const uint64_t dol_offset = uint64_t{be32(&header[0x00])} << 2;
  const uint64_t fst_offset = uint64_t{be32(&header[0x04])} << 2;
  const uint32_t fst_size = be32(&header[0x08]) << 2;
  const uint32_t fst_max = be32(&header[0x0C]) << 2;
  if (!machine.disc->Read(dol_offset, dol.size(), dol.data(), partition))
  {
    *error = "can't read the DOL header";
    return false;
  }

  // The debug monitor's reserve comes off the top of memory; bi2 may ask
  // for less memory than there is.
  const uint32_t mem1 = memory.Read32(0x28);
  const uint32_t monitor = be32(&bi2[0x00]);
  const uint32_t simulated = be32(&bi2[0x04]);
  const uint32_t top = RoundDown32(0x80000000 + mem1 - monitor);
  memory.Write32(0xE8, monitor);
  memory.Write32(0xEC, top);
  memory.Write32(0xF0, simulated && simulated < mem1 ? simulated : mem1);
  const uint32_t fst = RoundDown32((simulated && simulated < mem1 ? 0x80000000 + simulated : top) - fst_max);
  const uint32_t bi2_address = fst - 0x2000;
  memory.Write32(0xF4, bi2_address);
  if (!DiscRead(machine, 0x440, bi2_address, 0x2000, partition))
  {
    *error = "can't read bi2";
    return false;
  }
  // bi2 may also cap MEM2: the IOS globals above it move down with it.
  if (const uint32_t mem2 = Read32(machine, bi2_address + 0x2C); mem2 && mem2 < memory.Read32(0x3118))
  {
    const uint32_t old = memory.Read32(0x311C);
    for (const uint32_t at : {0x3120u, 0x3128u, 0x3130u, 0x3134u})
      memory.Write32(at, mem2 - (old - memory.Read32(at)));
    memory.Write32(0x311C, mem2);
  }

  // The DOL's 7 text and 11 data sections.
  uint32_t loaded = 0;
  for (size_t i = 0; i < 18; ++i)
  {
    const uint32_t offset = be32(&dol[i * 4]);
    const uint32_t address = be32(&dol[0x48 + i * 4]);
    const uint32_t size = be32(&dol[0x90 + i * 4]);
    if (!offset)
      continue;
    if (!DiscRead(machine, dol_offset + offset, address, size, partition))
    {
      *error = "a DOL section is outside memory or the disc";
      return false;
    }
    loaded += RoundUp32(size);
  }
  if (!DiscRead(machine, fst_offset, fst, RoundUp32(fst_size), partition))
  {
    *error = "can't read the FST";
    return false;
  }

  const uint32_t entry = be32(&dol[0xE0]);
  memory.Write32(0x30, 0);
  memory.Write32(0x34, fst);
  memory.Write32(0x38, fst);
  memory.Write32(0x3C, fst_max);
  if (entry >= 0x80004000)
    memory.Write32(0x3110, fst);
  memory.Write32(0x30D4, loaded + RoundUp32(fst_size));
  memory.Write8(0x315D, 0x80);
  // The apploader's own version, which the OS reports.
  memory.Write32(0x3188, 0x00150202);

  machine.cpu.state.gpr[3] = entry;
  machine.cpu.state.pc = entry;
  return true;
}

std::vector<uint8_t> ReadPartitionBlob(Machine& machine, uint64_t partition, uint32_t size_at,
                                       uint32_t offset_at, uint32_t fixed_offset, uint32_t fixed_size)
{
  std::array<uint8_t, 8> field{};
  uint32_t size = fixed_size, offset = fixed_offset;
  if (size_at)
  {
    machine.disc->Read(partition + size_at, 4, field.data(), ios::PARTITION_NONE);
    size = uint32_t{field[0]} << 24 | field[1] << 16 | field[2] << 8 | field[3];
    machine.disc->Read(partition + offset_at, 4, field.data(), ios::PARTITION_NONE);
    offset = (uint32_t{field[0]} << 24 | field[1] << 16 | field[2] << 8 | field[3]) << 2;
  }
  std::vector<uint8_t> blob(size);
  if (size && !machine.disc->Read(partition + offset, size, blob.data(), ios::PARTITION_NONE))
    blob.clear();
  return blob;
}
}  // namespace

bool BootWiiDisc(Machine& machine, std::string* error)
{
  const uint64_t partition = machine.disc->GamePartition();
  const std::vector<uint8_t> tmd = ReadPartitionBlob(machine, partition, 0x2A4, 0x2A8, 0, 0);
  const std::vector<uint8_t> ticket = ReadPartitionBlob(machine, partition, 0, 0, 0, 0x2A4);
  if (tmd.size() < 0x1E4 || ticket.size() < 0x2A4)
  {
    *error = "the game partition has no TMD or ticket";
    return false;
  }

  Memory& memory = machine.memory;
  // The system menu's IOS set up memory before the menu ran.
  std::memset(memory.mem1, 0, 0x3FFF);
  WriteIosMemory(memory, IOS80, false);

  WriteSystemMenuState(machine);
  memory.Write32(0x3194, 0);
  memory.Write32(0x3198, static_cast<uint32_t>(partition >> 2));
  if (!SetupWiiMemory(machine, error))
    return false;
  // The game's IOS (from its TMD) replaces the menu's.
  WriteIosMemory(memory, IOS21, true);

  DiscRead(machine, 0, 0, 0x20, ios::PARTITION_NONE);
  DiscRead(machine, 0, 0x3180, 4, partition);

  Cpu& cpu = machine.cpu;
  CPUState& s = cpu.state;
  s.msr = 0x2032;  // RI | DR | IR | FP
  cpu.Spr(1008) = 0x0011C664;  // HID0
  s.hid2 = 0xE0000000;  // HID2: LSQE | WPE | PSE
  cpu.Spr(920) = s.hid2;
  cpu.Spr(1011) = 0x83900000;  // HID4
  constexpr std::pair<uint32_t, uint32_t> bats[] = {
      {528, 0x80001FFF}, {529, 0x00000002},  // IBAT0
      {536, 0x80001FFF}, {537, 0x00000002},  // DBAT0
      {538, 0xC0001FFF}, {539, 0x0000002A},  // DBAT1
      {560, 0x90001FFF}, {561, 0x10000002},  // IBAT4
      {568, 0x90001FFF}, {569, 0x10000002},  // DBAT4
      {570, 0xD0001FFF}, {571, 0x1000002A},  // DBAT5
  };
  for (const auto& [spr, value] : bats)
    cpu.Spr(spr) = value;
  // Default DSI, FPU and syscall handlers: rfi.
  memory.Write32(0x300, 0x4C000064);
  memory.Write32(0x800, 0x4C000064);
  memory.Write32(0xC00, 0x4C000064);
  s.gpr[1] = 0x816FFFF0;

  if (!LoadGame(machine, partition, error))
    return false;

  const int32_t verdict = machine.ios.Es().DiVerify(tmd, ticket);
  if (verdict != ios::IPC_SUCCESS)
    std::fprintf(stderr, "[native] ES_DIVerify: %d\n", verdict);
  return true;
}
}  // namespace dualize
