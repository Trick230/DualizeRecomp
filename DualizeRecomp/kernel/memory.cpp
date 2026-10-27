#include "kernel/memory.hpp"

#include <atomic>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace dualize
{
namespace
{
// 4 GiB plus a guard for accesses that start just below the top.
constexpr uint64_t SPACE = (uint64_t{1} << 32) + 0x10000;
// Views are placed at the host's allocation granularity (64 KiB on Windows).
constexpr uint32_t GRANULE = 0x10000;
constexpr uint32_t L1_VIEW = GRANULE;
constexpr uint64_t SECTION = uint64_t{Memory::MEM1_SIZE} + Memory::MEM2_SIZE + L1_VIEW;

struct View
{
  uint32_t ea;
  uint64_t offset;
  uint32_t size;
};
constexpr View VIEWS[] = {
    {0x80000000, 0, Memory::MEM1_SIZE},
    {0x90000000, Memory::MEM1_SIZE, Memory::MEM2_SIZE},
    {0xC0000000, 0, Memory::MEM1_SIZE},
    {0xD0000000, Memory::MEM1_SIZE, Memory::MEM2_SIZE},
    {Memory::L1_BASE, uint64_t{Memory::MEM1_SIZE} + Memory::MEM2_SIZE, L1_VIEW},
};

// The live reservation, for the fault handler (one machine per process).
std::atomic<uint8_t*> s_flat{nullptr};

void ReportStray(uint8_t* page)
{
  std::fprintf(stderr, "[native] guest access to unmapped %08x: backed with zeros\n",
               static_cast<uint32_t>(page - s_flat.load(std::memory_order_relaxed)));
}

#ifdef _WIN32
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        void*, ULONG);
VirtualAlloc2Fn s_virtual_alloc2;
MapViewOfFile3Fn s_map_view3;
PVOID s_handler;

// Placeholder reservations (Windows 10 1803+): split one out exactly where
// a view or a zero page goes, then replace it.
bool Split(uint8_t* at, size_t size)
{
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(at, &info, sizeof(info)) || info.State != MEM_RESERVE)
    return false;
  if (info.BaseAddress == at && info.RegionSize == size)
    return true;
  return VirtualFree(at, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
}

LONG CALLBACK StrayAccess(EXCEPTION_POINTERS* info)
{
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  uint8_t* const flat = s_flat.load(std::memory_order_acquire);
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
      !flat)
    return EXCEPTION_CONTINUE_SEARCH;
  uint8_t* const address = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
  if (address < flat || address >= flat + SPACE)
    return EXCEPTION_CONTINUE_SEARCH;
  uint8_t* const page = flat + ((address - flat) & ~uint64_t{GRANULE - 1});
  if (!Split(page, GRANULE) ||
      !s_virtual_alloc2(GetCurrentProcess(), page, GRANULE,
                        MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                        nullptr, 0))
    return EXCEPTION_CONTINUE_SEARCH;
  ReportStray(page);
  return EXCEPTION_CONTINUE_EXECUTION;
}

void Release(uint8_t* flat)
{
  for (uint8_t* p = flat; p < flat + SPACE;)
  {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)))
      break;
    if (info.Type == MEM_MAPPED)
      UnmapViewOfFile(info.AllocationBase);
    else if (info.State != MEM_FREE)
      VirtualFree(info.AllocationBase, 0, MEM_RELEASE);
    p = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
}
#else
struct sigaction s_previous;

void StrayAccess(int signal, siginfo_t* info, void* context)
{
  uint8_t* const flat = s_flat.load(std::memory_order_acquire);
  uint8_t* const address = static_cast<uint8_t*>(info->si_addr);
  if (flat && address >= flat && address < flat + SPACE)
  {
    uint8_t* const page = flat + ((address - flat) & ~uint64_t{GRANULE - 1});
    if (mmap(page, GRANULE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) != MAP_FAILED)
    {
      ReportStray(page);
      return;
    }
  }
  if (s_previous.sa_flags & SA_SIGINFO)
    s_previous.sa_sigaction(signal, info, context);
  else if (s_previous.sa_handler != SIG_DFL && s_previous.sa_handler != SIG_IGN)
    s_previous.sa_handler(signal);
  else
  {
    sigaction(signal, &s_previous, nullptr);
    raise(signal);
  }
}
#endif
}  // namespace

Memory::Memory()
{
#ifdef _WIN32
  const HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
  s_virtual_alloc2 = kernel ? reinterpret_cast<VirtualAlloc2Fn>(
                                  reinterpret_cast<void*>(GetProcAddress(kernel, "VirtualAlloc2")))
                            : nullptr;
  s_map_view3 = kernel ? reinterpret_cast<MapViewOfFile3Fn>(
                             reinterpret_cast<void*>(GetProcAddress(kernel, "MapViewOfFile3")))
                       : nullptr;
  if (!s_virtual_alloc2 || !s_map_view3)
    return;
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(SECTION >> 32),
                                      static_cast<DWORD>(SECTION), nullptr);
  if (!section)
    return;
  m_section = section;
  uint8_t* const base = static_cast<uint8_t*>(
      s_virtual_alloc2(GetCurrentProcess(), nullptr, SPACE, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                       PAGE_NOACCESS, nullptr, 0));
  if (!base)
    return;
  for (const View& view : VIEWS)
  {
    if (!Split(base + view.ea, view.size) ||
        !s_map_view3(section, GetCurrentProcess(), base + view.ea, view.offset, view.size,
                     MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
    {
      Release(base);
      return;
    }
  }
  s_handler = AddVectoredExceptionHandler(1, StrayAccess);
#else
#ifdef __linux__
  m_fd = memfd_create("dualize-ram", 0);
#else
  char name[64];
  std::snprintf(name, sizeof(name), "/dualize-ram-%d", static_cast<int>(getpid()));
  m_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  shm_unlink(name);
#endif
  if (m_fd < 0 || ftruncate(m_fd, static_cast<off_t>(SECTION)) != 0)
    return;
  void* const reserved =
      mmap(nullptr, SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reserved == MAP_FAILED)
    return;
  uint8_t* const base = static_cast<uint8_t*>(reserved);
  for (const View& view : VIEWS)
  {
    if (mmap(base + view.ea, view.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, m_fd,
             static_cast<off_t>(view.offset)) == MAP_FAILED)
    {
      munmap(base, SPACE);
      return;
    }
  }
  struct sigaction action = {};
  action.sa_sigaction = StrayAccess;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, &s_previous);
#ifdef __APPLE__
  sigaction(SIGBUS, &action, nullptr);
#endif
#endif
  flat = base;
  mem1 = base + 0x80000000u;
  mem2 = base + 0x90000000u;
  l1 = base + L1_BASE;
  s_flat.store(base, std::memory_order_release);
}

Memory::~Memory()
{
  s_flat.store(nullptr, std::memory_order_release);
#ifdef _WIN32
  if (s_handler)
    RemoveVectoredExceptionHandler(s_handler);
  s_handler = nullptr;
  if (flat)
    Release(flat);
  if (m_section)
    CloseHandle(static_cast<HANDLE>(m_section));
#else
  if (flat)
  {
    sigaction(SIGSEGV, &s_previous, nullptr);
    munmap(flat, SPACE);
  }
  if (m_fd >= 0)
    close(m_fd);
#endif
}
}  // namespace dualize
