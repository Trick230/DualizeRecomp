// My Dream's Weather Link: the game reads the current weather of the city
// set in the Forecast Channel from that channel's files on the NAND. Here
// its reader answers with the weather where the host is instead: the city
// of the host's time zone, its weather fetched from Open-Meteo (no account,
// no key) with the curl the host has, on a thread from the start. The game
// reads it once, as it boots, waiting a few seconds for it if need be.
// With no answer (offline, an unknown zone) the reader reports no data and
// the game keeps its own calendar of weather by day.
//
// The reader's functions come from config/nights.sigs. What the game reads of
// the current weather: the icon (Forecast Channel codes: the game ranks them
// clear, cloudy or stormy), the temperature, the wind, and two amounts its
// own calendar uses for rain (5 and more: rain) and snow (5 and more: snow).

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "kernel/memory.hpp"
#include "machine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
using dualize::Cpu;
using dualize::GetMachine;

Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// The current weather as the reader lays it out (0x11C bytes).
constexpr uint32_t CURRENT_SIZE = 0x11C, CURRENT_ICON = 0x10, CURRENT_CELSIUS = 0x112, CURRENT_FAHRENHEIT = 0x113,
                   CURRENT_WIND_KMH = 0x115, CURRENT_WIND_MPH = 0x116, CURRENT_RAIN = 0x118, CURRENT_SNOW = 0x11A;
constexpr uint32_t NO_DATA = static_cast<uint32_t>(-10);  // what the reader returns without the channel's files

struct Weather
{
  uint16_t icon;
  int8_t celsius;
  uint8_t wind_kmh;
  uint16_t rain, snow;
};

// A WMO weather code (Open-Meteo's) as the Forecast Channel's icon and the
// game's rain and snow amounts.
Weather FromWmo(int code, bool day, double celsius, double wind_kmh)
{
  Weather w{};
  if (code <= 1)
    w.icon = 0x65;  // sunny
  else if (code == 2)
    w.icon = 0x66;  // partly cloudy
  else if (code == 3)
    w.icon = 0x6A;  // cloudy
  else if (code == 45 || code == 48)
    w.icon = 0x7C;  // fog
  else if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82))
    w.icon = 0x71;  // showers
  else if (code >= 61 && code <= 67)
    w.icon = 0x6F;  // rain
  else if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    w.icon = 0x74;  // snow
  else if (code >= 95)
    w.icon = 0x7D;  // thunderstorms
  else
    w.icon = 0x6A;
  if (!day)
    w.icon |= 0x8000;
  switch (code)
  {
  case 51: case 53: case 56: case 61: case 66: case 80:
    w.rain = 3;  // light
    break;
  case 55: case 57: case 63: case 81:
    w.rain = 10;
    break;
  case 65: case 67: case 82: case 95: case 96: case 99:
    w.rain = 25;  // heavy
    break;
  }
  if ((code >= 71 && code <= 77) || code == 85 || code == 86)
    w.snow = 5;
  w.celsius = static_cast<int8_t>(std::clamp(std::lround(celsius), -127L, 127L));
  w.wind_kmh = static_cast<uint8_t>(std::clamp(std::lround(wind_kmh), 0L, 255L));
  return w;
}

