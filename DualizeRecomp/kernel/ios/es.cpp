// /dev/es: the title context (set when the boot verifies the disc's TMD and
// ticket), the PPC's uid/gid from /sys/uid.sys, and the two queries NiGHTS
// makes. Ported from Dolphin's ESDevice for the calls a disc title reaches.

#include "kernel/ios/devices.hpp"

#include <array>
#include <cstdio>
#include <map>

namespace dualize::ios
{
namespace
{
constexpr int32_t ES_EINVAL = -1017;
constexpr int32_t ES_SHORT_READ = -1009;
constexpr int32_t ES_FD_EXHAUSTED = -1016;
constexpr uint32_t FIRST_PPC_UID = 0x1000;
constexpr uint64_t SYSTEM_MENU = 0x0000000100000002;
constexpr const char* UID_MAP_PATH = "/sys/uid.sys";

enum : uint32_t
{
  IOCTLV_GETTITLEDIR = 0x1D,
  IOCTLV_GETTITLEID = 0x20,
};

// TMD and ticket field offsets (RSA-2048 signed blobs).
constexpr size_t TMD_HEADER_SIZE = 0x1E4;
constexpr size_t TMD_CONTENT_SIZE = 0x24;
constexpr size_t TMD_TITLE_ID = 0x18C;
constexpr size_t TMD_GROUP_ID = 0x198;
constexpr size_t TMD_NUM_CONTENTS = 0x1DE;
constexpr size_t TICKET_SIZE = 0x2A4;
constexpr size_t TICKET_TITLE_ID = 0x1DC;

uint64_t Be64(const uint8_t* p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = v << 8 | p[i];
  return v;
}
uint32_t Be32(const uint8_t* p)
{
  return uint32_t{p[0]} << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
uint16_t Be16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}

bool TmdValid(const std::vector<uint8_t>& tmd)
{
  return tmd.size() >= TMD_HEADER_SIZE &&
         tmd.size() >= TMD_HEADER_SIZE + Be16(&tmd[TMD_NUM_CONTENTS]) * TMD_CONTENT_SIZE;
}

std::string TitlePath(uint64_t title_id, const char* leaf)
{
  char path[64];
  std::snprintf(path, sizeof(path), "/title/%08x/%08x/%s", static_cast<uint32_t>(title_id >> 32),
                static_cast<uint32_t>(title_id), leaf);
  return path;
}

class EsDevice final : public Es
{
public:
  EsDevice(const Host& host, Nand& nand) : Es(host, "/dev/es"), m_nand(nand) {}

  std::optional<Reply> Open(const OpenRequest& request) override
  {
    for (int32_t& fd : m_contexts)
    {
      if (fd < 0)
      {
        fd = request.fd;
        m_active = true;
        return Reply{IPC_SUCCESS};
      }
    }
    return Reply{ES_FD_EXHAUSTED};
  }

  std::optional<Reply> Close(int32_t fd) override
  {
    for (int32_t& context : m_contexts)
    {
      if (context == fd)
      {
        context = -1;
        m_active = false;
        return Reply{IPC_SUCCESS};
      }
    }
    return Reply{ES_EINVAL};
  }

  std::optional<Reply> IOCtlV(const IOCtlVRequest& request) override
  {
    bool open = false;
    for (int32_t context : m_contexts)
      open |= context == request.fd;
    if (!open)
      return Reply{ES_EINVAL};
    switch (request.request)
    {
    case IOCTLV_GETTITLEID:
      if (!request.HasVectors(0, 1))
        return Reply{ES_EINVAL};
      if (!m_title_active)
        return Reply{ES_EINVAL};
      m_memory.Write64(request.io[0].address, m_title_id);
      return Reply{IPC_SUCCESS};
    case IOCTLV_GETTITLEDIR:
    {
      if (!request.HasVectors(1, 1))
        return Reply{ES_EINVAL};
      // Without the terminator, as IOS.
      const std::string path = TitlePath(m_memory.Read64(request.in[0].address), "data");
      m_memory.CopyTo(request.io[0].address, path.data(), static_cast<uint32_t>(path.size()));
      return Reply{IPC_SUCCESS};
    }
    default:
      std::fprintf(stderr, "[native-ios] /dev/es: unhandled ioctlv %#x\n", request.request);
      return Reply{IPC_EINVAL};
    }
  }

  int32_t DiVerify(const std::vector<uint8_t>& tmd, const std::vector<uint8_t>& ticket) override
  {
    m_title_active = false;
    if (!TmdValid(tmd) || ticket.size() < TICKET_SIZE)
      return ES_EINVAL;
    const uint64_t title_id = Be64(&tmd[TMD_TITLE_ID]);
    if (title_id != Be64(&ticket[TICKET_TITLE_ID]))
      return ES_EINVAL;
    m_title_active = true;
    m_title_id = title_id;

    std::vector<uint8_t> installed;
    if (m_nand.ReadFile(TitlePath(title_id, "content/title.tmd"), &installed) != IPC_SUCCESS ||
        !TmdValid(installed))
    {
      if (const int32_t result = WriteTmd(title_id, tmd))
        return result;
    }

    const uint32_t uid = UidFor(title_id);
    if (uid == 0)
      return ES_SHORT_READ;
    m_ppc_uid = uid;
    m_ppc_gid = Be16(&tmd[TMD_GROUP_ID]);
    m_nand.SetPpcIds(m_ppc_uid, m_ppc_gid);

    // May already exist; only the metadata call's result counts.
    constexpr NandModes data_modes{NAND_RW, NAND_NONE, NAND_NONE};
    const std::string data_dir = TitlePath(title_id, "data");
    m_nand.CreateDirectory(0, 0, data_dir, 0, data_modes);
    return m_nand.SetMetadata(0, data_dir, m_ppc_uid, m_ppc_gid, 0, data_modes);
  }

