// /dev/usb/oh1/57e/305: the Wii's Bluetooth controller as IOS exposes it over
// USB, with the Wii Remotes paired to it. The game's BT stack sends HCI
// commands and ACL data; this answers as a controller would and runs each
// remote's L2CAP (signalling, SDP, HID control and interrupt channels).
// Follows Dolphin's BTEmu and WiimoteDevice, timings and quirks included.

#include "kernel/ios/devices.hpp"
#include "kernel/ios/wiimote.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>

namespace dualize::ios
{
namespace
{
enum : uint32_t
{
  USBV0_CTRLMSG = 0,
  USBV0_BLKMSG = 1,
  USBV0_INTRMSG = 2,
};

enum : uint8_t
{
  ENDPOINT_HCI_EVENT = 0x81,
  ENDPOINT_ACL_IN = 0x82,
  ENDPOINT_ACL_OUT = 0x02,
};

// HCI command opcodes.
enum : uint16_t
{
  HCI_INQUIRY = 0x0401,
  HCI_INQUIRY_CANCEL = 0x0402,
  HCI_CREATE_CON = 0x0405,
  HCI_DISCONNECT = 0x0406,
  HCI_ACCEPT_CON = 0x0409,
  HCI_LINK_KEY_REP = 0x040B,
  HCI_LINK_KEY_NEG_REP = 0x040C,
  HCI_CHANGE_CON_PACKET_TYPE = 0x040F,
  HCI_AUTH_REQ = 0x0411,
  HCI_REMOTE_NAME_REQ = 0x0419,
  HCI_READ_REMOTE_FEATURES = 0x041B,
  HCI_READ_REMOTE_VER_INFO = 0x041D,
  HCI_READ_CLOCK_OFFSET = 0x041F,
  HCI_SNIFF_MODE = 0x0803,
  HCI_WRITE_LINK_POLICY_SETTINGS = 0x080D,
  HCI_RESET = 0x0C03,
  HCI_SET_EVENT_FILTER = 0x0C05,
  HCI_WRITE_PIN_TYPE = 0x0C0A,
  HCI_READ_STORED_LINK_KEY = 0x0C0D,
  HCI_DELETE_STORED_LINK_KEY = 0x0C12,
  HCI_WRITE_LOCAL_NAME = 0x0C13,
  HCI_WRITE_PAGE_TIMEOUT = 0x0C18,
  HCI_WRITE_SCAN_ENABLE = 0x0C1A,
  HCI_WRITE_UNIT_CLASS = 0x0C24,
  HCI_HOST_BUFFER_SIZE = 0x0C33,
  HCI_WRITE_LINK_SUPERVISION_TIMEOUT = 0x0C37,
  HCI_WRITE_INQUIRY_SCAN_TYPE = 0x0C43,
  HCI_WRITE_INQUIRY_MODE = 0x0C45,
  HCI_WRITE_PAGE_SCAN_TYPE = 0x0C47,
  HCI_READ_LOCAL_VER = 0x1001,
  HCI_READ_LOCAL_FEATURES = 0x1003,
  HCI_READ_BUFFER_SIZE = 0x1005,
  HCI_READ_BDADDR = 0x1009,
  HCI_VENDOR_FC4C = 0xFC4C,
  HCI_VENDOR_FC4F = 0xFC4F,
};

// HCI events.
enum : uint8_t
{
  EVENT_INQUIRY_COMPL = 0x01,
  EVENT_INQUIRY_RESULT = 0x02,
  EVENT_CON_COMPL = 0x03,
  EVENT_CON_REQ = 0x04,
  EVENT_DISCON_COMPL = 0x05,
  EVENT_AUTH_COMPL = 0x06,
  EVENT_REMOTE_NAME_REQ_COMPL = 0x07,
  EVENT_READ_REMOTE_FEATURES_COMPL = 0x0B,
  EVENT_READ_REMOTE_VER_INFO_COMPL = 0x0C,
  EVENT_COMMAND_COMPL = 0x0E,
  EVENT_COMMAND_STATUS = 0x0F,
  EVENT_ROLE_CHANGE = 0x12,
  EVENT_NUM_COMPL_PKTS = 0x13,
  EVENT_MODE_CHANGE = 0x14,
  EVENT_RETURN_LINK_KEYS = 0x15,
  EVENT_READ_CLOCK_OFFSET_COMPL = 0x1C,
  EVENT_CON_PKT_TYPE_CHANGED = 0x1D,
};

constexpr uint8_t HCI_PAGE_SCAN_ENABLE = 0x02;
constexpr uint8_t HCI_LINK_ACL = 0x01;
constexpr uint16_t ACL_PACKET_START = 0x2000;  // HCI_MK_CON_HANDLE(h, PACKET_START, POINT2POINT)

// Controller buffers.
constexpr uint16_t ACL_PKT_SIZE = 339;
constexpr uint16_t ACL_PKT_NUM = 10;
constexpr uint8_t SCO_PKT_SIZE = 64;
constexpr uint16_t SCO_PKT_NUM = 0;
constexpr size_t ACL_POOL_MAX = 100;

// L2CAP.
constexpr uint16_t L2CAP_NULL_CID = 0x0000;
constexpr uint16_t L2CAP_SIGNAL_CID = 0x0001;
constexpr uint16_t L2CAP_MTU_DEFAULT = 672;
constexpr uint16_t L2CAP_FLUSH_TIMO_DEFAULT = 0xFFFF;
constexpr uint16_t L2CAP_PSM_SDP = 0x0001;
constexpr uint16_t L2CAP_PSM_HID_CNTL = 0x0011;
constexpr uint16_t L2CAP_PSM_HID_INTR = 0x0013;
enum : uint8_t
{
  L2CAP_COMMAND_REJ = 0x01,
  L2CAP_CONNECT_REQ = 0x02,
  L2CAP_CONNECT_RSP = 0x03,
  L2CAP_CONFIG_REQ = 0x04,
  L2CAP_CONFIG_RSP = 0x05,
  L2CAP_DISCONNECT_REQ = 0x06,
  L2CAP_DISCONNECT_RSP = 0x07,
};
constexpr uint16_t L2CAP_SUCCESS = 0x0000;
constexpr uint16_t L2CAP_NO_RESOURCES = 0x0004;
constexpr uint8_t L2CAP_OPT_MTU = 0x01;
constexpr uint8_t L2CAP_OPT_FLUSH_TIMO = 0x02;

// HID transaction headers.
constexpr uint8_t HID_SET_REPORT_OUTPUT = 0x52;  // SET_REPORT << 4 | OUTPUT
constexpr uint8_t HID_DATA_OUTPUT = 0xA2;        // DATA << 4 | OUTPUT
constexpr uint8_t HID_DATA_INPUT = 0xA1;         // DATA << 4 | INPUT
constexpr uint8_t HID_HANDSHAKE_SUCCESS = 0x00;

constexpr int REMOTES = 5;  // four Wii Remotes and the Balance Board slot
constexpr int BALANCE_BOARD = 4;
using BdAddr = std::array<uint8_t, 6>;
constexpr BdAddr CONTROLLER_BD = {0x11, 0x02, 0x19, 0x79, 0x00, 0xFF};

uint16_t Le16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] | p[1] << 8);
}
void PutLe16(uint8_t* p, uint16_t v)
{
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
uint16_t Be16(const uint8_t* p)
{
  return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
uint32_t Be32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

// The Wii Remote's SDP record, as the attribute responses page it out
// (continuation 0, 0x76, 0xEC, 0x162).
constexpr uint8_t SDP_PACKET1[] = {
    0x00, 0x7b, 0x00, 0x76, 0x36, 0x01, 0xcc, 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x00, 0x09,
    0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x24, 0x09, 0x00, 0x04, 0x35, 0x0d, 0x35, 0x06, 0x19, 0x01,
    0x00, 0x09, 0x00, 0x11, 0x35, 0x03, 0x19, 0x00, 0x11, 0x09, 0x00, 0x05, 0x35, 0x03, 0x19, 0x10,
    0x02, 0x09, 0x00, 0x06, 0x35, 0x09, 0x09, 0x65, 0x6e, 0x09, 0x00, 0x6a, 0x09, 0x01, 0x00, 0x09,
    0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x24, 0x09, 0x01, 0x00, 0x09, 0x00, 0x0d, 0x35,
    0x0f, 0x35, 0x0d, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x13, 0x35, 0x03, 0x19, 0x00, 0x11,
    0x09, 0x01, 0x00, 0x25, 0x13, 0x4e, 0x69, 0x6e, 0x74, 0x65, 0x6e, 0x64, 0x6f, 0x20, 0x52, 0x56,
    0x4c, 0x2d, 0x43, 0x4e, 0x54, 0x2d, 0x30, 0x31, 0x09, 0x01, 0x02, 0x00, 0x76,
};
constexpr uint8_t SDP_PACKET2[] = {
    0x00, 0x7b, 0x00, 0x76, 0x01, 0x25, 0x13, 0x4e, 0x69, 0x6e, 0x74, 0x65, 0x6e, 0x64, 0x6f, 0x20,
    0x52, 0x56, 0x4c, 0x2d, 0x43, 0x4e, 0x54, 0x2d, 0x30, 0x31, 0x09, 0x01, 0x02, 0x25, 0x08, 0x4e,
    0x69, 0x6e, 0x74, 0x65, 0x6e, 0x64, 0x6f, 0x09, 0x02, 0x00, 0x09, 0x01, 0x00, 0x09, 0x02, 0x01,
    0x09, 0x01, 0x11, 0x09, 0x02, 0x02, 0x08, 0x04, 0x09, 0x02, 0x03, 0x08, 0x33, 0x09, 0x02, 0x04,
    0x28, 0x00, 0x09, 0x02, 0x05, 0x28, 0x01, 0x09, 0x02, 0x06, 0x35, 0xdf, 0x35, 0xdd, 0x08, 0x22,
    0x25, 0xd9, 0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x10, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75,
    0x08, 0x95, 0x01, 0x06, 0x00, 0xff, 0x09, 0x01, 0x91, 0x00, 0x85, 0x11, 0x95, 0x01, 0x09, 0x01,
    0x91, 0x00, 0x85, 0x12, 0x95, 0x02, 0x09, 0x01, 0x91, 0x00, 0x02, 0x00, 0xec,
};
constexpr uint8_t SDP_PACKET3[] = {
    0x00, 0x7b, 0x00, 0x76, 0x85, 0x13, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x14, 0x95, 0x01,
    0x09, 0x01, 0x91, 0x00, 0x85, 0x15, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x16, 0x95, 0x15,
    0x09, 0x01, 0x91, 0x00, 0x85, 0x17, 0x95, 0x06, 0x09, 0x01, 0x91, 0x00, 0x85, 0x18, 0x95, 0x15,
    0x09, 0x01, 0x91, 0x00, 0x85, 0x19, 0x95, 0x01, 0x09, 0x01, 0x91, 0x00, 0x85, 0x1a, 0x95, 0x01,
    0x09, 0x01, 0x91, 0x00, 0x85, 0x20, 0x95, 0x06, 0x09, 0x01, 0x81, 0x00, 0x85, 0x21, 0x95, 0x15,
    0x09, 0x01, 0x81, 0x00, 0x85, 0x22, 0x95, 0x04, 0x09, 0x01, 0x81, 0x00, 0x85, 0x30, 0x95, 0x02,
    0x09, 0x01, 0x81, 0x00, 0x85, 0x31, 0x95, 0x05, 0x09, 0x01, 0x81, 0x00, 0x85, 0x32, 0x95, 0x0a,
    0x09, 0x01, 0x81, 0x00, 0x85, 0x33, 0x95, 0x11, 0x09, 0x01, 0x02, 0x01, 0x62,
};
constexpr uint8_t SDP_PACKET4[] = {
    0x00, 0x70, 0x00, 0x6d, 0x81, 0x00, 0x85, 0x34, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85,
    0x35, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x36, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00,
    0x85, 0x37, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x3d, 0x95, 0x15, 0x09, 0x01, 0x81,
    0x00, 0x85, 0x3e, 0x95, 0x15, 0x09, 0x01, 0x81, 0x00, 0x85, 0x3f, 0x95, 0x15, 0x09, 0x01,
    0x81, 0x00, 0xc0, 0x09, 0x02, 0x07, 0x35, 0x08, 0x35, 0x06, 0x09, 0x04, 0x09, 0x09, 0x01,
    0x00, 0x09, 0x02, 0x08, 0x28, 0x00, 0x09, 0x02, 0x09, 0x28, 0x01, 0x09, 0x02, 0x0a, 0x28,
    0x01, 0x09, 0x02, 0x0b, 0x09, 0x01, 0x00, 0x09, 0x02, 0x0c, 0x09, 0x0c, 0x80, 0x09, 0x02,
    0x0d, 0x28, 0x00, 0x09, 0x02, 0x0e, 0x28, 0x00, 0x00,
};
constexpr uint8_t SDP_PACKET_10001[] = {
    0x00, 0x60, 0x00, 0x5d, 0x36, 0x00, 0x5a, 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00,
    0x01, 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x12, 0x00, 0x09, 0x00, 0x04, 0x35, 0x0d,
    0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x00, 0x01, 0x09,
    0x00, 0x05, 0x35, 0x03, 0x19, 0x10, 0x02, 0x09, 0x00, 0x09, 0x35, 0x08, 0x35, 0x06,
    0x19, 0x12, 0x00, 0x09, 0x01, 0x00, 0x09, 0x02, 0x00, 0x09, 0x01, 0x00, 0x09, 0x02,
    0x01, 0x09, 0x05, 0x7e, 0x09, 0x02, 0x02, 0x09, 0x03, 0x06, 0x09, 0x02, 0x03, 0x09,
    0x06, 0x00, 0x09, 0x02, 0x04, 0x28, 0x01, 0x09, 0x02, 0x05, 0x09, 0x00, 0x02, 0x00,
};

std::pair<const uint8_t*, uint32_t> SdpAttributePacket(uint32_t handle, uint32_t continuation)
{
  if (handle == 0x10000)
  {
    switch (continuation)
    {
    case 0:
      return {SDP_PACKET1, sizeof(SDP_PACKET1)};
    case 0x76:
      return {SDP_PACKET2, sizeof(SDP_PACKET2)};
    case 0xEC:
      return {SDP_PACKET3, sizeof(SDP_PACKET3)};
    case 0x162:
      return {SDP_PACKET4, sizeof(SDP_PACKET4)};
    }
  }
  if (handle == 0x10001)
    return {SDP_PACKET_10001, sizeof(SDP_PACKET_10001)};
  return {nullptr, 0};
}

// An HCI event being built: code, length, parameters (little-endian).
struct HciEvent
{
  std::array<uint8_t, 260> data;
  uint16_t size = 2;

  HciEvent() = default;
  explicit HciEvent(uint8_t code) { data[0] = code; }
  HciEvent& U8(uint8_t v)
  {
    data[size++] = v;
    return *this;
  }
  HciEvent& U16(uint16_t v)
  {
    PutLe16(&data[size], v);
    size += 2;
    return *this;
  }
  HciEvent& Bytes(const uint8_t* p, size_t n)
  {
    std::memcpy(&data[size], p, n);
    size = static_cast<uint16_t>(size + n);
    return *this;
  }
};

class Bluetooth;

// One remote's end of the link: baseband connection and L2CAP channels.
class RemoteDevice final : public WiimoteLink
{
public:
  enum class Baseband : uint8_t
  {
    Inactive,
    RequestConnection,
    Complete,
  };

  RemoteDevice(Bluetooth& bt, int index, const WiimoteHost& host,
               const std::filesystem::path& eeprom_dir);

  const BdAddr& Bd() const { return m_bd; }
  int Index() const { return m_index; }
  const char* Name() const
  {
    return m_index == BALANCE_BOARD ? "Nintendo RVL-WBC-01" : "Nintendo RVL-CNT-01";
  }
  uint16_t ConnectionHandle() const { return static_cast<uint16_t>(0x100 + m_index); }

  bool Connected() const { return m_baseband == Baseband::Complete; }
  bool ScanEnabled() const { return !Connected() && m_hid; }

  bool EventConnectionRequest();
  bool EventConnectionAccept();
  void EventDisconnect() { Reset(); }

  void Update();
  void UpdateInput();
  void ExecuteL2cap(const uint8_t* packet, uint32_t size);
  void SendInputReport(const uint8_t* report, uint32_t size) override;

  void DoState(StateIO& io);

private:
  struct Channel
  {
    enum class State : uint8_t
    {
      Inactive,
      ConfigurationPending,
      Complete,
    };
    uint16_t psm = 0;
    uint16_t remote_cid = L2CAP_NULL_CID;
    uint16_t remote_mtu = 0;
    State state = State::Inactive;

    bool Accepted() const { return remote_cid != L2CAP_NULL_CID; }
    bool Complete() const { return Accepted() && remote_mtu != 0 && state == State::Complete; }
  };

  void SetBaseband(Baseband state);
  void Reset();
  Channel* FindChannel(uint16_t psm);
  uint16_t NewChannelId() const;
  bool LinkChannel(uint16_t psm);

  void SignalChannel(const uint8_t* data, uint32_t size);
  void ReceiveConnectionRequest(uint8_t ident, const uint8_t* data);
  void ReceiveConnectionResponse(const uint8_t* data);
  void ReceiveConfigurationRequest(uint8_t ident, const uint8_t* data, uint32_t size);
  void ReceiveConfigurationResponse(const uint8_t* data);
  void ReceiveDisconnectionRequest(uint8_t ident, const uint8_t* data);
  void SendConnectionRequest(uint16_t psm);
  void SendConfigurationRequest(uint16_t cid, uint16_t mtu, uint16_t flush_timeout);
  void SendCommand(uint8_t ident, uint8_t code, const uint8_t* data, uint8_t size);
  void HandleSdp(uint16_t cid, const uint8_t* data, uint32_t size);
  void SendFrame(uint16_t cid, const uint8_t* payload, uint32_t size);

  Bluetooth& m_bt;
  const int m_index;
  const BdAddr m_bd;
  std::unique_ptr<Wiimote> m_hid;  // null: no remote on this slot

  Baseband m_baseband = Baseband::Inactive;
  bool m_hid_linking = false;
  std::map<uint16_t, Channel> m_channels;  // by local cid
  uint32_t m_connection_request_counter = 0;
};

class Bluetooth final : public Device
{
public:
  Bluetooth(const Host& host, const Config& config, Nand& nand)
      : Device(host, "/dev/usb/oh1/57e/305"), m_wiimote_host(config.wiimotes)
  {
    for (int i = 0; i < REMOTES; ++i)
      m_remotes[i] = std::make_unique<RemoteDevice>(*this, i, m_wiimote_host, config.nand_root);
    WriteDeviceInfo(nand);
  }

  std::optional<Reply> Close(int32_t fd) override
  {
    m_scan_enable = 0;
    m_last_ticks = 0;
    m_packet_count = {};
    m_hci_endpoint = {};
    m_acl_endpoint = {};
    return Device::Close(fd);
  }

  std::optional<Reply> IOCtlV(const IOCtlVRequest& request) override
  {
    switch (request.request)
    {
    case USBV0_CTRLMSG:
    {
      // An HCI command: setup packet in five in-vectors, command in io[0].
      if (request.in.size() < 5 || request.io.empty())
        return Reply{IPC_EINVAL};
      const uint16_t length = static_cast<uint16_t>(
          __builtin_bswap16(m_memory.Read16(request.in[4].address)));
      ExecuteHciCommand(request.io[0].address, length);
      m_host.reply(m_host.user, request.address, length, 0);
      return std::nullopt;
    }
    case USBV0_BLKMSG:
    {
      if (request.in.size() < 2 || request.io.empty())
        return Reply{IPC_EINVAL};
      const uint8_t endpoint = m_memory.Read8(request.in[0].address);
      const uint32_t data = request.io[0].address;
      if (endpoint == ENDPOINT_ACL_OUT)
      {
        // Header: connection handle and flags, length; then L2CAP.
        const uint8_t* header = m_memory.Pointer(data, 4);
        if (!header)
          return Reply{IPC_EINVAL};
        const uint16_t size = Le16(header + 2);
        if (const uint8_t* packet = m_memory.Pointer(data + 4, size))
          SendToRemote(Le16(header) & 0x0FFF, packet, size);
        return Reply{IPC_SUCCESS};
      }
      if (endpoint == ENDPOINT_ACL_IN)
      {
        m_acl_endpoint = {request.address, data};
        return std::nullopt;
      }
      std::fprintf(stderr, "[native-bt] bulk message to endpoint %02x\n", endpoint);
      return Reply{IPC_SUCCESS};
    }
    case USBV0_INTRMSG:
    {
      if (request.in.empty() || request.io.empty())
        return Reply{IPC_EINVAL};
      const uint8_t endpoint = m_memory.Read8(request.in[0].address);
      if (endpoint == ENDPOINT_HCI_EVENT)
      {
        m_hci_endpoint = {request.address, request.io[0].address};
        return std::nullopt;
      }
      std::fprintf(stderr, "[native-bt] interrupt message to endpoint %02x\n", endpoint);
      return Reply{IPC_SUCCESS};
    }
    default:
      std::fprintf(stderr, "[native-bt] unknown ioctlv %u\n", request.request);
      return Reply{IPC_SUCCESS};
    }
  }

  void Update() override
  {
    if (!m_event_queue.empty() && m_hci_endpoint.request)
    {
      FillHciEndpoint(m_event_queue.front());
      m_event_queue.pop_front();
    }
    if (!m_acl_pool.empty() && m_acl_endpoint.request && m_event_queue.empty())
    {
      const AclPacket& packet = m_acl_pool.front();
      FillAclEndpoint(packet.handle, packet.data.data(), packet.size);
      m_acl_pool.pop_front();
    }
    for (auto& remote : m_remotes)
      remote->Update();
    // Input reports at the remotes' rate.
    const uint64_t now = m_host.now(m_host.user);
    if (now - m_last_ticks > m_host.ticks_per_second / Wiimote::UPDATE_FREQ)
    {
      for (auto& remote : m_remotes)
        remote->UpdateInput();
      m_last_ticks = now;
    }
    SendNumberOfCompletedPackets();
  }

  // --- for the remotes ---
  bool RemoteConnect(const RemoteDevice& remote)
  {
    // With page scan off the controller never hears the remote.
    if (!(m_scan_enable & HCI_PAGE_SCAN_ENABLE))
      return false;
    AddEvent(HciEvent(EVENT_CON_REQ)
                 .Bytes(remote.Bd().data(), 6)
                 .Bytes(CLASS, 3)
                 .U8(HCI_LINK_ACL));
    return true;
  }

  void RemoteDisconnect(const RemoteDevice& remote)
  {
    SendDisconnect(remote.ConnectionHandle(), 0x13);
  }

  void SendAcl(const RemoteDevice& remote, const uint8_t* data, uint32_t size)
  {
    if (m_acl_endpoint.request && !m_hci_endpoint.request && m_event_queue.empty())
    {
      FillAclEndpoint(remote.ConnectionHandle(), data, size);
      return;
    }
    if (m_acl_pool.size() >= ACL_POOL_MAX || size > ACL_PKT_SIZE)
    {
      std::fprintf(stderr, "[native-bt] ACL packet dropped (%u bytes, %zu queued)\n", size,
                   m_acl_pool.size());
      return;
    }
    AclPacket& packet = m_acl_pool.emplace_back();
    std::memcpy(packet.data.data(), data, size);
    packet.size = static_cast<uint16_t>(size);
    packet.handle = remote.ConnectionHandle();
  }

  void DoState(StateIO& io) override
  {
    DoBaseState(io);
    io.Do(m_hci_endpoint);
    io.Do(m_acl_endpoint);
    io.Do(m_last_ticks);
    io.Do(m_packet_count);
    io.Do(m_scan_enable);
    uint32_t events = static_cast<uint32_t>(m_event_queue.size());
    io.Do(events);
    if (io.Reading())
      m_event_queue.resize(events);
    for (HciEvent& event : m_event_queue)
    {
      io.Do(event.size);
      io.Bytes(event.data.data(), event.size);
    }
    uint32_t packets = static_cast<uint32_t>(m_acl_pool.size());
    io.Do(packets);
    if (io.Reading())
      m_acl_pool.resize(packets);
    for (AclPacket& packet : m_acl_pool)
      io.Do(packet);
    for (auto& remote : m_remotes)
      remote->DoState(io);
  }

  static constexpr uint8_t CLASS[3] = {0x00, 0x04, 0x48};
  static constexpr uint8_t FEATURES[8] = {0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
  static constexpr uint8_t LMP_VERSION = 0x2;
  static constexpr uint16_t LMP_SUBVERSION = 0x229;
  static constexpr uint16_t MANUFACTURER = 0x000F;

private:
  struct Endpoint
  {
    uint32_t request = 0;  // 0: none
    uint32_t data = 0;
  };

  struct AclPacket
  {
    std::array<uint8_t, ACL_PKT_SIZE> data;
    uint16_t size;
    uint16_t handle;
  };

  RemoteDevice* RemoteByBd(const uint8_t* bd)
  {
    if (bd[5] >= REMOTES)
      return nullptr;
    RemoteDevice* remote = m_remotes[bd[5]].get();
    return std::memcmp(remote->Bd().data(), bd, 6) == 0 ? remote : nullptr;
  }
  RemoteDevice* RemoteByHandle(uint16_t handle)
  {
    const uint32_t index = handle & 0xFF;
    if (index < REMOTES)
      return m_remotes[index].get();
    std::fprintf(stderr, "[native-bt] no remote with connection handle %04x\n", handle);
    return nullptr;
  }

  void SendToRemote(uint16_t handle, const uint8_t* data, uint32_t size)
  {
    if (RemoteDevice* remote = RemoteByHandle(handle))
    {
      ++m_packet_count[remote->Index()];
      remote->ExecuteL2cap(data, size);
    }
  }

  void FillHciEndpoint(const HciEvent& event)
  {
    m_memory.CopyTo(m_hci_endpoint.data, event.data.data(), event.size);
    m_host.reply(m_host.user, m_hci_endpoint.request, event.size, 0);
    m_hci_endpoint = {};
  }

  void FillAclEndpoint(uint16_t handle, const uint8_t* data, uint32_t size)
  {
    uint8_t header[4];
    PutLe16(header, static_cast<uint16_t>(handle | ACL_PACKET_START));
    PutLe16(header + 2, static_cast<uint16_t>(size));
    m_memory.CopyTo(m_acl_endpoint.data, header, 4);
    m_memory.CopyTo(m_acl_endpoint.data + 4, data, size);
    m_host.reply(m_host.user, m_acl_endpoint.request, static_cast<int32_t>(4 + size), 0);
    m_acl_endpoint = {};
  }

  void AddEvent(HciEvent& event)
  {
    event.data[1] = static_cast<uint8_t>(event.size - 2);
    if (!m_hci_endpoint.request)
    {
      m_event_queue.push_back(event);
      return;
    }
    if (m_event_queue.empty())
    {
      FillHciEndpoint(event);
      return;
    }
    // Keep order: the oldest goes out, this one waits.
    m_event_queue.push_back(event);
    FillHciEndpoint(m_event_queue.front());
    m_event_queue.pop_front();
  }
  void AddEvent(HciEvent&& event) { AddEvent(event); }

  void CommandComplete(uint16_t opcode, const uint8_t* data = nullptr, size_t size = 0)
  {
    HciEvent event(EVENT_COMMAND_COMPL);
    event.U8(1).U16(opcode);
    if (size)
      event.Bytes(data, size);
    AddEvent(event);
  }
  void CommandComplete(uint16_t opcode, std::initializer_list<uint8_t> data)
  {
    CommandComplete(opcode, data.begin(), data.size());
  }
  void CommandStatus(uint16_t opcode) { AddEvent(HciEvent(EVENT_COMMAND_STATUS).U8(0).U8(1).U16(opcode)); }

  void SendConnectionComplete(const uint8_t* bd, uint8_t status)
  {
    AddEvent(HciEvent(EVENT_CON_COMPL)
                 .U8(status)
                 .U16(static_cast<uint16_t>(0x100 + bd[5]))
                 .Bytes(bd, 6)
                 .U8(HCI_LINK_ACL)
                 .U8(0));
  }

  void SendDisconnect(uint16_t handle, uint8_t reason)
  {
    if (RemoteByHandle(handle))
      AddEvent(HciEvent(EVENT_DISCON_COMPL).U8(0).U16(handle).U8(reason));
  }

  void SendInquiryResponse()
  {
    const auto it = std::find_if(m_remotes.begin(), m_remotes.end(),
                                 [](const auto& r) { return r->ScanEnabled(); });
    if (it == m_remotes.end())
    {
      AddEvent(HciEvent(EVENT_INQUIRY_COMPL).U8(0).U8(0));
      return;
    }
    // One response: address, page scan rep/period/mode, class, clock offset.
    AddEvent(HciEvent(EVENT_INQUIRY_RESULT)
                 .U8(1)
                 .Bytes((*it)->Bd().data(), 6)
                 .U8(1)
                 .U8(0)
                 .U8(0)
                 .Bytes(CLASS, 3)
                 .U16(0x3818));
    AddEvent(HciEvent(EVENT_INQUIRY_COMPL).U8(0).U8(1));
  }

  void SendLinkKeyNotification(uint8_t count)
  {
    HciEvent event(EVENT_RETURN_LINK_KEYS);
    event.U8(count);
    for (uint8_t i = 0; i < count; ++i)
    {
      uint8_t key[16];
      std::memset(key, 0xA0 + i, sizeof(key));
      event.Bytes(m_remotes[i]->Bd().data(), 6).Bytes(key, sizeof(key));
    }
    AddEvent(event);
  }

  void SendNumberOfCompletedPackets()
  {
    HciEvent event(EVENT_NUM_COMPL_PKTS);
    event.U8(REMOTES);
    uint32_t total = 0;
    for (int i = 0; i < REMOTES; ++i)
    {
      event.U16(m_remotes[i]->ConnectionHandle()).U16(static_cast<uint16_t>(m_packet_count[i]));
      total += m_packet_count[i];
      m_packet_count[i] = 0;
    }
    if (total)
      AddEvent(event);
  }

  void ExecuteHciCommand(uint32_t address, uint16_t length)
  {
    // Opcode, parameter length, parameters.
    uint8_t cmd[3 + 255] = {};
    if (!m_memory.CopyFrom(cmd, address, sizeof(cmd)))
      m_memory.CopyFrom(cmd, address, std::min<uint32_t>(length, sizeof(cmd)));
    const uint16_t opcode = Le16(cmd);
    const uint8_t* p = cmd + 3;
    switch (opcode)
    {
    case HCI_RESET:
    case HCI_VENDOR_FC4C:
    case HCI_VENDOR_FC4F:
    case HCI_SET_EVENT_FILTER:
    case HCI_WRITE_PIN_TYPE:
    case HCI_WRITE_LOCAL_NAME:
    case HCI_WRITE_PAGE_TIMEOUT:
    case HCI_WRITE_UNIT_CLASS:
    case HCI_HOST_BUFFER_SIZE:
    case HCI_WRITE_INQUIRY_SCAN_TYPE:
    case HCI_WRITE_INQUIRY_MODE:
    case HCI_WRITE_PAGE_SCAN_TYPE:
    case HCI_INQUIRY_CANCEL:
      CommandComplete(opcode, {0});
      break;
    case HCI_WRITE_SCAN_ENABLE:
      m_scan_enable = p[0];
      CommandComplete(opcode, {0});
      break;
    case HCI_READ_BUFFER_SIZE:
      CommandComplete(opcode, {0, ACL_PKT_SIZE & 0xFF, ACL_PKT_SIZE >> 8, SCO_PKT_SIZE,
                               ACL_PKT_NUM & 0xFF, ACL_PKT_NUM >> 8, SCO_PKT_NUM & 0xFF,
                               SCO_PKT_NUM >> 8});
      break;
    case HCI_READ_LOCAL_VER:
      // HCI 1.1 rev 0x40a7, LMP 1.1, manufacturer 0x000F, LMP subversion 0x430e.
      CommandComplete(opcode, {0, 0x03, 0xA7, 0x40, 0x03, 0x0F, 0x00, 0x0E, 0x43});
      break;
    case HCI_READ_LOCAL_FEATURES:
      CommandComplete(opcode, {0, 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80});
      break;
    case HCI_READ_BDADDR:
    {
      uint8_t reply[7] = {0};
      std::memcpy(reply + 1, CONTROLLER_BD.data(), 6);
      CommandComplete(opcode, reply, sizeof(reply));
      break;
    }
    case HCI_READ_STORED_LINK_KEY:
    {
      const uint8_t count = p[6] == 1 ? REMOTES : 0;  // read_all
      SendLinkKeyNotification(count);
      CommandComplete(opcode, {0, 255, 0, count, 0});
      break;
    }
    case HCI_DELETE_STORED_LINK_KEY:
      if (RemoteByBd(p))
        CommandComplete(opcode, {0, 0, 0});
      break;
    case HCI_LINK_KEY_REP:
    case HCI_LINK_KEY_NEG_REP:
    {
      uint8_t reply[7] = {0};
      std::memcpy(reply + 1, p, 6);
      CommandComplete(opcode, reply, sizeof(reply));
      break;
    }
    case HCI_WRITE_LINK_SUPERVISION_TIMEOUT:
      CommandComplete(opcode, {0, p[0], p[1]});
      break;
    case HCI_INQUIRY:
      CommandStatus(opcode);
      SendInquiryResponse();
      break;
    case HCI_REMOTE_NAME_REQ:
      CommandStatus(opcode);
      if (const RemoteDevice* remote = RemoteByBd(p))
      {
        uint8_t name[248] = {};
        std::memcpy(name, remote->Name(), std::strlen(remote->Name()));
        AddEvent(HciEvent(EVENT_REMOTE_NAME_REQ_COMPL).U8(0).Bytes(p, 6).Bytes(name, sizeof(name)));
      }
      break;
    case HCI_CREATE_CON:
    {
      CommandStatus(opcode);
      RemoteDevice* remote = RemoteByBd(p);
      const bool ok = remote && remote->EventConnectionRequest();
      SendConnectionComplete(p, ok ? 0x00 : 0x08);
      break;
    }
    case HCI_ACCEPT_CON:
    {
      CommandStatus(opcode);
      RemoteDevice* remote = RemoteByBd(p);
      if (remote && remote->EventConnectionAccept())
      {
        // Role 0: the controller becomes master.
        if (p[6] == 0)
          AddEvent(HciEvent(EVENT_ROLE_CHANGE).U8(0).Bytes(p, 6).U8(0x00));
        SendConnectionComplete(p, 0x00);
      }
      else
      {
        SendConnectionComplete(p, 0x08);
      }
      break;
    }
    case HCI_DISCONNECT:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      SendDisconnect(handle, p[2]);
      if (RemoteDevice* remote = RemoteByHandle(handle))
        remote->EventDisconnect();
      break;
    }
    case HCI_CHANGE_CON_PACKET_TYPE:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      if (RemoteByHandle(handle))
        AddEvent(HciEvent(EVENT_CON_PKT_TYPE_CHANGED).U8(0).U16(handle).U16(Le16(p + 2)));
      break;
    }
    case HCI_READ_CLOCK_OFFSET:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      if (RemoteByHandle(handle))
        AddEvent(HciEvent(EVENT_READ_CLOCK_OFFSET_COMPL).U8(0).U16(handle).U16(0x3818));
      break;
    }
    case HCI_READ_REMOTE_VER_INFO:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      if (RemoteByHandle(handle))
      {
        AddEvent(HciEvent(EVENT_READ_REMOTE_VER_INFO_COMPL)
                     .U8(0)
                     .U16(handle)
                     .U8(LMP_VERSION)
                     .U16(MANUFACTURER)
                     .U16(LMP_SUBVERSION));
      }
      break;
    }
    case HCI_READ_REMOTE_FEATURES:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      if (RemoteByHandle(handle))
        AddEvent(HciEvent(EVENT_READ_REMOTE_FEATURES_COMPL).U8(0).U16(handle).Bytes(FEATURES, 8));
      break;
    }
    case HCI_WRITE_LINK_POLICY_SETTINGS:
      CommandStatus(opcode);
      break;
    case HCI_AUTH_REQ:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      if (RemoteByHandle(handle))
        AddEvent(HciEvent(EVENT_AUTH_COMPL).U8(0).U16(handle));
      break;
    }
    case HCI_SNIFF_MODE:
    {
      const uint16_t handle = Le16(p);
      CommandStatus(opcode);
      if (RemoteByHandle(handle))
        AddEvent(HciEvent(EVENT_MODE_CHANGE).U8(0).U16(handle).U8(0x02).U16(Le16(p + 2)));
      break;
    }
    default:
      CommandComplete(opcode);
      if ((opcode >> 10) != 0x3F)
        std::fprintf(stderr, "[native-bt] unknown HCI command %04x\n", opcode);
      break;
    }
  }

  // SYSCONF's BT.DINF lists the paired and active remotes; the game connects
  // the ones it finds there.
  static void WriteDeviceInfo(Nand& nand)
  {
    constexpr const char* PATH = "/shared2/sys/SYSCONF";
    std::vector<uint8_t> sysconf;
    if (nand.ReadFile(PATH, &sysconf) != IPC_SUCCESS || sysconf.size() != 0x4000)
    {
      std::fprintf(stderr, "[native-bt] no SYSCONF; remotes stay unpaired\n");
      return;
    }
    // ConfPads: count, 10 registered, 5 active, 1 unknown, each address +
    // 64-byte name.
    constexpr size_t PAD = 6 + 0x40, SIZE = 1 + PAD * (10 + REMOTES + 1);
    std::array<uint8_t, SIZE> pads{};
    pads[0] = REMOTES;
    for (int i = 0; i < REMOTES; ++i)
    {
      const char* name = i == BALANCE_BOARD ? "Nintendo RVL-WBC-01" : "Nintendo RVL-CNT-01";
      for (uint8_t* pad : {&pads[1 + PAD * i], &pads[1 + PAD * (10 + i)]})
      {
        const BdAddr bd = {0x11, 0x02, 0x19, 0x79, 0, static_cast<uint8_t>(i)};
        std::reverse_copy(bd.begin(), bd.end(), pad);
        std::memcpy(pad + 6, name, 20);
      }
    }
    const uint16_t entries = Be16(&sysconf[4]);
    for (uint16_t e = 0; e < entries && 6 + 2 * e + 2 <= sysconf.size(); ++e)
    {
      const uint16_t offset = Be16(&sysconf[6 + 2 * e]);
      if (offset + 1 + 7 + 2 + SIZE > sysconf.size())
        continue;
      const uint8_t description = sysconf[offset];
      const size_t name_length = (description & 0x1F) + 1;
      // A big array (type 1) named BT.DINF of exactly the struct's size.
      if (description >> 5 != 1 || name_length != 7 ||
          std::memcmp(&sysconf[offset + 1], "BT.DINF", 7) != 0 ||
          Be16(&sysconf[offset + 8]) + 1u != SIZE)
        continue;
      const uint32_t data = offset + 10;
      if (std::memcmp(&sysconf[data], pads.data(), SIZE) == 0)
        return;
      if (nand.WriteFile(0, 0, PATH, {NAND_RW, NAND_RW, NAND_RW}, data, pads.data(), SIZE) !=
          IPC_SUCCESS)
        std::fprintf(stderr, "[native-bt] can't write BT.DINF\n");
      return;
    }
    std::fprintf(stderr, "[native-bt] SYSCONF has no BT.DINF; remotes stay unpaired\n");
  }

  const WiimoteHost m_wiimote_host;
  std::array<std::unique_ptr<RemoteDevice>, REMOTES> m_remotes;
  Endpoint m_hci_endpoint, m_acl_endpoint;
  uint64_t m_last_ticks = 0;
  std::array<uint32_t, REMOTES> m_packet_count{};
  uint8_t m_scan_enable = 0;
  std::deque<HciEvent> m_event_queue;
  std::deque<AclPacket> m_acl_pool;
};