// The host's time zone, as an IANA name ("Europe/Lisbon").
std::string TimeZoneName()
{
  if (const char* tz = std::getenv("TZ"); tz != nullptr && std::strchr(tz, '/') != nullptr)
    return tz[0] == ':' ? tz + 1 : tz;
#ifdef _WIN32
  // Windows names its zones its own way; ICU (in Windows since 10 1903) maps
  // them, by the user's country where a zone spans several ("GMT Standard
  // Time" is London, Lisbon and Dublin).
  DYNAMIC_TIME_ZONE_INFORMATION info{};
  if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID || info.TimeZoneKeyName[0] == 0)
    return {};
  char region[8]{};
  wchar_t geo[8]{};
  if (GetUserDefaultGeoName(geo, 8) == 3)
  {
    region[0] = static_cast<char>(geo[0]);
    region[1] = static_cast<char>(geo[1]);
  }
  using Map = int32_t (*)(const wchar_t*, int32_t, const char*, wchar_t*, int32_t, int*);
  HMODULE icu = LoadLibraryW(L"icu.dll");
  if (icu == nullptr)
    return {};
  const auto map = reinterpret_cast<Map>(GetProcAddress(icu, "ucal_getTimeZoneIDForWindowsID"));
  std::string name;
  if (map != nullptr)
  {
    wchar_t id[64]{};
    int status = 0;
    int32_t length = map(info.TimeZoneKeyName, -1, region[0] != 0 ? region : nullptr, id, 63, &status);
    if (status > 0 || length <= 0)
    {
      status = 0;
      length = map(info.TimeZoneKeyName, -1, nullptr, id, 63, &status);
    }
    if (status <= 0 && length > 0)
    {
      for (int32_t i = 0; i < length; ++i)
        name += static_cast<char>(id[i]);
    }
  }
  FreeLibrary(icu);
  return name;
#else
  char target[256]{};
  const ssize_t length = readlink("/etc/localtime", target, sizeof(target) - 1);
  if (length <= 0)
    return {};
  const std::string path(target, static_cast<size_t>(length));
  const size_t at = path.find("zoneinfo/");
  return at == std::string::npos ? std::string{} : path.substr(at + 9);
#endif
}

// What curl prints for `url`, or nothing.
std::string Fetch(const std::string& url)
{
#ifdef _WIN32
  FILE* pipe = _popen(("curl -s -f --max-time 10 \"" + url + "\" 2>NUL").c_str(), "rb");
#else
  FILE* pipe = popen(("curl -s -f --max-time 10 '" + url + "' 2>/dev/null").c_str(), "r");
#endif
  if (pipe == nullptr)
    return {};
  std::string out;
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0 && out.size() < (1u << 20))
    out.append(buffer, n);
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return out;
}

// The number after "key": in `json`, from `from` on.
std::optional<double> Number(const std::string& json, const std::string& key, size_t from = 0)
{
  const size_t at = json.find("\"" + key + "\":", from);
  if (at == std::string::npos)
    return std::nullopt;
  const char* start = json.c_str() + at + key.size() + 3;
  char* end = nullptr;
  const double value = std::strtod(start, &end);
  return end == start ? std::nullopt : std::optional<double>(value);
}

std::optional<Weather> FetchWeather()
{
  const std::string zone = TimeZoneName();
  const size_t slash = zone.rfind('/');
  if (slash == std::string::npos || zone.compare(0, 4, "Etc/") == 0)
    return std::nullopt;
  std::string city;
  for (char c : zone.substr(slash + 1))
  {
    if (c == '_')
      city += "%20";
    else if (std::isalnum(static_cast<unsigned char>(c)) || c == '-')
      city += c;
  }
  const std::string place =
      Fetch("https://geocoding-api.open-meteo.com/v1/search?count=1&format=json&name=" + city);
  const auto latitude = Number(place, "latitude"), longitude = Number(place, "longitude");
  if (!latitude || !longitude)
    return std::nullopt;
  char url[256];
  std::snprintf(url, sizeof(url),
                "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
                "&current=temperature_2m,weather_code,wind_speed_10m,is_day",
                *latitude, *longitude);
  const std::string json = Fetch(url);
  const size_t current = json.find("\"current\":");
  if (current == std::string::npos)
    return std::nullopt;
  const auto code = Number(json, "weather_code", current), celsius = Number(json, "temperature_2m", current),
             wind = Number(json, "wind_speed_10m", current), day = Number(json, "is_day", current);
  if (!code || !celsius || !wind)
    return std::nullopt;
  const Weather weather = FromWmo(static_cast<int>(*code), !day || *day != 0, *celsius, *wind);
  std::fprintf(stderr, "[weather] %s: WMO %d, %.1f C, wind %.0f km/h\n", zone.c_str(), static_cast<int>(*code),
               *celsius, *wind);
  return weather;
}

