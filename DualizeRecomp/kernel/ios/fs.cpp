// /dev/fs: the NAND file system over a host directory, with Dolphin's
// metadata file (fst.bin: owner, group, modes and attribute per entry) and
// Dolphin's model of the FS module's timing (cluster cache, superblock
// flushes), so the same NAND works under either chassis.

#include "kernel/ios/devices.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace dualize::ios
{
namespace
{
namespace fs = std::filesystem;

enum class Result
{
  Success,
  Invalid,
  AccessDenied,
  SuperblockWriteFailed,
  SuperblockInitFailed,
  AlreadyExists,
  NotFound,
  FstFull,
  NoFreeSpace,
  NoFreeHandle,
  TooManyPathComponents,
  InUse,
  BadBlock,
  EccError,
  CriticalEccError,
  FileNotEmpty,
  CheckFailed,
  UnknownError,
  ShortRead,
};

int32_t Convert(Result result)
{
  return result == Result::Success ? IPC_SUCCESS : -(static_cast<int32_t>(result) + 100);
}

enum Mode : uint8_t
{
  MODE_NONE = 0,
  MODE_READ = 1,
  MODE_WRITE = 2,
  MODE_RW = 3,
};

struct Modes
{
  uint8_t owner = MODE_NONE, group = MODE_NONE, other = MODE_NONE;
  bool operator==(const Modes&) const = default;
};

struct Metadata
{
  uint32_t uid = 0;
  uint16_t gid = 0;
  bool is_file = false;
  Modes modes;
  uint8_t attribute = 0;
};

struct FstEntry
{
  std::string name;
  Metadata data;
  std::vector<FstEntry> children;

  bool CheckPermission(uint32_t uid, uint16_t gid, uint8_t mode) const
  {
    if (uid == 0)
      return true;
    uint8_t file_mode = data.modes.other;
    if (data.uid == uid)
      file_mode = data.modes.owner;
    else if (data.gid == gid)
      file_mode = data.modes.group;
    return (mode & file_mode) == mode;
  }
};

constexpr size_t MAX_PATH_LENGTH = 64;
constexpr size_t MAX_PATH_DEPTH = 8;
constexpr size_t MAX_FILENAME_LENGTH = 12;
constexpr uint32_t CLUSTER_SIZE = 16384;
constexpr uint32_t TOTAL_CLUSTERS = 0x7EC0;
constexpr uint32_t RESERVED_CLUSTERS = 0x0300;
constexpr uint32_t USABLE_CLUSTERS = TOTAL_CLUSTERS - RESERVED_CLUSTERS;
constexpr uint32_t TOTAL_INODES = 0x17FF;
constexpr uint32_t MAX_HANDLES = 16;

enum Ioctl : uint32_t
{
  IOCTL_FORMAT = 1,
  IOCTL_GETSTATS = 2,
  IOCTL_CREATEDIR = 3,
  IOCTLV_READDIR = 4,
  IOCTL_SETATTR = 5,
  IOCTL_GETATTR = 6,
  IOCTL_DELETE = 7,
  IOCTL_RENAME = 8,
  IOCTL_CREATEFILE = 9,
  IOCTL_SETFILEVERCTRL = 10,
  IOCTL_GETFILESTATS = 11,
  IOCTLV_GETUSAGE = 12,
  IOCTL_SHUTDOWN = 13,
};

// The FS module's costs for IOS versions below 28 (NiGHTS runs IOS21).
constexpr int64_t SUPERBLOCK_WRITE = Tb(4100000);
constexpr int64_t CLUSTER_WRITE = Tb(370000);
constexpr int64_t CLUSTER_READ = Tb(165000);
constexpr int64_t FREE_CLUSTER_CHECK = Tb(1000);
int64_t MemcpyTicks(uint64_t size)
{
  return Tb(size + 3);
}

bool IsValidNonRootPath(std::string_view path)
{
  return path.size() > 1 && path.size() <= MAX_PATH_LENGTH && path[0] == '/' && path.back() != '/';
}
bool IsValidPath(std::string_view path)
{
  return path == "/" || IsValidNonRootPath(path);
}
bool IsPrintable(char c)
{
  return c >= 0x20 && c < 0x7F;
}

struct SplitPath
{
  std::string parent, name;
};
SplitPath Split(std::string_view path)
{
  const size_t last = path.rfind('/');
  return {std::string(path.substr(0, std::max<size_t>(1, last))), std::string(path.substr(last + 1))};
}

std::vector<std::string> Components(std::string_view path)
{
  std::vector<std::string> parts;
  size_t start = 1;
  while (start <= path.size())
  {
    const size_t end = std::min(path.find('/', start), path.size());
    parts.emplace_back(path.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

bool IsIllegal(char c)
{
  return static_cast<unsigned char>(c) <= 0x1F || std::string_view("\"*/:<>?\\|\x7f").find(c) !=
                                                       std::string_view::npos;
}

// Dolphin's NAND path escaping (Common::EscapeFileName).
std::string EscapeName(const std::string& name)
{
  if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c == '.'; }))
  {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i)
      out += "__2e__";
    return out;
  }
  std::string doubled;
  for (size_t i = 0; i < name.size(); ++i)
  {
    if (name[i] == '_' && i + 1 < name.size() && name[i + 1] == '_')
    {
      doubled += "__5f____5f__";
      ++i;
    }
    else
    {
      doubled.push_back(name[i]);
    }
  }
  std::string out;
  for (char c : doubled)
  {
    if (IsIllegal(c))
    {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "__%02x__", static_cast<unsigned char>(c));
      out += buf;
    }
    else
    {
      out.push_back(c);
    }
  }
  return out;
}