RemoteDevice::RemoteDevice(Bluetooth& bt, int index, const WiimoteHost& host,
                           const std::filesystem::path& eeprom_dir)
    : m_bt(bt), m_index(index),
      m_bd{0x11, 0x02, 0x19, 0x79, 0x00, static_cast<uint8_t>(index)}
{
  if (host.slots & (1u << index))
  {
    m_hid = std::make_unique<Wiimote>(index, host, eeprom_dir, *this);
    // A remote that is on asks the controller to connect.
    SetBaseband(Baseband::RequestConnection);
  }
}

void RemoteDevice::SetBaseband(Baseband state)
{
  m_connection_request_counter = Wiimote::UPDATE_FREQ;
  const bool was_connected = Connected();
  m_baseband = state;
  if (m_hid && Connected() != was_connected)
    m_hid->Reset();
}

void RemoteDevice::Reset()
{
  SetBaseband(Baseband::Inactive);
  m_hid_linking = false;
  m_channels.clear();
}

bool RemoteDevice::EventConnectionRequest()
{
  if (!ScanEnabled())
    return false;
  SetBaseband(Baseband::Complete);
  return true;
}

bool RemoteDevice::EventConnectionAccept()
{
  if (!ScanEnabled())
    return false;
  SetBaseband(Baseband::Complete);
  m_hid_linking = true;
  return true;
}

