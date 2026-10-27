#include "apu/audio_out.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dualize::apu
{
namespace
{
constexpr uint32_t RATE_DIVIDEND = 108000000;
// Frames gathered before one SDL_PutAudioStreamData (8 ms at 32 kHz).
constexpr size_t FLUSH_FRAMES = 256;
// Queue targets, in milliseconds of input (one more flush waits as look-ahead).
constexpr uint32_t TARGET_MS = 40;
// Past this the guest runs faster than real time; input is skipped until the
// queue is down to the resume level.
constexpr uint32_t LIMIT_MS = 160;
constexpr uint32_t RESUME_MS = 80;
// Largest rate correction: 0.5%, below what pitch perception notices.
constexpr float MAX_CORRECTION = 0.005f;
// Speaker input beyond this is skipped (the guest runs faster than real time).
constexpr uint32_t SPEAKER_LIMIT_MS = 100;
}  // namespace

AudioOut::~AudioOut()
{
  Stop();
}

bool AudioOut::Start()
{
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
  {
    std::fprintf(stderr, "audio: SDL audio unavailable: %s\n", SDL_GetError());
    return false;
  }
  m_rate = 32000;
  const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, static_cast<int>(m_rate)};
  // A logical device of our own, so speaker streams can bind to it as well;
  // paused until the first target is queued.
  m_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
  if (m_device)
    SDL_PauseAudioDevice(m_device);
  m_stream = m_device ? SDL_CreateAudioStream(&spec, &spec) : nullptr;
  if (!m_stream || !SDL_BindAudioStream(m_device, m_stream))
  {
    std::fprintf(stderr, "audio: no playback device: %s\n", SDL_GetError());
    SDL_DestroyAudioStream(m_stream);
    m_stream = nullptr;
    if (m_device)
      SDL_CloseAudioDevice(m_device);
    m_device = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  m_pending.reserve(FLUSH_FRAMES * 2);
  m_playing = false;
  m_ratio = 1.0f;
  return true;
}

void AudioOut::Stop()
{
  if (!m_stream)
    return;
  for (Speaker& speaker : m_speakers)
  {
    if (speaker.stream)
      SDL_DestroyAudioStream(speaker.stream);
    speaker = {};
  }
  SDL_DestroyAudioStream(m_stream);
  m_stream = nullptr;
  SDL_CloseAudioDevice(m_device);
  m_device = 0;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::SetGain(float gain)
{
  m_gain = gain;
  if (m_stream)
    SDL_SetAudioStreamGain(m_stream, gain);
  for (Speaker& speaker : m_speakers)
  {
    if (speaker.stream)
      SDL_SetAudioStreamGain(speaker.stream, gain);
  }
}

void AudioOut::PushSpeaker(int index, const int16_t* samples, uint32_t count, uint32_t rate,
                           uint8_t left_volume, uint8_t right_volume)
{
  if (!m_stream || index < 0 || index >= SPEAKERS || count == 0 || rate == 0)
    return;
  Speaker& speaker = m_speakers[index];
  const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
  if (!speaker.stream)
  {
    // Binding sets the device side to the device's format.
    speaker.stream = SDL_CreateAudioStream(&spec, &spec);
    if (!speaker.stream || !SDL_BindAudioStream(m_device, speaker.stream))
    {
      std::fprintf(stderr, "audio: no stream for Wii Remote speaker %d: %s\n", index + 1,
                   SDL_GetError());
      SDL_DestroyAudioStream(speaker.stream);
      speaker.stream = nullptr;
      return;
    }
    SDL_SetAudioStreamGain(speaker.stream, m_gain);
    speaker.rate = rate;
    std::fprintf(stderr, "audio: Wii Remote speaker %d at %u Hz\n", index + 1, rate);
  }
  else if (rate != speaker.rate)
  {
    SDL_SetAudioStreamFormat(speaker.stream, &spec, nullptr);
    speaker.rate = rate;
  }
  if (uint32_t(std::max(SDL_GetAudioStreamQueued(speaker.stream), 0)) >
      rate * 4 * SPEAKER_LIMIT_MS / 1000)
    return;

  speaker.frames.resize(size_t(count) * 2);
  for (uint32_t i = 0; i < count; ++i)
  {
    speaker.frames[2 * i] = int16_t(samples[i] * left_volume / 255);
    speaker.frames[2 * i + 1] = int16_t(samples[i] * right_volume / 255);
  }
  SDL_PutAudioStreamData(speaker.stream, speaker.frames.data(),
                         int(speaker.frames.size() * sizeof(int16_t)));
}

void AudioOut::Push(const int16_t* samples, uint32_t count, uint32_t rate_divisor)
{
  if (!m_stream)
    return;
  const uint32_t rate = RATE_DIVIDEND / rate_divisor;
  if (rate != m_rate)
  {
    Flush();
    if (!m_held.empty())
      SDL_PutAudioStreamData(m_stream, m_held.data(), int(m_held.size() * sizeof(int16_t)));
    m_held.clear();
    m_rate = rate;
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, static_cast<int>(m_rate)};
    SDL_SetAudioStreamFormat(m_stream, &spec, nullptr);
  }
  for (uint32_t i = 0; i < count; ++i)
  {
    const uint16_t right = static_cast<uint16_t>(samples[2 * i]);
    const uint16_t left = static_cast<uint16_t>(samples[2 * i + 1]);
    m_pending.push_back(static_cast<int16_t>(__builtin_bswap16(left)));
    m_pending.push_back(static_cast<int16_t>(__builtin_bswap16(right)));
  }
  if (m_pending.size() >= FLUSH_FRAMES * 2)
    Flush();
}

void AudioOut::Flush()
{
  if (m_pending.empty())
    return;
  const int bytes = static_cast<int>(m_pending.size() * sizeof(int16_t));
  const uint32_t bytes_per_ms = m_rate * 4 / 1000;
  const uint32_t queued = static_cast<uint32_t>(std::max(SDL_GetAudioStreamQueued(m_stream), 0));

  // Faster than real time: skip input until the queue is back near its
  // target. Skipping in long runs, with the cut faded out and the resume
  // faded in, sounds like a jump instead of a buzz.
  if (m_dropping ? queued > RESUME_MS * bytes_per_ms : queued >= LIMIT_MS * bytes_per_ms)
  {
    if (!m_dropping && !m_held.empty())
    {
      Fade(m_held.data() + m_held.size() - FADE_FRAMES * 2, false);
      SDL_PutAudioStreamData(m_stream, m_held.data(), int(m_held.size() * sizeof(int16_t)));
      m_held.clear();
    }
    m_dropping = true;
  }
  else if (m_dropping)
  {
    m_dropping = false;
    Fade(m_pending.data(), true);
  }
  const bool keep = !m_dropping;
  // One flush of look-ahead, so a cut can still fade the audio before it.
  if (keep)
  {
    if (!m_held.empty())
      SDL_PutAudioStreamData(m_stream, m_held.data(), int(m_held.size() * sizeof(int16_t)));
    m_held.swap(m_pending);
  }
  m_pending.clear();

  // DUALIZE_AUDIO_STATS=1: queue depth, rate ratio and drops once a second.
  static const bool stats = [] {
    const char* v = std::getenv("DUALIZE_AUDIO_STATS");
    return v && *v == '1';
  }();
  if (stats)
  {
    m_stat_frames += bytes / 4;
    m_stat_dropped += keep ? 0 : bytes / 4;
    m_stat_min_ms = std::min(m_stat_min_ms, queued / bytes_per_ms);
    if (m_stat_frames >= m_rate)
    {
      std::fprintf(stderr, "[audio] queue %u ms (min %u), ratio %.4f, dropped %u frames\n",
                   queued / bytes_per_ms, m_stat_min_ms, m_ratio, m_stat_dropped);
      m_stat_frames = m_stat_dropped = 0;
      m_stat_min_ms = ~0u;
    }
  }

  const uint32_t now_queued = queued + bytes;
  if (!m_playing)
  {
    // Start once a full target is queued, so the first frames never starve.
    if (now_queued >= TARGET_MS * bytes_per_ms)
    {
      SDL_ResumeAudioDevice(m_device);
      m_playing = true;
    }
    return;
  }

  // Drain faster above the target and slower below it, smoothed so the
  // correction never steps audibly.
  const float error = (float(now_queued) / float(TARGET_MS * bytes_per_ms)) - 1.0f;
  const float wanted = 1.0f + std::clamp(error * MAX_CORRECTION, -MAX_CORRECTION, MAX_CORRECTION);
  const float ratio = m_ratio + (wanted - m_ratio) * 0.05f;
  if (std::fabs(ratio - m_ratio) > 1e-5f)
  {
    m_ratio = ratio;
    SDL_SetAudioStreamFrequencyRatio(m_stream, m_ratio);
  }
}

void AudioOut::Fade(int16_t* frames, bool in)
{
  for (size_t i = 0; i < FADE_FRAMES; ++i)
  {
    const int weight = int(in ? i : FADE_FRAMES - 1 - i);
    frames[2 * i] = int16_t(frames[2 * i] * weight / int(FADE_FRAMES));
    frames[2 * i + 1] = int16_t(frames[2 * i + 1] * weight / int(FADE_FRAMES));
  }
}
}  // namespace dualize::apu
