// DualizeRecomp: NiGHTS: Journey of Dreams, recompiled.
//
//   DualizeRecomp --game <extracted disc>
//       [--user <dir>] [--load-state <file>] [--automation-dir <dir>]
//       [--unthrottled] [--no-vsync] [--no-interpolation] [--present-rate <hz>]
//       [--window <width>x<height>] [--msaa <1|4>] [--render-scale <x>] [--idle-pc <hex>]
//       [--hash-every <fields>] [--record-input <file>] [--replay-input <file>]
//
// Automation: each file dropped in <dir>/commands holds key=value lines, as
// command=save_state|load_state with path=<file>;
// command=pad with port=0..3 and controls (classic_a=1, classic_lx=-0.5, a=1,
// main_x=1, ...); command=pad_frames, the same held for frames=<n> fields;
// command=clear_pad; command=stop.

#include "machine.hpp"
#include "profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace
{
namespace fs = std::filesystem;
using dualize::Machine;
using dualize::MachineConfig;

fs::path Utf8Path(std::string_view text)
{
  return fs::path(std::u8string(text.begin(), text.end()));
}

fs::path DefaultUserDir()
{
#ifdef _WIN32
  PWSTR local = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
  {
    fs::path dir = fs::path(local) / "DualizeRecomp";
    CoTaskMemFree(local);
    return dir;
  }
#else
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".local" / "share" / "DualizeRecomp";
#endif
  return "DualizeRecomp";
}

using AutomationPad = Machine::AutomationPad;
namespace ios = dualize::ios;

// Field names follow scripts/automation.py.
struct WiiButton
{
  std::string_view name;
  bool classic;
  uint16_t bit;
};
constexpr WiiButton WII_BUTTONS[] = {
    {"wii_a", false, ios::WIIMOTE_A},
    {"wii_b", false, ios::WIIMOTE_B},
    {"wii_1", false, ios::WIIMOTE_ONE},
    {"wii_2", false, ios::WIIMOTE_TWO},
    {"wii_plus", false, ios::WIIMOTE_PLUS},
    {"wii_minus", false, ios::WIIMOTE_MINUS},
    {"wii_home", false, ios::WIIMOTE_HOME},
    {"wii_dpad_up", false, ios::WIIMOTE_UP},
    {"wii_dpad_down", false, ios::WIIMOTE_DOWN},
    {"wii_dpad_left", false, ios::WIIMOTE_LEFT},
    {"wii_dpad_right", false, ios::WIIMOTE_RIGHT},
    {"classic_a", true, ios::CLASSIC_A},
    {"classic_b", true, ios::CLASSIC_B},
    {"classic_x", true, ios::CLASSIC_X},
    {"classic_y", true, ios::CLASSIC_Y},
    {"classic_zl", true, ios::CLASSIC_ZL},
    {"classic_zr", true, ios::CLASSIC_ZR},
    {"classic_plus", true, ios::CLASSIC_PLUS},
    {"classic_minus", true, ios::CLASSIC_MINUS},
    {"classic_home", true, ios::CLASSIC_HOME},
    {"classic_dpad_up", true, ios::CLASSIC_UP},
    {"classic_dpad_down", true, ios::CLASSIC_DOWN},
    {"classic_dpad_left", true, ios::CLASSIC_LEFT},
    {"classic_dpad_right", true, ios::CLASSIC_RIGHT},
    {"classic_l", true, ios::CLASSIC_L},
    {"classic_r", true, ios::CLASSIC_R},
};
constexpr std::pair<std::string_view, uint16_t> GC_BUTTONS[] = {
    {"a", 0x0100},         {"b", 0x0200},          {"x", 0x0400},       {"y", 0x0800},
    {"start", 0x1000},     {"z", 0x0010},          {"dpad_up", 0x0008}, {"dpad_down", 0x0004},
    {"dpad_left", 0x0001}, {"dpad_right", 0x0002}, {"l", 0x0040},       {"r", 0x0020},
};

// Sets one control; false for an unknown name.
bool SetControl(AutomationPad& pad, std::string_view key, float v)
{
  for (const WiiButton& b : WII_BUTTONS)
  {
    if (key == b.name)
    {
      if (v >= 0.5f)
        (b.classic ? pad.wii.classic_buttons : pad.wii.buttons) |= b.bit;
      return true;
    }
  }
  for (const auto& [name, bit] : GC_BUTTONS)
  {
    if (key == name)
    {
      pad.gc = true;
      if (v >= 0.5f)
        pad.gc_buttons |= bit;
      return true;
    }
  }
  const std::pair<std::string_view, float*> axes[] = {
      {"classic_lx", &pad.wii.left_x},
      {"classic_ly", &pad.wii.left_y},
      {"classic_rx", &pad.wii.right_x},
      {"classic_ry", &pad.wii.right_y},
      {"classic_l_analog", &pad.wii.left_trigger},
      {"classic_r_analog", &pad.wii.right_trigger},
      {"main_x", &pad.gc_main_x},
      {"main_y", &pad.gc_main_y},
      {"c_x", &pad.gc_c_x},
      {"c_y", &pad.gc_c_y},
      {"l_analog", &pad.gc_left},
      {"r_analog", &pad.gc_right},
  };
  for (const auto& [name, value] : axes)
  {
    if (key == name)
    {
      pad.gc |= !key.starts_with("classic_");
      *value = std::clamp(v, key.ends_with("analog") ? 0.0f : -1.0f, 1.0f);
      return true;
    }
  }
  return false;
}

void RunCommand(Machine& machine, const fs::path& file, const std::atomic<bool>& done)
{
  std::ifstream in(file);
  std::string line, command;
  fs::path path;
  int port = 0;
  uint64_t frames = 0;
  std::vector<std::pair<std::string, std::string>> controls;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "command")
      command = value;
    else if (key == "path")
      path = Utf8Path(value);
    else if (key == "port")
      port = std::atoi(value.c_str());
    else if (key == "frames")
      frames = std::strtoull(value.c_str(), nullptr, 10);
    else
      controls.emplace_back(key, value);
  }
  const std::string name = file.filename().string();
  if (command == "pad" || command == "pad_frames")
  {
    AutomationPad pad;
    for (const auto& [key, value] : controls)
    {
      if (!SetControl(pad, key, std::strtof(value.c_str(), nullptr)))
        std::fprintf(stderr, "[native] %s: unknown pad field '%s'\n", name.c_str(), key.c_str());
    }
    machine.SetAutomationPad(port, &pad);
    if (command == "pad_frames")
    {
      // Held for that many VI fields, then released.
      const uint64_t start = machine.FieldCount();
      while (!done && machine.FieldCount() - start < frames)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      machine.SetAutomationPad(port, nullptr);
    }
  }
  else if (command == "clear_pad")
    machine.SetAutomationPad(port, nullptr);
  else if (command == "save_state" && !path.empty())
    machine.RequestSave(path);
  else if (command == "load_state" && !path.empty())
    machine.RequestLoad(path);
  else if (command == "stop")
    machine.RequestStop();
  else
    std::fprintf(stderr, "[native] %s: unknown command '%s'\n", name.c_str(), command.c_str());
}

