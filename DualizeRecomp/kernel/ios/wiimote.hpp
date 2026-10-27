#pragma once

// An emulated Wii Remote as the Bluetooth HID device sees it: output reports
// in, input reports out. Buttons, EEPROM, the speaker and the IR camera's
// registers, plus a Classic Controller on the extension port. Motion is a
// remote lying still and the camera sees no sensor bar: NiGHTS plays on the
// Classic Controller's sticks and buttons.

#include "kernel/ios/ios.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace dualize::ios
{
// Wii Remote buttons (core report bits).
enum : uint16_t
{
  WIIMOTE_LEFT = 0x0001,
  WIIMOTE_RIGHT = 0x0002,
  WIIMOTE_DOWN = 0x0004,
  WIIMOTE_UP = 0x0008,
  WIIMOTE_PLUS = 0x0010,
  WIIMOTE_TWO = 0x0100,
  WIIMOTE_ONE = 0x0200,
  WIIMOTE_B = 0x0400,
  WIIMOTE_A = 0x0800,
  WIIMOTE_MINUS = 0x1000,
  WIIMOTE_HOME = 0x8000,
};

// Classic Controller buttons (pressed = 1; the wire format inverts them).
enum : uint16_t
{
  CLASSIC_RIGHT = 0x0080,
  CLASSIC_DOWN = 0x0040,
  CLASSIC_L = 0x0020,
  CLASSIC_MINUS = 0x0010,
  CLASSIC_HOME = 0x0008,
  CLASSIC_PLUS = 0x0004,
  CLASSIC_R = 0x0002,
  CLASSIC_ZL = 0x8000,
  CLASSIC_B = 0x4000,
  CLASSIC_Y = 0x2000,
  CLASSIC_A = 0x1000,
  CLASSIC_X = 0x0800,
  CLASSIC_ZR = 0x0400,
  CLASSIC_LEFT = 0x0200,
  CLASSIC_UP = 0x0100,
};

// One remote's controls, in host units.
struct WiimoteInput
{
  uint16_t buttons = 0;  // WIIMOTE_*
  bool classic = false;  // a Classic Controller is attached
  uint16_t classic_buttons = 0;  // CLASSIC_*
  // Sticks in [-1, 1], up and right positive; triggers in [0, 1].
  float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
  float left_trigger = 0, right_trigger = 0;
};

// The host side of the remotes.
struct WiimoteHost
{
  void* user = nullptr;
  uint32_t slots = 0;  // bit i: a remote is on slot i
  void (*read)(void* user, int index, WiimoteInput* input) = nullptr;
  void (*rumble)(void* user, int index, bool on) = nullptr;
  // Mono samples at rate Hz with per-side volumes 0..255; null mutes.
  void (*speaker)(void* user, int index, const int16_t* samples, uint32_t count, uint32_t rate,
                  uint8_t left_volume, uint8_t right_volume) = nullptr;
};

// Where a remote's input reports go (the HID interrupt channel).
class WiimoteLink
{
public:
  virtual void SendInputReport(const uint8_t* report, uint32_t size) = 0;

protected:
  ~WiimoteLink() = default;
};

class Wiimote
{
public:
  static constexpr int UPDATE_FREQ = 200;  // input reports per second
  static constexpr uint32_t EEPROM_SIZE = 0x1700;

  Wiimote(int index, const WiimoteHost& host, std::filesystem::path eeprom_dir, WiimoteLink& link);

  // Linked or unlinked: back to power-on state.
  void Reset();
  // A HID output report (report id first).
  void OutputReport(const uint8_t* data, uint32_t size);
  // Reads the host's controls into the state the next Update reports.
  void PrepareInput();
  void Update();
  // Buttons that wake an unlinked remote.
  bool AnyButtonPressed() const;
  void DoState(StateIO& io);

private:
  enum class Extension : uint8_t
  {
    None,
    Classic,
  };

  struct ReadRequest
  {
    uint8_t space = 0;
    uint8_t slave = 0;
    uint16_t address = 0;
    uint16_t size = 0;
  };

  struct EncryptionKey
  {
    std::array<uint8_t, 8> ft{}, sb{};
  };

  void SetRumble(bool on);
  void SendAck(uint8_t report, uint8_t error);
  void RequestStatus();
  void WriteData(const uint8_t* data);
  void ReadData(const uint8_t* data);
  bool ProcessReadRequest();
  void SpeakerData(const uint8_t* data, uint32_t length);
  void SendDataReport();
  void SwapExtension(Extension desired);
  void ResetExtension();
  void UpdateClassicData();

  // The I2C bus: speaker 0x51, camera 0x58, extension 0x52. Byte counts.
  int BusRead(uint8_t slave, uint8_t address, int count, uint8_t* out);
  int BusWrite(uint8_t slave, uint8_t address, int count, const uint8_t* in);

  const int m_index;
  const WiimoteHost& m_host;
  const std::filesystem::path m_eeprom_path, m_mii_path;
  WiimoteLink& m_link;

  WiimoteInput m_input;

  uint8_t m_reporting_mode = 0x30;
  bool m_reporting_continuous = false;
  bool m_speaker_mute = false;
  // Status report payload: buttons (2), flags, 2 padding, battery.
  uint16_t m_buttons = 0;
  uint8_t m_status_flags = 0;  // bit0 battery low, 1 extension, 2 speaker, 3 IR, 4-7 LEDs
  uint8_t m_battery = 0;
  std::array<uint8_t, EEPROM_SIZE> m_eeprom{};
  bool m_eeprom_dirty = false;
  ReadRequest m_read_request;

  std::array<uint8_t, 0x100> m_speaker_reg{};
  int32_t m_adpcm_predictor = 0, m_adpcm_step = 127;

  std::array<uint8_t, 0x100> m_camera_reg{};
  bool m_camera_enabled = false;

  Extension m_extension = Extension::None;
  std::array<uint8_t, 0x100> m_ext_reg{};
  EncryptionKey m_ext_key;
  bool m_ext_key_dirty = true;
};
}  // namespace dualize::ios
