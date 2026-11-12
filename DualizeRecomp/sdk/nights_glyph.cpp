// The game's text glyphs drawn at their true width and evenly spaced. A glyph
// sprite draws the window of its cell in the glyph cache (tex_w x tex_h
// texels, the atlas's ink) as a quad of 2 * trunc(half * scale) units each way
// around its centre, where half is the window's half plus one texel; a line
// lays its glyphs out the same way, each advancing 2 * trunc(half * scale)
// plus the line's spacing, all in whole units. Down, that is the same ratio
// for every glyph of a font (22 units for 20 texels); across, the extra texel
// and the truncation make it vary with the glyph: 4-texel i/l/t came out 1.0
// unit per texel, 8-texel a/n 0.75, 14-texel m 0.86, so narrow letters looked
// bold and wide ones thin, and their gaps varied as much.
//
// Here every glyph is tex_w texels at the ratio it has down (times the scale
// across over the one down), and a line's glyphs sit at even gaps between the
// first's left side and the last's right side where the game puts them, so the
// line keeps its place and length. The line's draw hands each glyph the
// distance from the game's centre to that one; once the sprite batch has the
// glyph's quad, its sides move there.

#include "core/cpu.h"
#include "ppc_natives.h"

#include "kernel/memory.hpp"
#include "machine.hpp"
#include "sdk/nights_options.hpp"

#include <bit>
#include <cstdint>
#include <vector>

using dualize::nights::CpuOf;

namespace
{
// TextLine: its glyph nodes (8 bytes each, the node first) and their count.
constexpr uint32_t LINE_NODES = 0x10, LINE_COUNT = 0x14, NODE_STRIDE = 8;
// GlyphNode: its character (the first word's high half), its sprite and its
// centre in the line (two ints).
constexpr uint32_t NODE_CHARACTER = 0x0, NODE_SPRITE = 0x4, NODE_X = 0x8;
// GlyphSprite: half size (units before the scale) and cell window (texels).
constexpr uint32_t SPRITE_HALF_W = 0xC, SPRITE_HALF_H = 0x10, SPRITE_WINDOW_W = 0x1C, SPRITE_WINDOW_H = 0x20;
// SpriteBatch: its quads (4 vertices of x, y, z, colour, u, v) and their count.
constexpr uint32_t BATCH_COUNT = 0x88, BATCH_QUADS = 0x8C, QUAD_SIZE = 0x60, VERTEX_SIZE = 0x18;
constexpr uint32_t ADD_QUAD_SLOT = 0x20;
// The text's scale (across, down), behind a pointer.
constexpr uint32_t SCALE_X = 0x4;
// GlyphSprite_Draw's frame: the batch it added to.
constexpr uint32_t FRAME_BATCH = 0x18;
// Characters with no ink: they keep the width the game gives them.
constexpr uint32_t SPACE = 0x20, IDEOGRAPHIC_SPACE = 0x3000;

// The line being drawn: each glyph's centre moved by this much.
uint32_t g_line = 0;
std::vector<float> g_shift;
// The sprite the line draws now and its shift.
uint32_t g_sprite = 0;
float g_sprite_shift = 0.0f;

int32_t Int(dualize::Memory& memory, uint32_t address)
{
  return static_cast<int32_t>(dualize::Load32(memory.Effective(address, 4)));
}

// A sprite's width across at its ratio down, `scale_x` the scale across.
float TrueWidth(dualize::Memory& memory, uint32_t sprite, float scale_x)
{
  const int32_t window_w = Int(memory, sprite + SPRITE_WINDOW_W), window_h = Int(memory, sprite + SPRITE_WINDOW_H);
  if (window_w <= 0 || window_h <= 0)
    return 0.0f;
  return static_cast<float>(window_w) * 2.0f * static_cast<float>(Int(memory, sprite + SPRITE_HALF_H)) * scale_x /
         static_cast<float>(window_h);
}

// The line's glyphs at even gaps (their own scale left out, so a glyph popping
// in doesn't move the others), between the ends the game gave them. Spaces
// keep the width the game gives them, so words stay as far apart.
void LayOut(dualize::Memory& memory, uint32_t line)
{
  const auto word = [&](uint32_t address) { return dualize::Load32(memory.Effective(address, 4)); };
  const uint32_t nodes = word(line + LINE_NODES), count = word(line + LINE_COUNT);
  const float scale_x = std::bit_cast<float>(word(word(NIGHTS_GLYPH_SCALE) + SCALE_X));
  g_line = line;
  g_shift.assign(count, 0.0f);
  if (count < 2 || count > 4096)
    return;
  std::vector<float> width(count), centre(count);
  float inked = 0.0f;
  for (uint32_t i = 0; i < count; ++i)
  {
    const uint32_t node = word(nodes + i * NODE_STRIDE);
    const uint32_t sprite = word(node + NODE_SPRITE);
    const uint32_t character = word(node + NODE_CHARACTER) >> 16;
    if (sprite == 0)
      width[i] = 0.0f;
    else if (character == SPACE || character == IDEOGRAPHIC_SPACE)
      width[i] = 2.0f * static_cast<float>(static_cast<int32_t>(
                            static_cast<float>(Int(memory, sprite + SPRITE_HALF_W)) * scale_x));
    else
      width[i] = TrueWidth(memory, sprite, scale_x);
    centre[i] = static_cast<float>(Int(memory, node + NODE_X));
    inked += width[i];
  }
  const float left = centre[0] - 0.5f * width[0];
  const float right = centre[count - 1] + 0.5f * width[count - 1];
  const float gap = (right - left - inked) / static_cast<float>(count - 1);
  if (!(gap >= 0.0f))
    return;
  float x = left;
  for (uint32_t i = 0; i < count; ++i)
  {
    g_shift[i] = x + 0.5f * width[i] - centre[i];
    x += width[i] + gap;
  }
}
}  // namespace