RemoteDevice::Channel* RemoteDevice::FindChannel(uint16_t psm)
{
  for (auto& [cid, channel] : m_channels)
  {
    if (channel.psm == psm)
      return &channel;
  }
  return nullptr;
}

uint16_t RemoteDevice::NewChannelId() const
{
  uint16_t cid = 0x40;
  while (m_channels.contains(cid))
    ++cid;
  return cid;
}

bool RemoteDevice::LinkChannel(uint16_t psm)
{
  const Channel* channel = FindChannel(psm);
  if (!channel)
  {
    SendConnectionRequest(psm);
    return false;
  }
  return channel->Complete();
}

void RemoteDevice::Update()
{
  if (m_baseband == Baseband::RequestConnection && m_bt.RemoteConnect(*this))
    SetBaseband(Baseband::Inactive);
  if (!Connected())
    return;
  // Configure each channel the game accepted.
  for (auto& [cid, channel] : m_channels)
  {
    if (channel.Accepted() && channel.state == Channel::State::Inactive)
    {
      channel.state = Channel::State::ConfigurationPending;
      SendConfigurationRequest(channel.remote_cid, 185, L2CAP_FLUSH_TIMO_DEFAULT);
    }
  }
  // After an accepted connection the remote opens its HID channels.
  if (m_hid_linking && LinkChannel(L2CAP_PSM_HID_CNTL) && LinkChannel(L2CAP_PSM_HID_INTR))
    m_hid_linking = false;
}

