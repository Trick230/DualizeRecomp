#include "apu/native_dsp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <thread>
#include <utility>

// Bit-for-bit the behaviour of Dolphin's DSP interface and DSP HLE (DSP.cpp,
// DSPHLE, MailHandler, the ROM, INIT and AXWii ucodes, the accelerator), so
// emulated audio and savestates stay identical with either one running. One
// difference: AX mixes each command list on a thread of its own, and its
// mails reach the CPU a fixed DSP time later (DspHost::schedule_mix_done).

namespace dualize::apu
{
namespace
{
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// How long an AX command list takes the DSP, in 729 MHz CPU ticks (half a
// millisecond, a busy DSP on hardware): its mails arrive then.
constexpr int64_t AX_MIX_CYCLES = 729000000 / 2000;

constexpr u32 UCODE_ROM = 0x00000000;
constexpr u32 UCODE_INIT_AUDIO_SYSTEM = 0x00000001;
constexpr u32 UCODE_NULL = 0xFFFFFFFF;

constexpr u32 TASK_MAIL_MASK = 0xFFFF0000;
constexpr u32 TASK_MAIL_TO_CPU = 0xDCD10000;
constexpr u32 DSP_INIT = TASK_MAIL_TO_CPU | 0x0000;
constexpr u32 DSP_RESUME = TASK_MAIL_TO_CPU | 0x0001;
constexpr u32 DSP_YIELD = TASK_MAIL_TO_CPU | 0x0002;
constexpr u32 DSP_SYNC = TASK_MAIL_TO_CPU | 0x0004;
constexpr u32 TASK_MAIL_TO_DSP = 0xCDD10000;
constexpr u32 MAIL_RESUME = TASK_MAIL_TO_DSP | 0x0000;
constexpr u32 MAIL_NEW_UCODE = TASK_MAIL_TO_DSP | 0x0001;
constexpr u32 MAIL_RESET = TASK_MAIL_TO_DSP | 0x0002;
constexpr u32 MAIL_CONTINUE = TASK_MAIL_TO_DSP | 0x0003;
constexpr u32 MAIL_CMDLIST = 0xBABE0000;
constexpr u32 MAIL_CMDLIST_MASK = 0xFFFF0000;

// Interface registers (offsets within the 0xCC005000 block).
enum : u32
{
  DSP_MAIL_TO_DSP_HI = 0x5000,
  DSP_MAIL_TO_DSP_LO = 0x5002,
  DSP_MAIL_FROM_DSP_HI = 0x5004,
  DSP_MAIL_FROM_DSP_LO = 0x5006,
  DSP_CONTROL = 0x500A,
  AR_INFO = 0x5012,
  AR_MODE = 0x5016,
  AR_REFRESH = 0x501a,
  AR_DMA_MMADDR_H = 0x5020,
  AR_DMA_MMADDR_L = 0x5022,
  AR_DMA_ARADDR_H = 0x5024,
  AR_DMA_ARADDR_L = 0x5026,
  AR_DMA_CNT_H = 0x5028,
  AR_DMA_CNT_L = 0x502A,
  AUDIO_DMA_START_HI = 0x5030,
  AUDIO_DMA_START_LO = 0x5032,
  AUDIO_DMA_CONTROL_LEN = 0x5036,
  AUDIO_DMA_BLOCKS_LEFT = 0x503A,
};

// DSP_CONTROL bits.
constexpr u16 CTRL_RESET = 1 << 0;
constexpr u16 CTRL_HALT = 1 << 2;
constexpr u16 CTRL_AID = 1 << 3;
constexpr u16 CTRL_ARAM = 1 << 5;
constexpr u16 CTRL_DSP = 1 << 7;
constexpr u16 CTRL_DMA_STATE = 1 << 9;
constexpr u16 CTRL_INIT_CODE = 1 << 10;
constexpr u16 CTRL_INIT = 1 << 11;
// Bits the DSP emulator owns.
constexpr u16 DSP_CONTROL_MASK = 0x0C07;
// Bits a control write copies: reset, assert, halt, init code, init, the
// three interrupt masks and the padding.
constexpr u16 CTRL_WRITTEN = 0xF000 | 0x0C07 | 0x0150;
constexpr u16 CTRL_INTERRUPTS = DSP_INT_DSP | DSP_INT_ARAM | DSP_INT_AID;

constexpr u16 AUDIO_DMA_ENABLE = 0x8000;
constexpr u16 AUDIO_DMA_BLOCKS = 0x7FFF;

inline u16 Load16(const u8* p)
{
  u16 v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline u32 Load32(const u8* p)
{
  u32 v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void Store16(u8* p, u16 v)
{
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void Store32(u8* p, u32 v)
{
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline u16* HighPart(u32* v)
{
  return reinterpret_cast<u16*>(v) + 1;
}
inline u16* LowPart(u32* v)
{
  return reinterpret_cast<u16*>(v);
}

u32 HashEctor(const u8* data, size_t len)
{
  u32 crc = 0;
  for (size_t i = 0; i < len; i++)
  {
    crc ^= data[i];
    crc = std::rotl(crc, 3);
  }
  return crc;
}

u32 Adler32(const u8* data, size_t len)
{
  u32 a = 1, b = 0;
  for (size_t i = 0; i < len; i++)
  {
    a = (a + data[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

inline s16 ClampS16(s64 sample)
{
  return static_cast<s16>(std::clamp<s64>(sample, -0x8000, 0x7FFF));
}

class State
{
public:
  explicit State(const DspStateIO& io) : m_io(io) {}
  bool Reading() const { return m_io.reading; }
  template <typename T>
  void Do(T& value)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    m_io.bytes(m_io.user, &value, sizeof(value));
  }
  void Do(bool& value)
  {
    u8 stable = value;
    m_io.bytes(m_io.user, &stable, 1);
    if (m_io.reading)
      value = stable != 0;
  }
  void Do(std::optional<u32>& value)
  {
    bool present = value.has_value();
    Do(present);
    u32 v = value.value_or(0);
    if (present)
      Do(v);
    if (m_io.reading)
      value = present ? std::optional<u32>(v) : std::nullopt;
  }

private:
  const DspStateIO& m_io;
};

// ---------------------------------------------------------------------------
// AX parameter blocks (Wii layout), as u16 words in host order.

struct VolumeData
{
  u16 volume;
  u16 volume_delta;
};
struct PBMixerWii
{
  VolumeData main_left, main_right;
  VolumeData auxA_left, auxA_right;
  VolumeData auxB_left, auxB_right;
  VolumeData auxC_left, auxC_right;
  VolumeData main_surround;
  VolumeData auxA_surround;
  VolumeData auxB_surround;
  VolumeData auxC_surround;
};
struct PBMixerWM
{
  VolumeData main0, aux0;
  VolumeData main1, aux1;
  VolumeData main2, aux2;
  VolumeData main3, aux3;
};
struct PBInitialTimeDelay
{
  u16 on;
  u16 addrMemHigh;
  u16 addrMemLow;
  u16 offsetLeft;
  u16 offsetRight;
  u16 targetLeft;
  u16 targetRight;
};
struct PBUpdate
{
  u16 pb_offset;
  u16 new_value;
};
using PBUpdateData = std::array<PBUpdate, 32>;
struct PBUpdatesWii
{
  u16 num_updates[3];
  u16 data_hi;
  u16 data_lo;
};
struct PBDpopWii
{
  s16 main_left;
  s16 auxA_left;
  s16 auxB_left;
  s16 auxC_left;
  s16 main_right;
  s16 auxA_right;
  s16 auxB_right;
  s16 auxC_right;
  s16 main_surround;
  s16 auxA_surround;
  s16 auxB_surround;
  s16 auxC_surround;
};
struct PBDpopWM
{
  s16 main0;
  s16 main1;
  s16 main2;
  s16 main3;
  s16 aux0;
  s16 aux1;
  s16 aux2;
  s16 aux3;
};
struct PBVolumeEnvelope
{
  s16 cur_volume;
  s16 cur_volume_delta;
};
struct PBAudioAddr
{
  u16 looping;
  u16 sample_format;
  u16 loop_addr_hi;
  u16 loop_addr_lo;
  u16 end_addr_hi;
  u16 end_addr_lo;
  u16 cur_addr_hi;
  u16 cur_addr_lo;
};
struct PBADPCMInfo
{
  s16 coefs[16];
  u16 gain;
  u16 pred_scale;
  s16 yn1;
  s16 yn2;
};
struct PBSampleRateConverter
{
  u16 ratio_hi;
  u16 ratio_lo;
  u16 cur_addr_frac;
  s16 last_samples[4];
};
struct PBSampleRateConverterWM
{
  u16 cur_addr_frac;
  s16 last_samples[4];
};
struct PBADPCMLoopInfo
{
  u16 pred_scale;
  u16 yn1;
  u16 yn2;
};
struct PBLowPassFilter
{
  u16 on;
  s16 yn1;
  u16 a0;
  s16 b0;
};
struct PBHighPassFilter
{
  u16 on;
  u16 unk[3];
};
struct PBBiquadFilter
{
  u16 on;
  s16 xn1;
  s16 xn2;
  s16 yn1;
  s16 yn2;
  s16 b0;
  s16 b1;
  s16 b2;
  s16 a1;
  s16 a2;
};
union PBInfImpulseResponseWM
{
  u16 on;  // 0: off, 2: biquad, other: low-pass
  PBLowPassFilter lpf;
  PBBiquadFilter biquad;
};
struct AXPBWii
{
  u16 next_pb_hi;
  u16 next_pb_lo;
  u16 this_pb_hi;
  u16 this_pb_lo;
  u16 src_type;
  u16 coef_select;
  u16 mixer_control_hi;
  u16 mixer_control_lo;
  u16 running;
  u16 is_stream;
  PBMixerWii mixer;
  PBInitialTimeDelay initial_time_delay;
  PBDpopWii dpop;
  PBUpdatesWii updates;  // not present in every version
  PBVolumeEnvelope vol_env;
  PBAudioAddr audio_addr;
  PBADPCMInfo adpcm;
  PBSampleRateConverter src;
  PBADPCMLoopInfo adpcm_loop_info;
  PBLowPassFilter lpf;
  union
  {
    PBHighPassFilter hpf;
    PBBiquadFilter biquad;
  };
  u16 remote;
  u16 remote_mixer_control;
  PBMixerWM remote_mixer;
  PBDpopWM remote_dpop;
  PBSampleRateConverterWM remote_src;
  PBInfImpulseResponseWM remote_iir;
  u16 pad[2];
};
static_assert(sizeof(AXPBWii) == 310);  // Dolphin's layout, updates included

enum
{
  SRCTYPE_POLYPHASE = 0,
  SRCTYPE_LINEAR = 1,
};

enum AXMixControl : u32
{
  MIX_MAIN_L = 0x000001,
  MIX_MAIN_L_RAMP = 0x000002,
  MIX_MAIN_R = 0x000004,
  MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S = 0x000010,
  MIX_MAIN_S_RAMP = 0x000020,
  MIX_AUXA_L = 0x000040,
  MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R = 0x000100,
  MIX_AUXA_R_RAMP = 0x000200,
  MIX_AUXA_S = 0x000400,
  MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000,
  MIX_AUXB_L_RAMP = 0x002000,
  MIX_AUXB_R = 0x004000,
  MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S = 0x010000,
  MIX_AUXB_S_RAMP = 0x020000,
  MIX_AUXC_L = 0x040000,
  MIX_AUXC_L_RAMP = 0x080000,
  MIX_AUXC_R = 0x100000,
  MIX_AUXC_R_RAMP = 0x200000,
  MIX_AUXC_S = 0x400000,
  MIX_AUXC_S_RAMP = 0x800000,
};

u32 ConvertMixerControl(u32 mixer_control)
{
  u32 ret = 0;
  if (mixer_control & 0x00000001)
    ret |= MIX_MAIN_L;
  if (mixer_control & 0x00000002)
    ret |= MIX_MAIN_R;
  if (mixer_control & 0x00000004)
    ret |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
  if (mixer_control & 0x00000008)
    ret |= MIX_MAIN_S;
  if (mixer_control & 0x00000010)
    ret |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
  if (mixer_control & 0x00010000)
    ret |= MIX_AUXA_L;
  if (mixer_control & 0x00020000)
    ret |= MIX_AUXA_R;
  if (mixer_control & 0x00040000)
    ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
  if (mixer_control & 0x00080000)
    ret |= MIX_AUXA_S;
  if (mixer_control & 0x00100000)
    ret |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
  if (mixer_control & 0x00200000)
    ret |= MIX_AUXB_L;
  if (mixer_control & 0x00400000)
    ret |= MIX_AUXB_R;
  if (mixer_control & 0x00800000)
    ret |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
  if (mixer_control & 0x01000000)
    ret |= MIX_AUXB_S;
  if (mixer_control & 0x02000000)
    ret |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
  if (mixer_control & 0x04000000)
    ret |= MIX_AUXC_L;
  if (mixer_control & 0x08000000)
    ret |= MIX_AUXC_R;
  if (mixer_control & 0x10000000)
    ret |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
  if (mixer_control & 0x20000000)
    ret |= MIX_AUXC_S;
  if (mixer_control & 0x40000000)
    ret |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
  return ret;
}

void GenerateVolumeRamp(u16* output, u16 vol1, u16 vol2, size_t nvals)
{
  float curr = vol1;
  for (size_t i = 0; i < nvals; ++i)
  {
    curr += (vol2 - vol1) / (float)nvals;
    output[i] = (u16)curr;
  }
}

// Resamples count output samples from next() at ratio (16.16), carrying
// the four-sample history and the fractional position across calls.
template <typename Next>
u32 ResampleAudio(Next&& next, s16* output, u32 count, s16* last_samples, u32 curr_pos, u32 ratio,
                  int srctype, const s16* coeffs)
{
  if (coeffs && srctype == SRCTYPE_POLYPHASE)
  {
    s16 temp[4];
    u32 idx = 0;
    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];
    for (u32 i = 0; i < count; ++i)
    {
      curr_pos += ratio;
      while (curr_pos >= 0x10000)
      {
        temp[idx++ & 3] = next();
        curr_pos -= 0x10000;
      }
      const u16 curr_pos_frac = ((curr_pos & 0xFFFF) >> 9) << 2;
      const s16* c = &coeffs[curr_pos_frac];
      const s64 t0 = temp[idx++ & 3];
      const s64 t1 = temp[idx++ & 3];
      const s64 t2 = temp[idx++ & 3];
      const s64 t3 = temp[idx++ & 3];
      const s64 samp = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;
      output[i] = ClampS16(samp);
    }
    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  }
  else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE)
  {
    s16 temp[4];
    u32 idx = 0;
    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];
    for (u32 i = 0; i < count; ++i)
    {
      curr_pos += ratio;
      while (curr_pos >= 0x10000)
      {
        temp[idx++ & 3] = next();
        curr_pos -= 0x10000;
      }
      const u16 curr_frac = curr_pos & 0xFFFF;
      const u16 inv_curr_frac = -curr_frac;
      s16 sample;
      if (curr_frac)
      {
        const s32 s0 = temp[idx++ & 3];
        const s32 s1 = temp[idx++ & 3];
        sample = ((s0 * inv_curr_frac) + (s1 * curr_frac)) >> 16;
        idx += 2;
      }
      else
      {
        sample = temp[idx++ & 3];
        idx += 3;
      }
      output[i] = sample;
    }
    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  }
  else  // nearest
  {
    for (u32 i = 0; i < count; ++i)
      output[i] = next();
    std::memcpy(last_samples, output + count - 4, 4 * sizeof(u16));
  }
  return curr_pos;
}

void MixAdd(int* out, const s16* input, u32 count, VolumeData* vd, s16* dpop, bool ramp)
{
  u16& volume = vd->volume;
  const u16 volume_delta = ramp ? vd->volume_delta : 0;
  for (u32 i = 0; i < count; ++i)
  {
    s64 sample = input[i];
    sample *= volume;
    sample >>= 15;
    const s16 sample16 = ClampS16((s32)sample);
    out[i] += sample16;
    volume += volume_delta;
    *dpop = sample16;
  }
}

void LowPassFilter(s16* samples, u32 count, PBLowPassFilter& f)
{
  for (u32 i = 0; i < count; ++i)
    f.yn1 = samples[i] = ClampS16((f.a0 * (s32)samples[i] + f.b0 * (s32)f.yn1) >> 15);
}

void BiquadFilter(s16* samples, u32 count, PBBiquadFilter& f)
{
  for (u32 i = 0; i < count; ++i)
  {
    const s16 xn0 = samples[i];
    s64 tmp = 0;
    tmp += f.b0 * s32(xn0);
    tmp += f.b1 * s32(f.xn1);
    tmp += f.b2 * s32(f.xn2);
    tmp += f.a1 * s32(f.yn1);
    tmp += f.a2 * s32(f.yn2);
    tmp <<= 2;
    if (tmp & 0x10000)
      tmp += 0x8000;
    else
      tmp += 0x7FFF;
    tmp >>= 16;
    const s16 yn0 = ClampS16(tmp);
    f.xn2 = f.xn1;
    f.yn2 = f.yn1;
    f.xn1 = xn0;
    f.yn1 = yn0;
    samples[i] = yn0;
  }
}

constexpr u32 START_END_ADDRESS_MASK = 0x3fffffff;
constexpr u32 CURRENT_ADDRESS_MASK = 0xbfffffff;

// The DSP's sample accelerator as AX drives it: ARAM reads with ADPCM or PCM
// decoding, stopping (or looping, per the voice) at the end address.
struct Accelerator
{
  u32 start_address = 0;
  u32 end_address = 0;
  u32 current_address = 0;
  u16 sample_format = 0;
  s16 gain = 0;
  s16 yn1 = 0;
  s16 yn2 = 0;
  u16 pred_scale = 0;
  u16 input = 0;
  bool reads_stopped = false;

  void SetYn2(s16 value)
  {
    yn2 = value;
    reads_stopped = false;
  }
  void SetPredScale(u16 value) { pred_scale = value & 0x7f; }

  void DoState(State& p)
  {
    p.Do(start_address);
    p.Do(end_address);
    p.Do(current_address);
    p.Do(sample_format);
    p.Do(yn1);
    p.Do(yn2);
    p.Do(pred_scale);
    p.Do(reads_stopped);
  }
};

struct NextUCodeInfo
{
  u32 mram_dest_addr;
  u16 mram_size;
  u16 mram_dram_addr;
  u32 iram_mram_addr;
  u16 iram_size;
  u16 iram_dest;
  u16 iram_startpc;
  u32 dram_mram_addr;
  u16 dram_size;
  u16 dram_dest;
};

struct RomBootInfo
{
  u32 ram_address;
  u32 length;
  u32 imem_address;
  u32 dmem_length;
  u32 start_pc;
};

enum class MailState : int
{
  WaitingForCmdListSize,
  WaitingForCmdListAddress,
  WaitingForNextTask,
};

// AX for Wii: the mixing state of one ucode instance.
struct AxWii
{
  int main_left[32 * 5]{};
  int main_right[32 * 5]{};
  int main_surround[32 * 5]{};
  int auxA_left[32 * 5]{};
  int auxA_right[32 * 5]{};
  int auxA_surround[32 * 5]{};
  int auxB_left[32 * 5]{};
  int auxB_right[32 * 5]{};
  int auxB_surround[32 * 5]{};
  int auxC_left[32 * 3]{};
  int auxC_right[32 * 3]{};
  int auxC_surround[32 * 3]{};
  int wm0[6 * 3]{};
  int aux0[6 * 3]{};
  int wm1[6 * 3]{};
  int aux1[6 * 3]{};
  int wm2[6 * 3]{};
  int aux2[6 * 3]{};
  int wm3[6 * 3]{};
  int aux3[6 * 3]{};

  u16 cmdlist[512]{};
  u32 cmdlist_size = 0;
  MailState mail_state = MailState::WaitingForCmdListSize;
  std::optional<u32> coeffs_checksum;
  std::array<s16, 0x800> coeffs{};
  u16 compressor_pos = 0;
  Accelerator accelerator;
  AXPBWii* acc_pb = nullptr;

  bool old_axwii = false;
  bool new_filter = false;
  u16 last_main_volume = 0x8000;
  u16 last_aux_volumes[3] = {0x8000, 0x8000, 0x8000};
};

struct Ucode
{
  enum class Kind
  {
    Rom,
    Init,
    AxWii,
  };

  Ucode(u32 crc_, Kind kind_) : crc(crc_), kind(kind_)
  {
    if (kind == Kind::AxWii)
    {
      ax = std::make_unique<AxWii>();
      ax->old_axwii = crc == 0xfa450138 || crc == 0x7699af32;
      ax->new_filter = crc == 0x347112ba || crc == 0x4cc52064;
    }
  }

  u32 crc;
  Kind kind;

  // UCodeInterface
  bool upload_setup_in_progress = false;
  NextUCodeInfo next_ucode{};
  int next_ucode_steps = 0;
  bool needs_resume_mail = false;

  // ROMUCode
  RomBootInfo rom{};
  int rom_boot_task_num_steps = 0;
  u32 rom_next_parameter = 0;

  std::unique_ptr<AxWii> ax;
};
}  // namespace

struct DspCore::Impl
{
  struct AudioDma
  {
    u32 current_source_address = 0;
    u16 remaining_blocks_count = 0;
    u32 source_address = 0;
    u16 control = 0;
  };
  struct AramDma
  {
    u32 mm_addr = 0;
    u32 ar_addr = 0;
    u32 cnt = 0;  // count : 31, dir : 1 (0: MRAM -> ARAM)
  };
  static_assert(sizeof(AudioDma) == 16 && sizeof(AramDma) == 12);

  DspHost host;
  std::vector<std::string> coefficient_paths;
  u32 ram_mask;
  u32 exram_mask;

  // DSP interface (DSPManager)
  AudioDma audio_dma;
  AramDma aram_dma;
  u16 dsp_control = CTRL_HALT;
  u16 aram_info = 0;
  u16 aram_mode = 1;      // ARAM controller has initialised
  u16 aram_refresh = 156;  // 156 MHz
  int dsp_slice = 0;

  // DSP HLE
  u16 hle_control = CTRL_HALT | CTRL_INIT;
  u64 init_code_clear_time = 0;
  u32 cpu_mailbox = 0;
  u32 dsp_mailbox = 0;
  std::unique_ptr<Ucode> ucode;
  std::unique_ptr<Ucode> last_ucode;
  u32 reported_crc = UCODE_NULL;

  // Mail handler
  std::deque<std::pair<u32, bool>> pending_mails;
  u32 last_mail = 0;
  bool halted = false;

  // The AX mixer thread. A command list runs there from the moment its
  // address arrives, and the mails it sends are held until the mix is due.
  // What touches the ucode's state (a mail, a new ucode, an ARAM DMA, a
  // savestate) waits for it first; the audio DMA plays the other of AX's two
  // output buffers, so it never does.
  enum MixState : u32
  {
    MIX_IDLE,
    MIX_QUEUED,
    MIX_QUIT,
  };
  std::atomic<u32> mix_state{MIX_IDLE};
  bool mixing = false;  // set on the mixer thread while it runs a list
  std::vector<std::pair<u32, bool>> held_mails;
  std::thread mixer;

  static u32 MaskFor(u32 size)
  {
    return size ? std::bit_ceil(size) - 1 : 0;
  }

  Impl(const DspHost& host_, std::vector<std::string> paths)
      : host(host_), coefficient_paths(std::move(paths)), ram_mask(MaskFor(host_.ram_size)),
        exram_mask(MaskFor(host_.exram_size))
  {
    SetUCode(UCODE_ROM);
    halted = (hle_control & CTRL_HALT) != 0;
    mixer = std::thread([this] { MixLoop(); });
  }

  ~Impl()
  {
    JoinMix();
    mix_state.store(MIX_QUIT, std::memory_order_release);
    mix_state.notify_one();
    mixer.join();
  }

  // ------------------------------------------------------------- mixer

  void MixLoop()
  {
    for (;;)
    {
      mix_state.wait(MIX_IDLE, std::memory_order_acquire);
      if (mix_state.load(std::memory_order_acquire) == MIX_QUIT)
        return;
      mixing = true;
      HandleCommandList(*ucode);
      PushMail(DSP_YIELD, true);
      mixing = false;
      mix_state.store(MIX_IDLE, std::memory_order_release);
      mix_state.notify_one();
    }
  }

  void JoinMix()
  {
    while (mix_state.load(std::memory_order_acquire) == MIX_QUEUED)
      mix_state.wait(MIX_QUEUED, std::memory_order_acquire);
  }

  // The mix is due: what it mailed reaches the CPU.
  void ReleaseMix()
  {
    JoinMix();
    for (const auto& [mail, interrupt] : held_mails)
      PushMail(mail, interrupt);
    held_mails.clear();
  }

  // ------------------------------------------------------------- memory

  // Dolphin's GetPointerForRange: MEM1 or MEM2, any mirror; nullptr when the
  // range leaves the region.
  u8* Span(u32 address, u32 size) const
  {
    address &= 0x3FFFFFFF;
    if (address < host.ram_size)
      return size <= host.ram_size - address ? host.ram + address : nullptr;
    if (host.exram && (address >> 28) == 0x1 && (address & 0x0fffffff) < host.exram_size)
    {
      const u32 offset = address & exram_mask;
      return size <= host.exram_size - offset ? host.exram + offset : nullptr;
    }
    return nullptr;
  }

  void CopyFromSwapped16(u16* dst, u32 address, u32 size) const
  {
    const u8* src = Span(address, size);
    if (!src)
      return;
    for (u32 i = 0; i < size / 2; ++i)
      dst[i] = Load16(src + 2 * i);
  }
  void CopyToSwapped16(u32 address, const u16* src, u32 size) const
  {
    u8* dst = Span(address, size);
    if (!dst)
      return;
    for (u32 i = 0; i < size / 2; ++i)
      Store16(dst + 2 * i, src[i]);
  }
  void CopyToSwapped32(u32 address, const int* src, u32 size) const
  {
    u8* dst = Span(address, size);
    if (!dst)
      return;
    for (u32 i = 0; i < size / 4; ++i)
      Store32(dst + 4 * i, static_cast<u32>(src[i]));
  }
  void CopyFromSwapped32(int* dst, u32 address, u32 size) const
  {
    const u8* src = Span(address, size);
    if (!src)
      return;
    for (u32 i = 0; i < size / 4; ++i)
      dst[i] = static_cast<int>(Load32(src + 4 * i));
  }

  // On Wii the DSP reads EXRAM through bit 28 and MEM1 otherwise.
  u8 ReadAram(u32 address) const
  {
    if (address & 0x10000000)
      return host.exram[address & exram_mask];
    const u32 physical = address & ram_mask;
    return physical < host.ram_size ? host.ram[physical] : 0;
  }

  // ------------------------------------------------------------- interrupts

  void UpdateInterrupts()
  {
    const bool set = ((dsp_control >> 1) & dsp_control & CTRL_INTERRUPTS) != 0;
    host.set_interrupt(host.user, set);
  }

  void GenerateInterrupt(u32 type)
  {
    dsp_control |= type & CTRL_INTERRUPTS;
    UpdateInterrupts();
  }

  // ------------------------------------------------------------- mail

  void PushMail(u32 mail, bool interrupt = false, int cycles_into_future = 0)
  {
    if (mixing)
    {
      held_mails.emplace_back(mail, interrupt);
      return;
    }
    if (interrupt)
    {
      if (pending_mails.empty())
        host.schedule_interrupt(host.user, cycles_into_future, DSP_INT_DSP);
      else
        pending_mails.front().second = true;
    }
    pending_mails.emplace_back(mail, false);
  }

  u16 ReadMailHigh()
  {
    if (!halted && !pending_mails.empty())
      last_mail = pending_mails.front().first;
    return u16(last_mail >> 16);
  }

  u16 ReadMailLow()
  {
    if (!halted && !pending_mails.empty())
    {
      last_mail = pending_mails.front().first;
      const bool generate_interrupt = pending_mails.front().second;
      pending_mails.pop_front();
      if (generate_interrupt)
        host.schedule_interrupt(host.user, 0, DSP_INT_DSP);
    }
    last_mail &= ~0x80000000u;
    return u16(last_mail & 0xffff);
  }

  // ------------------------------------------------------------- ucodes

  std::unique_ptr<Ucode> MakeUCode(u32 crc)
  {
    switch (crc)
    {
    case UCODE_NULL:
      return nullptr;
    case UCODE_ROM:
      return std::make_unique<Ucode>(crc, Ucode::Kind::Rom);
    case UCODE_INIT_AUDIO_SYSTEM:
      return std::make_unique<Ucode>(crc, Ucode::Kind::Init);
    default:
      if (crc != reported_crc)
      {
        reported_crc = crc;
        std::fprintf(stderr, "native DSP: AX ucode %08x\n", crc);
      }
      return std::make_unique<Ucode>(crc, Ucode::Kind::AxWii);
    }
  }

  void InitializeUCode(Ucode& u)
  {
    switch (u.kind)
    {
    case Ucode::Kind::Rom:
      PushMail(0x8071FEED);
      break;
    case Ucode::Kind::Init:
      PushMail(0x80544348);
      break;
    case Ucode::Kind::AxWii:
      PushMail(DSP_INIT, true);
      LoadCoefficients(*u.ax, false, 0);
      break;
    }
  }

  void SetUCode(u32 crc)
  {
    JoinMix();
    pending_mails.clear();
    ucode = MakeUCode(crc);
    InitializeUCode(*ucode);
  }

  void SwapUCode(u32 crc)
  {
    pending_mails.clear();
    if (last_ucode && last_ucode->crc == crc)
    {
      ucode = std::move(last_ucode);
    }
    else
    {
      if (!last_ucode)
        last_ucode = std::move(ucode);
      ucode = MakeUCode(crc);
      InitializeUCode(*ucode);
    }
  }

  bool LoadCoefficients(AxWii& ax, bool require_same_checksum, u32 desired_checksum)
  {
    constexpr size_t raw_size = 0x800 * 2;
    static constexpr std::array<u8, raw_size> builtin = {
#include "apu/dsp_coef.inc"
    };
    const auto use = [&](const u8* raw) {
      const u32 checksum = Adler32(raw, raw_size);
      if (require_same_checksum && checksum != desired_checksum)
        return false;
      for (size_t i = 0; i < ax.coeffs.size(); ++i)
        ax.coeffs[i] = static_cast<s16>(Load16(&raw[2 * i]));
      ax.coeffs_checksum = checksum;
      return true;
    };
    ax.coeffs_checksum = std::nullopt;
    // A dump of the console's coefficient ROM, if the user has one, wins.
    for (const std::string& path : coefficient_paths)
    {
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      if (!file || static_cast<size_t>(file.tellg()) != raw_size)
        continue;
      std::array<u8, raw_size> raw;
      file.seekg(0);
      file.read(reinterpret_cast<char*>(raw.data()), raw_size);
      if (use(raw.data()))
        return true;
    }
    return use(builtin.data());
  }

  // Returns true when the mail replaced the running ucode.
  bool PrepareBootUCode(Ucode& u, u32 mail)
  {
    NextUCodeInfo& n = u.next_ucode;
    switch (u.next_ucode_steps)
    {
    case 0:
      n.mram_dest_addr = mail;
      break;
    case 1:
      n.mram_size = mail & 0xffff;
      break;
    case 2:
      n.mram_dram_addr = mail & 0xffff;
      break;
    case 3:
      n.iram_mram_addr = mail;
      break;
    case 4:
      n.iram_size = mail & 0xffff;
      break;
    case 5:
      n.iram_dest = mail & 0xffff;
      break;
    case 6:
      n.iram_startpc = mail & 0xffff;
      break;
    case 7:
      n.dram_mram_addr = mail;
      break;
    case 8:
      n.dram_size = mail & 0xffff;
      break;
    case 9:
      n.dram_dest = mail & 0xffff;
      break;
    }
    u.next_ucode_steps++;
    if (u.next_ucode_steps != 10)
      return false;
    u.next_ucode_steps = 0;
    u.needs_resume_mail = true;
    u.upload_setup_in_progress = false;
    const u8* code = Span(n.iram_mram_addr, n.iram_size);
    SwapUCode(code ? HashEctor(code, n.iram_size) : 0);
    return true;
  }

  void HandleMail(u32 mail)
  {
    if (!ucode)
      return;
    Ucode& u = *ucode;
    switch (u.kind)
    {
    case Ucode::Kind::Rom:
      HandleRomMail(u, mail);
      break;
    case Ucode::Kind::Init:
      break;
    case Ucode::Kind::AxWii:
      HandleAxMail(u, mail);
      break;
    }
  }

  void HandleRomMail(Ucode& u, u32 mail)
  {
    if (u.rom_next_parameter == 0)
    {
      if ((mail & 0xFFFF0000) != 0x80F30000)
        PushMail(0xFEEE0000 | (mail & 0xFFFF));
      else
        u.rom_next_parameter = mail;
      return;
    }
    switch (u.rom_next_parameter)
    {
    case 0x80F3A001:
      u.rom.ram_address = mail;
      break;
    case 0x80F3A002:
      u.rom.length = mail & 0xffff;
      break;
    case 0x80F3B002:
      u.rom.dmem_length = mail & 0xffff;
      break;
    case 0x80F3C002:
      u.rom.imem_address = mail & 0xffff;
      break;
    case 0x80F3D001:
    {
      u.rom.start_pc = mail & 0xffff;
      const u8* code = Span(u.rom.ram_address, u.rom.length);
      SetUCode(code ? HashEctor(code, u.rom.length) : 0);
      return;  // u is gone
    }
    default:
      break;
    }
    u.rom_next_parameter = 0;
  }

  void HandleAxMail(Ucode& u, u32 mail)
  {
    if (u.upload_setup_in_progress)
    {
      PrepareBootUCode(u, mail);
      return;
    }
    AxWii& ax = *u.ax;
    switch (ax.mail_state)
    {
    case MailState::WaitingForCmdListSize:
      if ((mail & MAIL_CMDLIST_MASK) == MAIL_CMDLIST)
      {
        ax.cmdlist_size = static_cast<u16>(mail & ~MAIL_CMDLIST_MASK);
        ax.mail_state = MailState::WaitingForCmdListAddress;
      }
      break;
    case MailState::WaitingForCmdListAddress:
      CopyCmdList(ax, mail, ax.cmdlist_size);
      ax.cmdlist_size = 0;
      ax.mail_state = MailState::WaitingForNextTask;
      mix_state.store(MIX_QUEUED, std::memory_order_release);
      mix_state.notify_one();
      host.schedule_mix_done(host.user, AX_MIX_CYCLES);
      break;
    case MailState::WaitingForNextTask:
      if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP)
        mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);
      switch (mail)
      {
      case MAIL_RESUME:
        PushMail(DSP_RESUME, true);
        ax.mail_state = MailState::WaitingForCmdListSize;
        break;
      case MAIL_NEW_UCODE:
        u.upload_setup_in_progress = true;
        ax.mail_state = MailState::WaitingForCmdListSize;
        break;
      case MAIL_RESET:
        SetUCode(UCODE_ROM);
        return;  // u is gone
      case MAIL_CONTINUE:
        ax.mail_state = MailState::WaitingForCmdListSize;
        break;
      default:
        break;
      }
      break;
    }
  }

  void CopyCmdList(AxWii& ax, u32 addr, u16 size)
  {
    if (size >= std::size(ax.cmdlist))
      return;
    CopyFromSwapped16(ax.cmdlist, addr, size * sizeof(u16));
  }

  // ------------------------------------------------------------- AX

  void HandleCommandList(Ucode& u)
  {
    AxWii& ax = *u.ax;
    const u16* cmdlist = ax.cmdlist;
    u32 pb_addr = 0;
    u32 curr_idx = 0;
    auto next16 = [&] { return cmdlist[curr_idx++]; };
    auto next32 = [&] {
      const u32 hi = cmdlist[curr_idx++];
      return (hi << 16) | cmdlist[curr_idx++];
    };
    auto addresses = [&](u32* out, int n) {
      for (int i = 0; i < n; ++i)
        out[i] = (u32(cmdlist[curr_idx + 2 * i]) << 16) | cmdlist[curr_idx + 2 * i + 1];
      curr_idx += 2 * n;
    };

    bool end = false;
    while (!end)
    {
      const u16 cmd = next16();
      // The old command set (Wii Sports, the startup menu) has a separate
      // PB-address command and an output command without a volume.
      if (ax.old_axwii)
      {
        switch (cmd)
        {
        case 0x00:
          SetupProcessing(ax, next32());
          break;
        case 0x01:
        case 0x02:
          AddToLR(ax, next32(), cmd == 0x02);
          break;
        case 0x03:
          AddSubToLR(ax, next32());
          break;
        case 0x04:
          pb_addr = next32();
          break;
        case 0x05:
          ProcessPBList(u, pb_addr);
          break;
        case 0x06:
        case 0x07:
        case 0x08:
        {
          const u16 volume = next16();
          const u32 write_addr = next32();
          const u32 read_addr = next32();
          MixAUXSamples(ax, cmd - 0x06, write_addr, read_addr, volume);
          break;
        }
        case 0x09:
        case 0x0a:
        {
          const u16 volume = next16();
          u32 addrs[6];
          addresses(addrs, 6);
          UploadAUXMixLRSC(ax, cmd == 0x0a, addrs, volume);
          break;
        }
        case 0x0B:
        {
          const u16 threshold = next16();
          const u16 frames = next16();
          RunCompressor(ax, threshold, frames, next32(), 3);
          break;
        }
        case 0x0C:
        case 0x0D:
        {
          const u32 surround_addr = next32();
          const u32 lr_addr = next32();
          OutputSamples(ax, lr_addr, surround_addr, 0x8000, cmd == 0x0D);
          break;
        }
        case 0x0E:
        {
          u32 addrs[4];
          addresses(addrs, 4);
          OutputWMSamples(ax, addrs);
          break;
        }
        case 0x0F:
          end = true;
          break;
        }
      }
      else
      {
        switch (cmd)
        {
        case 0x00:
          SetupProcessing(ax, next32());
          break;
        case 0x01:
        case 0x02:
          AddToLR(ax, next32(), cmd == 0x02);
          break;
        case 0x03:
          AddSubToLR(ax, next32());
          break;
        case 0x04:
          ProcessPBList(u, next32());
          break;
        case 0x05:
        case 0x06:
        case 0x07:
        {
          const u16 volume = next16();
          const u32 write_addr = next32();
          const u32 read_addr = next32();
          MixAUXSamples(ax, cmd - 0x05, write_addr, read_addr, volume);
          break;
        }
        case 0x08:
        case 0x09:
        {
          const u16 volume = next16();
          u32 addrs[6];
          addresses(addrs, 6);
          UploadAUXMixLRSC(ax, cmd == 0x09, addrs, volume);
          break;
        }
        case 0x0A:
        {
          const u16 threshold = next16();
          const u16 frames = next16();
          RunCompressor(ax, threshold, frames, next32(), 3);
          break;
        }
        case 0x0B:
        case 0x0C:
        {
          // The 1.0 system menu's output command has no volume.
          const u16 volume = u.crc == 0xd9c4bf34 ? 0x8000 : next16();
          const u32 surround_addr = next32();
          const u32 lr_addr = next32();
          OutputSamples(ax, lr_addr, surround_addr, volume, cmd == 0x0C);
          break;
        }
        case 0x0D:
        {
          u32 addrs[4];
          addresses(addrs, 4);
          OutputWMSamples(ax, addrs);
          break;
        }
        case 0x0E:
          end = true;
          break;
        }
      }
    }
  }

  void SetupProcessing(AxWii& ax, u32 init_addr)
  {
    struct Buffer
    {
      int* ptr;
      int samples_per_milli;
    };
    const std::array<Buffer, 20> buffers = {{
        {ax.main_left, 32}, {ax.main_right, 32}, {ax.main_surround, 32},
        {ax.auxA_left, 32}, {ax.auxA_right, 32}, {ax.auxA_surround, 32},
        {ax.auxB_left, 32}, {ax.auxB_right, 32}, {ax.auxB_surround, 32},
        {ax.auxC_left, 32}, {ax.auxC_right, 32}, {ax.auxC_surround, 32},
        {ax.wm0, 6},        {ax.aux0, 6},        {ax.wm1, 6},
        {ax.aux1, 6},       {ax.wm2, 6},         {ax.aux2, 6},
        {ax.wm3, 6},        {ax.aux3, 6},
    }};
    constexpr int millis = 3;
    std::array<u16, 3 * buffers.size()> init{};
    CopyFromSwapped16(init.data(), init_addr, sizeof(init));
    for (size_t i = 0; i < buffers.size(); ++i)
    {
      const Buffer& buf = buffers[i];
      const s32 value = s32((u32(init[3 * i]) << 16) | init[3 * i + 1]);
      const s16 delta = init[3 * i + 2];
      const int n = millis * buf.samples_per_milli;
      if (value == 0)
      {
        std::memset(buf.ptr, 0, n * sizeof(int));
      }
      else
      {
        for (int j = 0; j < n; ++j)
          buf.ptr[j] = value + j * delta;
      }
    }
  }

  void AddToLR(AxWii& ax, u32 val_addr, bool neg)
  {
    const u8* ptr = Span(val_addr, 32 * 3 * sizeof(int));
    if (!ptr)
      return;
    for (int i = 0; i < 32 * 3; ++i)
    {
      int val = static_cast<int>(Load32(ptr + 4 * i));
      if (neg)
        val = -val;
      ax.main_left[i] += val;
      ax.main_right[i] += val;
    }
  }

  void AddSubToLR(AxWii& ax, u32 val_addr)
  {
    const u8* ptr = Span(val_addr, 2 * 32 * 3 * sizeof(int));
    if (!ptr)
      return;
    for (int i = 0; i < 32 * 3; ++i)
      ax.main_left[i] += static_cast<int>(Load32(ptr + 4 * i));
    ptr += 32 * 3 * sizeof(int);
    for (int i = 0; i < 32 * 3; ++i)
      ax.main_right[i] -= static_cast<int>(Load32(ptr + 4 * i));
  }

  void ReadPB(u32 crc, u32 addr, AXPBWii& pb)
  {
    u16* dst = reinterpret_cast<u16*>(&pb);
    constexpr u32 updates_begin = offsetof(AXPBWii, updates);
    constexpr u32 updates_end = offsetof(AXPBWii, updates) + sizeof(PBUpdatesWii);
    constexpr u32 gap_begin = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
    constexpr u32 gap_end = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);
    u8* bytes = reinterpret_cast<u8*>(&pb);
    switch (crc)
    {
    case 0x7699af32:
    case 0xfa450138:
      CopyFromSwapped16(dst, addr, gap_begin);
      std::memset(bytes + gap_begin, 0, gap_end - gap_begin);
      CopyFromSwapped16(reinterpret_cast<u16*>(bytes + gap_end), addr + gap_begin,
                        sizeof(pb) - gap_end);
      break;
    case 0xd9c4bf34:
    case 0xadbc06bd:
      CopyFromSwapped16(dst, addr, updates_begin);
      std::memset(bytes + updates_begin, 0, sizeof(PBUpdatesWii));
      CopyFromSwapped16(reinterpret_cast<u16*>(bytes + updates_end), addr + updates_begin,
                        gap_begin - updates_end);
      std::memset(bytes + gap_begin, 0, gap_end - gap_begin);
      CopyFromSwapped16(reinterpret_cast<u16*>(bytes + gap_end), addr + gap_begin,
                        sizeof(pb) - gap_end);
      break;
    case 0x347112ba:
    case 0x4cc52064:
      CopyFromSwapped16(dst, addr, updates_begin);
      std::memset(bytes + updates_begin, 0, sizeof(PBUpdatesWii));
      CopyFromSwapped16(reinterpret_cast<u16*>(bytes + updates_end), addr + updates_begin,
                        sizeof(pb) - updates_end);
      break;
    }
  }

  void WritePB(u32 crc, u32 addr, const AXPBWii& pb)
  {
    const u8* src = reinterpret_cast<const u8*>(&pb);
    constexpr u32 updates_begin = offsetof(AXPBWii, updates);
    constexpr u32 updates_end = offsetof(AXPBWii, updates) + sizeof(PBUpdatesWii);
    constexpr u32 gap_begin = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
    constexpr u32 gap_end = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);
    auto words = [src](u32 offset) { return reinterpret_cast<const u16*>(src + offset); };
    switch (crc)
    {
    case 0x7699af32:
    case 0xfa450138:
      CopyToSwapped16(addr, words(0), gap_begin);
      CopyToSwapped16(addr + gap_begin, words(gap_end), sizeof(pb) - gap_end);
      break;
    case 0xd9c4bf34:
    case 0xadbc06bd:
      CopyToSwapped16(addr, words(0), updates_begin);
      CopyToSwapped16(addr + updates_begin, words(updates_end), gap_begin - updates_end);
      CopyToSwapped16(addr + gap_begin, words(gap_end), sizeof(pb) - gap_end);
      break;
    case 0x347112ba:
    case 0x4cc52064:
      CopyToSwapped16(addr, words(0), updates_begin);
      CopyToSwapped16(addr + updates_begin, words(updates_end), sizeof(pb) - updates_end);
      break;
    }
  }

