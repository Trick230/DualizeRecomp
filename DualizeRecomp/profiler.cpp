#include "profiler.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dualize::profiler
{
namespace
{
constexpr int MAX_FRAMES = 24;

struct Target
{
  std::string name;
  HANDLE thread;
  std::vector<uint64_t> samples;  // per sample: frame count, then the frames
};

std::mutex s_mutex;
std::vector<Target> s_targets;
std::thread s_sampler;
std::atomic<bool> s_running{false};
std::string s_path;

// Runs with the target suspended, so it must not allocate: the target may
// hold the heap lock.
int Walk(const CONTEXT& start, uint64_t* frames)
{
  CONTEXT context = start;
  int count = 0;
  while (count < MAX_FRAMES && context.Rip)
  {
    frames[count++] = context.Rip;
    DWORD64 image_base = 0;
    RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function)
    {
      // A leaf without unwind data: the return address is on top of the stack.
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                     &establisher, nullptr);
  }
  return count;
}

void Sample()
{
  timeBeginPeriod(1);
  while (s_running.load(std::memory_order_relaxed))
  {
    {
      std::lock_guard lock(s_mutex);
      for (Target& target : s_targets)
      {
        if (SuspendThread(target.thread) == static_cast<DWORD>(-1))
          continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        uint64_t frames[MAX_FRAMES];
        const int count = GetThreadContext(target.thread, &context) ? Walk(context, frames) : 0;
        ResumeThread(target.thread);
        if (count)
        {
          target.samples.push_back(static_cast<uint64_t>(count));
          target.samples.insert(target.samples.end(), frames, frames + count);
        }
      }
    }
    Sleep(1);
  }
  timeEndPeriod(1);
}
}  // namespace

void RegisterThisThread(const char* name)
{
  static const char* path = std::getenv("DUALIZE_PROFILE");
  if (!path || !*path)
    return;
  HANDLE thread = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                       THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    return;
  std::lock_guard lock(s_mutex);
  s_targets.push_back({name, thread, {}});
  if (!s_running.exchange(true))
  {
    s_path = path;
    s_sampler = std::thread(Sample);
  }
}

void Shutdown()
{
  if (!s_running.exchange(false))
    return;
  s_sampler.join();
  std::FILE* out = std::fopen(s_path.c_str(), "w");
  if (out)
  {
    // The module's load address, so addresses map back to the executable's
    // symbols whatever ASLR did.
    std::fprintf(out, "base %llx\n",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
    // Every loaded module, so samples outside the executable (drivers, Dawn,
    // the kernel) still get a name.
    HMODULE modules[512];
    DWORD bytes = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
    {
      for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i)
      {
        MODULEINFO info{};
        char name[MAX_PATH];
        if (GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info)) &&
            GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, sizeof(name)))
          std::fprintf(out, "module %llx %lx %s\n",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.lpBaseOfDll)),
                       static_cast<unsigned long>(info.SizeOfImage), name);
      }
    }
  }
  for (Target& target : s_targets)
  {
    for (size_t i = 0; out && i < target.samples.size();)
    {
      const uint64_t count = target.samples[i++];
      std::fprintf(out, "%s", target.name.c_str());
      for (uint64_t f = 0; f < count; ++f)
        std::fprintf(out, " %llx", static_cast<unsigned long long>(target.samples[i++]));
      std::fputc('\n', out);
    }
    CloseHandle(target.thread);
  }
  if (out)
    std::fclose(out);
  s_targets.clear();
}
}  // namespace dualize::profiler

#else

namespace dualize::profiler
{
void RegisterThisThread(const char*) {}
void Shutdown() {}
}  // namespace dualize::profiler

#endif