std::string UnescapeName(std::string name)
{
  size_t pos = 0;
  while ((pos = name.find("__", pos)) != std::string::npos)
  {
    if (pos + 6 <= name.size() && name[pos + 4] == '_' && name[pos + 5] == '_')
    {
      char* end = nullptr;
      const std::string hex = name.substr(pos + 2, 2);
      const long c = std::strtol(hex.c_str(), &end, 16);
      if (end == hex.c_str() + 2)
        name.replace(pos, 6, 1, static_cast<char>(c));
    }
    ++pos;
  }
  return name;
}

class HostFile
{
public:
  explicit HostFile(const fs::path& path) : m_file(OpenFile(path)) {}
  ~HostFile()
  {
    if (m_file)
      std::fclose(m_file);
  }
  HostFile(const HostFile&) = delete;
  HostFile& operator=(const HostFile&) = delete;
  bool Ok() const { return m_file != nullptr; }
  uint32_t Size()
  {
    std::fflush(m_file);
    std::fseek(m_file, 0, SEEK_END);
    return static_cast<uint32_t>(std::ftell(m_file));
  }
  uint32_t Read(uint32_t offset, uint8_t* data, uint32_t size)
  {
    std::fseek(m_file, static_cast<long>(offset), SEEK_SET);
    return static_cast<uint32_t>(std::fread(data, 1, size, m_file));
  }
  bool Write(uint32_t offset, const uint8_t* data, uint32_t size)
  {
    std::fseek(m_file, static_cast<long>(offset), SEEK_SET);
    return std::fwrite(data, 1, size, m_file) == size && std::fflush(m_file) == 0;
  }

private:
  static FILE* OpenFile(const fs::path& path)
  {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"r+b");
#else
    return std::fopen(path.c_str(), "r+b");
#endif
  }
  FILE* m_file;
};

class FsDevice final : public Nand
{
public:
  FsDevice(const Host& host, const Config& config) : Nand(host, "/dev/fs")
  {
    m_root = config.nand_root;
    std::error_code error;
    fs::create_directories(m_root, error);
    ResetFst();
    LoadFst();
    // IOS empties /tmp at boot.
    if (DeletePath(0, 0, "/tmp") == Result::Success)
      CreatePath(0, 0, "/tmp", 0, {MODE_RW, MODE_RW, MODE_RW}, false);
  }

  std::optional<Reply> Open(const OpenRequest& request) override
  {
    int64_t ticks = IPC_OVERHEAD;
    if (m_fds.size() >= MAX_HANDLES)
      return Reply{Convert(Result::NoFreeHandle), ticks};
    if (request.path.size() >= 64)
      return Reply{Convert(Result::Invalid), ticks};
    const uint32_t fd = static_cast<uint32_t>(request.fd);
    if (request.path == "/dev/fs")
    {
      m_fds[fd] = Fd{m_ppc_uid, m_ppc_gid, -1, false};
      return Reply{static_cast<int32_t>(fd), ticks};
    }
    ticks += LookupTicks(request.path, false);
    int handle = -1;
    const Result result = OpenFile(request.path, static_cast<uint8_t>(request.flags & 3), &handle);
    if (result != Result::Success)
      return Reply{Convert(result), ticks};
    m_fds[fd] = Fd{m_ppc_uid, m_ppc_gid, handle, false};
    return Reply{static_cast<int32_t>(fd), ticks};
  }

  std::optional<Reply> Close(int32_t fd) override
  {
    int64_t ticks = IPC_OVERHEAD;
    const auto it = m_fds.find(static_cast<uint32_t>(fd));
    if (it == m_fds.end())
      return Reply{IPC_SUCCESS, ticks};
    if (it->second.handle >= 0)
    {
      if (m_cache_fd == static_cast<int64_t>(fd))
      {
        ticks += FlushCache();
        m_cache_fd = -1;
      }
      if (it->second.superblock_flush)
        ticks += SUPERBLOCK_WRITE;
      m_handles[it->second.handle] = Handle{};
      ReleaseUnusedFiles();
    }
    m_fds.erase(it);
    return Reply{IPC_SUCCESS, ticks};
  }

  std::optional<Reply> Read(const ReadWriteRequest& request) override
  {
    return ReadWrite(request, false);
  }
  std::optional<Reply> Write(const ReadWriteRequest& request) override
  {
    return ReadWrite(request, true);
  }

  std::optional<Reply> Seek(const SeekRequest& request) override
  {
    const int64_t ticks = IPC_OVERHEAD;
    Handle* handle = HandleOf(request.fd);
    if (!handle)
      return Reply{Convert(Result::Invalid), ticks};
    const uint32_t size = handle->file->Size();
    uint32_t position;
    switch (request.mode)
    {
    case 0:
      position = static_cast<uint32_t>(request.offset);
      break;
    case 1:
      position = handle->offset + static_cast<uint32_t>(request.offset);
      break;
    case 2:
      position = size + static_cast<uint32_t>(request.offset);
      break;
    default:
      return Reply{Convert(Result::Invalid), ticks};
    }
    if (position > size)
      return Reply{Convert(Result::Invalid), ticks};
    handle->offset = position;
    return Reply{static_cast<int32_t>(position), ticks};
  }