  void ApplyUpdatesForMs(int curr_ms, AXPBWii& pb, const u16* num_updates,
                         const PBUpdateData& updates)
  {
    u16* pb_mem = reinterpret_cast<u16*>(&pb);
    u32 start_idx = 0;
    for (int i = 0; i < curr_ms; ++i)
      start_idx += num_updates[i];
    for (u32 i = start_idx; i < start_idx + num_updates[curr_ms] && i < updates.size(); ++i)
    {
      const u16 offset = updates[i].pb_offset;
      if (offset < sizeof(pb) / 2)
        pb_mem[offset] = updates[i].new_value;
    }
  }

  // --- the accelerator, bound to the voice being processed

  u16 CurrentSample(const Accelerator& acc) const
  {
    switch (acc.sample_format & 3)
    {
    case 0:  // 4-bit
    {
      u16 val = ReadAram(acc.current_address >> 1);
      return (acc.current_address & 1) ? (val & 0xf) : (val >> 4);
    }
    case 1:  // 8-bit
      return ReadAram(acc.current_address);
    case 2:  // 16-bit
      return (ReadAram(acc.current_address * 2) << 8) | ReadAram(acc.current_address * 2 + 1);
    default:  // garbage, but the address still moves
      return 0;
    }
  }

  u16 ReadSample(AxWii& ax, const s16* coefs)
  {
    Accelerator& acc = ax.accelerator;
    if (acc.reads_stopped)
      return 0x0000;

    const u16 decode = (acc.sample_format >> 2) & 3;
    const u16 gain_scale = (acc.sample_format >> 4) & 3;
    u16 val = 0;
    u8 step_size = 0;
    s16 raw_sample = (decode == 1 || decode == 3) ? acc.input : CurrentSample(acc);

    const int coef_idx = (acc.pred_scale >> 4) & 0x7;
    const s32 coef1 = coefs[coef_idx * 2 + 0];
    const s32 coef2 = coefs[coef_idx * 2 + 1];

    if (decode == 0)  // ADPCM
    {
      raw_sample &= 0xF;
      const int scale = 1 << (acc.pred_scale & 0xF);
      if (raw_sample >= 8)
        raw_sample -= 16;
      const s32 val32 = (scale * raw_sample) + ((0x400 + coef1 * acc.yn1 + coef2 * acc.yn2) >> 11);
      val = static_cast<s16>(std::clamp<s32>(val32, -0x7FFF, 0x7FFF));
      step_size = 2;
      acc.yn2 = acc.yn1;
      acc.yn1 = val;
      acc.current_address += 1;
      // The loop and end handling follows the hardware's frame-header skip.
      if ((acc.end_address & 0xf) == 0x0 && acc.current_address == acc.end_address)
      {
        acc.current_address = acc.start_address + 1;
      }
      else if ((acc.end_address & 0xf) == 0x1 && acc.current_address == acc.end_address - 1)
      {
        acc.current_address = acc.start_address;
      }
      else if ((acc.current_address & 15) == 0)
      {
        acc.pred_scale = ReadAram((acc.current_address & ~15) >> 1);
        acc.current_address += 2;
        step_size += 2;
      }
    }
    else  // PCM, from ARAM or from the input register
    {
      u8 gain_shift = 0;
      switch (gain_scale)
      {
      case 0:
        gain_shift = 11;
        break;
      case 1:
        gain_shift = 0;
        break;
      case 2:
        gain_shift = 16;
        break;
      default:
        break;
      }
      const s32 val32 = ((static_cast<s32>(acc.gain) * raw_sample) >> gain_shift) +
                        (((coef1 * acc.yn1) >> gain_shift) + ((coef2 * acc.yn2) >> gain_shift));
      val = static_cast<s16>(val32);
      acc.yn2 = acc.yn1;
      acc.yn1 = val;
      step_size = 2;
      if (decode != 1)
        acc.current_address += 1;
    }

    if (acc.current_address == (acc.end_address + step_size - 1))
    {
      acc.current_address = acc.start_address;
      acc.reads_stopped = true;
      SampleReadEnd(ax);
    }
    acc.current_address &= CURRENT_ADDRESS_MASK;
    return val;
  }