void RemoteDevice::UpdateInput()
{
  if (m_connection_request_counter)
    --m_connection_request_counter;
  if (!m_hid)
    return;
  if (m_baseband == Baseband::Inactive)
  {
    // A button press wakes a disconnected remote.
    if (!m_connection_request_counter)
    {
      m_hid->PrepareInput();
      if (m_hid->AnyButtonPressed())
        SetBaseband(Baseband::RequestConnection);
    }
    return;
  }
  const Channel* channel = FindChannel(L2CAP_PSM_HID_INTR);
  if (channel && channel->Complete())
  {
    m_hid->PrepareInput();
    m_hid->Update();
  }
}

void RemoteDevice::SendFrame(uint16_t cid, const uint8_t* payload, uint32_t size)
{
  uint8_t frame[4 + 1024];
  PutLe16(frame, static_cast<uint16_t>(size));
  PutLe16(frame + 2, cid);
  std::memcpy(frame + 4, payload, size);
  m_bt.SendAcl(*this, frame, 4 + size);
}

void RemoteDevice::SendInputReport(const uint8_t* report, uint32_t size)
{
  const Channel* channel = FindChannel(L2CAP_PSM_HID_INTR);
  if (!channel)
    return;
  uint8_t payload[1 + 22];
  payload[0] = HID_DATA_INPUT;
  std::memcpy(payload + 1, report, size);
  SendFrame(channel->remote_cid, payload, 1 + size);
}

