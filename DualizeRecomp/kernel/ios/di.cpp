// /dev/di: IOS's disc interface driver and the drive behind it.
//
// Commands queue and run one at a time like IOS's own driver. Drive commands
// finish after the drive's latency; reads complete one block at a time at the
// drive buffer's transfer rate, each block landing in memory when it is done.
// The timing, the results and the savestate layout are those of Dolphin's
// DIDevice + DVDInterface with fast disc speed, so states and replays agree.

#include "kernel/ios/devices.hpp"

#include <algorithm>
#include <array>
#include <deque>

namespace dualize::ios
{
namespace
{
enum : uint8_t
{
  DVDLowInquiry = 0x12,
  DVDLowReadDiskID = 0x70,
  DVDLowRead = 0x71,
  DVDLowWaitForCoverClose = 0x79,
  DVDLowGetCoverRegister = 0x7a,
  DVDLowNotifyReset = 0x7e,
  DVDLowSetSpinupFlag = 0x7f,
  DVDLowReadDvdPhysical = 0x80,
  DVDLowReadDvdCopyright = 0x81,
  DVDLowReadDvdDiscKey = 0x82,
  DVDLowGetLength = 0x83,
  DVDLowGetImmBuf = 0x84,
  DVDLowMaskCoverInterrupt = 0x85,
  DVDLowClearCoverInterrupt = 0x86,
  DVDLowUnmaskStatusInterrupts = 0x87,
  DVDLowGetCoverStatus = 0x88,
  DVDLowUnmaskCoverInterrupt = 0x89,
  DVDLowReset = 0x8a,
  DVDLowOpenPartition = 0x8b,
  DVDLowClosePartition = 0x8c,
  DVDLowUnencryptedRead = 0x8d,
  DVDLowEnableDvdVideo = 0x8e,
  DVDLowGetNoDiscOpenPartitionParams = 0x90,
  DVDLowNoDiscOpenPartition = 0x91,
  DVDLowGetNoDiscBufferSizes = 0x92,
  DVDLowOpenPartitionWithTmdAndTicket = 0x93,
  DVDLowOpenPartitionWithTmdAndTicketView = 0x94,
  DVDLowGetStatusRegister = 0x95,
  DVDLowGetControlRegister = 0x96,
  DVDLowReportKey = 0xa4,
  DVDLowSeek = 0xab,
  DVDLowReadDvd = 0xd0,
  DVDLowReadDvdConfig = 0xd1,
  DVDLowStopLaser = 0xd2,
  DVDLowOffset = 0xd9,
  DVDLowReadDiskBca = 0xda,
  DVDLowRequestDiscStatus = 0xdb,
  DVDLowRequestRetryNumber = 0xdc,
  DVDLowSetMaximumRotation = 0xdd,
  DVDLowSerMeasControl = 0xdf,
  DVDLowRequestError = 0xe0,
  DVDLowAudioStream = 0xe1,
  DVDLowRequestAudioStatus = 0xe2,
  DVDLowStopMotor = 0xe3,
  DVDLowAudioBufferConfig = 0xe4,
};

// /dev/di results.
enum Result : int32_t
{
  Success = 1,
  DriveError = 2,
  CoverClosed = 4,
  ReadTimedOut = 16,
  SecurityError = 32,
  VerifyError = 64,
  BadArgument = 128,
};

// The drive's state and sense codes (the top byte and the rest of a
// RequestError reply).
enum class DriveState : uint8_t
{
  Ready = 0,
  ReadyNoReadsMade = 1,
  CoverOpened = 2,
  DiscChangeDetected = 3,
  NoMediumPresent = 4,
  MotorStopped = 5,
  DiscIdNotRead = 6,
};
enum : uint32_t
{
  ERROR_NONE = 0,
  ERROR_MOTOR_STOPPED = 0x20400,
  ERROR_NO_DISC_ID = 0x20401,
  ERROR_READ = 0x31100,
  ERROR_INVALID_COMMAND = 0x52000,
  ERROR_NO_AUDIO_BUF = 0x52001,
  ERROR_BLOCK_OOB = 0x52100,
  ERROR_INVALID_PERIOD = 0x52402,
  ERROR_MEDIUM_CHANGED = 0x62800,
};

// Status and cover register bits.
enum : uint32_t
{
  DISR_DEINTMASK = 1 << 1,
  DISR_DEINT = 1 << 2,
  DISR_TCINTMASK = 1 << 3,
  DISR_TCINT = 1 << 4,
  DICVR_CVRINTMASK = 1 << 1,
  DICVR_CVRINT = 1 << 2,
};

enum : uint32_t
{
  EVENT_FINISH,  // data: the Result
  EVENT_DRIVE,   // data: whether the drive command succeeded
  EVENT_BLOCK,   // data: index of the read's block that finished
};

constexpr uint32_t ECC_BLOCK_SIZE = 0x8000;
constexpr uint32_t WII_BLOCK_DATA_SIZE = 0x7C00;
constexpr uint64_t COMMAND_LATENCY_US = 300;
constexpr uint64_t READ_LATENCY_US = 600;
constexpr uint64_t BUFFER_TRANSFER_RATE = 32 * 1024 * 1024;

constexpr uint64_t AlignUp(uint64_t value, uint64_t size)
{
  return (value + size - 1) / size * size;
}

// A disc read: where it goes and how it splits into drive blocks.
struct DiscRead
{
  uint64_t offset;
  uint32_t length;
  uint32_t address;
  uint64_t partition;
  uint32_t block_size;