  // The end of the sample: loop, or stop the voice.
  static void SampleReadEnd(AxWii& ax)
  {
    AXPBWii* pb = ax.acc_pb;
    Accelerator& acc = ax.accelerator;
    if (pb->audio_addr.looping)
    {
      acc.SetPredScale(pb->adpcm_loop_info.pred_scale);
      if (pb->is_stream != 1)
      {
        acc.yn1 = pb->adpcm_loop_info.yn1;
        acc.SetYn2(pb->adpcm_loop_info.yn2);
      }
      else
      {
        acc.SetYn2(acc.yn2);
      }
    }
    else
    {
      pb->running = 0;
    }
  }

  void GetInputSamples(AxWii& ax, AXPBWii& pb, s16* samples, u16 count, const s16* coeffs)
  {
    Accelerator& acc = ax.accelerator;
    ax.acc_pb = &pb;
    acc.start_address = ((u32(pb.audio_addr.loop_addr_hi) << 16) | pb.audio_addr.loop_addr_lo) &
                        START_END_ADDRESS_MASK;
    acc.end_address = ((u32(pb.audio_addr.end_addr_hi) << 16) | pb.audio_addr.end_addr_lo) &
                      START_END_ADDRESS_MASK;
    acc.current_address = ((u32(pb.audio_addr.cur_addr_hi) << 16) | pb.audio_addr.cur_addr_lo) &
                          CURRENT_ADDRESS_MASK;
    acc.sample_format = pb.audio_addr.sample_format;
    acc.yn1 = pb.adpcm.yn1;
    acc.SetYn2(pb.adpcm.yn2);
    acc.gain = static_cast<s16>(pb.adpcm.gain);
    acc.SetPredScale(pb.adpcm.pred_scale);

    if (coeffs)
      coeffs += pb.coef_select * 0x200;
    const u32 ratio = (u32(pb.src.ratio_hi) << 16) | pb.src.ratio_lo;
    const u32 curr_pos =
        ResampleAudio([&] { return static_cast<s16>(ReadSample(ax, pb.adpcm.coefs)); }, samples,
                      count, pb.src.last_samples, pb.src.cur_addr_frac, ratio, pb.src_type, coeffs);
    pb.src.cur_addr_frac = (curr_pos & 0xFFFF);
    pb.audio_addr.cur_addr_hi = static_cast<u16>(acc.current_address >> 16);
    pb.audio_addr.cur_addr_lo = static_cast<u16>(acc.current_address);
    pb.adpcm.yn1 = acc.yn1;
    pb.adpcm.yn2 = acc.yn2;
    pb.adpcm.pred_scale = acc.pred_scale;
  }

