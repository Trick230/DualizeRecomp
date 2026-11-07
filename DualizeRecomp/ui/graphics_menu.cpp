#include "ui/graphics_menu.hpp"

#include "machine.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace dualize
{
namespace
{
constexpr std::array<uint32_t, 4> MSAA_VALUES{1, 2, 4, 8};
constexpr std::array<float, 6> SCALE_VALUES{1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};

enum Row
{
  ROW_MSAA,
  ROW_SCALE,
  ROW_INTERPOLATION,
  ROW_VSYNC,
  ROW_AO,
  ROW_COUNT,
};

struct RowText
{
  const char* name;
  const char* help;
};
constexpr std::array<RowText, ROW_COUNT> ROWS{{
    {"Anti-Aliasing", "Smooth the edges of 3D objects (multisampling)"},
    {"Resolution Scale", "Render above the window's resolution for a sharper image"},
    {"Frame Interpolation", "Show smooth motion at your display's refresh rate"},
    {"V-Sync", "Present in step with the display to avoid tearing"},
    {"Ambient Occlusion", "Darken creases and corners of the 3D world"},
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
T Step(const std::array<T, N>& values, T value, int direction)
{
  const int i = static_cast<int>(IndexOf(values, value)) + direction;
  return values[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(N) - 1))];
}

std::string ValueText(const GraphicsSettings& s, int row)
{
  char text[32];
  switch (row)
  {
  case ROW_MSAA:
    if (s.msaa <= 1)
      return "OFF";
    std::snprintf(text, sizeof(text), "%ux", s.msaa);
    return text;
  case ROW_SCALE:
    std::snprintf(text, sizeof(text), s.render_scale == std::floor(s.render_scale) ? "%.0fx" : "%.1fx",
                  s.render_scale);
    return text;
  case ROW_INTERPOLATION:
    return s.interpolate ? "ON" : "OFF";
  case ROW_VSYNC:
    return s.vsync ? "ON" : "OFF";
  case ROW_AO:
    return s.ambient_occlusion ? "ON" : "OFF";
  }
  return {};
}

bool SameAs(const GraphicsSettings& a, const GraphicsSettings& b)
{
  return a.msaa == b.msaa && a.render_scale == b.render_scale && a.interpolate == b.interpolate &&
         a.vsync == b.vsync && a.ambient_occlusion == b.ambient_occlusion;
}

ImU32 Rgba(int r, int g, int b, float a = 1.0f)
{
  return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

// The text centered vertically at y, from x, at a size in canvas units.
void Text(ImDrawList* draw, float size, ImVec2 pos, ImU32 color, const char* text, bool shadow = true)
{
  ImFont* font = ImGui::GetFont();
  if (shadow)
    draw->AddText(font, size, {pos.x + size * 0.06f, pos.y + size * 0.06f}, Rgba(0, 0, 30, 0.6f), text);
  draw->AddText(font, size, pos, color, text);
}

ImVec2 TextSize(float size, const char* text)
{
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}
}  // namespace

GraphicsSettings GraphicsSettings::FromConfig(const MachineConfig& config)
{
  GraphicsSettings s;
  s.msaa = config.msaa;
  s.render_scale = config.render_scale;
  s.interpolate = config.interpolate;
  s.vsync = config.vsync;
  s.ambient_occlusion = config.ambient_occlusion;
  return s;
}

void GraphicsSettings::ApplyTo(MachineConfig& config) const
{
  config.msaa = msaa;
  config.render_scale = render_scale;
  config.interpolate = interpolate;
  config.vsync = vsync;
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
    if (key == "msaa")
      msaa = MSAA_VALUES[IndexOf(MSAA_VALUES, static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10)))];
    else if (key == "render_scale")
      render_scale = std::clamp(std::strtof(value.c_str(), nullptr), 0.25f, 4.0f);
    else if (key == "interpolation")
      interpolate = on;
    else if (key == "vsync")
      vsync = on;
    else if (key == "ambient_occlusion")
      ambient_occlusion = on;
  }
  return true;
}

bool GraphicsSettings::Save(const std::filesystem::path& path) const
{
  std::ofstream file(path, std::ios::trunc);
  file << "# DualizeRecomp graphics settings (the in-game Graphics Settings menu writes this file)\n"
       << "msaa = " << msaa << "\n"
       << "render_scale = " << render_scale << "\n"
       << "interpolation = " << (interpolate ? "true" : "false") << "\n"
       << "vsync = " << (vsync ? "true" : "false") << "\n"
       << "ambient_occlusion = " << (ambient_occlusion ? "true" : "false") << "\n";
  return static_cast<bool>(file);
}

