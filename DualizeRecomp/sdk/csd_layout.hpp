#pragma once

// A screen's CSD layout through the game's own calls, made from a native on
// scratch room below the stack pointer (a node handle, a name, and a member
// pointing at a cast for the cast methods). Shared by the Options screen's
// pages and the pause window.

#include "core/cpu.h"
#include "ppc_natives.h"

#include "cpu/cpu.hpp"
#include "kernel/memory.hpp"
#include "machine.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>

namespace dualize::nights
{
class Layout
{
public:
  Layout(Cpu& cpu, CPUState* ctx, uint32_t holder)
      : m_cpu(cpu), m_memory(GetMachine(cpu).memory), m_ctx(ctx), m_sp(ctx->gpr[1])
  {
    ctx->gpr[1] = m_sp - SCRATCH;
    m_scene = cpu.CallGuest(CSD_SCENE_NODE, {holder});
  }
  ~Layout() { m_ctx->gpr[1] = m_sp; }
  Layout(const Layout&) = delete;
  Layout& operator=(const Layout&) = delete;

  // The pattern of the cast named by the string at `name` (guest memory).
  void SetPattern(uint32_t name, uint32_t pattern)
  {
    WithNode(name, [&](uint32_t node) { m_cpu.CallGuest(CSD_SET_PATTERN, {node, pattern}); });
  }

  // The cast named `name` (its runtime object), 0 if the scene has none.
  uint32_t Cast(std::string_view name)
  {
    const uint32_t text = m_sp - NAME;
    uint8_t* bytes = m_memory.Effective(text, static_cast<uint32_t>(name.size()) + 1);
    std::memcpy(bytes, name.data(), name.size());
    bytes[name.size()] = 0;
    uint32_t cast = 0;
    WithNode(text, [&](uint32_t node) { cast = Load(Load(node + NODE_MEMBER) + MEMBER_CAST); });
    return cast;
  }
  uint32_t Parent(uint32_t cast) const { return cast != 0 ? Load(cast + CAST_PARENT) : 0; }
  void SetHidden(uint32_t cast, bool hidden)
  {
    if (cast == 0)
      return;
    m_cpu.CallGuest(CSD_CAST_SET_HIDE, {Member(cast), hidden ? 1u : 0u});
  }
  // A colour (RGBA) over the one the cast's animation gives it.
  void SetColor(uint32_t cast, uint32_t color)
  {
    const uint32_t value = m_sp - COLOR;
    dualize::Store32(m_memory.Effective(value, 4), color);
    m_cpu.CallGuest(CSD_CAST_SET_COLOR, {Member(cast), value});
  }
  // A place (from its parent's, in screen heights) or a scale over the ones
  // the cast's animation gives it.
  void SetY(uint32_t cast, float y) { WithFloat(CSD_CAST_SET_Y, cast, y); }
  void SetScaleX(uint32_t cast, float scale) { WithFloat(CSD_CAST_SET_SCALE_X, cast, scale); }
  void SetScaleY(uint32_t cast, float scale) { WithFloat(CSD_CAST_SET_SCALE_Y, cast, scale); }

  static constexpr uint32_t CAST_PARENT = 0x80;

private:
  static constexpr uint32_t SCRATCH = 0x100, HANDLE = 0x80, NAME = 0x60, COLOR = 0x40, MEMBER = 0x30;
  // A node holds a member, the member its cast.
  static constexpr uint32_t NODE_MEMBER = 0x34, MEMBER_CAST = 0xC;

  uint32_t Load(uint32_t address) const { return dualize::Load32(m_memory.Effective(address, 4)); }
  uint32_t Member(uint32_t cast)
  {
    const uint32_t member = m_sp - MEMBER;
    dualize::Store32(m_memory.Effective(member + MEMBER_CAST, 4), cast);
    return member;
  }
  void WithFloat(uint32_t method, uint32_t cast, float value)
  {
    if (cast == 0)
      return;
    const auto f1 = m_ctx->fpr[1];
    m_ctx->fpr[1] = value;
    m_cpu.CallGuest(method, {Member(cast)});
    m_ctx->fpr[1] = f1;
  }
  void WithNode(uint32_t name, const std::function<void(uint32_t)>& use)
  {
    const uint32_t handle = m_sp - HANDLE;
    m_cpu.CallGuest(CSD_FIND_NODE, {handle, m_scene, name});
    if (const uint32_t node = m_cpu.CallGuest(CSD_SCENE_NODE, {handle}))
      use(node);
    m_cpu.CallGuest(CSD_RELEASE_HANDLE, {handle, 0});
  }

  Cpu& m_cpu;
  Memory& m_memory;
  CPUState* m_ctx;
  uint32_t m_sp, m_scene = 0;
};
}  // namespace dualize::nights