  struct Buffers
  {
    int* main_left;
    int* main_right;
    int* main_surround;
    int* auxA_left;
    int* auxA_right;
    int* auxA_surround;
    int* auxB_left;
    int* auxB_right;
    int* auxB_surround;
    int* auxC_left;
    int* auxC_right;
    int* auxC_surround;
    int* wm[8];  // main0, aux0, main1, aux1, ...
  };

  void ProcessVoice(AxWii& ax, AXPBWii& pb, const Buffers& buffers, u16 count, u32 mctrl,
                    const s16* coeffs)
  {
    if (pb.running != 1)
      return;

    s16 samples[96];
    GetInputSamples(ax, pb, samples, count, coeffs);

    for (u32 i = 0; i < count; ++i)
    {
      const s32 volume = (u16)pb.vol_env.cur_volume;
      const s32 sample = ((s32)samples[i] * volume) >> 15;
      samples[i] = ClampS16(sample);
      pb.vol_env.cur_volume += pb.vol_env.cur_volume_delta;
    }

    if (pb.lpf.on != 0)
      LowPassFilter(samples, count, pb.lpf);
    if (ax.new_filter && pb.biquad.on != 0)
      BiquadFilter(samples, count, pb.biquad);

    struct Channel
    {
      u32 mix;
      int* out;
      VolumeData* volume;
      s16* dpop;
    };
    const Channel channels[] = {
        {MIX_MAIN_L, buffers.main_left, &pb.mixer.main_left, &pb.dpop.main_left},
        {MIX_MAIN_R, buffers.main_right, &pb.mixer.main_right, &pb.dpop.main_right},
        {MIX_MAIN_S, buffers.main_surround, &pb.mixer.main_surround, &pb.dpop.main_surround},
        {MIX_AUXA_L, buffers.auxA_left, &pb.mixer.auxA_left, &pb.dpop.auxA_left},
        {MIX_AUXA_R, buffers.auxA_right, &pb.mixer.auxA_right, &pb.dpop.auxA_right},
        {MIX_AUXA_S, buffers.auxA_surround, &pb.mixer.auxA_surround, &pb.dpop.auxA_surround},
        {MIX_AUXB_L, buffers.auxB_left, &pb.mixer.auxB_left, &pb.dpop.auxB_left},
        {MIX_AUXB_R, buffers.auxB_right, &pb.mixer.auxB_right, &pb.dpop.auxB_right},
        {MIX_AUXB_S, buffers.auxB_surround, &pb.mixer.auxB_surround, &pb.dpop.auxB_surround},
        {MIX_AUXC_L, buffers.auxC_left, &pb.mixer.auxC_left, &pb.dpop.auxC_left},
        {MIX_AUXC_R, buffers.auxC_right, &pb.mixer.auxC_right, &pb.dpop.auxC_right},
        {MIX_AUXC_S, buffers.auxC_surround, &pb.mixer.auxC_surround, &pb.dpop.auxC_surround},
    };
    // Each channel's ramp flag is the bit above its enable flag.
    for (const Channel& c : channels)
    {
      if (mctrl & c.mix)
        MixAdd(c.out, samples, count, c.volume, c.dpop, (mctrl & (c.mix << 1)) != 0);
    }

    if (!pb.remote)
      return;

    if (ax.new_filter && pb.remote_iir.on != 0)
    {
      if (pb.remote_iir.on == 2)
        BiquadFilter(samples, count, pb.remote_iir.biquad);
      else
        LowPassFilter(samples, count, pb.remote_iir.lpf);
    }

    const u16 wm_count = count == 96 ? 18 : 6;
    s16 wm_samples[18];
    u32 sample_index = 0;
    const u32 curr_pos = ResampleAudio([&] { return samples[sample_index++]; }, wm_samples,
                                       wm_count, pb.remote_src.last_samples,
                                       pb.remote_src.cur_addr_frac, 0x55555, SRCTYPE_POLYPHASE,
                                       coeffs);
    pb.remote_src.cur_addr_frac = curr_pos & 0xFFFF;

    VolumeData* wm_volumes[8] = {&pb.remote_mixer.main0, &pb.remote_mixer.aux0,
                                 &pb.remote_mixer.main1, &pb.remote_mixer.aux1,
                                 &pb.remote_mixer.main2, &pb.remote_mixer.aux2,
                                 &pb.remote_mixer.main3, &pb.remote_mixer.aux3};
    s16* wm_dpops[8] = {&pb.remote_dpop.main0, &pb.remote_dpop.aux0, &pb.remote_dpop.main1,
                        &pb.remote_dpop.aux1,  &pb.remote_dpop.main2, &pb.remote_dpop.aux2,
                        &pb.remote_dpop.main3, &pb.remote_dpop.aux3};
    for (int n = 0; n < 8; ++n)
    {
      const u32 bits = (pb.remote_mixer_control >> (2 * n)) & 3;
      if (bits)
        MixAdd(buffers.wm[n], wm_samples, wm_count, wm_volumes[n], wm_dpops[n], (bits & 2) != 0);
    }
  }

