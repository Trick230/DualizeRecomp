#pragma once

// The Wii Remotes at the Revolution SDK's WUD boundary. WPAD talks to its
// remotes through WUD, which runs the Bluetooth stack (BTA/BTE, HCI and L2CAP
// over IOS's USB driver). Here WUD's device side is the host's: WPAD's output
// reports go straight to the remote models, their input reports go to WPAD's
// receive callback, and connections reach WPAD's connection callback. The
// Bluetooth stack never starts. sdk/rvl_wud.cpp has the natives.

#include "cpu/cpu.hpp"
#include "kernel/ios/wiimote.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>

namespace dualize
{
class Machine;

class Wud
{
public:
  static constexpr int REMOTES = 4;

  explicit Wud(Machine& machine);
  ~Wud();

  void Start(const ios::WiimoteHost& host, const std::filesystem::path& eeprom_dir);

  // WPAD's alarm, every millisecond: connections, the remotes' report clock
  // and the reports they sent since. Runs WPAD's callbacks on the guest.
  void Pump(Cpu& cpu);
  // An output report (report id first) for the remote on that WUD handle.
  void Send(uint8_t handle, const uint8_t* report, uint32_t size);
  // btm_remove_acl: drops the link to that device address (WUD's byte order).
  void Disconnect(const uint8_t* address);
  // WUD's shutdown: every link drops at once.
  void Shutdown();

  // The layout /dev/usb/oh1/57e/305 saved before the SDK-level WUD, so older
  // states load and newer ones keep the format.
  void DoState(ios::StateIO& io);
  // After a state loads: alarms the guest's Bluetooth stack left armed (in
  // states saved before the SDK-level WUD) are cancelled.
  void AfterLoad(Cpu& cpu);

private:
  struct Remote final : ios::WiimoteLink
  {
    void SendInputReport(const uint8_t* report, uint32_t size) override;

    std::unique_ptr<ios::Wiimote> hid;  // null: no remote on this slot
    bool connected = false;
    bool requesting = false;  // on, asking to connect
    bool closing = false;
    // Updates left before a button press may reconnect a dropped remote.
    uint32_t reconnect_wait = 0;
    std::deque<std::array<uint8_t, 23>> reports;  // size first
  };

  void Connect(Cpu& cpu, int handle);
  void Close(Cpu& cpu, int handle);
  void Deliver(Cpu& cpu, int handle);
  void UpdateRemotes();
  void WriteLinkCount() const;

  Machine& m_machine;
  ios::WiimoteHost m_host;
  std::array<Remote, REMOTES> m_remotes;
  uint64_t m_last_update = 0;
};
}  // namespace dualize