GraphicsMenu::GraphicsMenu(const hid::Gamepads& gamepads, std::function<uint32_t()> extra_buttons,
                           std::function<bool()> classic, std::filesystem::path settings_path,
                           GraphicsSettings running)
    : m_gamepads(gamepads), m_extra_buttons(std::move(extra_buttons)), m_classic(std::move(classic)),
      m_settings_path(std::move(settings_path)), m_running(running), m_settings(running)
{
  GraphicsSettings saved = running;
  if (saved.Load(m_settings_path))
    m_settings = saved;
}

void GraphicsMenu::Open()
{
  m_open.store(true, std::memory_order_release);
}

void GraphicsMenu::Close()
{
  m_settings.Save(m_settings_path);
  // The game gets the pads back once the button that closed the menu is up.
  m_closing = true;
}

void GraphicsMenu::Change(int row, int direction)
{
  GraphicsSettings& s = m_settings;
  switch (row)
  {
  case ROW_MSAA:
    s.msaa = Step(MSAA_VALUES, s.msaa, direction);
    break;
  case ROW_SCALE:
    s.render_scale = Step(SCALE_VALUES, s.render_scale, direction);
    break;
  case ROW_INTERPOLATION:
    s.interpolate = !s.interpolate;
    break;
  case ROW_VSYNC:
    s.vsync = !s.vsync;
    break;
  case ROW_AO:
    s.ambient_occlusion = !s.ambient_occlusion;
    break;
  }
}

void GraphicsMenu::Navigate()
{
  // The game's A and B where the game reads them: the GameCube pad's sit
  // bottom and left, the Classic Controller's right and bottom.
  const bool classic = m_classic && m_classic();
  const uint32_t accept = classic ? hid::PAD_EAST : hid::PAD_SOUTH;
  const uint32_t back = classic ? hid::PAD_SOUTH : hid::PAD_WEST;
  hid::PadState pad;
  uint32_t buttons = m_extra_buttons ? m_extra_buttons() : 0;
  for (int port = 0; port < hid::Gamepads::MAX_PORTS; ++port)
  {
    if (!m_gamepads.Read(port, &pad))
      continue;
    buttons |= pad.buttons;
    if (pad.left_y > 0.5f)
      buttons |= hid::PAD_DPAD_UP;
    if (pad.left_y < -0.5f)
      buttons |= hid::PAD_DPAD_DOWN;
    if (pad.left_x < -0.5f)
      buttons |= hid::PAD_DPAD_LEFT;
    if (pad.left_x > 0.5f)
      buttons |= hid::PAD_DPAD_RIGHT;
  }
  // The keyboard too, for players without a pad.
  if (ImGui::IsKeyDown(ImGuiKey_UpArrow))
    buttons |= hid::PAD_DPAD_UP;
  if (ImGui::IsKeyDown(ImGuiKey_DownArrow))
    buttons |= hid::PAD_DPAD_DOWN;
  if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))
    buttons |= hid::PAD_DPAD_LEFT;
  if (ImGui::IsKeyDown(ImGuiKey_RightArrow))
    buttons |= hid::PAD_DPAD_RIGHT;
  if (ImGui::IsKeyDown(ImGuiKey_Enter))
    buttons |= accept;
  if (ImGui::IsKeyDown(ImGuiKey_Escape) || ImGui::IsKeyDown(ImGuiKey_Backspace))
    buttons |= back;

  if (m_closing)
  {
    if (buttons == 0)
    {
      m_closing = false;
      m_open.store(false, std::memory_order_release);
    }
    return;
  }
  const uint32_t pressed = buttons & ~m_held;
  m_held = buttons;
  if (pressed & hid::PAD_DPAD_UP)
    m_row = (m_row + ROW_COUNT - 1) % ROW_COUNT;
  if (pressed & hid::PAD_DPAD_DOWN)
    m_row = (m_row + 1) % ROW_COUNT;
  if (pressed & hid::PAD_DPAD_LEFT)
    Change(m_row, -1);
  if (pressed & (hid::PAD_DPAD_RIGHT | accept))
    Change(m_row, +1);
  if (pressed & (back | hid::PAD_START))
    Close();
}

