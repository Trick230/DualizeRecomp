#include "ui/graphics_menu.hpp"

#include "machine.hpp"
#include "ui/game_assets.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

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

// Where the art is on the disc (the Options screen's textures and the font
// its help line uses), and the pieces of it the menu draws, in texels.
constexpr const char* OPTION_TEXTURES = "files/csd/textures/option_allus.tls";
constexpr const char* OPTION_TEXTURES_FRENCH = "files/csd/textures/option_allfr.tls";
constexpr const char* FONT = "files/fontJapan";
enum Sheet
{
  SHEET_TITLES,  // the screens' script titles
  SHEET_PARTS,   // row bars, moons, the icon frame
  SHEET_PILL,    // the value box's half
  SHEET_ICONS,   // the rows' icons
  SHEET_FILL,    // a white rounded square, tinted
  SHEET_LABELS,  // the list's labels and values, grey on black (made a white mask)
  SHEET_NAMES,   // the rows' names, made from the labels' letters (a white mask)
  SHEET_COUNT,
};
constexpr std::array<uint32_t, SHEET_COUNT> SHEET_IMAGES{0, 2, 3, 4, 16, 6, 6};
struct Piece
{
  Sheet sheet;
  float x0, y0, x1, y1;
};
constexpr Piece TITLE_OPTIONS{SHEET_TITLES, 4, 6, 254, 40};
constexpr Piece BAR{SHEET_PARTS, 166, 71, 251, 121};
constexpr Piece BAR_LIT{SHEET_PARTS, 164, 3, 252, 63};
constexpr Piece MOON_LEFT{SHEET_PARTS, 0, 69, 37, 125};
constexpr Piece MOON_RIGHT{SHEET_PARTS, 40, 69, 78, 125};
constexpr Piece FRAME{SHEET_PARTS, 80, 0, 160, 96};
constexpr Piece PILL{SHEET_PILL, 75, 32, 128, 128};
constexpr Piece ICON_TV{SHEET_ICONS, 0, 0, 128, 128};
constexpr Piece FILL{SHEET_FILL, 0, 0, 64, 64};
constexpr Piece VALUE_ON{SHEET_LABELS, 26, 194, 53, 209};
constexpr Piece VALUE_OFF{SHEET_LABELS, 103, 194, 140, 209};

// Button icons in the game's font (its messages write them as Greek letters).
constexpr char32_t GLYPH_DPAD = 0x397, GLYPH_B = 0x399;

// The Options screen's own entrance (at 30 frames a second): the icon in its
// frame slides in from the right, then the list slides in as it fades in,
// then the help line; leaving, the whole screen fades out.
constexpr double FADE_SECONDS = 0.1, ICON_SECONDS = 0.2, LIST_DELAY = 0.17, LIST_SECONDS = 0.13, HELP_DELAY = 0.3,
                 CLOSE_SECONDS = 0.2;
constexpr float ICON_SLIDE = 400.0f, LIST_SLIDE = 190.0f;