void RemoteDevice::ExecuteL2cap(const uint8_t* packet, uint32_t size)
{
  if (size < 4)
    return;
  const uint16_t length = Le16(packet);
  const uint16_t dcid = Le16(packet + 2);
  const uint8_t* data = packet + 4;
  const uint32_t data_size = size - 4;
  if (length != data_size)
    return;
  if (dcid == L2CAP_SIGNAL_CID)
  {
    SignalChannel(data, data_size);
    return;
  }
  const auto it = m_channels.find(dcid);
  if (it == m_channels.end() || data_size == 0)
  {
    std::fprintf(stderr, "[native-bt] L2CAP data for unknown channel %04x\n", dcid);
    return;
  }
  const Channel& channel = it->second;
  switch (channel.psm)
  {
  case L2CAP_PSM_SDP:
    HandleSdp(dcid, data, data_size);
    break;
  case L2CAP_PSM_HID_CNTL:
    if (data[0] == HID_SET_REPORT_OUTPUT)
    {
      const uint8_t handshake = HID_HANDSHAKE_SUCCESS;
      SendFrame(channel.remote_cid, &handshake, 1);
      if (m_hid)
        m_hid->OutputReport(data + 1, data_size - 1);
    }
    break;
  case L2CAP_PSM_HID_INTR:
    if (data[0] == HID_DATA_OUTPUT && m_hid)
      m_hid->OutputReport(data + 1, data_size - 1);
    break;
  default:
    std::fprintf(stderr, "[native-bt] channel %04x has unknown PSM %04x\n", dcid, channel.psm);
    break;
  }
}

