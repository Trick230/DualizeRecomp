#pragma once

// Audio output: the guest's DMA stream to the default playback device through
// an SDL3 audio stream (SDL resamples to the device rate). A small rate
// adjustment keeps the queue near its target, so neither underruns nor
// latency build up when the guest's clock drifts from the device's. Wii
// Remote speakers play through streams of their own on the same device, which
// SDL mixes in.

#include <array>
#include <cstdint>
#include <vector>

struct SDL_AudioStream;

namespace dualize::apu
{
class AudioOut
{
public:
  AudioOut() = default;
  ~AudioOut();
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;

  // False when there is no playback device; Push then discards samples.
  bool Start();
  void Stop();
  // Linear gain, 0 mutes.
  void SetGain(float gain);
  // count stereo frames of big-endian s16 (right, left) at 108 MHz / rate_divisor.
  void Push(const int16_t* samples, uint32_t count, uint32_t rate_divisor);
  // count mono host-endian samples from Wii Remote speaker `index` at rate Hz,
  // with per-channel volumes 0..255.
  void PushSpeaker(int index, const int16_t* samples, uint32_t count, uint32_t rate,
                   uint8_t left_volume, uint8_t right_volume);

private:
  void Flush();
  // Linear ramp over the first FADE_FRAMES stereo frames at frames.
  static void Fade(int16_t* frames, bool in);
  static constexpr size_t FADE_FRAMES = 64;

  struct Speaker
  {
    SDL_AudioStream* stream = nullptr;
    uint32_t rate = 0;
    std::vector<int16_t> frames;  // stereo scratch
  };
  static constexpr int SPEAKERS = 4;

  uint32_t m_device = 0;  // SDL_AudioDeviceID
  SDL_AudioStream* m_stream = nullptr;
  float m_gain = 1.0f;
  std::array<Speaker, SPEAKERS> m_speakers;
  uint32_t m_rate = 0;
  bool m_playing = false;
  float m_ratio = 1.0f;
  bool m_dropping = false;
  std::vector<int16_t> m_pending;  // little-endian left, right
  std::vector<int16_t> m_held;     // the previous flush, not yet queued
  uint32_t m_stat_frames = 0;
  uint32_t m_stat_dropped = 0;
  uint32_t m_stat_min_ms = ~0u;
};
}  // namespace dualize::apu
