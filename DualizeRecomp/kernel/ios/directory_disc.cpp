#include "kernel/ios/directory_disc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

namespace dualize::ios
{
namespace
{
namespace fs = std::filesystem;

constexpr uint64_t GAME_PARTITION = 0xF800000;
constexpr uint64_t PARTITION_DATA_OFFSET = 0x20000;
constexpr uint64_t PARTITION_TABLE = 0x40000;
constexpr uint64_t REGION_DATA = 0x4E000;
constexpr uint32_t TICKET_SIZE = 0x2A4;
constexpr uint32_t TMD_OFFSET = 0x2C0;
constexpr uint32_t H3_OFFSET = 0x4000;
constexpr uint32_t H3_SIZE = 0x18000;
constexpr uint32_t MAX_TMD_SIZE = 0x49E4;
constexpr uint64_t APPLOADER_ADDRESS = 0x2440;
constexpr uint64_t BLOCK_DATA_SIZE = 0x7C00;
constexpr uint64_t BLOCK_TOTAL_SIZE = 0x8000;
constexpr uint64_t SL_DVD_SIZE = 4699979776;
constexpr uint64_t DL_DVD_SIZE = 8511160320;
constexpr uint32_t ENTRY_SIZE = 12;

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}
void Put32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
{
  if (data.size() < offset + 4)
    data.resize(offset + 4);
  data[offset] = static_cast<uint8_t>(value >> 24);
  data[offset + 1] = static_cast<uint8_t>(value >> 16);
  data[offset + 2] = static_cast<uint8_t>(value >> 8);
  data[offset + 3] = static_cast<uint8_t>(value);
}
uint32_t Get32(const std::vector<uint8_t>& data, size_t offset)
{
  return uint32_t{data[offset]} << 24 | data[offset + 1] << 16 | data[offset + 2] << 8 |
         data[offset + 3];
}

FILE* OpenRead(const fs::path& path)
{
#ifdef _WIN32
  return _wfopen(path.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}
bool Seek(FILE* file, uint64_t offset)
{
#ifdef _WIN32
  return _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
  return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool ReadWhole(const fs::path& path, std::vector<uint8_t>* out, size_t max_size = SIZE_MAX)
{
  FILE* file = OpenRead(path);
  if (!file)
    return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  out->resize(std::min<size_t>(static_cast<size_t>(size), max_size));
  const bool ok = std::fread(out->data(), 1, out->size(), file) == out->size();
  std::fclose(file);
  return ok;
}

// A run of bytes in an address space: from memory, or from a host file.
struct Chunk
{
  uint64_t offset;
  uint64_t size;
  int32_t file;  // index into the file table, or -1 for `bytes`
  uint32_t bytes;  // index into the blob table
};

class Space
{
public:
  void AddBytes(uint64_t offset, std::vector<uint8_t> bytes)
  {
    m_chunks.push_back({offset, bytes.size(), -1, static_cast<uint32_t>(m_blobs.size())});
    m_blobs.push_back(std::move(bytes));
  }
  void AddFile(uint64_t offset, uint64_t size, int32_t file)
  {
    if (size)
      m_chunks.push_back({offset, size, file, 0});
  }
  void Finish()
  {
    std::sort(m_chunks.begin(), m_chunks.end(),
              [](const Chunk& a, const Chunk& b) { return a.offset < b.offset; });
  }

  template <typename ReadFile>
  bool Read(uint64_t offset, uint32_t size, uint8_t* out, const ReadFile& read_file) const
  {
    // Gaps read as zeros.
    std::memset(out, 0, size);
    auto it = std::upper_bound(m_chunks.begin(), m_chunks.end(), offset,
                               [](uint64_t o, const Chunk& c) { return o < c.offset; });
    if (it != m_chunks.begin())
      --it;
    const uint64_t end = offset + size;
    for (; it != m_chunks.end() && it->offset < end; ++it)
    {
      const uint64_t from = std::max(offset, it->offset);
      const uint64_t to = std::min(end, it->offset + it->size);
      if (from >= to)
        continue;
      uint8_t* dest = out + (from - offset);
      const uint64_t within = from - it->offset;
      if (it->file < 0)
        std::memcpy(dest, m_blobs[it->bytes].data() + within, to - from);
      else if (!read_file(it->file, within, static_cast<uint32_t>(to - from), dest))
        return false;
    }
    return true;
  }

private:
  std::vector<Chunk> m_chunks;
  std::vector<std::vector<uint8_t>> m_blobs;
};

struct Node
{
  std::string name;
  bool folder = false;
  uint64_t size = 0;  // bytes, or descendants for folders
  fs::path path;
  std::vector<Node> children;
};

void Scan(const fs::path& dir, std::vector<Node>* nodes)
{
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, error))
  {
    Node node;
    const std::u8string name = entry.path().filename().u8string();
    node.name.assign(name.begin(), name.end());
    node.path = entry.path();
    node.folder = entry.is_directory(error);
    if (node.folder)
      Scan(entry.path(), &node.children);
    else
      node.size = entry.file_size(error);
    nodes->push_back(std::move(node));
  }
}

size_t CountDescendants(std::vector<Node>* nodes)
{
  size_t count = 0;
  for (Node& node : *nodes)
  {
    ++count;
    if (node.folder)
    {
      node.size = CountDescendants(&node.children);
      count += node.size;
    }
  }
  return count;
}

uint32_t NameSize(const std::vector<Node>& nodes)
{
  uint32_t size = 0;
  for (const Node& node : nodes)
    size += static_cast<uint32_t>(node.name.size() + 1) + (node.folder ? NameSize(node.children) : 0);
  return size;
}

std::string Upper(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

class DirectoryDisc final : public Disc
{
public:
  bool Open(const fs::path& root, std::string* error)
  {
    std::vector<uint8_t> boot, bi2, apploader, header, region, ticket, tmd, cert, h3;
    if (!ReadWhole(root / "sys" / "boot.bin", &boot) || boot.size() < 0x440)
      return Fail(error, "sys/boot.bin is missing or short");
    boot.resize(0x440);
    ReadWhole(root / "sys" / "bi2.bin", &bi2);
    bi2.resize(0x2000);
    if (!ReadWhole(root / "sys" / "apploader.img", &apploader) || apploader.size() < 0x20 ||
        0x20 + Get32(apploader, 0x14) + Get32(apploader, 0x18) != apploader.size())
    {
      return Fail(error, "sys/apploader.img is missing or not an apploader");
    }
    if (!ReadWhole(root / "ticket.bin", &ticket, TICKET_SIZE) ||
        !ReadWhole(root / "tmd.bin", &tmd, MAX_TMD_SIZE))
    {
      return Fail(error, "ticket.bin or tmd.bin is missing");
    }

    // Partition data: header, bi2, apploader, DOL, FST, files.
    const uint64_t dol_address = AlignUp(APPLOADER_ADDRESS + apploader.size() + 0x20, 0x20);
    std::error_code ec;
    const fs::path dol = root / "sys" / "main.dol";
    const uint64_t dol_size = fs::file_size(dol, ec);
    if (ec)
      return Fail(error, "sys/main.dol is missing");
    m_partition.AddBytes(0x440, std::move(bi2));
    m_partition.AddBytes(APPLOADER_ADDRESS, std::move(apploader));
    m_partition.AddFile(dol_address, dol_size, AddFile(dol));
    Put32(boot, 0x420, static_cast<uint32_t>(dol_address >> 2));
    const uint64_t fst_address = AlignUp(dol_address + dol_size + 0x20, 0x20);
    std::vector<Node> nodes;
    Scan(root / "files", &nodes);
    const uint64_t data_size = BuildFst(std::move(nodes), fst_address, &boot);
    m_partition.AddBytes(0, boot);
    m_partition.Finish();

    // Outside the partitions: disc header, partition table, region data and
    // the game partition's header.
    ReadWhole(root / "disc" / "header.bin", &header, 0x100);
    const size_t header_size = header.size();
    header.resize(0x100);
    if (header_size < 0x100)
      std::copy(boot.begin() + header_size, boot.begin() + 0x100, header.begin() + header_size);
    if (header_size < 0x61)
      header[0x60] = header[0x61] = 0;
    m_wii_hashes = header[0x60] == 0;
    m_raw.AddBytes(0, std::move(header));

    std::vector<uint8_t> table(0x28);
    Put32(table, 0x00, 1);
    Put32(table, 0x04, static_cast<uint32_t>((PARTITION_TABLE + 0x20) >> 2));
    Put32(table, 0x20, static_cast<uint32_t>(GAME_PARTITION >> 2));
    Put32(table, 0x24, 0);  // game
    m_raw.AddBytes(PARTITION_TABLE, std::move(table));

    std::vector<uint8_t> region_data(0x20, 0x80);
    std::fill(region_data.begin(), region_data.begin() + 0x10, 0);
    if (ReadWhole(root / "disc" / "region.bin", &region, 0x20))
      std::copy(region.begin(), region.end(), region_data.begin());
    m_raw.AddBytes(REGION_DATA, std::move(region_data));

    const uint32_t tmd_size = static_cast<uint32_t>(tmd.size());
    const uint32_t cert_offset = static_cast<uint32_t>(AlignUp(TMD_OFFSET + tmd_size, 0x20));
    ReadWhole(root / "cert.bin", &cert, H3_OFFSET - cert_offset);
    const uint64_t encrypted_size = AlignUp(data_size, BLOCK_DATA_SIZE) / BLOCK_DATA_SIZE * BLOCK_TOTAL_SIZE;
    std::vector<uint8_t> partition_header(0x1C);
    Put32(partition_header, 0x00, tmd_size);
    Put32(partition_header, 0x04, TMD_OFFSET >> 2);
    Put32(partition_header, 0x08, static_cast<uint32_t>(cert.size()));
    Put32(partition_header, 0x0C, cert_offset >> 2);
    Put32(partition_header, 0x10, H3_OFFSET >> 2);
    Put32(partition_header, 0x14, static_cast<uint32_t>(PARTITION_DATA_OFFSET >> 2));
    Put32(partition_header, 0x18, static_cast<uint32_t>(encrypted_size >> 2));
    m_raw.AddBytes(GAME_PARTITION, std::move(ticket));
    m_raw.AddBytes(GAME_PARTITION + TICKET_SIZE, std::move(partition_header));
    m_raw.AddBytes(GAME_PARTITION + TMD_OFFSET, std::move(tmd));
    m_raw.AddBytes(GAME_PARTITION + cert_offset, std::move(cert));
    if (ReadWhole(root / "h3.bin", &h3, H3_SIZE))
      m_raw.AddBytes(GAME_PARTITION + H3_OFFSET, std::move(h3));
    m_raw.Finish();

    const uint64_t disc_size = AlignUp(GAME_PARTITION + PARTITION_DATA_OFFSET + encrypted_size, 0x10000);
    m_end_offset = disc_size <= SL_DVD_SIZE ? SL_DVD_SIZE : DL_DVD_SIZE;
    return true;
  }

  ~DirectoryDisc() override
  {
    for (OpenFile& open : m_open)
    {
      if (open.file)
        std::fclose(open.file);
    }
  }

  bool Read(uint64_t offset, uint32_t size, uint8_t* out, uint64_t partition) const override
  {
    const auto read_file = [this](int32_t file, uint64_t at, uint32_t length, uint8_t* dest) {
      return ReadFile(file, at, length, dest);
    };
    if (partition == PARTITION_NONE)
      return m_raw.Read(offset, size, out, read_file);
    if (partition != GAME_PARTITION)
      return false;
    return m_partition.Read(offset, size, out, read_file);
  }
  uint64_t GamePartition() const override { return GAME_PARTITION; }
  bool HasWiiHashes() const override { return m_wii_hashes; }
  uint64_t EndOffset() const override { return m_end_offset; }

private:
  static bool Fail(std::string* error, const char* message)
  {
    if (error)
      *error = message;
    return false;
  }

  int32_t AddFile(const fs::path& path)
  {
    m_files.push_back(path);
    return static_cast<int32_t>(m_files.size() - 1);
  }

  // Returns the data size of the partition.
  uint64_t BuildFst(std::vector<Node> nodes, uint64_t fst_address, std::vector<uint8_t>* boot)
  {
    const uint32_t name_table_size = static_cast<uint32_t>(AlignUp(NameSize(nodes), 4));
    const uint64_t total_entries = CountDescendants(&nodes) + 1;
    const uint64_t name_table = total_entries * ENTRY_SIZE;
    std::vector<uint8_t> fst(name_table + name_table_size);
    uint64_t data = AlignUp(fst_address + fst.size(), 0x8000);
    uint32_t entry = 0, name = 0;
    WriteEntry(fst, &entry, 1, 0, 0, static_cast<uint32_t>(total_entries));
    WriteDirectory(fst, nodes, &entry, &name, &data, 0, name_table);
    Put32(*boot, 0x424, static_cast<uint32_t>(fst_address >> 2));
    Put32(*boot, 0x428, static_cast<uint32_t>(fst.size() >> 2));
    Put32(*boot, 0x42C, static_cast<uint32_t>(fst.size() >> 2));
    m_partition.AddBytes(fst_address, std::move(fst));
    return data;
  }

  static void WriteEntry(std::vector<uint8_t>& fst, uint32_t* entry, uint8_t type,
                         uint32_t name_offset, uint32_t offset, uint32_t length)
  {
    Put32(fst, *entry, uint32_t{type} << 24 | (name_offset & 0xFFFFFF));
    Put32(fst, *entry + 4, offset);
    Put32(fst, *entry + 8, length);
    *entry += ENTRY_SIZE;
  }

  void WriteDirectory(std::vector<uint8_t>& fst, std::vector<Node>& nodes, uint32_t* entry,
                      uint32_t* name, uint64_t* data, uint32_t parent, uint64_t name_table)
  {
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) {
      const std::string ua = Upper(a.name), ub = Upper(b.name);
      return ua == ub ? a.name < b.name : ua < ub;
    });
    for (Node& node : nodes)
    {
      const uint32_t index = *entry / ENTRY_SIZE;
      if (node.folder)
        WriteEntry(fst, entry, 1, *name, parent, static_cast<uint32_t>(index + node.size + 1));
      else
        WriteEntry(fst, entry, 0, *name, static_cast<uint32_t>(*data >> 2),
                   static_cast<uint32_t>(node.size));
      std::memcpy(&fst[name_table + *name], node.name.c_str(), node.name.size() + 1);
      *name += static_cast<uint32_t>(node.name.size() + 1);
      if (node.folder)
      {
        WriteDirectory(fst, node.children, entry, name, data, index, name_table);
      }
      else
      {
        m_partition.AddFile(*data, node.size, AddFile(node.path));
        *data = AlignUp(*data + node.size, 0x8000);
      }
    }
  }

