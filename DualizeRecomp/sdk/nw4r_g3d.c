// Hooks in the game's nw4r g3d (see rvl_os.c for the contract every native
// follows).
//
// g3d builds every model matrix it loads as the camera's view matrix times the
// model's own: knowing the view, frame interpolation blends the models in the
// world and moves the camera along its own path between frames, instead of
// estimating the camera from the draws and guessing which ones follow it.

#include "core/cpu.h"

#define GATHER_PIPE 0xCC008000u

// aurora gx/recomp.hpp HostViewOpcode: a host packet, then the view matrix as
// 12 floats; the draws after it were built with that camera.
#define HOST_VIEW_OPCODE 0x70u

// ScnRoot::CalcView, just after it read the current camera's view matrix into
// its frame at r1 + 12 and before it hands it to every model of the scene.
// The models it computes for are drawn next, before the next CalcView, so the
// view goes into the GX stream here, ahead of their draws. Declines always:
// the game's own code goes on.
bool g3d_CalcViewCamera(CPUState* ctx) {
    const u32 view = ctx->gpr[1] + 12u;
    mem_write8_at(ctx, 0, GATHER_PIPE, (u8)HOST_VIEW_OPCODE);
    for (u32 i = 0; i < 12u; ++i)
        mem_write32_at(ctx, 0, GATHER_PIPE, mem_read32_at(ctx, 0, view + i * 4u));
    return false;
}