void RemoteDevice::SignalChannel(const uint8_t* data, uint32_t size)
{
  while (size >= 4)
  {
    const uint8_t code = data[0], ident = data[1];
    const uint16_t length = Le16(data + 2);
    data += 4;
    size -= 4;
    if (length > size)
      return;
    switch (code)
    {
    case L2CAP_COMMAND_REJ:
      std::fprintf(stderr, "[native-bt] L2CAP command rejected\n");
      break;
    case L2CAP_CONNECT_REQ:
      ReceiveConnectionRequest(ident, data);
      break;
    case L2CAP_CONNECT_RSP:
      ReceiveConnectionResponse(data);
      break;
    case L2CAP_CONFIG_REQ:
      ReceiveConfigurationRequest(ident, data, length);
      break;
    case L2CAP_CONFIG_RSP:
      ReceiveConfigurationResponse(data);
      break;
    case L2CAP_DISCONNECT_REQ:
      ReceiveDisconnectionRequest(ident, data);
      break;
    default:
      std::fprintf(stderr, "[native-bt] unknown L2CAP command %02x\n", code);
      return;
    }
    data += length;
    size -= length;
  }
}

void RemoteDevice::ReceiveConnectionRequest(uint8_t ident, const uint8_t* data)
{
  const uint16_t psm = Le16(data), scid = Le16(data + 2);
  uint16_t dcid = L2CAP_NULL_CID, result = L2CAP_NO_RESOURCES;
  // One channel per PSM.
  if (!FindChannel(psm))
  {
    dcid = NewChannelId();
    Channel& channel = m_channels[dcid];
    channel.psm = psm;
    channel.remote_cid = scid;
    result = L2CAP_SUCCESS;
  }
  uint8_t rsp[8];
  PutLe16(rsp, dcid);
  PutLe16(rsp + 2, scid);
  PutLe16(rsp + 4, result);
  PutLe16(rsp + 6, 0);  // no further info
  SendCommand(ident, L2CAP_CONNECT_RSP, rsp, sizeof(rsp));
}

