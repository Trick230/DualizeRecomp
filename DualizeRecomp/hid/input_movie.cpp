#include "hid/input_movie.hpp"

#include <bit>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dualize::hid
{
namespace
{
bool SameGc(bool plugged_a, const GcPadInput& a, bool plugged_b, const GcPadInput& b)
{
  return plugged_a == plugged_b && std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool SameWii(const ios::WiimoteInput& a, const ios::WiimoteInput& b)
{
  return a.buttons == b.buttons && a.classic == b.classic && a.classic_buttons == b.classic_buttons &&
         std::bit_cast<uint32_t>(a.left_x) == std::bit_cast<uint32_t>(b.left_x) &&
         std::bit_cast<uint32_t>(a.left_y) == std::bit_cast<uint32_t>(b.left_y) &&
         std::bit_cast<uint32_t>(a.right_x) == std::bit_cast<uint32_t>(b.right_x) &&
         std::bit_cast<uint32_t>(a.right_y) == std::bit_cast<uint32_t>(b.right_y) &&
         std::bit_cast<uint32_t>(a.left_trigger) == std::bit_cast<uint32_t>(b.left_trigger) &&
         std::bit_cast<uint32_t>(a.right_trigger) == std::bit_cast<uint32_t>(b.right_trigger);
}

float FloatBits(const std::string& hex)
{
  return std::bit_cast<float>(static_cast<uint32_t>(std::stoul(hex, nullptr, 16)));
}
}  // namespace

InputMovie::~InputMovie()
{
  if (m_file)
    std::fclose(m_file);
}

bool InputMovie::Record(const std::filesystem::path& path, std::string* error)
{
  m_file = std::fopen(path.string().c_str(), "w");
  if (!m_file)
  {
    *error = "can't write " + path.string();
    return false;
  }
  std::fprintf(m_file, "# DualizeRecomp input movie (hid/input_movie.hpp has the format)\n");
  return true;
}

bool InputMovie::Replay(const std::filesystem::path& path, std::string* error)
{
  std::ifstream file(path);
  if (!file)
  {
    *error = "can't read " + path.string();
    return false;
  }
  std::string line;
  int number = 0;
  while (std::getline(file, line))
  {
    ++number;
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream in(line);
    std::string kind;
    int port = 0;
    in >> kind;
    if (kind == "end")
    {
      in >> m_end_field;
      continue;
    }
    in >> port;
    bool ok = port >= 0 && port < PORTS;
    if (ok && kind == "gc")
    {
      GcEntry e{};
      unsigned plugged, buttons, v[8];
      in >> e.read >> e.field >> plugged >> std::hex >> buttons >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >>
          v[6] >> v[7];
      e.plugged = plugged != 0;
      e.input = {static_cast<uint16_t>(buttons), static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]),
                 static_cast<uint8_t>(v[2]), static_cast<uint8_t>(v[3]), static_cast<uint8_t>(v[4]),
                 static_cast<uint8_t>(v[5]), static_cast<uint8_t>(v[6]), static_cast<uint8_t>(v[7])};
      ok = static_cast<bool>(in);
      m_gc[port].entries.push_back(e);
    }
    else if (ok && kind == "wii")
    {
      WiiEntry e{};
      unsigned buttons, classic, classic_buttons;
      std::string f[6];
      in >> e.read >> e.field >> std::hex >> buttons >> classic >> classic_buttons >> f[0] >> f[1] >> f[2] >> f[3] >>
          f[4] >> f[5];
      ok = static_cast<bool>(in);
      if (ok)
        e.input = {static_cast<uint16_t>(buttons), classic != 0, static_cast<uint16_t>(classic_buttons),
                   FloatBits(f[0]), FloatBits(f[1]), FloatBits(f[2]), FloatBits(f[3]), FloatBits(f[4]),
                   FloatBits(f[5])};
      m_wii[port].entries.push_back(e);
    }
    else
    {
      ok = false;
    }
    if (!ok)
    {
      *error = path.string() + ":" + std::to_string(number) + ": not a movie line";
      return false;
    }
  }
  m_replay = true;
  return true;
}