void GraphicsMenu::Draw()
{
  const bool open = IsOpen();
  if (open != m_shown)
  {
    // Takes effect from the next frame's ImGui frame.
    aurora_imgui_set_active(open);
    m_shown = open;
    // What opened the menu (the game's A) is still down: wait for a release.
    m_held = ~0u;
  }
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!open || context == nullptr || !context->WithinFrameScope)
    return;

  Navigate();
  if (m_closing || !IsOpen())
    return;

  // A 1280x720 canvas, scaled to fit the window and centered.
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float scale = std::min(display.x / 1280.0f, display.y / 720.0f);
  const ImVec2 origin{(display.x - 1280.0f * scale) * 0.5f, (display.y - 720.0f * scale) * 0.5f};
  const auto at = [&](float x, float y) { return ImVec2{origin.x + x * scale, origin.y + y * scale}; };
  ImDrawList* draw = ImGui::GetForegroundDrawList();

  // Dim the game's Options screen under the menu.
  draw->AddRectFilled({0, 0}, display, Rgba(4, 6, 30, 0.72f));

  // Title, in the game's gold.
  const char* title = "Graphics Settings";
  const float title_size = 52.0f * scale;
  const ImVec2 title_extent = TextSize(title_size, title);
  Text(draw, title_size, {origin.x + (1280.0f * scale - title_extent.x) * 0.5f, at(0, 92).y}, Rgba(236, 214, 140),
       title);
  draw->AddLine(at(450, 152), at(830, 152), Rgba(214, 186, 110, 0.9f), 2.0f * scale);

  // The rows, as the Options screen draws them: a blue band with the value in
  // a lighter box at its right end, the selected one lit.
  const float row_x = 330.0f, row_w = 620.0f, row_h = 66.0f, row_gap = 18.0f;
  const float rows_y = 190.0f;
  for (int row = 0; row < ROW_COUNT; ++row)
  {
    const bool selected = row == m_row;
    const float y = rows_y + row * (row_h + row_gap);
    const ImVec2 min = at(row_x, y), max = at(row_x + row_w, y + row_h);
    const float round = row_h * 0.5f * scale;
    if (selected)
      draw->AddRectFilled({min.x - 6 * scale, min.y - 6 * scale}, {max.x + 6 * scale, max.y + 6 * scale},
                          Rgba(120, 200, 255, 0.35f), round + 6 * scale);
    draw->AddRectFilledMultiColor(min, max, selected ? Rgba(70, 120, 235) : Rgba(36, 54, 170),
                                  selected ? Rgba(70, 120, 235) : Rgba(36, 54, 170),
                                  selected ? Rgba(30, 60, 190) : Rgba(18, 22, 110),
                                  selected ? Rgba(30, 60, 190) : Rgba(18, 22, 110));
    draw->AddRect(min, max, selected ? Rgba(200, 235, 255) : Rgba(214, 186, 110, 0.9f), 0.0f, 0,
                  2.0f * scale);
    // The crescent caps at both ends.
    for (const float cx : {min.x, max.x})
    {
      draw->AddCircleFilled({cx, (min.y + max.y) * 0.5f}, row_h * 0.52f * scale, Rgba(214, 186, 110), 24);
      draw->AddCircleFilled({cx + (cx == min.x ? 9.0f : -9.0f) * scale, (min.y + max.y) * 0.5f},
                            row_h * 0.46f * scale, selected ? Rgba(50, 90, 215) : Rgba(27, 38, 140), 24);
    }
    const float text_size = 30.0f * scale;
    Text(draw, text_size, {at(row_x + 44, 0).x, (min.y + max.y - text_size) * 0.5f},
         selected ? Rgba(255, 255, 255) : Rgba(205, 215, 255), ROWS[row].name);

    const std::string value = ValueText(m_settings, row);
    const ImVec2 box_min = at(row_x + row_w - 150, y + 10), box_max = at(row_x + row_w - 28, y + row_h - 10);
    draw->AddRectFilled(box_min, box_max, selected ? Rgba(90, 170, 255) : Rgba(60, 120, 230), 8.0f * scale);
    const ImVec2 value_extent = TextSize(text_size, value.c_str());
    Text(draw, text_size,
         {(box_min.x + box_max.x - value_extent.x) * 0.5f, (box_min.y + box_max.y - value_extent.y) * 0.5f},
         Rgba(255, 255, 255), value.c_str(), false);
    if (selected)
    {
      // Arrows on the value box for left and right.
      const float ay = (box_min.y + box_max.y) * 0.5f, s = 8.0f * scale;
      draw->AddTriangleFilled({box_min.x - 4 * scale, ay}, {box_min.x + s - 4 * scale, ay - s},
                              {box_min.x + s - 4 * scale, ay + s}, Rgba(255, 255, 255));
      draw->AddTriangleFilled({box_max.x + 4 * scale, ay}, {box_max.x - s + 4 * scale, ay - s},
                              {box_max.x - s + 4 * scale, ay + s}, Rgba(255, 255, 255));
    }
  }

  // Help line and button guide, as at the bottom of the Options screen.
  const float help_size = 28.0f * scale;
  const char* help = SameAs(m_settings, m_running) ? ROWS[m_row].help : "Changes apply the next time the game starts";
  const ImVec2 help_extent = TextSize(help_size, help);
  Text(draw, help_size, {origin.x + (1280.0f * scale - help_extent.x) * 0.5f, at(0, 612).y}, Rgba(255, 255, 255),
       help);
  const char* guide = "(<)(>): Change     (B): Back";
  const ImVec2 guide_extent = TextSize(help_size, guide);
  Text(draw, help_size, {origin.x + (1280.0f * scale - guide_extent.x) * 0.5f, at(0, 652).y}, Rgba(255, 255, 255),
       guide);
}
}  // namespace dualize