void RemoteDevice::ReceiveConnectionResponse(const uint8_t* data)
{
  // dcid is the game's channel, scid ours.
  m_channels[Le16(data + 2)].remote_cid = Le16(data);
}

void RemoteDevice::ReceiveConfigurationRequest(uint8_t ident, const uint8_t* data, uint32_t size)
{
  if (size < 4)
    return;
  Channel& channel = m_channels[Le16(data)];
  // The response echoes each option.
  uint8_t rsp[255];
  PutLe16(rsp, channel.remote_cid);
  PutLe16(rsp + 2, 0);
  PutLe16(rsp + 4, L2CAP_SUCCESS);
  uint32_t rsp_size = 6;
  uint16_t remote_mtu = L2CAP_MTU_DEFAULT;
  for (uint32_t offset = 4; offset + 2 <= size;)
  {
    const uint8_t type = data[offset], length = data[offset + 1];
    if (offset + 2 + length > size || rsp_size + 2 + length > sizeof(rsp))
      break;
    if (type == L2CAP_OPT_MTU && length == 2)
      remote_mtu = Le16(data + offset + 2);
    else if (type != L2CAP_OPT_FLUSH_TIMO)
      std::fprintf(stderr, "[native-bt] unknown L2CAP option %02x\n", type);
    std::memcpy(rsp + rsp_size, data + offset, 2 + length);
    rsp_size += 2 + length;
    offset += 2 + length;
  }
  SendCommand(ident, L2CAP_CONFIG_RSP, rsp, static_cast<uint8_t>(rsp_size));
  channel.remote_mtu = remote_mtu;
}