float EaseOut(float t)
{
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float FigureAdvance(char c, float h)
{
  const float w = 0.56f * h;
  switch (c)
  {
  case '1':
    return 0.62f * w;
  case '.':
    return 0.2f * h;
  case 'x':
    return 0.62f * h;
  default:
    return w;
  }
}

// Figures and the x, which the labels' lettering lacks, stroked in its
// geometric style: `h` its capital height, `thick` its stroke.
void Figure(ImDrawList* draw, char c, ImVec2 at, float h, float thick, ImU32 color)
{
  const float x = at.x, top = at.y, w = 0.56f * h, pi = 3.14159265f;
  const auto line = [&](std::initializer_list<ImVec2> points) {
    for (const ImVec2& p : points)
      draw->PathLineTo(p);
    draw->PathStroke(color, 0, thick);
  };
  switch (c)
  {
  case '1':
    line({{x + 0.1f * w, top + 0.2f * h}, {x + 0.5f * w, top}, {x + 0.5f * w, top + h}});
    return;
  case '2':
    draw->PathArcTo({x + w * 0.5f, top + w * 0.5f}, w * 0.5f, pi, 2.0f * pi + 0.55f, 16);
    draw->PathLineTo({x, top + h});
    draw->PathLineTo({x + w, top + h});
    draw->PathStroke(color, 0, thick);
    return;
  case '3':
    draw->PathArcTo({x + w * 0.5f, top + 0.23f * h}, 0.23f * h, pi + 0.5f, 2.5f * pi, 16);
    draw->PathStroke(color, 0, thick);
    draw->PathArcTo({x + w * 0.5f, top + 0.72f * h}, 0.27f * h, -0.5f * pi, pi - 0.45f, 18);
    draw->PathStroke(color, 0, thick);
    return;
  case '4':
    line({{x + 0.74f * w, top + h}, {x + 0.74f * w, top}, {x, top + 0.7f * h}, {x + w, top + 0.7f * h}});
    return;
  case '5':
    draw->PathLineTo({x + 0.9f * w, top});
    draw->PathLineTo({x + 0.2f * w, top});
    draw->PathLineTo({x + 0.12f * w, top + 0.44f * h});
    draw->PathArcTo({x + 0.48f * w, top + 0.68f * h}, 0.31f * h, -2.3f, pi - 0.4f, 20);
    draw->PathStroke(color, 0, thick);
    return;
  case '8':
    draw->AddCircle({x + w * 0.5f, top + 0.22f * h}, 0.22f * h, color, 16, thick);
    draw->AddCircle({x + w * 0.5f, top + 0.72f * h}, 0.28f * h, color, 20, thick);
    return;
  case '.':
    draw->AddCircleFilled({x + 0.1f * h, top + h - thick * 0.6f}, thick * 0.75f, color);
    return;
  case 'x':
  {
    const float xh = 0.64f * h, xw = 0.62f * h;
    line({{x, top + h - xh}, {x + xw, top + h}});
    line({{x + xw, top + h - xh}, {x, top + h}});
    return;
  }
  }
}

float Clamp01(double v)
{
  return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

// What was drawn since `first` (white) coloured from `top` at screen height
// `y0` to `bottom` at `y1`.
void Shade(ImDrawList* draw, int first, float y0, float y1, ImU32 top, ImU32 bottom)
{
  const ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
  for (int i = first; i < draw->VtxBuffer.Size; ++i)
  {
    ImDrawVert& v = draw->VtxBuffer[i];
    const float t = std::clamp((v.pos.y - y0) / (y1 - y0), 0.0f, 1.0f);
    v.col = ImGui::ColorConvertFloat4ToU32(
        {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
  }
}

// What was drawn since `first` faded by `alpha` and moved across by `dx` pixels.
void Adjust(ImDrawList* draw, int first, float alpha, float dx = 0)
{
  for (int i = first; i < draw->VtxBuffer.Size; ++i)
  {
    ImDrawVert& v = draw->VtxBuffer[i];
    v.pos.x += dx;
    const uint32_t a = static_cast<uint32_t>((v.col >> IM_COL32_A_SHIFT & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    v.col = (v.col & ~IM_COL32_A_MASK) | a << IM_COL32_A_SHIFT;
  }
}

std::u32string Utf32(std::string_view text)
{
  return {text.begin(), text.end()};
}

// The labels' lettering: a 24-texel line each, the baseline under row 18.
// Where each letter the rows' names need is (the French labels have the V and
// a v), the I being the l's bar and the y a v with its right stroke carried
// on down.
constexpr uint32_t LABEL_LINE = 24, NAME_GAP = 2, NAME_SPACE = 7;
struct Letter
{
  char c;
  bool french;
  uint32_t top, x0, x1;
};
constexpr std::array<Letter, 24> LETTERS{{
    {'A', false, 24, 72, 89},   {'n', false, 0, 27, 37},    {'t', false, 24, 175, 182}, {'i', false, 0, 152, 155},
    {'-', false, 144, 130, 136}, {'l', false, 144, 56, 58}, {'a', false, 0, 15, 24},    {'s', false, 24, 89, 97},
    {'g', false, 0, 39, 51},    {'R', false, 24, 151, 162}, {'e', false, 24, 32, 43},   {'o', false, 24, 189, 201},
    {'u', false, 0, 54, 63},    {'S', false, 0, 111, 122},  {'c', false, 24, 45, 55},   {'F', false, 120, 52, 62},
    {'r', false, 48, 131, 137}, {'m', false, 168, 136, 152}, {'I', false, 144, 56, 58}, {'p', false, 24, 99, 111},
    {'V', true, 24, 1, 11},     {'v', true, 72, 30, 38},    {'b', false, 144, 42, 54},  {'O', false, 189, 103, 118},
}};

struct NameMaker
{
  const Image& us;
  const Image& french;
  Image out;

  // The letter's columns, grey; empty when the labels lack it.
  std::vector<std::array<uint8_t, LABEL_LINE>> Columns(char c) const
  {
    std::vector<std::array<uint8_t, LABEL_LINE>> columns;
    if (c == 'y')
    {
      columns = Columns('v');
      const auto v = columns;
      for (uint32_t y = 19; y < 23; ++y)
      {
        for (size_t x = 0; x + 4 < v.size(); ++x)
          columns[x][y] = std::max(columns[x][y], v[x + 4][y - 8]);
      }
      return columns;
    }
    for (const Letter& l : LETTERS)
    {
      if (l.c != c)
        continue;
      const Image& image = l.french ? french : us;
      for (uint32_t x = l.x0; x < l.x1; ++x)
      {
        auto& column = columns.emplace_back();
        for (uint32_t y = 0; y < LABEL_LINE; ++y)
          column[y] = image.rgba[(static_cast<size_t>(l.top + y) * image.width + x) * 4];
      }
      // An accent cuts off the V's right arm's top: it mirrors the left one.
      if (c == 'V')
        columns.push_back(columns.front());
      break;
    }
    return columns;
  }

  // `name` on line `line` of the output; its width in texels.
  uint32_t Write(std::string_view name, uint32_t line)
  {
    uint32_t pen = 0;
    for (char c : name)
    {
      if (c == ' ')
      {
        pen += NAME_SPACE;
        continue;
      }
      for (const auto& column : Columns(c))
      {
        for (uint32_t y = 0; y < LABEL_LINE && pen < out.width; ++y)
        {
          uint8_t* p = &out.rgba[(static_cast<size_t>(line * LABEL_LINE + y) * out.width + pen) * 4];
          p[0] = p[1] = p[2] = std::max(p[0], column[y]);
        }
        ++pen;
      }
      pen += NAME_GAP;
    }
    return std::min(pen, out.width);
  }
};
}  // namespace

struct GraphicsMenu::Assets
{
  bool loaded = false, ok = false;
  std::array<ImTextureID, SHEET_COUNT> sheets{};
  std::array<ImVec2, SHEET_COUNT> sizes{};
  std::array<uint32_t, ROW_COUNT> name_widths{};
  BitmapFont font;
  ImTextureID font_texture{};

  void Load(const std::filesystem::path& game)
  {
    loaded = true;
    for (int i = 0; i < SHEET_COUNT; ++i)
    {
      auto image = LoadTplImage(game / OPTION_TEXTURES, SHEET_IMAGES[i]);
      if (!image)
        return;
      if (i == SHEET_NAMES)
      {
        const auto french = LoadTplImage(game / OPTION_TEXTURES_FRENCH, SHEET_IMAGES[i]);
        if (!french || image->width != 256 || french->width != 256)
          return;
        NameMaker maker{*image, *french, {256, LABEL_LINE * ROW_COUNT, {}}};
        maker.out.rgba.assign(static_cast<size_t>(maker.out.width) * maker.out.height * 4, 0);
        for (int row = 0; row < ROW_COUNT; ++row)
          name_widths[row] = maker.Write(ROWS[row].name, row);
        image = std::move(maker.out);
      }
      if (i == SHEET_LABELS || i == SHEET_NAMES)
      {
        for (size_t p = 0; p < image->rgba.size(); p += 4)
        {
          // Edges filled out a little: the game draws its lettering fuller.
          image->rgba[p + 3] =
              static_cast<uint8_t>(std::lround(255.0 * std::pow(image->rgba[p] / 255.0, 0.6)));
          image->rgba[p] = image->rgba[p + 1] = image->rgba[p + 2] = 255;
        }
      }
      sheets[i] = aurora_imgui_add_texture(image->rgba.data(), image->width, image->height);
      sizes[i] = {static_cast<float>(image->width), static_cast<float>(image->height)};
    }
    if (!font.Load(game / FONT))
      return;
    font_texture = aurora_imgui_add_texture(font.Atlas().rgba.data(), font.Atlas().width, font.Atlas().height);
    ok = true;
  }
};

GraphicsMenu::~GraphicsMenu() = default;

namespace
{
// Drawing on a 1280x720 canvas, scaled to fit the window and centered.
struct Canvas
{
  ImDrawList* draw;
  const GraphicsMenu::Assets& assets;
  ImVec2 origin;
  float scale;

  ImVec2 At(float x, float y) const { return {origin.x + x * scale, origin.y + y * scale}; }

  // A piece stretched over the canvas rectangle; u0/u1 pick part of it across.
  void Image(const Piece& p, float x0, float y0, float x1, float y1, ImU32 tint = IM_COL32_WHITE, float u0 = -1,
             float u1 = -1) const
  {
    const ImVec2 size = assets.sizes[p.sheet];
    const float pu0 = u0 >= 0 ? u0 : p.x0, pu1 = u1 >= 0 ? u1 : p.x1;
    draw->AddImage(assets.sheets[p.sheet], At(x0, y0), At(x1, y1), {pu0 / size.x, p.y0 / size.y},
                   {pu1 / size.x, p.y1 / size.y}, tint);
  }

  // A piece at its texel size times `factor`, centered at (cx, cy).
  void Sprite(const Piece& p, float cx, float cy, float factor, ImU32 tint = IM_COL32_WHITE) const
  {
    const float w = (p.x1 - p.x0) * factor, h = (p.y1 - p.y0) * factor;
    Image(p, cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, tint);
  }

  // Stretched across, its `cap` texels at each end kept in proportion.
  void ThreeSlice(const Piece& p, float x0, float y0, float x1, float y1, float cap,
                  ImU32 tint = IM_COL32_WHITE) const
  {
    const float c = cap * (y1 - y0) / (p.y1 - p.y0);
    Image(p, x0, y0, x0 + c, y1, tint, p.x0, p.x0 + cap);
    Image(p, x0 + c, y0, x1 - c, y1, tint, p.x0 + cap, p.x1 - cap);
    Image(p, x1 - c, y0, x1, y1, tint, p.x1 - cap, p.x1);
  }

  // Stretched both ways, its `cap` texels at each edge drawn `factor` times as big.
  void NineSlice(const Piece& p, float x0, float y0, float x1, float y1, float cap, float factor,
                 ImU32 tint = IM_COL32_WHITE, bool hollow = false) const
  {
    const ImVec2 size = assets.sizes[p.sheet];
    const float c = cap * factor;
    const float xs[] = {x0, x0 + c, x1 - c, x1}, ys[] = {y0, y0 + c, y1 - c, y1};
    const float us[] = {p.x0, p.x0 + cap, p.x1 - cap, p.x1}, vs[] = {p.y0, p.y0 + cap, p.y1 - cap, p.y1};
    for (int j = 0; j < 3; ++j)
    {
      for (int i = 0; i < 3; ++i)
        if (!hollow || i != 1 || j != 1)
          draw->AddImage(assets.sheets[p.sheet], At(xs[i], ys[j]), At(xs[i + 1], ys[j + 1]),
                         {us[i] / size.x, vs[j] / size.y}, {us[i + 1] / size.x, vs[j + 1] / size.y}, tint);
    }
  }

  // Text in the game's font, `size` canvas pixels a cell.
  float TextWidth(std::u32string_view text, float size) const
  {
    const float k = size / assets.font.CellSize();
    float width = 0;
    for (char32_t c : text)
    {
      if (const BitmapFont::Glyph* g = assets.font.Find(c))
        width += (g->x1 - g->x0 + 2) * k;
    }
    return width;
  }

  void Text(std::u32string_view text, float x, float cy, float size, ImU32 color, bool shadow = true) const
  {
    const float k = size / assets.font.CellSize();
    const ImVec2 atlas{static_cast<float>(assets.font.Atlas().width), static_cast<float>(assets.font.Atlas().height)};
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
    {
      const float offset = pass == 0 ? 1.5f : 0.0f;
      const ImU32 tint = pass == 0 ? Rgba(0, 0, 20, 0.55f) : color;
      float pen = x;
      for (char32_t c : text)
      {
        const BitmapFont::Glyph* g = assets.font.Find(c);
        if (g == nullptr)
          continue;
        const float w = (g->x1 - g->x0) * k, h = (g->y1 - g->y0) * k;
        draw->AddImage(assets.font_texture, At(pen + k + offset, cy - h * 0.5f + offset),
                       At(pen + k + w + offset, cy + h * 0.5f + offset), {g->x0 / atlas.x, g->y0 / atlas.y},
                       {g->x1 / atlas.x, g->y1 / atlas.y}, tint);
        pen += w + 2 * k;
      }
    }
  }

  void TextCentered(std::u32string_view text, float cx, float cy, float size, ImU32 color, bool shadow = true) const
  {
    Text(text, cx - TextWidth(text, size) * 0.5f, cy, size, color, shadow);
  }
};
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
                           std::function<bool()> classic, std::filesystem::path game,
                           std::filesystem::path settings_path, GraphicsSettings running)
    : m_gamepads(gamepads), m_extra_buttons(std::move(extra_buttons)), m_classic(std::move(classic)),
      m_game(std::move(game)), m_assets(std::make_unique<Assets>()), m_settings_path(std::move(settings_path)), m_running(running), m_settings(running)
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
  m_close_time = ImGui::GetTime();
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
  // The game's own: on the Classic Controller a and b accept, x and y go back.
  const uint32_t accept = classic ? hid::PAD_EAST | hid::PAD_SOUTH : hid::PAD_SOUTH;
  const uint32_t back = classic ? hid::PAD_NORTH | hid::PAD_WEST : hid::PAD_WEST;
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
    if (buttons == 0 && ImGui::GetTime() - m_close_time >= CLOSE_SECONDS)
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
    m_open_time = -1;
  }
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (!open || context == nullptr || !context->WithinFrameScope)
    return;

  if (m_open_time < 0)
    m_open_time = ImGui::GetTime();
  Navigate();
  if (!IsOpen())
    return;
  const double since = ImGui::GetTime() - m_open_time;
  const float closing = m_closing ? 1.0f - Clamp01((ImGui::GetTime() - m_close_time) / CLOSE_SECONDS) : 1.0f;
  const float screen_alpha = Clamp01(since / FADE_SECONDS) * closing;
  const float icon_t = Clamp01(since / ICON_SECONDS), list_t = Clamp01((since - LIST_DELAY) / LIST_SECONDS);
  const float help_alpha = Clamp01((since - HELP_DELAY) / FADE_SECONDS) * closing;

  if (!m_assets->loaded)
    m_assets->Load(m_game);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float scale = std::min(display.x / 1280.0f, display.y / 720.0f);
  const Canvas canvas{ImGui::GetForegroundDrawList(), *m_assets,
                      {(display.x - 1280.0f * scale) * 0.5f, (display.y - 720.0f * scale) * 0.5f}, scale};
  ImDrawList* draw = canvas.draw;
  const int first_vertex = draw->VtxBuffer.Size;

  // The Options screen dims under the menu, its list and help line all but
  // hidden (bands that fade out at their ends).
  draw->AddRectFilled({0, 0}, display, Rgba(2, 4, 24, 0.9f));
  const auto band = [&](float x0, float y0, float x1, float y1) {
    const ImU32 solid = Rgba(2, 4, 24, 0.92f), clear = Rgba(2, 4, 24, 0.0f);
    const float fade = 60.0f;
    draw->AddRectFilledMultiColor(canvas.At(x0 - fade, y0), canvas.At(x0, y1), clear, solid, solid, clear);
    draw->AddRectFilled(canvas.At(x0, y0), canvas.At(x1, y1), solid);
    draw->AddRectFilledMultiColor(canvas.At(x1, y0), canvas.At(x1 + fade, y1), solid, clear, clear, solid);
  };
  band(520, 196, 1060, 600);
  band(300, 588, 980, 690);
  if (!m_assets->ok)
  {
    // Without the game's art (a different disc layout): plain text.
    for (int row = 0; row < ROW_COUNT; ++row)
    {
      const std::string line = std::string(row == m_row ? "> " : "  ") + ROWS[row].name + ": " +
                               ValueText(m_settings, row);
      draw->AddText(nullptr, 30.0f * scale, canvas.At(400, 220.0f + row * 60.0f), Rgba(255, 255, 255), line.c_str());
    }
    Adjust(draw, first_vertex, screen_alpha);
    return;
  }

  // As the Options screen lays its list out: the title, the selected row's
  // icon in its frame on the left, the rows on the right, help and buttons below.
  canvas.Sprite(TITLE_OPTIONS, 640, 122, 1.48f);
  Adjust(draw, first_vertex, screen_alpha);
  const int icon_vertex = draw->VtxBuffer.Size;
  canvas.NineSlice(FILL, 266, 299, 483, 516, 16, 1.0f, Rgba(108, 0, 206));
  canvas.NineSlice(FRAME, 257, 290, 492, 525, 20, 1.45f, IM_COL32_WHITE, true);
  canvas.Sprite(ICON_TV, 375, 407, 1.62f);
  Adjust(draw, icon_vertex, Clamp01(since / FADE_SECONDS) * closing, (1.0f - EaseOut(icon_t)) * ICON_SLIDE * scale);
  const int list_vertex = draw->VtxBuffer.Size;

  // The Options screen's rows at its own size (the art 1.48 times as big,
  // the lettering 1.5); it has four, so the five here sit closer.
  constexpr float ROW_PITCH = 80.0f, LIST_CENTER = 396.0f, ART = 1.48f, LETTERING = 1.5f;
  for (int row = 0; row < ROW_COUNT; ++row)
  {
    const bool selected = row == m_row;
    const float cy = LIST_CENTER + (row - ROW_COUNT / 2) * ROW_PITCH;
    const Piece& bar = selected ? BAR_LIT : BAR;
    const float half = (bar.y1 - bar.y0) * ART * 0.5f, grow = selected ? 3.0f : 0.0f;
    canvas.ThreeSlice(bar, 541 - grow, cy - half, 1019 + grow, cy + half, 16);
    canvas.Sprite(MOON_LEFT, 553, cy, 1.46f);
    canvas.Sprite(MOON_RIGHT, 1022, cy, 1.46f);
    // The name with the game's soft violet glow around it.
    const uint32_t width = m_assets->name_widths[row];
    const Piece name{SHEET_NAMES, 0, static_cast<float>(row * LABEL_LINE), static_cast<float>(width),
                     static_cast<float>((row + 1) * LABEL_LINE)};
    const float top = cy - 18.5f, bottom = top + LABEL_LINE * LETTERING, right = 572 + width * LETTERING;
    for (const ImVec2 d : {ImVec2{-1.5f, 0}, ImVec2{1.5f, 0}, ImVec2{0, -1.5f}, ImVec2{0, 1.5f}})
      canvas.Image(name, 572 + d.x, top + d.y, right + d.x, bottom + d.y, Rgba(170, 110, 255, 0.3f));
    canvas.Image(name, 572, top, right, bottom, selected ? Rgba(255, 255, 255) : Rgba(255, 214, 255));

    // The value in the game's box: a blue that lightens downwards, cyan on the
    // selected row (its bar showing through); ON and OFF in the labels' lettering.
    const std::string value = ValueText(m_settings, row);
    const int box_vertex = draw->VtxBuffer.Size;
    canvas.NineSlice(FILL, 900, cy - 21.5f, 1012, cy + 21.5f, 16, 0.75f, IM_COL32_WHITE);
    if (selected)
      Shade(draw, box_vertex, canvas.At(0, cy - 21.5f).y, canvas.At(0, cy + 21.5f).y, Rgba(104, 222, 255, 0.93f),
            Rgba(84, 190, 255, 0.93f));
    else
      Shade(draw, box_vertex, canvas.At(0, cy - 21.5f).y, canvas.At(0, cy + 21.5f).y, Rgba(36, 106, 255),
            Rgba(66, 140, 255));
    if (value == "ON" || value == "OFF")
      canvas.Sprite(value == "ON" ? VALUE_ON : VALUE_OFF, 956, cy, LETTERING);
    else
    {
      // Figures drawn at the lettering's size: capitals 14 texels high.
      constexpr float CAPITAL = 14.0f * LETTERING, STROKE = 2.7f, GAP = 2.5f;
      float width = 0;
      for (char c : value)
        width += FigureAdvance(c, CAPITAL) + GAP;
      float pen = 956 - (width - GAP) * 0.5f;
      for (char c : value)
      {
        const ImVec2 at = canvas.At(pen, cy - CAPITAL * 0.5f);
        Figure(draw, c, at, CAPITAL * scale, STROKE * scale, Rgba(255, 255, 255));
        pen += FigureAdvance(c, CAPITAL) + GAP;
      }
    }
    if (selected)
    {
      // Left and right change the value.
      const ImVec2 l = canvas.At(905, cy), r = canvas.At(1007, cy);
      const float s = 5.0f * scale;
      draw->AddTriangleFilled(l, {l.x + s, l.y - s}, {l.x + s, l.y + s}, Rgba(255, 255, 255));
      draw->AddTriangleFilled(r, {r.x - s, r.y - s}, {r.x - s, r.y + s}, Rgba(255, 255, 255));
    }
  }

  Adjust(draw, list_vertex, list_t * closing, (1.0f - EaseOut(list_t)) * LIST_SLIDE * scale);

  // Help and the buttons, with the GameCube pad's icons.
  const int help_vertex = draw->VtxBuffer.Size;
  const char* help = SameAs(m_settings, m_running) ? ROWS[m_row].help : "Changes apply the next time the game starts";
  canvas.TextCentered(Utf32(help), 640, 610, 28, Rgba(255, 255, 255));
  std::u32string guide{GLYPH_DPAD};
  guide += U": Change      ";
  guide += GLYPH_B;
  guide += U": Back";
  canvas.TextCentered(guide, 640, 664, 28, Rgba(255, 255, 255));
  Adjust(draw, help_vertex, help_alpha);
}
}  // namespace dualize
