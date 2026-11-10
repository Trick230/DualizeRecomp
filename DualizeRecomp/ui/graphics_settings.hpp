#pragma once

// The graphics settings, changed on the graphics page of the game's own
// Options screen (sdk/nights_graphics.cpp) and kept in <user>/graphics.ini.
// Command-line flags override them for one run.

#include <cstdint>
#include <filesystem>
#include <string>

namespace dualize
{
struct MachineConfig;

struct GraphicsSettings
{
  // The window's width out of fullscreen (its height keeps the game's aspect).
  uint32_t window_width = 1280;
  bool fullscreen = false;  // borderless over the whole display
  float render_scale = 1.0f;
  bool vsync = true;
  // Frames shown a second: 30 is the game's own (no interpolation), 0 as
  // many as the display or, without vsync, the GPU allows.
  uint32_t frame_rate = 0;
  uint32_t msaa = 8;
  bool transparency_aa = true;
  bool ambient_occlusion = false;

  static GraphicsSettings FromConfig(const MachineConfig& config);
  void ApplyTo(MachineConfig& config) const;
  // False when the file is missing or unreadable (the settings stay as they are).
  bool Load(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path) const;

  bool operator==(const GraphicsSettings&) const = default;
};

// The graphics page's options, top to bottom.
enum class GraphicsOption
{
  WindowSize,
  Fullscreen,
  ResolutionScale,
  VSync,
  FrameRate,
  AntiAliasing,
  TransparencyAA,
  AmbientOcclusion,
  Count,
};
constexpr int GRAPHICS_OPTION_COUNT = static_cast<int>(GraphicsOption::Count);

const char* OptionName(GraphicsOption option);
// One line, at most HELP_LENGTH characters (the room the game's message has).
const char* OptionHelp(GraphicsOption option);
constexpr size_t HELP_LENGTH = 42;
std::string OptionValue(const GraphicsSettings& settings, GraphicsOption option);
// The option's next value (direction 1) or previous (-1); at either end it
// stays, or with `wrap` goes round to the other.
void StepOption(GraphicsSettings& settings, GraphicsOption option, int direction, bool wrap);
}  // namespace dualize