  std::optional<Reply> IOCtl(const IOCtlRequest& request) override
  {
    const auto it = m_fds.find(static_cast<uint32_t>(request.fd));
    if (it == m_fds.end())
      return Reply{Convert(Result::Invalid)};
    const Fd& fd = it->second;
    switch (request.request)
    {
    case IOCTL_FORMAT:
      return Reply{Convert(Result::AccessDenied), IPC_OVERHEAD};
    case IOCTL_GETSTATS:
      return GetStats(request);
    case IOCTL_CREATEDIR:
    case IOCTL_CREATEFILE:
    {
      Params params;
      if (!ReadParams(request, &params))
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      const bool file = request.request == IOCTL_CREATEFILE;
      const Result result = CreatePath(fd.uid, fd.gid, params.path, params.attribute, params.modes,
                                       file);
      // As Dolphin, which drops the ticks CreateFile measures.
      if (file)
        return Reply{Convert(result), 0};
      return Reply{Convert(result),
                   IPC_OVERHEAD + (result == Result::Success ? SUPERBLOCK_WRITE : 0)};
    }
    case IOCTL_SETATTR:
    {
      Params params;
      if (!ReadParams(request, &params))
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      const Result result = SetMetadata(fd.uid, params.path, params.uid, params.gid,
                                        params.attribute, params.modes);
      return Reply{Convert(result),
                   IPC_OVERHEAD + (result == Result::Success ? SUPERBLOCK_WRITE : 0)};
    }
    case IOCTL_GETATTR:
      return GetAttribute(fd, request);
    case IOCTL_DELETE:
    {
      if (request.buffer_in_size < 64)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      const std::string path = m_memory.ReadString(request.buffer_in, 64);
      return Reply{Convert(DeletePath(fd.uid, fd.gid, path)), IPC_OVERHEAD + SUPERBLOCK_WRITE};
    }
    case IOCTL_RENAME:
    {
      if (request.buffer_in_size < 128)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      const std::string from = m_memory.ReadString(request.buffer_in, 64);
      const std::string to = m_memory.ReadString(request.buffer_in + 64, 64);
      return Reply{Convert(RenamePath(fd.uid, fd.gid, from, to)), IPC_OVERHEAD + SUPERBLOCK_WRITE};
    }
    case IOCTL_SETFILEVERCTRL:
    {
      Params params;
      if (!ReadParams(request, &params))
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      return Reply{IPC_SUCCESS, IPC_OVERHEAD};
    }
    case IOCTL_GETFILESTATS:
    {
      if (request.buffer_out_size < 8)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      Handle* handle = HandleOf(request.fd);
      if (!handle)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      m_memory.Write32(request.buffer_out, handle->file->Size());
      m_memory.Write32(request.buffer_out + 4, handle->offset);
      return Reply{IPC_SUCCESS, IPC_OVERHEAD};
    }
    case IOCTL_SHUTDOWN:
      return Reply{IPC_SUCCESS, IPC_OVERHEAD};
    default:
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    }
  }

  std::optional<Reply> IOCtlV(const IOCtlVRequest& request) override
  {
    const auto it = m_fds.find(static_cast<uint32_t>(request.fd));
    if (it == m_fds.end())
      return Reply{Convert(Result::Invalid)};
    switch (request.request)
    {
    case IOCTLV_READDIR:
      return ReadDirectory(it->second, request);
    case IOCTLV_GETUSAGE:
      return GetUsage(request);
    default:
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    }
  }

  void DoState(StateIO& io) override
  {
    DoBaseState(io);
    io.Do(m_dirty_cache);
    io.Do(m_cache_chain);
    io.Do(m_cache_fd);
    uint32_t fds = static_cast<uint32_t>(m_fds.size());
    io.Do(fds);
    if (io.Reading())
      m_fds.clear();
    auto it = m_fds.begin();
    for (uint32_t i = 0; i < fds; ++i)
    {
      uint32_t key = 0;
      Fd fd;
      if (!io.Reading())
      {
        key = it->first;
        fd = it->second;
        ++it;
      }
      io.Do(key);
      io.Do(fd.uid);
      io.Do(fd.gid);
      io.Do(fd.handle);
      io.Do(fd.superblock_flush);
      if (io.Reading())
        m_fds[key] = fd;
    }
    for (Handle& handle : m_handles)
    {
      io.Do(handle.opened);
      io.Do(handle.mode);
      io.Do(handle.path);
      io.Do(handle.offset);
      if (io.Reading())
        handle.file = handle.opened ? OpenHostFile(HostPath(handle.path)) : nullptr;
    }
    if (io.Reading())
      ReleaseUnusedFiles();
  }

  // --- kernel calls ---