  // A few files stay open: reads come in runs from the same file.
  bool ReadFile(int32_t index, uint64_t offset, uint32_t size, uint8_t* out) const
  {
    OpenFile* slot = nullptr;
    for (OpenFile& open : m_open)
    {
      if (open.file && open.index == index)
      {
        slot = &open;
        break;
      }
    }
    if (!slot)
    {
      slot = &m_open[m_next_slot];
      m_next_slot = (m_next_slot + 1) % m_open.size();
      if (slot->file)
        std::fclose(slot->file);
      slot->file = OpenRead(m_files[index]);
      slot->index = index;
      slot->position = 0;
      if (!slot->file)
        return false;
    }
    if (slot->position != offset && !Seek(slot->file, offset))
      return false;
    const size_t read = std::fread(out, 1, size, slot->file);
    slot->position = offset + read;
    return read == size;
  }

  struct OpenFile
  {
    FILE* file = nullptr;
    int32_t index = -1;
    uint64_t position = 0;
  };

  Space m_raw;
  Space m_partition;
  std::vector<fs::path> m_files;
  mutable std::array<OpenFile, 8> m_open{};
  mutable size_t m_next_slot = 0;
  bool m_wii_hashes = true;
  uint64_t m_end_offset = SL_DVD_SIZE;
};
}  // namespace

std::shared_ptr<Disc> OpenDirectoryDisc(const fs::path& root, std::string* error)
{
  auto disc = std::make_shared<DirectoryDisc>();
  if (!disc->Open(root, error))
    return nullptr;
  return disc;
}
}  // namespace dualize::ios