  void ProcessPBList(Ucode& u, u32 pb_addr)
  {
    AxWii& ax = *u.ax;
    const s16* coeffs = ax.coeffs_checksum ? ax.coeffs.data() : nullptr;
    AXPBWii pb;
    while (pb_addr)
    {
      Buffers buffers = {ax.main_left,  ax.main_right, ax.main_surround, ax.auxA_left,
                         ax.auxA_right, ax.auxA_surround, ax.auxB_left, ax.auxB_right,
                         ax.auxB_surround, ax.auxC_left, ax.auxC_right, ax.auxC_surround,
                         {ax.wm0, ax.aux0, ax.wm1, ax.aux1, ax.wm2, ax.aux2, ax.wm3, ax.aux3}};

      ReadPB(u.crc, pb_addr, pb);
      const u32 mctrl = ConvertMixerControl((u32(pb.mixer_control_hi) << 16) | pb.mixer_control_lo);

      if (ax.old_axwii &&
          (pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2]))
      {
        // Parameter updates apply once per millisecond, so the old ucode
        // renders its 3 ms frame in three steps.
        PBUpdateData updates{};
        CopyFromSwapped16(reinterpret_cast<u16*>(updates.data()),
                          (u32(pb.updates.data_hi) << 16) | pb.updates.data_lo, sizeof(updates));
        for (int curr_ms = 0; curr_ms < 3; ++curr_ms)
        {
          ApplyUpdatesForMs(curr_ms, pb, pb.updates.num_updates, updates);
          ProcessVoice(ax, pb, buffers, 32,
                       ConvertMixerControl((u32(pb.mixer_control_hi) << 16) | pb.mixer_control_lo),
                       coeffs);
          int** regular = &buffers.main_left;
          for (int i = 0; i < 12; ++i)
            regular[i] += 32;
          for (int*& ptr : buffers.wm)
            ptr += 6;
        }
      }
      else
      {
        ProcessVoice(ax, pb, buffers, 96, mctrl, coeffs);
      }

      WritePB(u.crc, pb_addr, pb);
      pb_addr = (u32(pb.next_pb_hi) << 16) | pb.next_pb_lo;
    }
  }