template <typename Entry>
const Entry* InputMovie::Advance(Stream<Entry>& stream, uint64_t field, const char* kind, int port)
{
  const uint64_t read = stream.reads++;
  while (stream.cursor < stream.entries.size() && stream.entries[stream.cursor].read <= read)
  {
    const Entry& e = stream.entries[stream.cursor++];
    if (e.read == read && e.field != field && !m_diverged)
    {
      // The run no longer reads where the recorded one did.
      m_diverged = true;
      std::fprintf(stderr, "[movie] %s %d read %" PRIu64 " in field %" PRIu64 ", recorded in %" PRIu64 "\n",
                   kind, port, read, field, e.field);
    }
  }
  return stream.cursor ? &stream.entries[stream.cursor - 1] : nullptr;
}

void InputMovie::GcPad(int port, uint64_t field, bool* plugged, GcPadInput* input)
{
  if (port < 0 || port >= PORTS)
    return;
  Stream<GcEntry>& stream = m_gc[port];
  if (m_replay)
  {
    const GcEntry* e = Advance(stream, field, "gc", port);
    *plugged = e && e->plugged;
    *input = e ? e->input : GcPadInput{};
    return;
  }
  if (!m_file)
    return;
  const uint64_t read = stream.reads++;
  if (stream.written && SameGc(stream.last.plugged, stream.last.input, *plugged, *input))
    return;
  stream.last = {read, field, *plugged, *input};
  stream.written = true;
  WriteGc(port, stream.last);
}

void InputMovie::Wiimote(int slot, uint64_t field, ios::WiimoteInput* input)
{
  if (slot < 0 || slot >= PORTS)
    return;
  Stream<WiiEntry>& stream = m_wii[slot];
  if (m_replay)
  {
    const WiiEntry* e = Advance(stream, field, "wii", slot);
    *input = e ? e->input : ios::WiimoteInput{.classic = input->classic};
    return;
  }
  if (!m_file)
    return;
  const uint64_t read = stream.reads++;
  if (stream.written && SameWii(stream.last.input, *input))
    return;
  stream.last = {read, field, *input};
  stream.written = true;
  WriteWii(slot, stream.last);
}

void InputMovie::WriteGc(int port, const GcEntry& e)
{
  const GcPadInput& i = e.input;
  std::fprintf(m_file, "gc %d %" PRIu64 " %" PRIu64 " %d %04x %02x %02x %02x %02x %02x %02x %02x %02x\n", port, e.read,
               e.field, e.plugged ? 1 : 0, i.buttons, i.stick_x, i.stick_y, i.c_x, i.c_y, i.trigger_l, i.trigger_r,
               i.analog_a, i.analog_b);
  std::fflush(m_file);
}

void InputMovie::WriteWii(int slot, const WiiEntry& e)
{
  const ios::WiimoteInput& i = e.input;
  std::fprintf(m_file, "wii %d %" PRIu64 " %" PRIu64 " %04x %d %04x %08x %08x %08x %08x %08x %08x\n", slot, e.read,
               e.field, i.buttons, i.classic ? 1 : 0, i.classic_buttons, std::bit_cast<uint32_t>(i.left_x),
               std::bit_cast<uint32_t>(i.left_y), std::bit_cast<uint32_t>(i.right_x), std::bit_cast<uint32_t>(i.right_y),
               std::bit_cast<uint32_t>(i.left_trigger), std::bit_cast<uint32_t>(i.right_trigger));
  std::fflush(m_file);
}

void InputMovie::Finish(uint64_t field)
{
  if (!m_file)
    return;
  std::fprintf(m_file, "end %" PRIu64 "\n", field);
  std::fclose(m_file);
  m_file = nullptr;
}
}  // namespace dualize::hid