  uint32_t Blocks() const
  {
    if (length == 0)
      return 1;
    const uint64_t first_end = AlignUp(offset + 1, block_size);
    if (offset + length <= first_end)
      return 1;
    return 1 + static_cast<uint32_t>(AlignUp(offset + length - first_end, block_size) / block_size);
  }
  // Disc offset and length of block i.
  std::pair<uint64_t, uint32_t> Block(uint32_t i) const
  {
    const uint64_t first_end = AlignUp(offset + 1, block_size);
    const uint64_t start = i == 0 ? offset : first_end + uint64_t{i - 1} * block_size;
    const uint64_t end = std::min<uint64_t>(i == 0 ? first_end : start + block_size, offset + length);
    return {start, static_cast<uint32_t>(end - start)};
  }
};

class DiDevice final : public Device
{
public:
  DiDevice(const Host& host, const Config& config, uint32_t id)
      : Device(host, "/dev/di"), m_config(config), m_id(id)
  {
    // The console boots the game with the driver initialized and the game's
    // partition open.
    ResetRegisters();
    m_initialized = true;
    m_partition = m_config.disc->GamePartition();
  }

  std::optional<Reply> Open(const OpenRequest& request) override
  {
    InitializeIfFirstTime();
    return Device::Open(request);
  }

  std::optional<Reply> IOCtl(const IOCtlRequest&) override;
  std::optional<Reply> IOCtlV(const IOCtlVRequest&) override;
  void Event(uint32_t event, uint32_t data) override;

  void DoState(StateIO& io) override
  {
    DoBaseState(io);
    uint32_t count = static_cast<uint32_t>(m_queue.size());
    io.Do(count);
    if (io.Reading())
      m_queue.resize(count);
    for (uint32_t& address : m_queue)
      io.Do(address);
    io.Do(m_executing);
    io.Do(m_partition);
    io.Do(m_initialized);
    io.Do(m_last_length);
  }

  // The drive's reply to a read issued through the host's own drive model
  // (in flight when a state from before the native driver was loaded).
  void FinishHostCommand(bool success) { Finish(success ? Success : DriveError); }

private:
  // Dolphin's ExecutingCommandInfo, byte for byte.
  struct Executing
  {
    uint32_t address;
    uint8_t copy_immbuf;
    uint8_t padding[3];
  };