  void SetPpcIds(uint32_t uid, uint16_t gid) override
  {
    m_ppc_uid = uid;
    m_ppc_gid = gid;
  }
  int32_t CreateFile(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attribute,
                     NandModes modes) override
  {
    return Convert(CreatePath(uid, gid, path, attribute, ToModes(modes), true));
  }
  int32_t CreateDirectory(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attribute,
                          NandModes modes) override
  {
    return Convert(CreatePath(uid, gid, path, attribute, ToModes(modes), false));
  }
  int32_t CreateFullPath(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attribute,
                         NandModes modes) override
  {
    for (size_t slash = path.find('/', 1); slash != std::string::npos;
         slash = path.find('/', slash + 1))
    {
      const std::string prefix = path.substr(0, slash);
      if (FstEntry* entry = EntryFor(prefix))
      {
        if (entry->data.is_file)
          return Convert(Result::Invalid);
        continue;
      }
      const Result result = CreatePath(uid, gid, prefix, attribute, ToModes(modes), false);
      if (result != Result::Success)
        return Convert(result);
    }
    return IPC_SUCCESS;
  }
  int32_t SetMetadata(uint32_t caller, const std::string& path, uint32_t uid, uint16_t gid,
                      uint8_t attribute, NandModes modes) override
  {
    return Convert(SetMetadata(caller, path, uid, gid, attribute, ToModes(modes)));
  }
  int32_t Delete(uint32_t uid, uint16_t gid, const std::string& path) override
  {
    return Convert(DeletePath(uid, gid, path));
  }
  int32_t Rename(uint32_t uid, uint16_t gid, const std::string& from,
                 const std::string& to) override
  {
    return Convert(RenamePath(uid, gid, from, to));
  }
  bool Exists(const std::string& path) override { return EntryFor(path) != nullptr; }
  int32_t ReadFile(const std::string& path, std::vector<uint8_t>* data) override
  {
    std::error_code error;
    const fs::path host = HostPath(path);
    if (!IsValidNonRootPath(path) || !fs::is_regular_file(host, error))
      return Convert(Result::NotFound);
    std::shared_ptr<HostFile> file = OpenHostFile(host);
    if (!file)
      return Convert(Result::AccessDenied);
    data->resize(file->Size());
    const bool ok = file->Read(0, data->data(), static_cast<uint32_t>(data->size())) == data->size();
    file.reset();
    ReleaseUnusedFiles();
    return ok ? IPC_SUCCESS : Convert(Result::ShortRead);
  }
  int32_t WriteFile(uint32_t uid, uint16_t gid, const std::string& path, NandModes modes,
                    uint32_t offset, const void* data, uint32_t size) override
  {
    if (!Exists(path))
    {
      const Result result = CreatePath(uid, gid, path, 0, ToModes(modes), true);
      if (result != Result::Success)
        return Convert(result);
    }
    std::error_code error;
    const fs::path host = HostPath(path);
    if (!fs::is_regular_file(host, error))
      return Convert(Result::Invalid);
    std::shared_ptr<HostFile> file = OpenHostFile(host);
    if (!file)
      return Convert(Result::AccessDenied);
    if (offset == UINT32_MAX)
      offset = file->Size();
    const bool ok = file->Write(offset, static_cast<const uint8_t*>(data), size);
    file.reset();
    ReleaseUnusedFiles();
    return ok ? IPC_SUCCESS : Convert(Result::UnknownError);
  }

private:
  static Modes ToModes(NandModes modes) { return {modes.owner, modes.group, modes.other}; }

  struct Handle
  {
    bool opened = false;
    uint8_t mode = 0;
    std::string path;
    uint32_t offset = 0;
    std::shared_ptr<HostFile> file;
  };
  struct Fd
  {
    uint32_t uid = 0;
    uint16_t gid = 0;
    int32_t handle = -1;  // -1: the /dev/fs device itself
    bool superblock_flush = false;
  };
  struct Params
  {
    uint32_t uid;
    uint16_t gid;
    std::string path;
    Modes modes;
    uint8_t attribute;
  };

  fs::path HostPath(const std::string& wii_path) const
  {
    std::string escaped;
    for (const std::string& part : Components(wii_path))
      escaped += "/" + EscapeName(part);
    return fs::path(m_root.string() + escaped);
  }
  fs::path FstPath() const { return m_root / "fst.bin"; }

  static int64_t LookupTicks(const std::string& path, bool split)
  {
    const int64_t components = std::count(path.begin(), path.end(), '/');
    if (components == 0)
      return 0;
    if (path.back() == '/')
      return Tb(300);
    return split ? Tb(1000 + 340 * components) : Tb(680 * components);
  }

  // --- metadata (fst.bin) ---

  void ResetFst()
  {
    m_root_entry = {};
    m_root_entry.name = "/";
    m_root_entry.data.modes = {MODE_NONE, MODE_READ, MODE_READ};
  }

  void LoadFst()
  {
    FILE* file = OpenRead(FstPath());
    if (!file)
      return;
    bool ok = true;
    auto parse = [&](auto& self, size_t depth) -> FstEntry {
      FstEntry entry;
      uint8_t raw[0x20];
      if (depth > MAX_PATH_DEPTH || std::fread(raw, 1, sizeof(raw), file) != sizeof(raw))
      {
        ok = false;
        return entry;
      }
      entry.name.assign(reinterpret_cast<const char*>(raw), strnlen(reinterpret_cast<const char*>(raw), 12));
      entry.data.uid = uint32_t{raw[12]} << 24 | uint32_t{raw[13]} << 16 | uint32_t{raw[14]} << 8 | raw[15];
      entry.data.gid = static_cast<uint16_t>(raw[16] << 8 | raw[17]);
      entry.data.is_file = raw[18] != 0;
      entry.data.modes = {raw[19], raw[20], raw[21]};
      entry.data.attribute = raw[22];
      const uint32_t children =
          uint32_t{raw[28]} << 24 | uint32_t{raw[29]} << 16 | uint32_t{raw[30]} << 8 | raw[31];
      for (uint32_t i = 0; ok && i < children; ++i)
        entry.children.push_back(self(self, depth + 1));
      return entry;
    };
    FstEntry root = parse(parse, 0);
    std::fclose(file);
    if (ok)
      m_root_entry = std::move(root);
  }

