#pragma once

// Gamepads: SDL3 gamepads read on their own thread (1 kHz) into per-port
// snapshots, so the emulation thread only copies the latest state and never
// makes a driver call. Ports follow connection order; a newly connected pad
// takes the first free port. Buttons are positional (South is the bottom face
// button on every layout).

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace dualize::hid
{
enum PadButton : uint32_t
{
  PAD_SOUTH = 1u << 0,
  PAD_EAST = 1u << 1,
  PAD_WEST = 1u << 2,
  PAD_NORTH = 1u << 3,
  PAD_BACK = 1u << 4,
  PAD_GUIDE = 1u << 5,
  PAD_START = 1u << 6,
  PAD_LEFT_STICK = 1u << 7,
  PAD_RIGHT_STICK = 1u << 8,
  PAD_LEFT_SHOULDER = 1u << 9,
  PAD_RIGHT_SHOULDER = 1u << 10,
  PAD_DPAD_UP = 1u << 11,
  PAD_DPAD_DOWN = 1u << 12,
  PAD_DPAD_LEFT = 1u << 13,
  PAD_DPAD_RIGHT = 1u << 14,
};

struct PadState
{
  uint32_t buttons = 0;  // PadButton bits
  // Sticks in [-1, 1] after the dead zone, up and right positive.
  float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
  // Triggers in [0, 1].
  float left_trigger = 0, right_trigger = 0;
};

class Gamepads
{
public:
  static constexpr int MAX_PORTS = 4;

  Gamepads() = default;
  ~Gamepads();
  Gamepads(const Gamepads&) = delete;
  Gamepads& operator=(const Gamepads&) = delete;

  bool Start();
  void Stop();
  // False when no pad is on the port (state is then neutral).
  bool Read(int port, PadState* state) const;
  // Strong/weak motor strength in [0, 1]; applied by the pad thread.
  void SetRumble(int port, float strength);

private:
  struct Slot
  {
    // Seqlock: odd while the pad thread writes.
    std::atomic<uint32_t> sequence{0};
    std::atomic<bool> connected{false};
    PadState state;
    std::atomic<uint32_t> rumble{0};  // 0..65535
  };

  void Run();

  std::array<Slot, MAX_PORTS> m_slots;
  std::atomic<bool> m_running{false};
  std::thread m_thread;
};
}  // namespace dualize::hid
