// The game's text glyphs drawn at their true width. A glyph sprite draws the
// window of its cell in the glyph cache (tex_w x tex_h texels, the atlas's ink)
// as a quad of 2 * trunc(half * scale) units each way around its centre, where
// half is the window's half plus one texel. Down, that is the same ratio for
// every glyph of a font (22 units for 20 texels); across, the extra texel and
// the truncation make it vary with the glyph: 4-texel i/l/t come out 1.0 unit
// per texel, 8-texel a/n 0.75, 14-texel m 0.86. So narrow letters looked bold
// and wide ones thin. Here, once the sprite batch has the quad, its sides move
// to tex_w texels at the ratio the glyph has down (times the scale across over
// the one down), around the same centre: every glyph at one ratio, from the
// game's own numbers, and the line's layout as the game made it.

#include "core/cpu.h"
#include "ppc_natives.h"

#include "kernel/memory.hpp"
#include "machine.hpp"
#include "sdk/nights_options.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

using dualize::nights::CpuOf;

namespace
{
// GlyphSprite: half size (units before the scale) and cell window (texels).
constexpr uint32_t SPRITE_HALF_H = 0x10, SPRITE_WINDOW_W = 0x1C, SPRITE_WINDOW_H = 0x20;
// SpriteBatch: its quads (4 vertices of x, y, z, colour, u, v) and their count.
constexpr uint32_t BATCH_COUNT = 0x88, BATCH_QUADS = 0x8C, QUAD_SIZE = 0x60, VERTEX_SIZE = 0x18;
constexpr uint32_t ADD_QUAD_SLOT = 0x20;
// GlyphSprite_Draw's frame: the batch it added to.
constexpr uint32_t FRAME_BATCH = 0x18;
}  // namespace

// Right after GlyphSprite_Draw's batch->AddQuad: r3 its result, r28 the
// sprite, r29 the centre (two ints), r30 the scale (two floats). Declines.
extern "C" bool nights_GlyphQuad(CPUState* ctx)
{
  if (ctx->gpr[3] == 0)
    return false;
  dualize::Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const auto word = [&](uint32_t address) { return dualize::Load32(memory.Effective(address, 4)); };
  const uint32_t batch = word(ctx->gpr[1] + FRAME_BATCH);
  if (word(word(batch) + ADD_QUAD_SLOT) != NIGHTS_SPRITE_BATCH_ADD_QUAD)
    return false;
  const uint32_t sprite = ctx->gpr[28];
  const int32_t window_w = static_cast<int32_t>(word(sprite + SPRITE_WINDOW_W));
  const int32_t window_h = static_cast<int32_t>(word(sprite + SPRITE_WINDOW_H));
  const int32_t half_h = static_cast<int32_t>(word(sprite + SPRITE_HALF_H));
  if (window_w <= 0 || window_h <= 0)
    return false;
  const float scale_x = std::bit_cast<float>(word(ctx->gpr[30]));
  const float centre = static_cast<float>(static_cast<int32_t>(word(ctx->gpr[29])));
  const float half = 0.5f * static_cast<float>(window_w) * 2.0f * static_cast<float>(half_h) * scale_x /
                     static_cast<float>(window_h);

  const uint32_t quad = batch + BATCH_QUADS + (word(batch + BATCH_COUNT) - 1) * QUAD_SIZE;
  for (uint32_t v = 0; v < 4; ++v)
  {
    uint8_t* x = memory.Effective(quad + v * VERTEX_SIZE, 4);
    const float old = std::bit_cast<float>(dualize::Load32(x));
    dualize::Store32(x, std::bit_cast<uint32_t>(old < centre ? centre - half : centre + half));
  }
  return false;
}
