#pragma once

// The graphics settings, opened from the game's own Options screen (its
// "Check Aspect Ratio" row, which the runtime renames: the window keeps the
// aspect itself). Drawn with ImGui over the game on the render thread; while
// it is open the game reads its controllers as idle.

#include "hid/gamepads.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace dualize
{
struct MachineConfig;

// What the menu changes, kept in <user>/graphics.ini. Command-line flags
// override it for one run.
struct GraphicsSettings
{
  uint32_t msaa = 8;
  float render_scale = 1.0f;
  bool interpolate = true;
  bool vsync = true;
  bool ambient_occlusion = false;

  static GraphicsSettings FromConfig(const MachineConfig& config);
  void ApplyTo(MachineConfig& config) const;
  // False when the file is missing or unreadable (the settings stay as they are).
  bool Load(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path) const;
};

class GraphicsMenu
{
public:
  // extra_buttons: more host pad buttons held (the automation's); classic: whether the pads
  // play a Classic Controller (its A and B sit elsewhere than the GameCube pad's). Both
  // are read on the render thread.
  GraphicsMenu(const hid::Gamepads& gamepads, std::function<uint32_t()> extra_buttons,
               std::function<bool()> classic, std::filesystem::path game, std::filesystem::path settings_path,
               GraphicsSettings running);
  ~GraphicsMenu();

  // The game's art the menu draws with, loaded on its first draw.
  struct Assets;

  // CPU thread, from the Options screen's hook.
  void Open();
  // Any thread: the game's controllers read idle while true.
  bool IsOpen() const { return m_open.load(std::memory_order_acquire); }
  // Render thread, once per game frame between its begin and end.
  void Draw();

private:
  void Navigate();
  void Change(int row, int direction);
  void Close();

  const hid::Gamepads& m_gamepads;
  const std::function<uint32_t()> m_extra_buttons;
  const std::function<bool()> m_classic;
  const std::filesystem::path m_game;
  std::unique_ptr<Assets> m_assets;
  const std::filesystem::path m_settings_path;
  const GraphicsSettings m_running;  // what this run started with
  GraphicsSettings m_settings;       // what the menu shows
  std::atomic<bool> m_open{false};
  bool m_shown = false;  // ImGui active and drawing
  bool m_closing = false;  // closed, holding the pads until their buttons are up
  double m_open_time = -1, m_close_time = 0;  // ImGui time, for the animations
  int m_row = 0;
  uint32_t m_held = 0;  // buttons down last frame, for edges
  bool m_saved_notice = false;
};
}  // namespace dualize