  void MixAUXSamples(AxWii& ax, int aux_id, u32 write_addr, u32 read_addr, u16 volume)
  {
    std::array<u16, 96> volume_ramp;
    GenerateVolumeRamp(volume_ramp.data(), ax.last_aux_volumes[aux_id], volume,
                       volume_ramp.size());
    ax.last_aux_volumes[aux_id] = volume;

    int* const main_buffers[3] = {ax.main_left, ax.main_right, ax.main_surround};
    const int* buffers[3] = {};
    switch (aux_id)
    {
    case 0:
      buffers[0] = ax.auxA_left;
      buffers[1] = ax.auxA_right;
      buffers[2] = ax.auxA_surround;
      break;
    case 1:
      buffers[0] = ax.auxB_left;
      buffers[1] = ax.auxB_right;
      buffers[2] = ax.auxB_surround;
      break;
    case 2:
      buffers[0] = ax.auxC_left;
      buffers[1] = ax.auxC_right;
      buffers[2] = ax.auxC_surround;
      break;
    }

    // Send the aux buffers out for the effects, then mix the processed ones back in.
    if (write_addr)
    {
      for (const int* buffer : buffers)
      {
        CopyToSwapped32(write_addr, buffer, 3 * 32 * sizeof(int));
        write_addr += 3 * 32 * sizeof(int);
      }
    }

    const u8* ptr = Span(read_addr, 3 * 3 * 32 * sizeof(int));
    if (!ptr)
      return;
    for (int* main_buffer : main_buffers)
    {
      for (u32 j = 0; j < 3 * 32; ++j)
      {
        s64 sample = (s64)(s32)Load32(ptr);
        ptr += 4;
        sample *= volume_ramp[j];
        main_buffer[j] += (s32)(sample >> 15);
      }
    }
  }