  int32_t CreateTitleDirectories(uint64_t title_id, uint16_t gid) override
  {
    constexpr NandModes title_modes{NAND_RW, NAND_RW, NAND_READ};
    constexpr NandModes content_modes{NAND_RW, NAND_RW, NAND_NONE};
    constexpr NandModes data_modes{NAND_RW, NAND_NONE, NAND_NONE};
    const std::string content_dir = TitlePath(title_id, "content");
    if (const int32_t result = m_nand.CreateFullPath(0, 0, content_dir + "/", 0, title_modes))
      return result;
    if (const int32_t result = m_nand.SetMetadata(0, content_dir, 0, 0, 0, content_modes))
      return result;
    const std::string data_dir = TitlePath(title_id, "data");
    if (!m_nand.Exists(data_dir))
    {
      if (const int32_t result = m_nand.CreateDirectory(0, 0, data_dir, 0, data_modes))
        return result;
    }
    const uint32_t uid = UidFor(title_id);
    if (uid == 0)
      return ES_SHORT_READ;
    return m_nand.SetMetadata(0, data_dir, uid, gid, 0, data_modes);
  }

  void DoState(StateIO& io) override
  {
    DoBaseState(io);
    for (int32_t& fd : m_contexts)
      io.Do(fd);
    io.Do(m_title_active);
    io.Do(m_title_id);
    io.Do(m_ppc_uid);
    io.Do(m_ppc_gid);
    if (io.Reading())
      m_nand.SetPpcIds(m_ppc_uid, m_ppc_gid);
  }

private:
  int32_t WriteTmd(uint64_t title_id, const std::vector<uint8_t>& tmd)
  {
    const std::string temp = "/tmp/title.tmd";
    constexpr NandModes internal_modes{NAND_RW, NAND_RW, NAND_NONE};
    constexpr NandModes parent_modes{NAND_RW, NAND_RW, NAND_READ};
    m_nand.Delete(0, 0, temp);
    if (const int32_t result = m_nand.WriteFile(0, 0, temp, internal_modes, 0, tmd.data(),
                                                static_cast<uint32_t>(tmd.size())))
    {
      return result;
    }
    const std::string tmd_path = TitlePath(title_id, "content/title.tmd");
    if (const int32_t result = m_nand.CreateFullPath(0, 0, tmd_path, 0, parent_modes))
      return result;
    m_nand.SetMetadata(0, TitlePath(title_id, "content"), 0, 0, 0, internal_modes);
    return m_nand.Rename(0, 0, temp, tmd_path);
  }

  // /sys/uid.sys: big-endian (title u64, uid u32) records; a title without
  // one gets the next uid, appended.
  uint32_t UidFor(uint64_t title_id)
  {
    std::map<uint32_t, uint64_t> entries;
    std::vector<uint8_t> data;
    if (m_nand.ReadFile(UID_MAP_PATH, &data) == IPC_SUCCESS)
    {
      for (size_t i = 0; i + 12 <= data.size(); i += 12)
      {
        const uint64_t title = Be64(&data[i]);
        const uint32_t uid = Be32(&data[i + 8]);
        if (!title && !uid)
          break;
        entries.emplace(uid, title);
      }
    }
    if (entries.empty() && !Insert(entries, SYSTEM_MENU))
      return 0;
    for (const auto& [uid, title] : entries)
    {
      if (title == title_id)
        return uid;
    }
    return Insert(entries, title_id);
  }

  uint32_t Insert(std::map<uint32_t, uint64_t>& entries, uint64_t title_id)
  {
    const uint32_t uid = entries.empty() ? FIRST_PPC_UID : entries.rbegin()->first + 1;
    entries.emplace(uid, title_id);
    std::array<uint8_t, 12> record;
    for (int i = 0; i < 8; ++i)
      record[i] = static_cast<uint8_t>(title_id >> (56 - 8 * i));
    for (int i = 0; i < 4; ++i)
      record[8 + i] = static_cast<uint8_t>(uid >> (24 - 8 * i));
    constexpr NandModes modes{NAND_RW, NAND_RW, NAND_NONE};
    if (m_nand.WriteFile(0, 0, UID_MAP_PATH, modes, UINT32_MAX, record.data(), 12) != IPC_SUCCESS)
      return 0;
    return uid;
  }

  Nand& m_nand;
  std::array<int32_t, 3> m_contexts{-1, -1, -1};
  bool m_title_active = false;
  uint64_t m_title_id = 0;
  uint32_t m_ppc_uid = 0;
  uint16_t m_ppc_gid = 0;
};
}  // namespace

std::unique_ptr<Es> CreateEs(const Host& host, Nand& nand)
{
  return std::make_unique<EsDevice>(host, nand);
}
}  // namespace dualize::ios
