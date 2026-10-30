#pragma once

// Input movies: what the game read from its controllers, recorded read by read
// and played back the same way. The guest runs on its own clock (see
// Machine::SyncGpu), so the same start (boot or a savestate) and the same
// reads make the same run, however fast the host goes: a regression test or
// a bug report that replays exactly.
//
// The file is text, a line per change of one controller's state:
//
//   gc <port> <read> <field> <plugged> <buttons> <stick x> <stick y> <c x> <c y> <l> <r> <a> <b>
//   wii <slot> <read> <field> <buttons> <classic> <classic buttons> <6 floats as hex bits>
//   end <field>
//
// <read> counts that controller's reads from the start, which is what playback
// keys on; <field> is the VI field it happened in, which playback checks to
// say where a run left the recorded one. The values hold until the next line.

#include "hw/hw.hpp"
#include "kernel/ios/wiimote.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace dualize::hid
{
class InputMovie
{
public:
  static constexpr int PORTS = 4;

  ~InputMovie();

  bool Record(const std::filesystem::path& path, std::string* error);
  bool Replay(const std::filesystem::path& path, std::string* error);
  bool Recording() const { return m_file != nullptr; }
  bool Replaying() const { return m_replay; }

  // On the CPU thread, after the host's own reading: recording logs it,
  // playback replaces it.
  void GcPad(int port, uint64_t field, bool* plugged, GcPadInput* input);
  void Wiimote(int slot, uint64_t field, ios::WiimoteInput* input);
  // True once playback is past the recorded end.
  bool Ended(uint64_t field) const { return m_replay && field >= m_end_field; }
  // Recording: closes the movie at this field.
  void Finish(uint64_t field);

private:
  struct GcEntry
  {
    uint64_t read, field;
    bool plugged;
    GcPadInput input;
  };
  struct WiiEntry
  {
    uint64_t read, field;
    ios::WiimoteInput input;
  };
  template <typename Entry>
  struct Stream
  {
    std::vector<Entry> entries;  // playback
    size_t cursor = 0;
    uint64_t reads = 0;
    bool written = false;  // recording: a line is out
    Entry last{};
  };

  template <typename Entry>
  const Entry* Advance(Stream<Entry>& stream, uint64_t field, const char* kind, int port);
  void WriteGc(int port, const GcEntry& entry);
  void WriteWii(int slot, const WiiEntry& entry);

  FILE* m_file = nullptr;
  bool m_replay = false;
  bool m_diverged = false;
  uint64_t m_end_field = UINT64_MAX;
  std::array<Stream<GcEntry>, PORTS> m_gc;
  std::array<Stream<WiiEntry>, PORTS> m_wii;
};
}  // namespace dualize::hid
