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

// Every copy of an English message's first line in MEM2, where the game keeps
// its message data (the line ends the message, or a newline follows it).
std::vector<uint8_t*> FindMessageLines(const Memory& memory, std::u16string_view line);

// The Options list's labels texture in a language as the disc serves it
// (decoded, and its CMPR bytes): the pages draw their rows over a copy.
void KeepOptionLabels(Language language, const Image& image, std::span<const uint8_t> encoded);
// The Options title's first row (RGB5A3) in a language as the disc serves it
// and as the graphics page shows it.
void KeepOptionTitle(Language language, std::span<const uint8_t> original, std::span<const uint8_t> graphics);
// The Options screen was built: it shows its own page.
void ResetGraphicsPage();
// The settings list opening (its state 0, r30 the screen): the main page's
// rows drawn by the page, as the graphics page's are, once it opens.
void TakeOverOptionList(CPUState* ctx, uint32_t screen);
}  // namespace dualize::nights