// TextLine_Draw's loop, as it draws glyph node r30 (r3 the node) of line r27.
// Declines.
extern "C" bool nights_GlyphNodeDraw(CPUState* ctx)
{
  dualize::Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const uint32_t line = ctx->gpr[27], index = ctx->gpr[30];
  if (index == 0 || line != g_line)
    LayOut(memory, line);
  g_sprite = dualize::Load32(memory.Effective(ctx->gpr[3] + NODE_SPRITE, 4));
  g_sprite_shift = index < g_shift.size() ? g_shift[index] : 0.0f;
  return false;
}

// Right after GlyphSprite_Draw's batch->AddQuad: r3 its result, r28 the
// sprite, r29 the centre (two ints), r30 the scale (two floats). Declines.
extern "C" bool nights_GlyphQuad(CPUState* ctx)
{
  const uint32_t sprite = ctx->gpr[28];
  const float shift = sprite == g_sprite ? g_sprite_shift : 0.0f;
  g_sprite = 0;
  if (ctx->gpr[3] == 0)
    return false;
  dualize::Memory& memory = GetMachine(CpuOf(ctx)).memory;
  const auto word = [&](uint32_t address) { return dualize::Load32(memory.Effective(address, 4)); };
  const uint32_t batch = word(ctx->gpr[1] + FRAME_BATCH);
  if (word(word(batch) + ADD_QUAD_SLOT) != NIGHTS_SPRITE_BATCH_ADD_QUAD)
    return false;
  const float half = 0.5f * TrueWidth(memory, sprite, std::bit_cast<float>(word(ctx->gpr[30])));
  if (half <= 0.0f)
    return false;
  const float centre = static_cast<float>(Int(memory, ctx->gpr[29]));

  const uint32_t quad = batch + BATCH_QUADS + (word(batch + BATCH_COUNT) - 1) * QUAD_SIZE;
  for (uint32_t v = 0; v < 4; ++v)
  {
    uint8_t* x = memory.Effective(quad + v * VERTEX_SIZE, 4);
    const float old = std::bit_cast<float>(dualize::Load32(x));
    dualize::Store32(x, std::bit_cast<uint32_t>((old < centre ? centre - half : centre + half) + shift));
  }
  return false;
}
