#pragma once

// What the Options screen's natives (nights_options.cpp, and its graphics
// page in nights_graphics.cpp) share.

#include "core/cpu.h"
#include "sdk/nights_text.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dualize
{
class Cpu;
class Machine;
class Memory;
struct Image;
}  // namespace dualize

namespace dualize::nights
{
inline Cpu& CpuOf(CPUState* ctx)
{
  return *static_cast<Cpu*>(ctx->external_user_data);
}

// UTF-16BE, as the game's message data holds text.
inline std::u16string BigEndian(std::u16string_view text)
{
  std::u16string out;
  for (char16_t c : text)
    out.push_back(static_cast<char16_t>((c >> 8) | (c << 8)));
  return out;
}

// Every place `needle` is at in [data, data + size), in order. Two of its
// bytes (non-zero ones: much of memory is zeros) screen 32 places at once,
// on up to `threads` threads: the game waits for the searches of MEM1 and
// MEM2 (a search ahead of time, which it doesn't, takes fewer).
std::vector<uint8_t*> FindAll(uint8_t* data, size_t size, const uint8_t* needle, size_t n, uint32_t threads = 8);
// The same for several needles in one pass over memory: each one's places.
std::vector<std::vector<uint8_t*>> FindAllOf(uint8_t* data, size_t size, std::span<const std::span<const uint8_t>> needles,
                                             uint32_t threads = 8);

// Every copy of an English message's first line in MEM2, where the game keeps
// its message data (the line ends the message, or a newline follows it).
std::vector<uint8_t*> FindMessageLines(const Memory& memory, std::u16string_view line, uint32_t threads = 8);
// The same for several lines in one pass: each one's copies.
std::vector<std::vector<uint8_t*>> FindMessageLinesOf(const Memory& memory, std::span<const std::u16string_view> lines,
                                                      uint32_t threads = 8);
// How many times the game has read a message file: copies of a message
// found before it changes may be gone.
uint32_t MessageReads();

// The Options list's labels texture in a language as the disc serves it
// (decoded, and its CMPR bytes): the pages draw their rows over a copy.
void KeepOptionLabels(Language language, const Image& image, std::span<const uint8_t> encoded);
// The Options title's first row (RGB5A3) in a language as the disc serves it
// and as the graphics page shows it.
void KeepOptionTitle(Language language, std::span<const uint8_t> original, std::span<const uint8_t> graphics);
// The Options screen was built: it shows its own page.
void ResetGraphicsPage();
// The main page's first rows drawn ahead, as the screen loads (watched
// through the game's reads of the UI files).
void PrepareOptionList(const Machine& machine);
// Ends that drawing, before the machine's memory goes.
void StopPreparingOptionList();
// The settings list opening (its state 0, r30 the screen): the main page's
// rows drawn by the page, as the graphics page's are, once it opens.
void TakeOverOptionList(CPUState* ctx, uint32_t screen);
}  // namespace dualize::nights