  void InitializeIfFirstTime()
  {
    if (!m_initialized)
    {
      ResetRegisters();
      m_initialized = true;
    }
  }
  void ResetRegisters()
  {
    m_disr = (m_disr & ~(DISR_TCINT | DISR_DEINT)) | DISR_TCINTMASK | DISR_DEINTMASK;
    m_dicvr &= ~DICVR_CVRINTMASK;
    m_partition = PARTITION_NONE;
  }

  void ProcessQueued();
  std::optional<Result> Start(const IOCtlRequest& request);
  void Finish(Result result);

  int64_t Microseconds(uint64_t us) const { return static_cast<int64_t>(us * (m_host.ticks_per_second / 1000000)); }
  void Schedule(int64_t cycles, uint32_t event, uint32_t data)
  {
    m_host.schedule(m_host.user, cycles, m_id, event, data);
  }

  std::optional<Result> WriteIfFits(const IOCtlRequest& request, uint32_t value)
  {
    if (request.buffer_out_size < 4)
      return SecurityError;
    m_memory.Write32(request.buffer_out, value);
    return Success;
  }

  // A drive command that transfers length bytes to the output buffer.
  std::optional<Result> CheckDma(const IOCtlRequest& request, uint32_t length)
  {
    if (request.buffer_out_size < length)
      return ReadTimedOut;
    if ((length & 0x1f) != 0 || (request.buffer_out & 0x1f) != 0)
      return BadArgument;
    m_last_length = length;
    return std::nullopt;
  }
  // The drive finishes a command without disc access.
  void DriveDone(bool success) { Schedule(Microseconds(COMMAND_LATENCY_US), EVENT_DRIVE, success); }
  void DriveFail(uint32_t error)
  {
    m_error = error;
    DriveDone(false);
  }
  bool CheckReadPreconditions()
  {
    switch (m_drive_state)
    {
    case DriveState::DiscChangeDetected:
      m_error = ERROR_MEDIUM_CHANGED;
      return false;
    case DriveState::MotorStopped:
      m_error = ERROR_MOTOR_STOPPED;
      return false;
    case DriveState::DiscIdNotRead:
      m_error = ERROR_NO_DISC_ID;
      return false;
    default:
      return true;
    }
  }

  std::optional<DiscRead> ReadOf(const IOCtlRequest& request) const;
  void StartRead(const DiscRead& read);
  void ResetDrive(bool spinup)
  {
    m_drive_state = spinup ? DriveState::DiscIdNotRead : DriveState::DiscChangeDetected;
    m_error = ERROR_NONE;
  }

  const Config& m_config;
  const uint32_t m_id;

  // Saved, in Dolphin's DIDevice order.
  std::deque<uint32_t> m_queue;
  std::optional<Executing> m_executing;
  uint64_t m_partition = PARTITION_NONE;
  bool m_initialized = false;
  uint32_t m_last_length = 0;

