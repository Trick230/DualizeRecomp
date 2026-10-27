#include "hid/gamepads.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dualize::hid
{
namespace
{
// Radial dead zone and the point treated as full deflection, as fractions of
// the raw range; the span in between is rescaled to [0, 1].
constexpr float STICK_DEAD_ZONE = 0.10f;
constexpr float STICK_SATURATION = 0.95f;
constexpr float TRIGGER_DEAD_ZONE = 0.04f;
// Device enumeration (hotplug) runs every this many polls.
constexpr int ENUMERATE_EVERY = 250;

float Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
  return std::max(SDL_GetGamepadAxis(pad, axis) / 32767.0f, -1.0f);
}

void Stick(float x, float y, float* out_x, float* out_y)
{
  const float magnitude = std::sqrt(x * x + y * y);
  if (magnitude <= STICK_DEAD_ZONE)
  {
    *out_x = *out_y = 0;
    return;
  }
  const float scaled =
      std::min((magnitude - STICK_DEAD_ZONE) / (STICK_SATURATION - STICK_DEAD_ZONE), 1.0f);
  *out_x = std::clamp(x / magnitude * scaled, -1.0f, 1.0f);
  *out_y = std::clamp(y / magnitude * scaled, -1.0f, 1.0f);
}

float Trigger(float value)
{
  return value <= TRIGGER_DEAD_ZONE ? 0.0f :
                                      std::min((value - TRIGGER_DEAD_ZONE) / (1 - TRIGGER_DEAD_ZONE),
                                               1.0f);
}

PadState ReadPad(SDL_Gamepad* pad)
{
  struct Mapping
  {
    SDL_GamepadButton button;
    uint32_t bit;
  };
  static constexpr Mapping mappings[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, PAD_SOUTH},
      {SDL_GAMEPAD_BUTTON_EAST, PAD_EAST},
      {SDL_GAMEPAD_BUTTON_WEST, PAD_WEST},
      {SDL_GAMEPAD_BUTTON_NORTH, PAD_NORTH},
      {SDL_GAMEPAD_BUTTON_BACK, PAD_BACK},
      {SDL_GAMEPAD_BUTTON_GUIDE, PAD_GUIDE},
      {SDL_GAMEPAD_BUTTON_START, PAD_START},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_LEFT_STICK},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_RIGHT_STICK},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_DPAD_RIGHT},
  };
  PadState state;
  for (const Mapping& m : mappings)
  {
    if (SDL_GetGamepadButton(pad, m.button))
      state.buttons |= m.bit;
  }
  // SDL's Y axes grow downwards.
  Stick(Axis(pad, SDL_GAMEPAD_AXIS_LEFTX), -Axis(pad, SDL_GAMEPAD_AXIS_LEFTY), &state.left_x,
        &state.left_y);
  Stick(Axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -Axis(pad, SDL_GAMEPAD_AXIS_RIGHTY), &state.right_x,
        &state.right_y);
  state.left_trigger = Trigger(Axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
  state.right_trigger = Trigger(Axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
  return state;
}
}  // namespace

Gamepads::~Gamepads()
{
  Stop();
}

bool Gamepads::Start()
{
  if (m_running)
    return true;
  // Initialised here, not on the pad thread: SDL subsystems must not
  // initialise concurrently (audio starts on another thread at boot).
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
  {
    std::fprintf(stderr, "input: SDL gamepads unavailable: %s\n", SDL_GetError());
    return false;
  }
  m_running = true;
  m_thread = std::thread([this] { Run(); });
  return true;
}

void Gamepads::Stop()
{
  if (!m_running)
    return;
  m_running = false;
  if (m_thread.joinable())
    m_thread.join();
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool Gamepads::Read(int port, PadState* state) const
{
  const Slot& slot = m_slots[port];
  if (!slot.connected.load(std::memory_order_acquire))
  {
    *state = {};
    return false;
  }
  uint32_t before;
  do
  {
    before = slot.sequence.load(std::memory_order_acquire);
    std::memcpy(state, &slot.state, sizeof(PadState));
    std::atomic_thread_fence(std::memory_order_acquire);
  } while ((before & 1) || before != slot.sequence.load(std::memory_order_relaxed));
  return true;
}

void Gamepads::SetRumble(int port, float strength)
{
  m_slots[port].rumble.store(static_cast<uint32_t>(std::clamp(strength, 0.0f, 1.0f) * 65535),
                             std::memory_order_relaxed);
}

void Gamepads::Run()
{
  std::array<SDL_Gamepad*, MAX_PORTS> pads{};
  std::array<uint32_t, MAX_PORTS> applied_rumble{};
  int until_enumerate = 0;

  while (m_running.load(std::memory_order_relaxed))
  {
    SDL_UpdateGamepads();

    if (until_enumerate-- <= 0)
    {
      until_enumerate = ENUMERATE_EVERY;
      // Drop pads that went away, then give new ones the free ports.
      for (int port = 0; port < MAX_PORTS; ++port)
      {
        if (pads[port] && !SDL_GamepadConnected(pads[port]))
        {
          SDL_CloseGamepad(pads[port]);
          pads[port] = nullptr;
          m_slots[port].connected.store(false, std::memory_order_release);
        }
      }
      int count = 0;
      if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
      {
        for (int i = 0; i < count; ++i)
        {
          if (std::any_of(pads.begin(), pads.end(), [&](SDL_Gamepad* pad) {
                return pad && SDL_GetGamepadID(pad) == ids[i];
              }))
            continue;
          const auto free = std::find(pads.begin(), pads.end(), nullptr);
          if (free == pads.end())
            break;
          if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]))
          {
            *free = pad;
            applied_rumble[free - pads.begin()] = 0;
            std::fprintf(stderr, "input: port %d: %s\n", int(free - pads.begin()) + 1,
                         SDL_GetGamepadName(pad));
          }
        }
        SDL_free(ids);
      }
    }

    for (int port = 0; port < MAX_PORTS; ++port)
    {
      SDL_Gamepad* pad = pads[port];
      if (!pad)
        continue;
      Slot& slot = m_slots[port];
      const PadState state = ReadPad(pad);
      const uint32_t sequence = slot.sequence.load(std::memory_order_relaxed);
      slot.sequence.store(sequence + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      std::memcpy(&slot.state, &state, sizeof(PadState));
      slot.sequence.store(sequence + 2, std::memory_order_release);
      slot.connected.store(true, std::memory_order_release);

      const uint32_t rumble = slot.rumble.load(std::memory_order_relaxed);
      if (rumble != applied_rumble[port])
      {
        // Re-armed below while it stays on; a long duration would outlive a crash.
        SDL_RumbleGamepad(pad, uint16_t(rumble), uint16_t(rumble), rumble ? 250 : 0);
        applied_rumble[port] = rumble;
      }
    }
    // Keep a running motor alive past SDL's duration.
    if (until_enumerate % 100 == 0)
    {
      for (int port = 0; port < MAX_PORTS; ++port)
      {
        if (pads[port] && applied_rumble[port])
          SDL_RumbleGamepad(pads[port], uint16_t(applied_rumble[port]),
                            uint16_t(applied_rumble[port]), 250);
      }
    }
    SDL_DelayPrecise(1'000'000);  // 1 ms
  }

  for (int port = 0; port < MAX_PORTS; ++port)
  {
    if (pads[port])
    {
      SDL_RumbleGamepad(pads[port], 0, 0, 0);
      SDL_CloseGamepad(pads[port]);
    }
    m_slots[port].connected.store(false, std::memory_order_release);
  }
}
}  // namespace dualize::hid