  void UploadAUXMixLRSC(AxWii& ax, int aux_id, const u32* addresses, u16 volume)
  {
    int* aux_left = aux_id ? ax.auxB_left : ax.auxA_left;
    int* aux_right = aux_id ? ax.auxB_right : ax.auxA_right;
    int* aux_surround = aux_id ? ax.auxB_surround : ax.auxA_surround;
    int* auxc_buffer = aux_id ? ax.auxC_surround : ax.auxC_right;

    CopyToSwapped32(addresses[0], aux_left, 96 * sizeof(int));
    CopyToSwapped32(addresses[0] + 96 * sizeof(int), aux_right, 96 * sizeof(int));
    CopyToSwapped32(addresses[0] + 2 * 96 * sizeof(int), aux_surround, 96 * sizeof(int));
    CopyToSwapped32(addresses[1], auxc_buffer, 96 * sizeof(int));

    u16 volume_ramp[96];
    GenerateVolumeRamp(volume_ramp, ax.last_aux_volumes[aux_id], volume, 96);
    ax.last_aux_volumes[aux_id] = volume;

    int* mix_dest[4] = {ax.main_left, ax.main_right, ax.main_surround, ax.auxC_left};
    for (u32 mix_i = 0; mix_i < 4; ++mix_i)
    {
      // aux_left doubles as the download buffer, as on the DSP.
      CopyFromSwapped32(aux_left, addresses[2 + mix_i], 96 * sizeof(int));
      for (u32 i = 0; i < 96; ++i)
      {
        s64 sample = (s64)(s32)aux_left[i];
        sample *= volume_ramp[i];
        mix_dest[mix_i][i] += (s32)(sample >> 15);
      }
    }
  }

  void RunCompressor(AxWii& ax, u16 threshold, u16 release_frames, u32 table_addr, u32 millis)
  {
    bool triggered = false;
    for (u32 i = 0; i < 32 * millis; ++i)
    {
      if (std::abs(ax.main_left[i]) > int(threshold) ||
          std::abs(ax.main_right[i]) > int(threshold))
      {
        triggered = true;
        break;
      }
    }

    const u32 frame_byte_size = 32 * millis * sizeof(s16);
    u32 table_offset = 0;
    if (triggered)
    {
      table_offset = ax.compressor_pos * frame_byte_size;
      ax.compressor_pos = release_frames;
    }
    else if (ax.compressor_pos)
    {
      --ax.compressor_pos;
      constexpr u32 ATTACK_ENTRY_COUNT = 11;
      table_offset = (ATTACK_ENTRY_COUNT + ax.compressor_pos) * frame_byte_size;
    }
    else
    {
      return;
    }

    const u8* ramp = Span(table_addr + table_offset, 32 * millis * sizeof(u16));
    if (!ramp)
      return;
    for (u32 i = 0; i < 32 * millis; ++i)
    {
      const u16 coef = Load16(ramp + 2 * i);
      ax.main_left[i] = (s64(ax.main_left[i]) * coef) >> 15;
      ax.main_right[i] = (s64(ax.main_right[i]) * coef) >> 15;
    }
  }

  void OutputSamples(AxWii& ax, u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc)
  {
    std::array<u16, 96> volume_ramp;
    GenerateVolumeRamp(volume_ramp.data(), ax.last_main_volume, volume, volume_ramp.size());
    ax.last_main_volume = volume;

    CopyToSwapped32(surround_addr, ax.main_surround, 3 * 32 * sizeof(int));
    if (upload_auxc)
      CopyToSwapped32(surround_addr + 3 * 32 * sizeof(int), ax.auxC_left, 3 * 32 * sizeof(int));

    for (size_t i = 0; i < volume_ramp.size(); ++i)
    {
      const s64 left = (s64(ax.main_left[i]) * volume_ramp[i]) >> 15;
      const s64 right = (s64(ax.main_right[i]) * volume_ramp[i]) >> 15;
      ax.main_left[i] = ClampS16(left);
      ax.main_right[i] = ClampS16(right);
    }

    // Interleaved right, left: the order the audio DMA expects.
    if (u8* out = Span(lr_addr, 3 * 32 * 2 * sizeof(s16)))
    {
      for (size_t i = 0; i < 3 * 32; ++i)
      {
        Store16(out + 4 * i, static_cast<u16>(ax.main_right[i]));
        Store16(out + 4 * i + 2, static_cast<u16>(ax.main_left[i]));
      }
    }

    PushMail(DSP_SYNC, true);
  }

  void OutputWMSamples(AxWii& ax, const u32* addresses)
  {
    const int* buffers[] = {ax.wm0, ax.wm1, ax.wm2, ax.wm3};
    for (u32 i = 0; i < 4; ++i)
    {
      u8* out = Span(addresses[i], 3 * 6 * sizeof(s16));
      if (!out)
        continue;
      for (u32 j = 0; j < 3 * 6; ++j)
        Store16(out + 2 * j, static_cast<u16>(ClampS16(buffers[i][j])));
    }
  }

  // ------------------------------------------------------------- control

  u16 WriteHleControl(u16 value)
  {
    u16 temp = value;
    if ((hle_control ^ temp) & CTRL_HALT)
      halted = (temp & CTRL_HALT) != 0;
    if (temp & CTRL_RESET)
    {
      SetUCode(UCODE_ROM);
      temp &= ~CTRL_RESET;
    }
    if ((hle_control & CTRL_INIT) && !(temp & CTRL_INIT))
    {
      SetUCode(UCODE_INIT_AUDIO_SYSTEM);
      temp |= CTRL_INIT_CODE;
      init_code_clear_time = host.timebase(host.user) + 130;
    }
    hle_control = temp;
    return hle_control;
  }

  u16 ReadHleControl()
  {
    if (hle_control & CTRL_INIT_CODE)
    {
      if (host.timebase(host.user) >= init_code_clear_time)
        hle_control &= ~CTRL_INIT_CODE;
      else
        host.force_exception_check(host.user, 50);  // keep checking
    }
    return hle_control;
  }

  void WriteControl(u16 val)
  {
    const u16 tmp = (val & ~DSP_CONTROL_MASK) | (WriteHleControl(val) & DSP_CONTROL_MASK);
    if (val & CTRL_RESET)
      audio_dma.control = 0;
    // The interrupt bits are write-one-to-clear; DMAState belongs to ARAM.
    dsp_control = (dsp_control & ~CTRL_WRITTEN) | (tmp & CTRL_WRITTEN);
    dsp_control &= ~(tmp & CTRL_INTERRUPTS);
    UpdateInterrupts();
  }

  // ------------------------------------------------------------- DMA

  void DoAramDma()
  {
    dsp_control |= CTRL_DMA_STATE;

    u32 count = aram_dma.cnt & 0x7FFFFFFF;
    const bool to_mram = (aram_dma.cnt >> 31) != 0;
    // Transfer rate measured on hardware.
    host.schedule_aram_done(host.user, (count / 32) * 246);

    aram_dma.ar_addr &= 0x3ffffff;
    aram_dma.mm_addr &= 0x3ffffff;
    // Wii ARAM is MEM2; addresses past it reach nothing.
    if (aram_dma.ar_addr >= host.exram_size)
      return;

    const bool mirror_low = (aram_info & 0xf) == 4;
    while (count)
    {
      u8* mram = Span(aram_dma.mm_addr, 8);
      u8* aram = host.exram + (aram_dma.ar_addr & exram_mask);
      if (mram)
      {
        if (to_mram)
        {
          std::memcpy(mram, aram, 8);
        }
        else
        {
          if (mirror_low && aram_dma.ar_addr < 0x400000)
            std::memcpy(host.exram + ((aram_dma.ar_addr + 0x400000) & exram_mask), mram, 8);
          std::memcpy(aram, mram, 8);
        }
      }
      aram_dma.mm_addr += 8;
      aram_dma.ar_addr += 8;
      count -= 8;
    }
    aram_dma.cnt = (aram_dma.cnt & 0x80000000) | count;
  }

  void WriteAudioDmaControl(u16 val)
  {
    const bool already_enabled = (audio_dma.control & AUDIO_DMA_ENABLE) != 0;
    audio_dma.control = val;
    // A running transfer picks the new values up when it relatches.
    if (!already_enabled && (val & AUDIO_DMA_ENABLE))
    {
      audio_dma.current_source_address = audio_dma.source_address;
      audio_dma.remaining_blocks_count = val & AUDIO_DMA_BLOCKS;
      // Some games crash when this comes sooner (Dolphin issue 9509).
      host.schedule_interrupt(host.user, 200, DSP_INT_AID);
    }
  }

