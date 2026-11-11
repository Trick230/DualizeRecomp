#include "ui/graphics_settings.hpp"

#include "machine.hpp"

#include <aurora/fifo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace dualize
{
namespace
{
// 16:9 sizes; the list leaves out those the display has no room for.
constexpr std::array<uint32_t, 7> WINDOW_WIDTHS{1280, 1600, 1920, 2560, 3200, 3840, 5120};
constexpr std::array<float, 6> SCALE_VALUES{1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
constexpr std::array<uint32_t, 7> FRAME_RATE_VALUES{30, 60, 120, 144, 165, 240, 0};
constexpr std::array<uint32_t, 4> MSAA_VALUES{1, 2, 4, 8};
constexpr uint32_t GAME_FRAME_RATE = 30;

struct OptionText
{
  const char* name;
  const char* help;
};
constexpr std::array<OptionText, GRAPHICS_OPTION_COUNT> OPTIONS{{
    {"Fullscreen", "Fill the screen with a borderless window"},
    {"Window Size", "The window's size on the desktop"},
    {"Resolution Scale", "Draw the 3D world sharper than the window"},
    {"V-Sync", "Present in step with the display"},
    {"Frame Rate", "30 is the original; more is interpolated"},
    {"Anti-Aliasing", "Smooth the jagged edges of 3D objects"},
    {"Transparency Anti-Aliasing", "Smooth the cut edges of leaves and fences"},
    {"Ambient Occlusion", "Darken creases and corners of the world"},
}};

template <typename T, size_t N>
size_t IndexOf(const std::array<T, N>& values, T value)
{
  size_t best = 0;
  for (size_t i = 0; i < N; ++i)
  {
    if (std::abs(static_cast<double>(values[i]) - static_cast<double>(value)) <
        std::abs(static_cast<double>(values[best]) - static_cast<double>(value)))
      best = i;
  }
  return best;
}

template <typename T, size_t N>
T Step(const std::array<T, N>& values, T value, int direction, bool wrap)
{
  const int n = static_cast<int>(N);
  const int i = static_cast<int>(IndexOf(values, value)) + direction;
  return values[static_cast<size_t>(wrap ? (i + n) % n : std::clamp(i, 0, n - 1))];
}

uint32_t WindowHeight(uint32_t width)
{
  return width * 9 / 16;
}

// The sizes with room on the display (all of them before the window is up),
// and at least the smallest.
std::vector<uint32_t> WindowWidths()
{
  uint32_t width = 0, height = 0;
  aurora_fifo_display_area(&width, &height);
  std::vector<uint32_t> widths;
  for (uint32_t w : WINDOW_WIDTHS)
  {
    if (width == 0 || (w <= width && WindowHeight(w) < height) || widths.empty())
      widths.push_back(w);
  }
  return widths;
}

const char* OnOff(bool on)
{
  return on ? "ON" : "OFF";
}
}  // namespace

GraphicsSettings GraphicsSettings::FromConfig(const MachineConfig& config)
{
  GraphicsSettings s;
  s.window_width = config.width;
  s.fullscreen = config.fullscreen;
  s.render_scale = config.render_scale;
  s.vsync = config.vsync;
  s.frame_rate = !config.interpolate                              ? GAME_FRAME_RATE
                 : config.present_rate == AURORA_PRESENT_UNLIMITED ? 0
                                                                   : config.present_rate;
  s.transparency_aa = config.alpha_to_coverage;
  s.msaa = config.msaa;
  s.ambient_occlusion = config.ambient_occlusion;
  return s;
}

void GraphicsSettings::ApplyTo(MachineConfig& config) const
{
  config.width = window_width;
  config.height = WindowHeight(window_width);
  config.fullscreen = fullscreen;
  config.render_scale = render_scale;
  config.vsync = vsync;
  config.interpolate = frame_rate != GAME_FRAME_RATE;
  if (config.interpolate)
    config.present_rate = frame_rate == 0 ? AURORA_PRESENT_UNLIMITED : frame_rate;
  config.alpha_to_coverage = transparency_aa;
  config.msaa = msaa;
  config.ambient_occlusion = ambient_occlusion;
}

bool GraphicsSettings::Load(const std::filesystem::path& path)
{
  std::ifstream file(path);
  if (!file)
    return false;
  std::string line;
  while (std::getline(file, line))
  {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#')
      continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    const auto trim = [](std::string& s) {
      s.erase(0, s.find_first_not_of(" \t\r"));
      s.erase(s.find_last_not_of(" \t\r") + 1);
    };
    trim(key);
    trim(value);
    const bool on = value == "true" || value == "1" || value == "on";
    const uint32_t number = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    if (key == "window_width")
      window_width = WINDOW_WIDTHS[IndexOf(WINDOW_WIDTHS, number)];
    else if (key == "fullscreen")
      fullscreen = on;
    else if (key == "render_scale")
      render_scale = std::clamp(std::strtof(value.c_str(), nullptr), 0.25f, 4.0f);
    else if (key == "vsync")
      vsync = on;
    else if (key == "frame_rate")
      frame_rate = FRAME_RATE_VALUES[IndexOf(FRAME_RATE_VALUES, number)];
    else if (key == "interpolation" && !on)  // the earlier menu's
      frame_rate = GAME_FRAME_RATE;
    else if (key == "msaa")
      msaa = MSAA_VALUES[IndexOf(MSAA_VALUES, number)];
    else if (key == "transparency_aa")
      transparency_aa = on;
    else if (key == "ambient_occlusion")
      ambient_occlusion = on;
  }
  return true;
}

bool GraphicsSettings::Save(const std::filesystem::path& path) const
{
  std::ofstream file(path, std::ios::trunc);
  file << "# DualizeRecomp graphics settings (written by the Options screen's Graphics Settings)\n"
       << "window_width = " << window_width << "\n"
       << "fullscreen = " << (fullscreen ? "true" : "false") << "\n"
       << "render_scale = " << render_scale << "\n"
       << "vsync = " << (vsync ? "true" : "false") << "\n"
       << "# 30: the game's own; 0: no limit\n"
       << "frame_rate = " << frame_rate << "\n"
       << "msaa = " << msaa << "\n"
       << "transparency_aa = " << (transparency_aa ? "true" : "false") << "\n"
       << "ambient_occlusion = " << (ambient_occlusion ? "true" : "false") << "\n";
  return static_cast<bool>(file);
}

const char* OptionName(GraphicsOption option)
{
  return OPTIONS[static_cast<size_t>(option)].name;
}

const char* OptionHelp(GraphicsOption option)
{
  return OPTIONS[static_cast<size_t>(option)].help;
}

std::string OptionValue(const GraphicsSettings& s, GraphicsOption option)
{
  char text[32];
  switch (option)
  {
  case GraphicsOption::WindowSize:
    std::snprintf(text, sizeof(text), "%u\u00D7%u", s.window_width, WindowHeight(s.window_width));
    return text;
  case GraphicsOption::Fullscreen:
    return OnOff(s.fullscreen);
  case GraphicsOption::ResolutionScale:
    std::snprintf(text, sizeof(text), s.render_scale == std::floor(s.render_scale) ? "%.0fx" : "%.1fx",
                  s.render_scale);
    return text;
  case GraphicsOption::VSync:
    return OnOff(s.vsync);
  case GraphicsOption::FrameRate:
    if (s.frame_rate == 0)
      return "MAX";
    std::snprintf(text, sizeof(text), "%u", s.frame_rate);
    return text;
  case GraphicsOption::AntiAliasing:
    if (s.msaa <= 1)
      return "OFF";
    std::snprintf(text, sizeof(text), "%ux", s.msaa);
    return text;
  case GraphicsOption::TransparencyAA:
    return OnOff(s.transparency_aa);
  case GraphicsOption::AmbientOcclusion:
    return OnOff(s.ambient_occlusion);
  case GraphicsOption::Count:
    break;
  }
  return {};
}

void StepOption(GraphicsSettings& s, GraphicsOption option, int direction, bool wrap)
{
  switch (option)
  {
  case GraphicsOption::WindowSize:
  {
    const std::vector<uint32_t> widths = WindowWidths();
    const int n = static_cast<int>(widths.size());
    int i = 0;
    while (i + 1 < n && widths[static_cast<size_t>(i)] < s.window_width)
      ++i;
    i += direction;
    s.window_width = widths[static_cast<size_t>(wrap ? (i + n) % n : std::clamp(i, 0, n - 1))];
    break;
  }
  case GraphicsOption::Fullscreen:
    s.fullscreen = !s.fullscreen;
    break;
  case GraphicsOption::ResolutionScale:
    s.render_scale = Step(SCALE_VALUES, s.render_scale, direction, wrap);
    break;
  case GraphicsOption::VSync:
    s.vsync = !s.vsync;
    break;
  case GraphicsOption::FrameRate:
    s.frame_rate = Step(FRAME_RATE_VALUES, s.frame_rate, direction, wrap);
    break;
  case GraphicsOption::AntiAliasing:
    s.msaa = Step(MSAA_VALUES, s.msaa, direction, wrap);
    break;
  case GraphicsOption::TransparencyAA:
    s.transparency_aa = !s.transparency_aa;
    break;
  case GraphicsOption::AmbientOcclusion:
    s.ambient_occlusion = !s.ambient_occlusion;
    break;
  case GraphicsOption::Count:
    break;
  }
}
}  // namespace dualize