  void SaveFst() const
  {
    std::vector<uint8_t> out;
    auto collect = [&](auto& self, const FstEntry& entry) -> void {
      uint8_t raw[0x20] = {};
      std::memcpy(raw, entry.name.data(), std::min<size_t>(entry.name.size(), 12));
      raw[12] = static_cast<uint8_t>(entry.data.uid >> 24);
      raw[13] = static_cast<uint8_t>(entry.data.uid >> 16);
      raw[14] = static_cast<uint8_t>(entry.data.uid >> 8);
      raw[15] = static_cast<uint8_t>(entry.data.uid);
      raw[16] = static_cast<uint8_t>(entry.data.gid >> 8);
      raw[17] = static_cast<uint8_t>(entry.data.gid);
      raw[18] = entry.data.is_file;
      raw[19] = entry.data.modes.owner;
      raw[20] = entry.data.modes.group;
      raw[21] = entry.data.modes.other;
      raw[22] = entry.data.attribute;
      const uint32_t children = static_cast<uint32_t>(entry.children.size());
      raw[28] = static_cast<uint8_t>(children >> 24);
      raw[29] = static_cast<uint8_t>(children >> 16);
      raw[30] = static_cast<uint8_t>(children >> 8);
      raw[31] = static_cast<uint8_t>(children);
      out.insert(out.end(), raw, raw + sizeof(raw));
      for (const FstEntry& child : entry.children)
        self(self, child);
    };
    collect(collect, m_root_entry);
    const fs::path temp = FstPath().string() + ".tmp";
    {
      FILE* file = OpenWrite(temp);
      if (!file)
        return;
      const bool ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();
      std::fclose(file);
      if (!ok)
        return;
    }
    std::error_code error;
    fs::rename(temp, FstPath(), error);
  }

  // The entry for path, created with default metadata if the host has the
  // file but fst.bin does not know it.
  FstEntry* EntryFor(const std::string& path)
  {
    if (path == "/")
      return &m_root_entry;
    if (!IsValidNonRootPath(path))
      return nullptr;
    std::error_code error;
    const fs::path host = HostPath(path);
    const fs::file_status status = fs::status(host, error);
    if (error || !fs::exists(status))
      return nullptr;
    FstEntry* entry = &m_root_entry;
    for (const std::string& part : Components(path))
    {
      const auto next = std::find_if(entry->children.begin(), entry->children.end(),
                                     [&](const FstEntry& e) { return e.name == part; });
      if (next != entry->children.end())
      {
        entry = &*next;
      }
      else
      {
        entry = &entry->children.emplace_back();
        entry->name = part;
        entry->data.modes = {MODE_RW, MODE_RW, MODE_RW};
      }
    }
    entry->data.is_file = fs::is_regular_file(status);
    if (entry->data.is_file)
      entry->children.clear();
    return entry;
  }

  bool IsFileOpened(const std::string& path) const
  {
    return std::any_of(m_handles.begin(), m_handles.end(),
                       [&](const Handle& h) { return h.opened && h.path == path; });
  }
  bool IsDirectoryInUse(const std::string& path) const
  {
    return std::any_of(m_handles.begin(), m_handles.end(), [&](const Handle& h) {
      return h.opened && h.path.compare(0, path.size(), path) == 0;
    });
  }

  // --- operations ---

  std::shared_ptr<HostFile> OpenHostFile(const fs::path& host)
  {
    const std::string key = host.string();
    if (auto it = m_open_files.find(key); it != m_open_files.end())
      return it->second;
    auto file = std::make_shared<HostFile>(host);
    if (!file->Ok())
      return nullptr;
    m_open_files[key] = file;
    return file;
  }

  void ReleaseUnusedFiles()
  {
    for (auto it = m_open_files.begin(); it != m_open_files.end();)
      it = it->second.use_count() == 1 ? m_open_files.erase(it) : std::next(it);
  }

  Result OpenFile(const std::string& path, uint8_t mode, int* handle_index)
  {
    const auto free = std::find_if(m_handles.begin(), m_handles.end(),
                                   [](const Handle& h) { return !h.opened; });
    if (free == m_handles.end())
      return Result::NoFreeHandle;
    std::error_code error;
    const fs::path host = HostPath(path);
    if (fs::is_directory(host, error))
      return Result::Invalid;
    if (!fs::is_regular_file(host, error))
      return Result::NotFound;
    std::shared_ptr<HostFile> file = OpenHostFile(host);
    if (!file)
      return Result::AccessDenied;
    *free = Handle{true, mode, path, 0, std::move(file)};
    *handle_index = static_cast<int>(free - m_handles.begin());
    return Result::Success;
  }

  Handle* HandleOf(int32_t fd)
  {
    const auto it = m_fds.find(static_cast<uint32_t>(fd));
    if (it == m_fds.end() || it->second.handle < 0)
      return nullptr;
    Handle& handle = m_handles[it->second.handle];
    return handle.opened && handle.file ? &handle : nullptr;
  }

  Result CreatePath(uint32_t uid, uint16_t gid, const std::string& path, uint8_t attribute,
                    Modes modes, bool is_file)
  {
    if (!IsValidNonRootPath(path) || !std::all_of(path.begin(), path.end(), IsPrintable))
      return Result::Invalid;
    if (!is_file && static_cast<size_t>(std::count(path.begin(), path.end(), '/')) > MAX_PATH_DEPTH)
      return Result::TooManyPathComponents;
    const SplitPath split = Split(path);
    const fs::path host = HostPath(path);
    FstEntry* parent = EntryFor(split.parent);
    if (!parent)
      return Result::NotFound;
    if (!parent->CheckPermission(uid, gid, MODE_WRITE))
      return Result::AccessDenied;
    std::error_code error;
    if (fs::exists(host, error))
      return Result::AlreadyExists;
    if (is_file)
    {
      FILE* file = OpenWrite(host);
      if (!file)
        return Result::UnknownError;
      std::fclose(file);
    }
    else if (!fs::create_directory(host, error))
    {
      return Result::UnknownError;
    }
    FstEntry* child = EntryFor(path);
    *child = {};
    child->name = split.name;
    child->data = Metadata{uid, gid, is_file, modes, attribute};
    SaveFst();
    return Result::Success;
  }

