#include "kernel/ios/wiimote.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <string>

namespace dualize::ios
{
namespace
{
using SBox = std::array<uint8_t, 256>;
#include "wiimote_sboxes.inc"

enum : uint8_t
{
  REPORT_STATUS = 0x20,
  REPORT_READ_REPLY = 0x21,
  REPORT_ACK = 0x22,
  REPORT_DISABLED = 0x00,
  REPORT_CORE = 0x30,
  REPORT_INTERLEAVE1 = 0x3e,
  REPORT_INTERLEAVE2 = 0x3f,
};

enum : uint8_t
{
  OUT_RUMBLE = 0x10,
  OUT_LED = 0x11,
  OUT_REPORT_MODE = 0x12,
  OUT_IR_ENABLE = 0x13,
  OUT_SPEAKER_ENABLE = 0x14,
  OUT_REQUEST_STATUS = 0x15,
  OUT_WRITE_DATA = 0x16,
  OUT_READ_DATA = 0x17,
  OUT_SPEAKER_DATA = 0x18,
  OUT_SPEAKER_MUTE = 0x19,
  OUT_IR_ENABLE2 = 0x1a,
};

enum : uint8_t
{
  ERROR_SUCCESS = 0,
  ERROR_BUSY = 4,
  ERROR_INVALID_SPACE = 6,
  ERROR_NACK = 7,
  ERROR_INVALID_ADDRESS = 8,
};

enum : uint8_t
{
  SPACE_EEPROM = 0,
  SPACE_I2C = 1,
  SPACE_I2C_ALT = 2,
};

enum : uint8_t
{
  FLAG_BATTERY_LOW = 0x01,
  FLAG_EXTENSION = 0x02,
  FLAG_SPEAKER = 0x04,
  FLAG_IR = 0x08,
};

constexpr uint8_t EEPROM_I2C = 0x50;
constexpr uint8_t SPEAKER_I2C = 0x51;
constexpr uint8_t EXTENSION_I2C = 0x52;
constexpr uint8_t CAMERA_I2C = 0x58;
// Core button bits; the rest of the two bytes carry accelerometer LSBs.
constexpr uint16_t BUTTON_MASK = 0x9F1F;

// Camera register: the 36 bytes of object data the reports copy.
constexpr uint8_t CAMERA_DATA = 0x37;
constexpr uint8_t CAMERA_DATA_SIZE = 36;

// Extension register.
constexpr uint8_t EXT_CALIBRATION = 0x20;
constexpr uint8_t EXT_KEY = 0x40;
constexpr uint8_t EXT_ENCRYPTION = 0xF0;
constexpr uint8_t EXT_IDENTIFIER = 0xFA;
constexpr uint8_t ENCRYPTION_ENABLED = 0xAA;
constexpr std::array<uint8_t, 6> CLASSIC_ID = {0x00, 0x00, 0xA4, 0x20, 0x01, 0x01};

// Speaker register.
constexpr uint8_t SPEAKER_FORMAT = 2, SPEAKER_RATE = 3, SPEAKER_VOLUME = 5;
constexpr uint8_t FORMAT_ADPCM = 0x00, FORMAT_PCM = 0x40;

// A remote lying flat at rest: 0 g on x and y, 1 g on z (10-bit values).
constexpr uint8_t ACCEL_ZERO_G = 0x80, ACCEL_ONE_G = 0x9A;
constexpr uint16_t ACCEL_X = ACCEL_ZERO_G << 2, ACCEL_Y = ACCEL_ZERO_G << 2, ACCEL_Z = ACCEL_ONE_G << 2;

// Sensor bar calibration points the EEPROM ships with.
constexpr uint16_t IR_LOW_X = 0x7F, IR_LOW_Y = 0x5D, IR_HIGH_X = 0x380, IR_HIGH_Y = 0x2A2;

// Classic Controller sticks: 6-bit left, 5-bit right, 5-bit triggers;
// calibration in 8 bits around 0x80 with this gate.
constexpr uint8_t CAL_STICK_CENTER = 0x80;
constexpr uint8_t STICK_GATE_RADIUS = 0x61;
constexpr uint8_t LEFT_STICK_CENTER = CAL_STICK_CENTER >> 2, LEFT_STICK_RANGE = 0xFF >> 2;
constexpr uint8_t RIGHT_STICK_CENTER = CAL_STICK_CENTER >> 3, RIGHT_STICK_RANGE = 0xFF >> 3;
constexpr uint8_t TRIGGER_RANGE = 0x1F;

// Dolphin's battery default (95%) as the status report encodes it.
constexpr uint8_t BATTERY_LEVEL = 100;

constexpr int32_t ADPCM_DIFF[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
constexpr int32_t ADPCM_SCALE[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                                     230, 230, 230, 230, 307, 409, 512, 614};

// Where each data report mode puts its parts, offsets after the report id.
struct ReportLayout
{
  bool core, accel;
  uint8_t ir_offset, ir_size, ir_format_offset;
  uint8_t ext_offset, ext_size;
  uint8_t size;
};

const ReportLayout* LayoutOf(uint8_t mode)
{
  static constexpr ReportLayout layouts[] = {
      {true, false, 0, 0, 0, 0, 0, 2},     // 0x30 core
      {true, true, 0, 0, 0, 0, 0, 5},      // 0x31 core accel
      {true, false, 0, 0, 0, 2, 8, 10},    // 0x32 core ext8
      {true, true, 5, 12, 0, 0, 0, 17},    // 0x33 core accel IR12
      {true, false, 0, 0, 0, 2, 19, 21},   // 0x34 core ext19
      {true, true, 0, 0, 0, 5, 16, 21},    // 0x35 core accel ext16
      {true, false, 2, 10, 0, 12, 9, 21},  // 0x36 core IR10 ext9
      {true, true, 5, 10, 0, 15, 6, 21},   // 0x37 core accel IR10 ext6
  };
  static constexpr ReportLayout ext21 = {false, false, 0, 0, 0, 0, 21, 21};
  static constexpr ReportLayout interleave1 = {true, true, 3, 18, 0, 0, 0, 21};
  static constexpr ReportLayout interleave2 = {true, true, 3, 18, 18, 0, 0, 21};
  if (mode >= 0x30 && mode <= 0x37)
    return &layouts[mode - 0x30];
  switch (mode)
  {
  case 0x3d:
    return &ext21;
  case REPORT_INTERLEAVE1:
    return &interleave1;
  case REPORT_INTERLEAVE2:
    return &interleave2;
  default:
    return nullptr;
  }
}

// Sum of the bytes plus 0x55; each further checksum byte adds 0x55 again.
template <size_t N>
void SetChecksum(std::array<uint8_t, N>& data, size_t checksum_bytes)
{
  uint8_t sum = std::accumulate(data.begin(), data.end() - checksum_bytes, uint8_t{0x55});
  for (size_t i = N - checksum_bytes; i < N; ++i, sum += 0x55)
    data[i] = sum;
}

// [-1, 1] onto an integer range, rounding (ControllerEmu::MapFloat).
uint8_t MapFloat(float value, uint8_t zero, uint8_t neg, uint8_t pos)
{
  if (value > 0)
    return static_cast<uint8_t>(std::llround((pos - zero) * value + zero));
  return static_cast<uint8_t>(std::llround((zero - neg) * value + zero));
}

int16_t Clip16(int32_t a)
{
  if ((a + 32768) & ~65535)
    return static_cast<int16_t>((a >> 31) ^ 32767);
  return static_cast<int16_t>(a);
}

// The first-party key schedule: the Wii derives the key from 10 random bytes
// and one of 7 table indices it doesn't send, so try each.
std::array<uint8_t, 6> KeyData(const std::array<uint8_t, 10>& rand, int idx)
{
  static constexpr uint8_t ans_tbl[7][6] = {
      {0xa8, 0x77, 0xa6, 0xe0, 0xf7, 0x43}, {0x5a, 0x35, 0x85, 0xe2, 0x72, 0x97},
      {0x8f, 0xb7, 0x1a, 0x62, 0x87, 0x38}, {0x0d, 0x67, 0xc7, 0xbe, 0x4f, 0x3e},
      {0x20, 0x76, 0x37, 0x8f, 0x68, 0xb7}, {0xa9, 0x26, 0x3f, 0x2b, 0x10, 0xe3},
      {0x30, 0x7e, 0x90, 0x0e, 0x85, 0x0a},
  };
  const uint8_t* ans = ans_tbl[idx];
  std::array<uint8_t, 10> t0;
  for (size_t i = 0; i < t0.size(); ++i)
    t0[i] = keygen_sbox_1st_party[rand[i]];
  const auto f = [](uint8_t a, uint8_t rot, uint8_t sub, uint8_t x) {
    return static_cast<uint8_t>(static_cast<uint8_t>(std::rotr(a, rot) - sub) ^ x);
  };
  return {
      f(ans[0] ^ t0[5], t0[2], t0[9], t0[4]), f(ans[1] ^ t0[1], t0[0], t0[5], t0[7]),
      f(ans[2] ^ t0[6], t0[8], t0[2], t0[0]), f(ans[3] ^ t0[4], t0[7], t0[3], t0[2]),
      f(ans[4] ^ t0[1], t0[6], t0[3], t0[4]), f(ans[5] ^ t0[7], t0[8], t0[5], t0[9]),
  };
}
}  // namespace

Wiimote::Wiimote(int index, const WiimoteHost& host, std::filesystem::path eeprom_dir,
                 WiimoteLink& link)
    : m_index(index), m_host(host),
      m_eeprom_path(eeprom_dir / ("Wiimote" + std::to_string(index + 1) + ".bin")),
      m_mii_path(eeprom_dir / "mii.bin"), m_link(link)
{
  PrepareInput();
  Reset();
}

void Wiimote::PrepareInput()
{
  m_input = {};
  if (m_host.read)
    m_host.read(m_host.user, m_index, &m_input);
}

bool Wiimote::AnyButtonPressed() const
{
  return (m_input.buttons & BUTTON_MASK) || (m_input.classic && m_input.classic_buttons);
}

void Wiimote::SetRumble(bool on)
{
  if (m_host.rumble)
    m_host.rumble(m_host.user, m_index, on);
}

void Wiimote::Reset()
{
  SetRumble(false);
  m_reporting_mode = REPORT_CORE;
  m_reporting_continuous = false;
  m_speaker_mute = false;

  // The EEPROM persists across links in the user's NAND directory, like
  // Dolphin's Wiimote<n>.bin.
  if (m_eeprom_dirty)
  {
    std::ofstream file(m_eeprom_path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(m_eeprom.data()), m_eeprom.size());
    m_eeprom_dirty = false;
  }
  m_eeprom = {};
  if (std::ifstream file{m_eeprom_path, std::ios::binary})
  {
    file.read(reinterpret_cast<char*>(m_eeprom.data()), m_eeprom.size());
  }
  else
  {
    std::array<uint8_t, 11> ir = {
        IR_LOW_X & 0xFF,
        IR_LOW_Y & 0xFF,
        ((IR_LOW_Y & 0x300) >> 2) | ((IR_LOW_X & 0x300) >> 4) | ((IR_LOW_Y & 0x300) >> 6) |
            ((IR_HIGH_X & 0x300) >> 8),
        IR_HIGH_X & 0xFF,
        IR_LOW_Y & 0xFF,
        IR_HIGH_X & 0xFF,
        IR_HIGH_Y & 0xFF,
        ((IR_HIGH_Y & 0x300) >> 2) | ((IR_HIGH_X & 0x300) >> 4) | ((IR_HIGH_Y & 0x300) >> 6) |
            ((IR_LOW_X & 0x300) >> 8),
        IR_LOW_X & 0xFF,
        IR_HIGH_Y & 0xFF,
        0x00,
    };
    SetChecksum(ir, 1);
    std::array<uint8_t, 10> accel = {
        ACCEL_ZERO_G, ACCEL_ZERO_G, ACCEL_ZERO_G, 0, ACCEL_ONE_G, ACCEL_ONE_G, ACCEL_ONE_G, 0, 0, 0,
    };
    SetChecksum(accel, 1);
    std::copy(ir.begin(), ir.end(), m_eeprom.begin() + 0x00);
    std::copy(ir.begin(), ir.end(), m_eeprom.begin() + 0x0B);
    std::copy(accel.begin(), accel.end(), m_eeprom.begin() + 0x16);
    std::copy(accel.begin(), accel.end(), m_eeprom.begin() + 0x20);
    static constexpr uint8_t data_16d0[24] = {0x00, 0x00, 0x00, 0xFF, 0x11, 0xEE, 0x00, 0x00,
                                              0x33, 0xCC, 0x44, 0xBB, 0x00, 0x00, 0x66, 0x99,
                                              0x77, 0x88, 0x00, 0x00, 0x2B, 0x01, 0xE8, 0x13};
    std::copy(std::begin(data_16d0), std::end(data_16d0), m_eeprom.begin() + 0x16D0);
    // Two copies of a Mii the user exported, if any.
    constexpr size_t MII = 0x0FCA, MII_SIZE = 0x2F0;
    if (std::ifstream mii{m_mii_path, std::ios::binary})
    {
      mii.read(reinterpret_cast<char*>(m_eeprom.data() + MII), MII_SIZE);
      std::copy_n(m_eeprom.begin() + MII, MII_SIZE, m_eeprom.begin() + MII + MII_SIZE);
    }
  }
  m_read_request = {};

  m_extension = Extension::None;
  SwapExtension(m_input.classic ? Extension::Classic : Extension::None);

  m_speaker_reg = {};
  m_adpcm_predictor = 0;
  m_adpcm_step = 127;
  m_camera_reg = {};
  m_camera_enabled = false;

  m_buttons = 0;
  m_status_flags = 0;
  m_battery = 0;
}

void Wiimote::SwapExtension(Extension desired)
{
  // A change detaches first and attaches the new one an update later, the
  // way a real port sees an unplug then a plug.
  if (m_extension == desired)
    return;
  m_extension = m_extension != Extension::None ? Extension::None : desired;
  ResetExtension();
}

void Wiimote::ResetExtension()
{
  if (m_extension != Extension::Classic)
    return;
  m_ext_reg = {};
  m_ext_key = {};
  m_ext_key_dirty = true;
  std::copy(CLASSIC_ID.begin(), CLASSIC_ID.end(), m_ext_reg.begin() + EXT_IDENTIFIER);
  constexpr uint8_t MAX = CAL_STICK_CENTER + STICK_GATE_RADIUS;
  constexpr uint8_t MIN = CAL_STICK_CENTER - STICK_GATE_RADIUS;
  std::array<uint8_t, 16> calibration = {
      MAX, MIN, CAL_STICK_CENTER, MAX, MIN, CAL_STICK_CENTER, MAX, MIN, CAL_STICK_CENTER,
      MAX, MIN, CAL_STICK_CENTER, 0,   0,   0x00,             0x00,
  };
  SetChecksum(calibration, 2);
  std::copy(calibration.begin(), calibration.end(), m_ext_reg.begin() + EXT_CALIBRATION);
}

void Wiimote::UpdateClassicData()
{
  const WiimoteInput& in = m_input;
  const uint8_t lx = MapFloat(in.left_x, LEFT_STICK_CENTER, 0, LEFT_STICK_RANGE);
  const uint8_t ly = MapFloat(in.left_y, LEFT_STICK_CENTER, 0, LEFT_STICK_RANGE);
  const uint8_t rx = MapFloat(in.right_x, RIGHT_STICK_CENTER, 0, RIGHT_STICK_RANGE);
  const uint8_t ry = MapFloat(in.right_y, RIGHT_STICK_CENTER, 0, RIGHT_STICK_RANGE);
  const uint8_t lt = MapFloat(in.left_trigger, 0, 0, TRIGGER_RANGE);
  const uint8_t rt = MapFloat(in.right_trigger, 0, 0, TRIGGER_RANGE);
  const uint16_t buttons = static_cast<uint16_t>(~in.classic_buttons);
  uint8_t* d = m_ext_reg.data();
  d[0] = static_cast<uint8_t>((lx & 0x3F) | ((rx >> 3) & 3) << 6);
  d[1] = static_cast<uint8_t>((ly & 0x3F) | ((rx >> 1) & 3) << 6);
  d[2] = static_cast<uint8_t>((ry & 0x1F) | ((lt >> 3) & 3) << 5 | (rx & 1) << 7);
  d[3] = static_cast<uint8_t>((rt & 0x1F) | (lt & 7) << 5);
  d[4] = static_cast<uint8_t>(buttons);
  d[5] = static_cast<uint8_t>(buttons >> 8);
}

int Wiimote::BusRead(uint8_t slave, uint8_t address, int count, uint8_t* out)
{
  std::array<uint8_t, 0x100>* reg = nullptr;
  if (slave == SPEAKER_I2C)
    reg = &m_speaker_reg;
  else if (slave == CAMERA_I2C && m_camera_enabled)
    reg = &m_camera_reg;
  else if (slave == EXTENSION_I2C && m_extension == Extension::Classic)
    reg = &m_ext_reg;
  if (!reg)
    return 0;
  count = std::min(count, 0x100 - address);
  std::copy_n(reg->data() + address, count, out);
  if (reg == &m_ext_reg && m_ext_reg[EXT_ENCRYPTION] == ENCRYPTION_ENABLED)
  {
    if (m_ext_key_dirty)
    {
      // Key bytes 0x40-0x49 are the random half, 0x4A-0x4F the key, both
      // stored reversed.
      std::array<uint8_t, 10> rand;
      std::array<uint8_t, 6> key;
      for (size_t i = 0; i < rand.size(); ++i)
        rand[rand.size() - 1 - i] = m_ext_reg[EXT_KEY + i];
      for (size_t i = 0; i < key.size(); ++i)
        key[key.size() - 1 - i] = m_ext_reg[EXT_KEY + rand.size() + i];
      const SBox* a = &fallback_sbox_1st_party_a;
      const SBox* b = &fallback_sbox_1st_party_b;
      // A key no index produces (homebrew zeroes it): real extensions then
      // use other tables, known only by their XOR difference.
      uint8_t fallback_xor = 0x17;
      for (int idx = 0; idx < 7; ++idx)
      {
        if (KeyData(rand, idx) == key)
        {
          a = &sboxes_1st_party[idx];
          b = &sboxes_1st_party[idx + 1];
          fallback_xor = 0;
          break;
        }
      }
      // Each table byte mixes one key or random byte through each sbox.
      std::array<uint8_t, 16> v;
      std::copy(key.begin(), key.end(), v.begin());
      std::copy(rand.begin(), rand.end(), v.begin() + 6);
      constexpr uint8_t K = 0, R = 6;  // offsets of key[] and rand[] in v
      static constexpr uint8_t ft_a[8] = {K + 4, K + 2, K + 5, K + 0, K + 1, K + 3, R + 0, R + 1};
      static constexpr uint8_t ft_b[8] = {R + 3, R + 5, R + 7, R + 2, R + 4, R + 9, R + 6, R + 8};
      static constexpr uint8_t sb_a[8] = {K + 0, K + 5, K + 3, K + 2, K + 4, K + 1, R + 3, R + 2};
      static constexpr uint8_t sb_b[8] = {R + 1, R + 4, R + 0, R + 9, R + 7, R + 8, R + 5, R + 6};
      EncryptionKey& k = m_ext_key;
      for (size_t i = 0; i < 8; ++i)
      {
        k.ft[i] = static_cast<uint8_t>((*a)[v[ft_a[i]]] ^ (*b)[v[ft_b[i]]] ^ fallback_xor);
        k.sb[i] = static_cast<uint8_t>((*a)[v[sb_a[i]]] ^ (*b)[v[sb_b[i]]] ^ fallback_xor);
      }
      m_ext_key_dirty = false;
    }
    for (int i = 0; i < count; ++i)
    {
      const uint32_t a = (address + i) % 8;
      out[i] = static_cast<uint8_t>((out[i] - m_ext_key.ft[a]) ^ m_ext_key.sb[a]);
    }
  }
  return count;
}

int Wiimote::BusWrite(uint8_t slave, uint8_t address, int count, const uint8_t* in)
{
  if (slave == SPEAKER_I2C && address == 0)
  {
    SpeakerData(in, static_cast<uint32_t>(count));
    return count;
  }
  std::array<uint8_t, 0x100>* reg = nullptr;
  if (slave == SPEAKER_I2C)
    reg = &m_speaker_reg;
  else if (slave == CAMERA_I2C && m_camera_enabled)
    reg = &m_camera_reg;
  else if (slave == EXTENSION_I2C && m_extension == Extension::Classic)
    reg = &m_ext_reg;
  if (!reg)
    return 0;
  count = std::min(count, 0x100 - address);
  std::copy_n(in, count, reg->data() + address);
  if (reg == &m_ext_reg && address + count > EXT_KEY && address < EXT_KEY + 0x10)
    m_ext_key_dirty = true;
  return count;
}

void Wiimote::OutputReport(const uint8_t* data, uint32_t size)
{
  if (size < 2)
    return;
  const uint8_t id = data[0];
  const uint8_t* p = data + 1;
  const uint32_t payload = size - 1;
  // Every output report carries the rumble bit.
  SetRumble(p[0] & 1);
  const bool ack = p[0] & 2;
  const bool enable = p[0] & 4;
  switch (id)
  {
  case OUT_RUMBLE:
    break;
  case OUT_LED:
    m_status_flags = static_cast<uint8_t>((m_status_flags & 0x0F) | (p[0] & 0xF0));
    if (ack)
      SendAck(id, ERROR_SUCCESS);
    break;
  case OUT_REPORT_MODE:
    if (payload < 2 || !LayoutOf(p[1]))
      break;
    m_reporting_continuous = enable;
    m_reporting_mode = p[1];
    if (ack)
      SendAck(id, ERROR_SUCCESS);
    break;
  case OUT_IR_ENABLE:
    m_status_flags = static_cast<uint8_t>((m_status_flags & ~FLAG_IR) | (enable ? FLAG_IR : 0));
    m_camera_enabled = enable;
    if (ack)
      SendAck(id, ERROR_SUCCESS);
    break;
  case OUT_SPEAKER_ENABLE:
    m_status_flags =
        static_cast<uint8_t>((m_status_flags & ~FLAG_SPEAKER) | (enable ? FLAG_SPEAKER : 0));
    if (ack)
      SendAck(id, ERROR_SUCCESS);
    break;
  case OUT_REQUEST_STATUS:
    RequestStatus();
    break;
  case OUT_WRITE_DATA:
    if (payload >= 21)
      WriteData(p);
    break;
  case OUT_READ_DATA:
    if (payload >= 6)
      ReadData(p);
    break;
  case OUT_SPEAKER_DATA:
    if (payload >= 21 && !m_speaker_mute && (p[0] >> 3) <= 20)
      BusWrite(SPEAKER_I2C, 0, p[0] >> 3, p + 1);
    break;
  case OUT_SPEAKER_MUTE:
    m_speaker_mute = enable;
    if (ack)
      SendAck(id, ERROR_SUCCESS);
    break;
  case OUT_IR_ENABLE2:
    if (ack)
      SendAck(id, ERROR_SUCCESS);
    break;
  default:
    std::fprintf(stderr, "[native-wiimote] unknown output report %02x\n", id);
    break;
  }
}

void Wiimote::SendAck(uint8_t report, uint8_t error)
{
  const uint8_t r[] = {REPORT_ACK, static_cast<uint8_t>(m_buttons),
                       static_cast<uint8_t>(m_buttons >> 8), report, error};
  m_link.SendInputReport(r, sizeof(r));
}

void Wiimote::RequestStatus()
{
  const bool extension = m_extension != Extension::None;
  m_battery = BATTERY_LEVEL;
  m_status_flags = static_cast<uint8_t>((m_status_flags & ~(FLAG_EXTENSION | FLAG_BATTERY_LOW)) |
                                        (extension ? FLAG_EXTENSION : 0) |
                                        (m_battery < 0x20 ? FLAG_BATTERY_LOW : 0));
  const uint8_t r[] = {REPORT_STATUS, static_cast<uint8_t>(m_buttons),
                       static_cast<uint8_t>(m_buttons >> 8), m_status_flags, 0, 0, m_battery};
  m_link.SendInputReport(r, sizeof(r));
}

void Wiimote::WriteData(const uint8_t* p)
{
  const uint8_t space = (p[0] >> 2) & 3;
  const uint8_t slave = p[1] >> 1;
  const uint16_t address = static_cast<uint16_t>(p[2] << 8 | p[3]);
  const uint8_t size = p[4];
  const uint8_t* data = p + 5;
  if (size == 0 || size > 16)
    return;
  uint8_t error = ERROR_SUCCESS;
  switch (space)
  {
  case SPACE_EEPROM:
    if (address + size > EEPROM_SIZE)
    {
      error = ERROR_INVALID_ADDRESS;
    }
    else
    {
      std::copy_n(data, size, m_eeprom.data() + address);
      m_eeprom_dirty = true;
    }
    break;
  case SPACE_I2C:
  case SPACE_I2C_ALT:
    if (slave == EEPROM_I2C)
      error = ERROR_INVALID_ADDRESS;
    else if (BusWrite(slave, static_cast<uint8_t>(address), size, data) != size)
      error = ERROR_NACK;
    break;
  default:
    error = ERROR_INVALID_SPACE;
    break;
  }
  SendAck(OUT_WRITE_DATA, error);
}

void Wiimote::ReadData(const uint8_t* p)
{
  if (m_read_request.size)
  {
    SendAck(OUT_READ_DATA, ERROR_BUSY);
    return;
  }
  m_read_request.space = (p[0] >> 2) & 3;
  m_read_request.slave = p[1] >> 1;
  m_read_request.address = static_cast<uint16_t>(p[2] << 8 | p[3]);
  m_read_request.size = static_cast<uint16_t>(p[4] << 8 | p[5]);
  ProcessReadRequest();
}

bool Wiimote::ProcessReadRequest()
{
  // Replies go out 16 bytes per report, one report per update.
  const uint16_t count = std::min<uint16_t>(16, m_read_request.size);
  if (count == 0)
    return false;
  uint8_t r[22] = {REPORT_READ_REPLY, static_cast<uint8_t>(m_buttons),
                   static_cast<uint8_t>(m_buttons >> 8), 0,
                   static_cast<uint8_t>(m_read_request.address >> 8),
                   static_cast<uint8_t>(m_read_request.address)};
  uint8_t* data = r + 6;
  uint8_t error = ERROR_SUCCESS;
  uint8_t size_minus_one = 0;
  switch (m_read_request.space)
  {
  case SPACE_EEPROM:
    if (m_read_request.address + m_read_request.size > EEPROM_SIZE)
    {
      error = ERROR_INVALID_ADDRESS;
    }
    else
    {
      std::copy_n(m_eeprom.data() + m_read_request.address, count, data);
      size_minus_one = static_cast<uint8_t>(count - 1);
    }
    break;
  case SPACE_I2C:
  case SPACE_I2C_ALT:
  {
    if (m_read_request.slave == EEPROM_I2C)
    {
      error = ERROR_INVALID_ADDRESS;
      break;
    }
    const int read = BusRead(m_read_request.slave, static_cast<uint8_t>(m_read_request.address),
                             count, data);
    if (read != count)
    {
      error = ERROR_NACK;
      break;
    }
    size_minus_one = static_cast<uint8_t>(read - 1);
    break;
  }
  default:
    error = ERROR_INVALID_SPACE;
    break;
  }
  if (error != ERROR_SUCCESS)
  {
    m_read_request.size = 0;
    size_minus_one = 0xF;
  }
  else
  {
    m_read_request.address = static_cast<uint16_t>(m_read_request.address + count);
    m_read_request.size = static_cast<uint16_t>(m_read_request.size - count);
  }
  r[3] = static_cast<uint8_t>(size_minus_one << 4 | error);
  m_link.SendInputReport(r, sizeof(r));
  return true;
}

void Wiimote::SpeakerData(const uint8_t* data, uint32_t length)
{
  const uint16_t rate_divisor =
      static_cast<uint16_t>(m_speaker_reg[SPEAKER_RATE] | m_speaker_reg[SPEAKER_RATE + 1] << 8);
  if (!m_host.speaker || rate_divisor == 0 || length == 0)
    return;
  int16_t samples[40];
  uint32_t dividend, count;
  uint8_t volume_divisor;
  switch (m_speaker_reg[SPEAKER_FORMAT])
  {
  case FORMAT_PCM:
    for (uint32_t i = 0; i < length; ++i)
      samples[i] = static_cast<int16_t>(static_cast<int8_t>(data[i]) * 0x100);
    dividend = 12000000;
    volume_divisor = 0xFF;
    count = length;
    break;
  case FORMAT_ADPCM:
    // Yamaha ADPCM, high nibble first.
    for (uint32_t i = 0; i < length * 2; ++i)
    {
      const uint8_t nibble = (i & 1) ? data[i / 2] & 0xF : data[i / 2] >> 4;
      m_adpcm_predictor = Clip16(m_adpcm_predictor + m_adpcm_step * ADPCM_DIFF[nibble] / 8);
      m_adpcm_step = std::clamp((m_adpcm_step * ADPCM_SCALE[nibble]) >> 8, 127, 24576);
      samples[i] = static_cast<int16_t>(m_adpcm_predictor);
    }
    dividend = 6000000;
    volume_divisor = 0x7F;
    count = length * 2;
    break;
  default:
    return;
  }
  const uint8_t volume = m_speaker_reg[SPEAKER_VOLUME];
  volume_divisor = std::max(volume_divisor, volume);
  const uint8_t gain =
      static_cast<uint8_t>(std::min(static_cast<uint32_t>(volume * 256.f / volume_divisor), 255u));
  m_host.speaker(m_host.user, m_index, samples, count, dividend / rate_divisor * 2, gain, gain);
}

void Wiimote::Update()
{
  m_buttons = m_input.buttons & BUTTON_MASK;
  SwapExtension(m_input.classic ? Extension::Classic : Extension::None);
  if (m_extension == Extension::Classic)
    UpdateClassicData();

  // An extension plugged or unplugged: reports stop until the game asks for
  // status and a mode again.
  const bool extension = m_extension != Extension::None;
  if (extension != ((m_status_flags & FLAG_EXTENSION) != 0))
  {
    m_reporting_mode = REPORT_DISABLED;
    RequestStatus();
    return;
  }
  if (ProcessReadRequest())
    return;
  SendDataReport();
}

void Wiimote::SendDataReport()
{
  const ReportLayout* layout = LayoutOf(m_reporting_mode);
  if (!layout)
    return;
  uint8_t r[22] = {m_reporting_mode};
  uint8_t* d = r + 1;
  if (layout->core)
  {
    d[0] = static_cast<uint8_t>(m_buttons);
    d[1] = static_cast<uint8_t>(m_buttons >> 8);
  }
  if (layout->accel)
  {
    // The accelerometer's low bits ride in the button bytes (bits 5-6).
    uint8_t lsb0, lsb1;
    if (m_reporting_mode == REPORT_INTERLEAVE1)
    {
      d[2] = ACCEL_X >> 2;
      lsb0 = (ACCEL_Z >> 6) & 3;
      lsb1 = (ACCEL_Z >> 8) & 3;
    }
    else if (m_reporting_mode == REPORT_INTERLEAVE2)
    {
      d[2] = ACCEL_Y >> 2;
      lsb0 = (ACCEL_Z >> 2) & 3;
      lsb1 = (ACCEL_Z >> 4) & 3;
    }
    else
    {
      d[2] = ACCEL_X >> 2;
      d[3] = ACCEL_Y >> 2;
      d[4] = ACCEL_Z >> 2;
      lsb0 = ACCEL_X & 3;
      lsb1 = (ACCEL_Y >> 1) & 1;
    }
    d[0] = static_cast<uint8_t>(d[0] | lsb0 << 5);
    d[1] = static_cast<uint8_t>(d[1] | lsb1 << 5);
  }
  if (layout->ir_size)
  {
    // No sensor bar in view: every object slot reads empty.
    std::fill_n(m_camera_reg.data() + CAMERA_DATA, CAMERA_DATA_SIZE, uint8_t{0xFF});
    uint8_t* ir = d + layout->ir_offset;
    if (BusRead(CAMERA_I2C, CAMERA_DATA + layout->ir_format_offset, layout->ir_size, ir) !=
        layout->ir_size)
      std::fill_n(ir, layout->ir_size, uint8_t{0xFF});
  }
  if (layout->ext_size)
  {
    uint8_t* ext = d + layout->ext_offset;
    if (BusRead(EXTENSION_I2C, 0, layout->ext_size, ext) != layout->ext_size)
      std::fill_n(ext, layout->ext_size, uint8_t{0xFF});
  }
  m_link.SendInputReport(r, layout->size + 1u);
  if (m_reporting_mode == REPORT_INTERLEAVE1)
    m_reporting_mode = REPORT_INTERLEAVE2;
  else if (m_reporting_mode == REPORT_INTERLEAVE2)
    m_reporting_mode = REPORT_INTERLEAVE1;
}

void Wiimote::DoState(StateIO& io)
{
  io.Do(m_input);
  io.Do(m_reporting_mode);
  io.Do(m_reporting_continuous);
  io.Do(m_speaker_mute);
  io.Do(m_buttons);
  io.Do(m_status_flags);
  io.Do(m_battery);
  io.Do(m_eeprom);
  io.Do(m_eeprom_dirty);
  io.Do(m_read_request);
  io.Do(m_speaker_reg);
  io.Do(m_adpcm_predictor);
  io.Do(m_adpcm_step);
  io.Do(m_camera_reg);
  io.Do(m_camera_enabled);
  io.Do(m_extension);
  io.Do(m_ext_reg);
  if (io.Reading())
    m_ext_key_dirty = true;
}
}  // namespace dualize::ios