// Watches <dir>/commands and runs files in name order, deleting each.
void WatchAutomation(Machine& machine, fs::path dir, const std::atomic<bool>& done)
{
  const fs::path commands = dir / "commands";
  std::error_code error;
  fs::create_directories(commands, error);
  while (!done)
  {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(commands, error))
    {
      if (entry.is_regular_file(error))
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files)
    {
      RunCommand(machine, file, done);
      fs::remove(file, error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

int Usage()
{
  std::fprintf(stderr, "usage: DualizeRecomp --game <dir> [--user <dir>] "
                       "[--load-state <file>] [--automation-dir <dir>] [--unthrottled] "
                       "[--no-vsync] [--no-interpolation] [--present-rate <hz>] [--window <w>x<h>] "
                       "[--msaa <1|4>] [--render-scale <x>] [--idle-pc <hex>] [--hash-every <fields>] "
                       "[--record-input <file>] [--replay-input <file>]\n");
  return 2;
}
}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
  timeBeginPeriod(1);
#endif
  MachineConfig config;
  config.user = DefaultUserDir();
  config.idle_pc = 0x8036623C;  // NiGHTS' idle thread loop (see GameSettings/R7EE8P.ini)
  config.rso_list = 0x80003168;  // the RSO library's list of linked modules, in OS low memory
  fs::path load_state, automation;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (arg == "--game" && (v = value()))
      config.game = Utf8Path(v);
    else if (arg == "--user" && (v = value()))
      config.user = Utf8Path(v);
    else if (arg == "--load-state" && (v = value()))
      load_state = Utf8Path(v);
    else if (arg == "--automation-dir" && (v = value()))
      automation = Utf8Path(v);
    else if (arg == "--idle-pc" && (v = value()))
      config.idle_pc = static_cast<uint32_t>(std::strtoul(v, nullptr, 16));
    else if (arg == "--unthrottled")
      config.throttle = false;
    else if (arg == "--no-vsync")
      config.vsync = false;
    else if (arg == "--no-interpolation")
      config.interpolate = false;
    else if (arg == "--present-rate" && (v = value()))
      config.present_rate = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    else if (arg == "--window" && (v = value()) && std::sscanf(v, "%ux%u", &config.width, &config.height) == 2)
      continue;
    else if (arg == "--msaa" && (v = value()) && (std::string_view(v) == "1" || std::string_view(v) == "4"))
      config.msaa = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    else if (arg == "--render-scale" && (v = value()) && std::strtof(v, nullptr) >= 0.25f &&
             std::strtof(v, nullptr) <= 4.0f)
      config.render_scale = std::strtof(v, nullptr);
    else if (arg == "--hash-every" && (v = value()))
      config.hash_every = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    else if (arg == "--record-input" && (v = value()))
      config.record_input = Utf8Path(v);
    else if (arg == "--replay-input" && (v = value()))
      config.replay_input = Utf8Path(v);
    else
      return Usage();
  }
  if (config.game.empty())
    return Usage();

  auto machine = std::make_unique<Machine>(config);
  std::string error;
  if (!machine->Start(&error))
  {
    std::fprintf(stderr, "DualizeRecomp: %s\n", error.c_str());
    return 1;
  }
  if (!load_state.empty() && !machine->LoadState(load_state, &error))
  {
    std::fprintf(stderr, "DualizeRecomp: %s\n", error.c_str());
    return 1;
  }
  std::atomic<bool> done{false};
  std::thread watcher;
  if (!automation.empty())
    watcher = std::thread(WatchAutomation, std::ref(*machine), automation, std::cref(done));
  dualize::profiler::RegisterThisThread("cpu");
  machine->Run();
  dualize::profiler::Shutdown();
  done = true;
  if (watcher.joinable())
    watcher.join();
  const auto& stats = machine->cpu.GetStats();
  std::fprintf(stderr,
               "[native] dispatches %llu, safepoints %llu, interpreted %llu, entry misses %llu, fallbacks %llu, native exceptions %llu\n",
               static_cast<unsigned long long>(stats.dispatches),
               static_cast<unsigned long long>(stats.safepoints),
               static_cast<unsigned long long>(stats.interpreted),
               static_cast<unsigned long long>(stats.entry_misses),
               static_cast<unsigned long long>(stats.fallbacks),
               static_cast<unsigned long long>(stats.native_exceptions));
  return 0;
}