  Result DeletePath(uint32_t uid, uint16_t gid, const std::string& path)
  {
    if (!IsValidNonRootPath(path))
      return Result::Invalid;
    const fs::path host = HostPath(path);
    const SplitPath split = Split(path);
    FstEntry* parent = EntryFor(split.parent);
    if (!parent)
      return Result::NotFound;
    if (!parent->CheckPermission(uid, gid, MODE_WRITE))
      return Result::AccessDenied;
    std::error_code error;
    if (!fs::exists(host, error))
      return Result::NotFound;
    if (fs::is_regular_file(host, error) && !IsFileOpened(path))
      fs::remove(host, error);
    else if (fs::is_directory(host, error) && !IsDirectoryInUse(path))
      fs::remove_all(host, error);
    else
      return Result::InUse;
    const auto it = std::find_if(parent->children.begin(), parent->children.end(),
                                 [&](const FstEntry& e) { return e.name == split.name; });
    if (it != parent->children.end())
      parent->children.erase(it);
    SaveFst();
    return Result::Success;
  }

  Result RenamePath(uint32_t uid, uint16_t gid, const std::string& from, const std::string& to)
  {
    if (!IsValidNonRootPath(from) || !IsValidNonRootPath(to))
      return Result::Invalid;
    const SplitPath split_from = Split(from);
    const SplitPath split_to = Split(to);
    FstEntry* from_parent = EntryFor(split_from.parent);
    FstEntry* to_parent = EntryFor(split_to.parent);
    if (!from_parent || !to_parent)
      return Result::NotFound;
    if (!from_parent->CheckPermission(uid, gid, MODE_WRITE) ||
        !to_parent->CheckPermission(uid, gid, MODE_WRITE))
    {
      return Result::AccessDenied;
    }
    FstEntry* entry = EntryFor(from);
    if (!entry)
      return Result::NotFound;
    if (entry->data.is_file && split_from.name != split_to.name)
      return Result::Invalid;
    if ((!entry->data.is_file && IsDirectoryInUse(from)) || (entry->data.is_file && IsFileOpened(from)))
      return Result::InUse;
    const fs::path host_from = HostPath(from);
    const fs::path host_to = HostPath(to);
    std::error_code error;
    if (fs::exists(host_to, error))
    {
      const bool from_file = fs::is_regular_file(host_from, error);
      const bool to_file = fs::is_regular_file(host_to, error);
      if (from_file && to_file)
        fs::remove(host_to, error);
      else if (!from_file && !to_file)
        fs::remove_all(host_to, error);
      else
        return Result::Invalid;
    }
    fs::rename(host_from, host_to, error);
    if (error)
      return Result::NotFound;
    // EntryFor may grow the vectors the old pointers point into.
    FstEntry moved;
    FstEntry* old_parent = EntryFor(split_from.parent);
    const auto it = std::find_if(old_parent->children.begin(), old_parent->children.end(),
                                 [&](const FstEntry& e) { return e.name == split_from.name; });
    const bool had_old = it != old_parent->children.end();
    if (had_old)
    {
      moved = std::move(*it);
      old_parent->children.erase(it);
    }
    FstEntry* new_entry = EntryFor(to);
    new_entry->name = split_to.name;
    if (had_old)
    {
      new_entry->data = moved.data;
      new_entry->children = std::move(moved.children);
    }
    SaveFst();
    return Result::Success;
  }

  Result SetMetadata(uint32_t caller, const std::string& path, uint32_t uid, uint16_t gid,
                     uint8_t attribute, Modes modes)
  {
    if (!IsValidPath(path))
      return Result::Invalid;
    FstEntry* entry = EntryFor(path);
    if (!entry)
      return Result::NotFound;
    if (caller != 0 && (caller != entry->data.uid || uid != entry->data.uid))
      return Result::AccessDenied;
    std::error_code error;
    const bool empty = !entry->data.is_file || fs::file_size(HostPath(path), error) == 0;
    if (entry->data.uid != uid && entry->data.is_file && !empty)
      return Result::FileNotEmpty;
    if (entry->data.gid != gid || entry->data.uid != uid || entry->data.attribute != attribute ||
        !(entry->data.modes == modes))
    {
      entry->data.uid = uid;
      entry->data.gid = gid;
      entry->data.attribute = attribute;
      entry->data.modes = modes;
      SaveFst();
    }
    return Result::Success;
  }

  // --- requests ---

  bool ReadParams(const IOCtlRequest& request, Params* params) const
  {
    // ISFSParams: uid u32, gid u16, path[64], modes[3], attribute.
    if (request.buffer_in_size < 0x4A)
      return false;
    const uint32_t in = request.buffer_in;
    params->uid = m_memory.Read32(in);
    params->gid = m_memory.Read16(in + 4);
    params->path = m_memory.ReadString(in + 6, 64);
    params->modes = {m_memory.Read8(in + 70), m_memory.Read8(in + 71), m_memory.Read8(in + 72)};
    params->attribute = m_memory.Read8(in + 73);
    return true;
  }