// The host's weather, fetched once on a thread of its own.
class LiveWeather
{
public:
  void Start()
  {
    std::thread([this] {
      std::optional<Weather> weather = FetchWeather();
      std::lock_guard lock(m_mutex);
      m_weather = weather;
      m_done = true;
      m_changed.notify_all();
    }).detach();
    m_started = true;
  }

  // What was fetched, waiting up to `wait` for it; nothing when off or failed.
  std::optional<Weather> Get(std::chrono::milliseconds wait = {})
  {
    if (!m_started)
      return std::nullopt;
    std::unique_lock lock(m_mutex);
    m_changed.wait_for(lock, wait, [this] { return m_done; });
    return m_weather;
  }

private:
  std::atomic<bool> m_started{false};
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::optional<Weather> m_weather;
  bool m_done = false;
};

LiveWeather g_weather;

void Return(CPUState* ctx, uint32_t value)
{
  ctx->gpr[3] = value;
}

void Clear(CPUState* ctx, uint32_t address, uint32_t size)
{
  std::memset(GetMachine(CpuOf(ctx)).memory.Effective(address, size), 0, size);
}
}  // namespace

namespace dualize
{
void StartNightsWeather(bool live)
{
  if (live)
    g_weather.Start();
}
}  // namespace dualize

// load(work): the channel's files read and checked. Without host weather yet,
// what the reader returns when the files are missing.
extern "C" bool nights_WeatherLoad(CPUState* ctx)
{
  if (!g_weather.Get(std::chrono::seconds(5)))
  {
    Return(ctx, NO_DATA);
    return true;
  }
  uint8_t* loaded = GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[13] + NIGHTS_WEATHER_LOADED_SDA, 4);
  dualize::Store32(loaded, 1);
  Return(ctx, 0);
  return true;
}

// city(&code), count(&n), version(&n): the set city and two counts, unused by the game.
extern "C" bool nights_WeatherCity(CPUState* ctx)
{
  Clear(ctx, ctx->gpr[3], 4);
  Return(ctx, 0);
  return true;
}

extern "C" bool nights_WeatherCount(CPUState* ctx)
{
  return nights_WeatherCity(ctx);
}

extern "C" bool nights_WeatherVersion(CPUState* ctx)
{
  return nights_WeatherCity(ctx);
}

// location(code, &info) and forecast(code, time, &day): read by the game into
// buffers it never looks at.
extern "C" bool nights_WeatherLocation(CPUState* ctx)
{
  Return(ctx, 0);
  return true;
}

extern "C" bool nights_WeatherForecast(CPUState* ctx)
{
  Return(ctx, 0);
  return true;
}

// current(code, &weather): the weather now.
extern "C" bool nights_WeatherCurrent(CPUState* ctx)
{
  const std::optional<Weather> weather = g_weather.Get();
  if (!weather)
  {
    Return(ctx, NO_DATA);
    return true;
  }
  uint8_t* out = GetMachine(CpuOf(ctx)).memory.Effective(ctx->gpr[4], CURRENT_SIZE);
  std::memset(out, 0, CURRENT_SIZE);
  dualize::Store16(out + CURRENT_ICON, weather->icon);
  out[CURRENT_CELSIUS] = static_cast<uint8_t>(weather->celsius);
  out[CURRENT_FAHRENHEIT] = static_cast<uint8_t>(std::lround(weather->celsius * 1.8 + 32));
  out[CURRENT_WIND_KMH] = weather->wind_kmh;
  out[CURRENT_WIND_MPH] = static_cast<uint8_t>(std::lround(weather->wind_kmh / 1.609));
  dualize::Store16(out + CURRENT_RAIN, weather->rain);
  dualize::Store16(out + CURRENT_SNOW, weather->snow);
  Return(ctx, 0);
  return true;
}