void RemoteDevice::ReceiveConfigurationResponse(const uint8_t* data)
{
  m_channels[Le16(data)].state = Channel::State::Complete;
}

void RemoteDevice::ReceiveDisconnectionRequest(uint8_t ident, const uint8_t* data)
{
  m_channels.erase(Le16(data));
  uint8_t rsp[4];
  std::memcpy(rsp, data, 4);  // same dcid and scid
  SendCommand(ident, L2CAP_DISCONNECT_RSP, rsp, sizeof(rsp));
}

void RemoteDevice::SendConnectionRequest(uint16_t psm)
{
  const uint16_t cid = NewChannelId();
  m_channels[cid].psm = psm;
  uint8_t req[4];
  PutLe16(req, psm);
  PutLe16(req + 2, cid);
  SendCommand(L2CAP_CONNECT_REQ, L2CAP_CONNECT_REQ, req, sizeof(req));
}

void RemoteDevice::SendConfigurationRequest(uint16_t cid, uint16_t mtu, uint16_t flush_timeout)
{
  uint8_t req[12];
  PutLe16(req, cid);
  PutLe16(req + 2, 0);
  uint8_t size = 4;
  if (mtu != L2CAP_MTU_DEFAULT)
  {
    req[size] = L2CAP_OPT_MTU;
    req[size + 1] = 2;
    PutLe16(req + size + 2, mtu);
    size += 4;
  }
  if (flush_timeout != L2CAP_FLUSH_TIMO_DEFAULT)
  {
    req[size] = L2CAP_OPT_FLUSH_TIMO;
    req[size + 1] = 2;
    PutLe16(req + size + 2, flush_timeout);
    size += 4;
  }
  SendCommand(L2CAP_CONFIG_REQ, L2CAP_CONFIG_REQ, req, size);
}

void RemoteDevice::SendCommand(uint8_t ident, uint8_t code, const uint8_t* data, uint8_t size)
{
  uint8_t command[4 + 255];
  command[0] = code;
  command[1] = ident;
  PutLe16(command + 2, size);
  std::memcpy(command + 4, data, size);
  SendFrame(L2CAP_SIGNAL_CID, command, 4u + size);
}

void RemoteDevice::HandleSdp(uint16_t cid, const uint8_t* data, uint32_t size)
{
  // SDP is big-endian.
  uint8_t rsp[512];
  switch (data[0])
  {
  case 0x02:  // ServiceSearchRequest: one record, handle 0x10000
  {
    if (size < 13)
      return;
    const uint8_t search[] = {0x03, data[1], data[2], 0x00, 0x09, 0x00, 0x01, 0x00,
                              0x01, 0x00,    0x01,    0x00, 0x00, 0x00};
    SendFrame(cid, search, sizeof(search));
    break;
  }
  case 0x04:  // ServiceAttributeRequest
  {
    if (size < 14)
      return;
    const uint32_t handle = Be32(data + 5);
    // Attribute id list: a data sequence of one uint32 range or uint16 id.
    uint32_t offset = 11;
    offset += 3 + (data[offset + 2] == 0x0A ? 4 : 2);
    if (offset >= size)
      return;
    const uint32_t continuation = data[offset] == 0x02 && offset + 2 < size ? Be16(data + offset + 1)
                                                                            : 0;
    const auto [packet, packet_size] = SdpAttributePacket(handle, continuation);
    if (!packet)
    {
      std::fprintf(stderr, "[native-bt] SDP: no attributes for %08x/%x\n", handle, continuation);
      return;
    }
    rsp[0] = 0x05;
    rsp[1] = data[1];
    rsp[2] = data[2];
    std::memcpy(rsp + 3, packet, packet_size);
    SendFrame(cid, rsp, 3 + packet_size);
    break;
  }
  default:
    std::fprintf(stderr, "[native-bt] unknown SDP command %02x\n", data[0]);
    break;
  }
}

void RemoteDevice::DoState(StateIO& io)
{
  io.Do(m_baseband);
  io.Do(m_hid_linking);
  io.Do(m_connection_request_counter);
  uint32_t count = static_cast<uint32_t>(m_channels.size());
  io.Do(count);
  if (io.Reading())
  {
    m_channels.clear();
    for (uint32_t i = 0; i < count; ++i)
    {
      uint16_t cid = 0;
      Channel channel;
      io.Do(cid);
      io.Do(channel);
      m_channels.emplace(cid, channel);
    }
  }
  else
  {
    for (auto& [cid, channel] : m_channels)
    {
      uint16_t id = cid;
      io.Do(id);
      io.Do(channel);
    }
  }
  if (m_hid)
    m_hid->DoState(io);
}
}  // namespace

std::unique_ptr<Device> CreateBluetooth(const Host& host, const Config& config, Nand& nand)
{
  return std::make_unique<Bluetooth>(host, config, nand);
}
}  // namespace dualize::ios