  // The drive. Never observable across a savestate except through a pending
  // error, which a game reads back right away.
  DriveState m_drive_state = DriveState::Ready;
  uint32_t m_error = ERROR_NONE;
  uint32_t m_immbuf = 0;
  uint32_t m_disr = 0;
  uint32_t m_dicvr = 0;
};

std::optional<Reply> DiDevice::IOCtl(const IOCtlRequest& request)
{
  InitializeIfFirstTime();
  const bool idle = !m_executing;
  m_queue.push_back(request.address);
  if (idle)
    ProcessQueued();
  return std::nullopt;
}

void DiDevice::ProcessQueued()
{
  m_executing = Executing{m_queue.front(), 0, {}};
  m_queue.pop_front();
  const IOCtlRequest request(m_memory, m_executing->address);
  if (const std::optional<Result> result = Start(request))
    Schedule(IPC_OVERHEAD, EVENT_FINISH, static_cast<uint32_t>(*result));
}

std::optional<DiscRead> DiDevice::ReadOf(const IOCtlRequest& request) const
{
  const uint32_t in = request.buffer_in;
  switch (request.request)
  {
  case DVDLowRead:
    return DiscRead{uint64_t{m_memory.Read32(in + 8)} << 2, m_memory.Read32(in + 4), request.buffer_out,
                m_partition,
                m_partition != PARTITION_NONE && m_config.disc->HasWiiHashes() ? WII_BLOCK_DATA_SIZE :
                                                                                ECC_BLOCK_SIZE};
  case DVDLowReadDiskID:
    return DiscRead{0, 0x20, request.buffer_out, PARTITION_NONE, ECC_BLOCK_SIZE};
  case DVDLowUnencryptedRead:
    return DiscRead{uint64_t{m_memory.Read32(in + 8)} << 2, m_memory.Read32(in + 4), request.buffer_out,
                PARTITION_NONE, ECC_BLOCK_SIZE};
  default:
    return std::nullopt;
  }
}

// Schedules each block's completion: the command latency, then every block at
// the buffer's transfer rate (the drive serves the whole disc from its buffer).
void DiDevice::StartRead(const DiscRead& read)
{
  if (!CheckReadPreconditions())
    return DriveDone(false);
  if (read.offset + read.length > m_config.disc->EndOffset())
    return DriveFail(ERROR_BLOCK_OOB);

  int64_t ticks = Microseconds(READ_LATENCY_US);
  const uint32_t blocks = read.Blocks();
  for (uint32_t i = 0; i < blocks; ++i)
  {
    ticks += static_cast<int64_t>(uint64_t{read.Block(i).second} * m_host.ticks_per_second /
                                  BUFFER_TRANSFER_RATE);
    Schedule(ticks, EVENT_BLOCK, i);
  }
}

std::optional<Result> DiDevice::Start(const IOCtlRequest& request)
{
  if (request.buffer_in_size != 0x20)
    return SecurityError;
  const uint32_t in = request.buffer_in;

  switch (request.request)
  {
  case DVDLowInquiry:
    if (auto error = CheckDma(request, 0x20))
      return error;
    m_memory.Write32(request.buffer_out, 0x00000002);  // revision, device code
    m_memory.Write32(request.buffer_out + 4, 0x20060526);  // release date
    m_memory.Write32(request.buffer_out + 8, 0x41000000);  // version
    DriveDone(true);
    return std::nullopt;
  case DVDLowReadDiskID:
    if (auto error = CheckDma(request, 0x20))
      return error;
    if (m_drive_state == DriveState::DiscIdNotRead)
      m_drive_state = DriveState::ReadyNoReadsMade;
    else if (m_drive_state == DriveState::ReadyNoReadsMade)
      m_drive_state = DriveState::Ready;
    StartRead(*ReadOf(request));
    return std::nullopt;
  case DVDLowRead:
  {
    const uint32_t length = m_memory.Read32(in + 4);
    if (m_partition == PARTITION_NONE || request.buffer_out_size < length)
      return SecurityError;
    m_last_length = m_memory.Read32(in + 8);  // an actual mistake in IOS
    if (m_drive_state == DriveState::ReadyNoReadsMade)
      m_drive_state = DriveState::Ready;
    StartRead(*ReadOf(request));
    return std::nullopt;
  }
  case DVDLowWaitForCoverClose:
    return CoverClosed;
  case DVDLowGetCoverRegister:
    return WriteIfFits(request, m_dicvr);
  case DVDLowNotifyReset:
    ResetRegisters();
    return Success;
  case DVDLowSetSpinupFlag:
    return BadArgument;
  case DVDLowReadDvdPhysical:
  case DVDLowReadDvdDiscKey:
    if (auto error = CheckDma(request, 0x800))
      return error;
    DriveFail(ERROR_INVALID_COMMAND);
    return std::nullopt;
  case DVDLowReadDvdCopyright:
    m_executing->copy_immbuf = 1;
    DriveFail(ERROR_INVALID_COMMAND);
    return std::nullopt;
  case DVDLowGetLength:
    return WriteIfFits(request, m_last_length);
  case DVDLowGetImmBuf:
    return WriteIfFits(request, m_immbuf);
  case DVDLowMaskCoverInterrupt:
    m_dicvr &= ~DICVR_CVRINTMASK;
    return Success;
  case DVDLowClearCoverInterrupt:
    m_dicvr &= ~DICVR_CVRINT;
    return Success;
  case DVDLowUnmaskStatusInterrupts:
    return Success;
  case DVDLowGetCoverStatus:
    return WriteIfFits(request, 2);  // disc inside
  case DVDLowUnmaskCoverInterrupt:
    m_dicvr |= DICVR_CVRINTMASK;
    return Success;
  case DVDLowReset:
    ResetDrive(m_memory.Read32(in + 4) != 0);
    ResetRegisters();
    return Success;
  case DVDLowClosePartition:
    m_partition = PARTITION_NONE;
    return Success;
  case DVDLowUnencryptedRead:
  {
    struct Range
    {
      uint32_t start, end;
      bool error_001;
    };
    constexpr std::array<Range, 3> ranges = {
        Range{0, 0x14000, false},  // system area
        Range{0x460A0000, 0x460A0008, true},
        Range{0x7ED40000, 0x7ED40008, true},
    };
    const uint32_t length = m_memory.Read32(in + 4);
    const uint32_t position = m_memory.Read32(in + 8);
    const uint32_t end = position + (length >> 2);
    for (const Range& range : ranges)
    {
      if (range.start > position || position > range.end || range.start > end || end > range.end)
        continue;
      if (range.error_001)
      {
        // Reads past the end of a pressed disc must fail (a disc that answers
        // is a copy); an extracted disc would answer.
        m_last_length = length;
        if (m_drive_state == DriveState::ReadyNoReadsMade)
          m_drive_state = DriveState::Ready;
        DriveFail(ERROR_BLOCK_OOB);
        return std::nullopt;
      }
      if (auto error = CheckDma(request, length))
        return error;
      if (m_drive_state == DriveState::ReadyNoReadsMade)
        m_drive_state = DriveState::Ready;
      StartRead(*ReadOf(request));
      return std::nullopt;
    }
    return SecurityError;
  }
  case DVDLowGetStatusRegister:
    return WriteIfFits(request, m_disr);
  case DVDLowGetControlRegister:
    return WriteIfFits(request, 0);
  case DVDLowReportKey:
  case DVDLowSerMeasControl:
    if (auto error = CheckDma(request, 0x20))
      return error;
    DriveFail(ERROR_INVALID_COMMAND);
    return std::nullopt;
  case DVDLowSeek:
    DriveDone(true);  // the drive seeks on the next read anyway
    return std::nullopt;
  case DVDLowReadDvd:
    if (auto error = CheckDma(request, 0x800 * m_memory.Read32(in + 12)))
      return error;
    DriveFail(ERROR_INVALID_COMMAND);
    return std::nullopt;
  case DVDLowReadDvdConfig:
  case DVDLowStopLaser:
  case DVDLowOffset:
  case DVDLowRequestDiscStatus:
  case DVDLowRequestRetryNumber:
    m_executing->copy_immbuf = 1;
    DriveFail(ERROR_INVALID_COMMAND);
    return std::nullopt;
  case DVDLowSetMaximumRotation:
    DriveFail(ERROR_INVALID_COMMAND);
    return std::nullopt;
  case DVDLowReadDiskBca:
    if (auto error = CheckDma(request, 0x40))
      return error;
    // Retail discs read back as a BCA of zeroes with this one bit set.
    m_memory.Fill(request.buffer_out, 0, 0x40);
    m_memory.Write8(request.buffer_out + 0x33, 1);
    DriveDone(true);
    return std::nullopt;
  case DVDLowRequestError:
  {
    m_executing->copy_immbuf = 1;
    const uint32_t state =
        m_drive_state == DriveState::Ready ? 0 : static_cast<uint32_t>(m_drive_state) - 1;
    m_immbuf = state << 24 | m_error;
    m_error = ERROR_NONE;
    DriveDone(true);
    return std::nullopt;
  }
  case DVDLowAudioStream:
  case DVDLowRequestAudioStatus:
  case DVDLowAudioBufferConfig:
    // The Wii's drive has no audio streaming buffer.
    m_executing->copy_immbuf = request.request == DVDLowAudioBufferConfig;
    if (!CheckReadPreconditions())
      DriveDone(false);
    else if (request.request == DVDLowAudioBufferConfig && m_drive_state == DriveState::Ready)
      DriveFail(ERROR_INVALID_PERIOD);
    else
      DriveFail(ERROR_NO_AUDIO_BUF);
    return std::nullopt;
  case DVDLowStopMotor:
    m_executing->copy_immbuf = 1;
    if (m_drive_state == DriveState::Ready || m_drive_state == DriveState::ReadyNoReadsMade ||
        m_drive_state == DriveState::DiscIdNotRead)
    {
      m_drive_state = DriveState::MotorStopped;
    }
    DriveDone(true);
    return std::nullopt;
  default:
    // DVDLowOpenPartition and the other partition commands are ioctlv-only.
    return SecurityError;
  }
}

void DiDevice::Event(uint32_t event, uint32_t data)
{
  switch (event)
  {
  case EVENT_FINISH:
    Finish(static_cast<Result>(data));
    break;
  case EVENT_DRIVE:
    Finish(data ? Success : DriveError);
    break;
  case EVENT_BLOCK:
  {
    if (!m_executing)
      break;
    const IOCtlRequest request(m_memory, m_executing->address);
    const std::optional<DiscRead> read = ReadOf(request);
    if (!read)
      break;
    const auto [offset, length] = read->Block(data);
    // A block bound for unmapped memory is read and dropped, like a DMA there.
    uint8_t* out = m_memory.Pointer(read->address + static_cast<uint32_t>(offset - read->offset), length);
    const bool ok = !out || m_config.disc->Read(offset, length, out, read->partition);
    if (!ok)
      m_error = ERROR_READ;
    if (data + 1 == read->Blocks())
      Finish(ok ? Success : DriveError);
    break;
  }
  }
}

void DiDevice::Finish(Result result)
{
  if (!m_executing)
    return;
  const IOCtlRequest request(m_memory, m_executing->address);
  if (m_executing->copy_immbuf)
    m_memory.Write32(request.buffer_out, m_immbuf);
  m_host.reply(m_host.user, request.address, result, 0);
  m_executing.reset();
  if (!m_queue.empty())
    ProcessQueued();
}

std::optional<Reply> DiDevice::IOCtlV(const IOCtlVRequest& request)
{
  InitializeIfFirstTime();
  if (request.in.empty() || request.in[0].size != 0x20)
    return Reply{BadArgument};

  // Games open their partition through the system menu's boot; only the
  // partition change itself is supported.
  if (request.request == DVDLowOpenPartition && request.in.size() == 3 && request.io.size() == 2 &&
      m_config.open_partition)
  {
    m_partition = uint64_t{m_memory.Read32(request.in[0].address + 4)} << 2;
    const int32_t es_result =
        m_config.open_partition(m_host.user, m_partition, request.io[0].address);
    m_memory.Write32(request.io[1].address, static_cast<uint32_t>(es_result));
    return Reply{Success};
  }
  return Reply{BadArgument};
}
}  // namespace

std::unique_ptr<Device> CreateDi(const Host& host, const Config& config, uint32_t id)
{
  return std::make_unique<DiDevice>(host, config, id);
}

void FinishHostDiCommand(Device& di, bool success)
{
  static_cast<DiDevice&>(di).FinishHostCommand(success);
}
}  // namespace dualize::ios