  void UpdateAudioDma()
  {
    static const int16_t zero_samples[8 * 2] = {};
    if (!(audio_dma.control & AUDIO_DMA_ENABLE))
    {
      host.push_samples(host.user, zero_samples, 8);
      return;
    }
    if (const u8* samples = Span(audio_dma.current_source_address, 32))
      host.push_samples(host.user, reinterpret_cast<const int16_t*>(samples), 8);
    if (audio_dma.remaining_blocks_count != 0)
    {
      audio_dma.remaining_blocks_count--;
      audio_dma.current_source_address += 32;
    }
    if (audio_dma.remaining_blocks_count == 0)
    {
      audio_dma.current_source_address = audio_dma.source_address;
      audio_dma.remaining_blocks_count = audio_dma.control & AUDIO_DMA_BLOCKS;
      GenerateInterrupt(DSP_INT_AID);
    }
  }

  // ------------------------------------------------------------- state

  void DoUcodeState(State& p, Ucode& u)
  {
    auto shared = [&] {
      p.Do(u.upload_setup_in_progress);
      p.Do(u.next_ucode);
      p.Do(u.next_ucode_steps);
      p.Do(u.needs_resume_mail);
    };
    switch (u.kind)
    {
    case Ucode::Kind::Rom:
      p.Do(u.rom);
      p.Do(u.rom_boot_task_num_steps);
      p.Do(u.rom_next_parameter);
      shared();
      break;
    case Ucode::Kind::Init:
      break;
    case Ucode::Kind::AxWii:
    {
      AxWii& ax = *u.ax;
      shared();
      p.Do(ax.cmdlist);
      p.Do(ax.cmdlist_size);
      p.Do(ax.mail_state);
      p.Do(ax.main_left);
      p.Do(ax.main_right);
      p.Do(ax.main_surround);
      p.Do(ax.auxA_left);
      p.Do(ax.auxA_right);
      p.Do(ax.auxA_surround);
      p.Do(ax.auxB_left);
      p.Do(ax.auxB_right);
      p.Do(ax.auxB_surround);
      const std::optional<u32> old_checksum = ax.coeffs_checksum;
      p.Do(ax.coeffs_checksum);
      if (p.Reading() && ax.coeffs_checksum && old_checksum != ax.coeffs_checksum)
      {
        const u32 wanted = *ax.coeffs_checksum;
        if (!LoadCoefficients(ax, true, wanted))
          std::fprintf(stderr, "native DSP: the state's dsp_coef.bin (%08x) is missing\n", wanted);
      }
      p.Do(ax.compressor_pos);
      ax.accelerator.DoState(p);
      p.Do(ax.auxC_left);
      p.Do(ax.auxC_right);
      p.Do(ax.auxC_surround);
      p.Do(ax.wm0);
      p.Do(ax.wm1);
      p.Do(ax.wm2);
      p.Do(ax.wm3);
      p.Do(ax.aux0);
      p.Do(ax.aux1);
      p.Do(ax.aux2);
      p.Do(ax.aux3);
      p.Do(ax.last_main_volume);
      p.Do(ax.last_aux_volumes);
      break;
    }
    }
  }

  void DoState(State& p)
  {
    // DSPManager (Wii: ARAM is MEM2, saved with memory)
    p.Do(dsp_control);
    p.Do(audio_dma);
    p.Do(aram_dma);
    p.Do(aram_info);
    p.Do(aram_mode);
    p.Do(aram_refresh);
    p.Do(dsp_slice);

    // DSPHLE
    bool is_hle = true;
    p.Do(is_hle);
    p.Do(hle_control);
    p.Do(init_code_clear_time);
    p.Do(cpu_mailbox);
    p.Do(dsp_mailbox);

    int crc = ucode ? int(ucode->crc) : int(UCODE_NULL);
    int last_crc = last_ucode ? int(last_ucode->crc) : int(UCODE_NULL);
    const int crc_before = crc;
    const int last_crc_before = last_crc;
    p.Do(crc);
    p.Do(last_crc);
    if (crc != crc_before)
      ucode = MakeUCode(u32(crc));
    if (last_crc != last_crc_before)
      last_ucode = MakeUCode(u32(last_crc));
    if (ucode)
      DoUcodeState(p, *ucode);
    if (last_ucode)
      DoUcodeState(p, *last_ucode);

    // Mail handler
    u32 count = u32(pending_mails.size());
    p.Do(count);
    pending_mails.resize(count);
    for (auto& [mail, interrupt] : pending_mails)
    {
      p.Do(mail);
      p.Do(interrupt);
    }
    p.Do(last_mail);
    p.Do(halted);
  }
};

DspCore::DspCore(const DspHost& host, std::vector<std::string> coefficient_paths)
    : m_impl(std::make_unique<Impl>(host, std::move(coefficient_paths)))
{
}

DspCore::~DspCore() = default;

uint16_t DspCore::Read(uint32_t offset)
{
  Impl& d = *m_impl;
  switch (offset)
  {
  case DSP_MAIL_TO_DSP_HI:
    return u16(d.cpu_mailbox >> 16);
  case DSP_MAIL_TO_DSP_LO:
    return u16(d.cpu_mailbox);
  case DSP_MAIL_FROM_DSP_HI:
    return d.ReadMailHigh();
  case DSP_MAIL_FROM_DSP_LO:
    return d.ReadMailLow();
  case DSP_CONTROL:
    return (d.dsp_control & ~DSP_CONTROL_MASK) | (d.ReadHleControl() & DSP_CONTROL_MASK);
  case AR_INFO:
    return d.aram_info;
  case AR_MODE:
    return d.aram_mode;
  case AR_REFRESH:
    return d.aram_refresh;
  case AR_DMA_MMADDR_H:
    return *HighPart(&d.aram_dma.mm_addr);
  case AR_DMA_MMADDR_L:
    return *LowPart(&d.aram_dma.mm_addr);
  case AR_DMA_ARADDR_H:
    return *HighPart(&d.aram_dma.ar_addr);
  case AR_DMA_ARADDR_L:
    return *LowPart(&d.aram_dma.ar_addr);
  case AR_DMA_CNT_H:
    return *HighPart(&d.aram_dma.cnt);
  case AR_DMA_CNT_L:
    return *LowPart(&d.aram_dma.cnt);
  case AUDIO_DMA_START_HI:
    return *HighPart(&d.audio_dma.source_address);
  case AUDIO_DMA_START_LO:
    return *LowPart(&d.audio_dma.source_address);
  case AUDIO_DMA_CONTROL_LEN:
    return d.audio_dma.control;
  case AUDIO_DMA_BLOCKS_LEFT:
    // Zero-based; DreamMix World Fighters hangs if it never reaches zero.
    return d.audio_dma.remaining_blocks_count > 0 ? d.audio_dma.remaining_blocks_count - 1 : 0;
  default:
    return 0;
  }
}

void DspCore::Write(uint32_t offset, uint16_t value)
{
  Impl& d = *m_impl;
  if (offset == DSP_MAIL_TO_DSP_LO || offset == AR_DMA_CNT_L)
    d.JoinMix();
  switch (offset)
  {
  case DSP_MAIL_TO_DSP_HI:
    d.cpu_mailbox = (d.cpu_mailbox & 0xFFFF) | (u32(value) << 16);
    break;
  case DSP_MAIL_TO_DSP_LO:
    d.cpu_mailbox = (d.cpu_mailbox & 0xFFFF0000) | value;
    d.HandleMail(d.cpu_mailbox);
    d.cpu_mailbox &= 0x7FFFFFFF;
    break;
  case DSP_CONTROL:
    d.WriteControl(value);
    break;
  case AR_INFO:
    d.aram_info = value & 0x007f;
    break;
  case AR_REFRESH:
    d.aram_refresh = value & 0x07ff;
    break;
  case AR_DMA_MMADDR_H:
    *HighPart(&d.aram_dma.mm_addr) = value & 0x03ff;
    break;
  case AR_DMA_MMADDR_L:
    *LowPart(&d.aram_dma.mm_addr) = value & 0xffe0;
    break;
  case AR_DMA_ARADDR_H:
    *HighPart(&d.aram_dma.ar_addr) = value & 0x03ff;
    break;
  case AR_DMA_ARADDR_L:
    *LowPart(&d.aram_dma.ar_addr) = value & 0xffe0;
    break;
  case AR_DMA_CNT_H:
    *HighPart(&d.aram_dma.cnt) = value & (0x03ff | 0x8000);
    break;
  case AR_DMA_CNT_L:
    d.aram_dma.cnt = (d.aram_dma.cnt & 0xFFFF0000) | (value & 0xffe0);
    d.DoAramDma();
    break;
  case AUDIO_DMA_START_HI:
    *HighPart(&d.audio_dma.source_address) = value & 0x1fff;
    break;
  case AUDIO_DMA_START_LO:
    *LowPart(&d.audio_dma.source_address) = value & 0xffe0;
    break;
  case AUDIO_DMA_CONTROL_LEN:
    d.WriteAudioDmaControl(value);
    break;
  default:
    break;
  }
}

void DspCore::RaiseInterrupt(uint32_t type)
{
  m_impl->GenerateInterrupt(type);
}

void DspCore::CompleteMix()
{
  m_impl->ReleaseMix();
}

void DspCore::CompleteAramDma()
{
  m_impl->dsp_control &= ~CTRL_DMA_STATE;
  m_impl->GenerateInterrupt(DSP_INT_ARAM);
}

void DspCore::Update()
{
  Impl& d = *m_impl;
  if (d.ucode && d.ucode->kind == Ucode::Kind::AxWii && d.ucode->needs_resume_mail)
  {
    d.ucode->needs_resume_mail = false;
    d.PushMail(DSP_RESUME, true);
  }
}

void DspCore::UpdateAudioDma()
{
  m_impl->UpdateAudioDma();
}

void DspCore::DoState(const DspStateIO& io)
{
  // A mix in flight lands now; the state has no room for it.
  m_impl->ReleaseMix();
  State p(io);
  m_impl->DoState(p);
}
}  // namespace dualize::apu