  std::optional<Reply> ReadWrite(const ReadWriteRequest& request, bool write)
  {
    int64_t ticks = IPC_OVERHEAD;
    const auto it = m_fds.find(static_cast<uint32_t>(request.fd));
    Handle* handle = HandleOf(request.fd);
    if (it == m_fds.end() || !handle)
      return Reply{Convert(Result::Invalid), ticks};
    uint8_t* data = m_memory.Pointer(request.buffer, request.size);
    ticks += ReadWriteTicks(*handle, it->first, write, request.size);
    if (!(handle->mode & (write ? MODE_WRITE : MODE_READ)))
      return Reply{Convert(Result::AccessDenied), ticks};
    if (!data && request.size)
      return Reply{Convert(Result::Invalid), ticks};
    if (write)
    {
      if (!handle->file->Write(handle->offset, data, request.size))
        return Reply{Convert(Result::AccessDenied), ticks};
      handle->offset += request.size;
      return Reply{static_cast<int32_t>(request.size), ticks};
    }
    const uint32_t size = handle->file->Size();
    const uint32_t count = std::min(request.size, size > handle->offset ? size - handle->offset : 0);
    const uint32_t read = handle->file->Read(handle->offset, data, count);
    handle->offset += read;
    return Reply{static_cast<int32_t>(read), ticks};
  }

  int64_t FlushCache()
  {
    if (m_cache_fd < 0 || !m_dirty_cache)
      return 0;
    m_dirty_cache = false;
    m_fds[static_cast<uint32_t>(m_cache_fd)].superblock_flush = true;
    return CLUSTER_WRITE;
  }

  bool HasCache(uint32_t fd, uint32_t offset) const
  {
    return m_cache_fd == static_cast<int64_t>(fd) && m_cache_chain == offset / CLUSTER_SIZE;
  }

  int64_t PopulateCache(uint32_t fd, uint32_t offset, uint32_t file_size)
  {
    if (HasCache(fd, offset))
      return 0;
    int64_t ticks = FlushCache();
    if ((offset % CLUSTER_SIZE != 0 || offset != file_size) && offset < file_size)
      ticks += CLUSTER_READ;
    m_cache_fd = fd;
    m_cache_chain = offset / CLUSTER_SIZE;
    return ticks;
  }

  int64_t ReadWriteTicks(Handle& handle, uint32_t fd, bool write, uint32_t size)
  {
    int64_t ticks = 0;
    const uint32_t file_size = handle.file->Size();
    uint32_t offset = handle.offset;
    uint32_t count = write ? size : std::min(size, file_size > offset ? file_size - offset : 0);
    while (count != 0)
    {
      uint32_t length;
      if (!HasCache(fd, offset) && count >= CLUSTER_SIZE && offset % CLUSTER_SIZE == 0)
      {
        ticks += write ? CLUSTER_WRITE : CLUSTER_READ;
        length = CLUSTER_SIZE;
        if (write)
          m_fds[fd].superblock_flush = true;
      }
      else
      {
        ticks += PopulateCache(fd, offset, file_size);
        const uint32_t start = offset - m_cache_chain * CLUSTER_SIZE;
        length = std::min(CLUSTER_SIZE - start, count);
        ticks += MemcpyTicks(length);
        if (write)
          ticks += FREE_CLUSTER_CHECK;
        m_dirty_cache = write;
        if (write && (offset + length) % CLUSTER_SIZE == 0)
          ticks += FlushCache();
      }
      offset += length;
      count -= length;
    }
    return ticks;
  }

  std::optional<Reply> GetStats(const IOCtlRequest& request)
  {
    if (request.buffer_out_size < 28)
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    uint64_t clusters = 0, inodes = 0;
    if (!DirectoryStats("/", &clusters, &inodes))
      return Reply{Convert(Result::NotFound)};
    const uint32_t used = static_cast<uint32_t>(std::min<uint64_t>(clusters, USABLE_CLUSTERS));
    const uint32_t used_inodes = static_cast<uint32_t>(std::min<uint64_t>(inodes, TOTAL_INODES));
    const uint32_t out = request.buffer_out;
    m_memory.Write32(out + 0, CLUSTER_SIZE);
    m_memory.Write32(out + 4, USABLE_CLUSTERS - used);
    m_memory.Write32(out + 8, used);
    m_memory.Write32(out + 12, 0);
    m_memory.Write32(out + 16, RESERVED_CLUSTERS);
    m_memory.Write32(out + 20, TOTAL_INODES - used_inodes);
    m_memory.Write32(out + 24, used_inodes);
    return Reply{IPC_SUCCESS};
  }

  std::optional<Reply> GetAttribute(const Fd& fd, const IOCtlRequest& request)
  {
    if (request.buffer_in_size < 64 || request.buffer_out_size < 0x4A)
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    const std::string path = m_memory.ReadString(request.buffer_in, 64);
    const int64_t ticks = IPC_OVERHEAD + LookupTicks(path, true);
    const FstEntry* entry = nullptr;
    if (path == "/")
    {
      entry = &m_root_entry;
    }
    else
    {
      if (!IsValidNonRootPath(path))
        return Reply{Convert(Result::Invalid), ticks};
      const FstEntry* parent = EntryFor(Split(path).parent);
      if (!parent)
        return Reply{Convert(Result::NotFound), ticks};
      if (!parent->CheckPermission(fd.uid, fd.gid, MODE_READ))
        return Reply{Convert(Result::AccessDenied), ticks};
      entry = EntryFor(path);
    }
    if (!entry)
      return Reply{Convert(Result::NotFound), ticks};
    const uint32_t out = request.buffer_out;
    m_memory.Fill(out, 0, 0x4A);
    m_memory.Write32(out, entry->data.uid);
    m_memory.Write16(out + 4, entry->data.gid);
    m_memory.Write8(out + 70, entry->data.modes.owner);
    m_memory.Write8(out + 71, entry->data.modes.group);
    m_memory.Write8(out + 72, entry->data.modes.other);
    m_memory.Write8(out + 73, entry->data.attribute);
    return Reply{IPC_SUCCESS, ticks};
  }

  // Directory listing in IOS's order: fst.bin order reversed, then names
  // unknown to it, descending.
  std::optional<Reply> ReadDirectory(const Fd& fd, const IOCtlVRequest& request)
  {
    if (request.in.empty() || request.in.size() != request.io.size() || request.in.size() > 2 ||
        request.in[0].size != 64)
    {
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    }
    uint32_t list_address = 0, count_address, max_count;
    if (request.in.size() == 2)
    {
      if (request.in[1].size != 4 || request.io[1].size != 4)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      max_count = m_memory.Read32(request.in[1].address);
      count_address = request.io[1].address;
      list_address = request.io[0].address;
      if (request.io[0].size != 13 * max_count)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      m_memory.Write32(count_address, max_count);
    }
    else
    {
      if (request.io[0].size != 4)
        return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
      max_count = m_memory.Read32(request.io[0].address);
      count_address = request.io[0].address;
    }
    const std::string path = m_memory.ReadString(request.in[0].address, 64);
    if (!IsValidPath(path))
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    const FstEntry* entry = EntryFor(path);
    if (!entry)
      return Reply{Convert(Result::NotFound), IPC_OVERHEAD};
    if (!entry->CheckPermission(fd.uid, fd.gid, MODE_READ))
      return Reply{Convert(Result::AccessDenied), IPC_OVERHEAD};
    if (entry->data.is_file)
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};

    std::vector<std::string> names;
    std::error_code error;
    for (const fs::directory_entry& child : fs::directory_iterator(HostPath(path), error))
    {
      if (path == "/" && !child.is_directory(error))
        continue;
      std::string name = child.path().filename().string();
      if (name.find("__") != std::string::npos)
        name = UnescapeName(name);
      if (name.size() > MAX_FILENAME_LENGTH || name.find('/') != std::string::npos)
        continue;
      names.push_back(std::move(name));
    }
    std::unordered_map<std::string, int> keys;
    for (size_t i = 0; i < entry->children.size(); ++i)
      keys.emplace(entry->children[i].name, static_cast<int>(i));
    const auto key = [&](const std::string& name) {
      const auto it = keys.find(name);
      return it != keys.end() ? it->second : -1;
    };
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
      const int ka = key(a), kb = key(b);
      return ka != kb ? ka > kb : a > b;
    });

    if (!list_address)
    {
      m_memory.Write32(count_address, static_cast<uint32_t>(names.size()));
      return Reply{IPC_SUCCESS, IPC_OVERHEAD};
    }
    for (size_t i = 0; i < names.size() && i < max_count; ++i)
    {
      m_memory.Fill(list_address, 0, 13);
      m_memory.CopyTo(list_address, names[i].data(), static_cast<uint32_t>(names[i].size()));
      list_address += static_cast<uint32_t>(names[i].size()) + 1;
    }
    m_memory.Write32(count_address, std::min<uint32_t>(max_count, static_cast<uint32_t>(names.size())));
    return Reply{IPC_SUCCESS, IPC_OVERHEAD};
  }

  bool DirectoryStats(const std::string& path, uint64_t* clusters, uint64_t* inodes) const
  {
    std::error_code error;
    const fs::path host = HostPath(path);
    if (!fs::is_directory(host, error))
      return false;
    *clusters = 0;
    *inodes = 1;
    const bool root = path == "/";
    for (auto it = fs::recursive_directory_iterator(host, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error))
    {
      if (root && it.depth() == 0 && !it->is_directory(error))
        continue;
      ++*inodes;
      if (it->is_regular_file(error))
        *clusters += (it->file_size(error) + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    }
    return true;
  }

  std::optional<Reply> GetUsage(const IOCtlVRequest& request)
  {
    if (!request.HasVectors(1, 2) || request.in[0].size != 64 || request.io[0].size != 4 ||
        request.io[1].size != 4)
    {
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    }
    const std::string path = m_memory.ReadString(request.in[0].address, 64);
    if (!IsValidPath(path))
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    uint64_t clusters = 0, inodes = 0;
    std::error_code error;
    if (!fs::exists(HostPath(path), error))
      return Reply{Convert(Result::NotFound), IPC_OVERHEAD};
    if (!DirectoryStats(path, &clusters, &inodes))
      return Reply{Convert(Result::Invalid), IPC_OVERHEAD};
    m_memory.Write32(request.io[0].address, static_cast<uint32_t>(std::min<uint64_t>(clusters, USABLE_CLUSTERS)));
    m_memory.Write32(request.io[1].address, static_cast<uint32_t>(std::min<uint64_t>(inodes, TOTAL_INODES)));
    return Reply{IPC_SUCCESS, IPC_OVERHEAD};
  }

  static FILE* OpenRead(const fs::path& path)
  {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
  }
  static FILE* OpenWrite(const fs::path& path)
  {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
  }

  uint32_t m_ppc_uid = 0;
  uint16_t m_ppc_gid = 0;
  fs::path m_root;
  FstEntry m_root_entry;
  std::array<Handle, MAX_HANDLES> m_handles;
  std::map<std::string, std::shared_ptr<HostFile>> m_open_files;
  std::map<uint32_t, Fd> m_fds;
  bool m_dirty_cache = false;
  uint32_t m_cache_chain = 0;
  int64_t m_cache_fd = -1;
};
}  // namespace

std::unique_ptr<Nand> CreateFs(const Host& host, const Config& config)
{
  return std::make_unique<FsDevice>(host, config);
}
}  // namespace dualize::ios
