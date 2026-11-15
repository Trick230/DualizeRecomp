// Host implementations of nw4r g3d's draw (G3DState's loads and the shape
// loads), see rvl_os.c for the contract every native follows.
//
// Each one does what the guest's function does to G3DState's caches and puts
// the same commands in the GX stream, through the same host GX calls (the
// matrix loads, the shape packet). Unlike the calc natives they don't replay
// the guest's instructions: only memory and the stream come out identical,
// the cycles charged are the guest's typical path and the volatile registers
// are left as they were. Each checks everything it will touch before it
// touches anything, and declines what it doesn't cover (a synced load, which
// flushes GX state through GXCallDisplayList).

#include "core/cpu.h"

#include "ppc_natives.h"
#include "sdk/guest_memory.h"

bool rvl_GXLoadPosMtxImm(CPUState* ctx);
bool rvl_GXLoadNrmMtxImm(CPUState* ctx);
bool rvl_GXLoadTexMtxImm(CPUState* ctx);
bool g3d_CallPrimitiveDisplayList(CPUState* ctx);

#define GATHER_PIPE 0xCC008000u

// G3DState's statics (g3d_state.cpp), from the natives list.
#define STATE ((u32)G3D_STATE)
enum {
    STATE_SYNC = 0x000,            // sSyncGX: the next display list flushes GX state
    STATE_GEN_MODE = 0x004,        // sGenMode2: 4 counts, cull mode, dirty flags
    STATE_TEX_SCALE = 0x010,       // sTexCoordScale
    STATE_SHP_CACHE = 0x204,       // sShp: the vertex format of the last shape
    STATE_IDENTITY = 0x210,        // the current matrices are PNMTX0 and identities
    STATE_TEX_MTX_TYPE = 0x218,    // sPreTexMtxState: 8 function types
    STATE_VIEW_NRM_ARRAY = 0x240,  // sPosNrmArrayState: view normal matrices
};

// TexCoordScaleState.
enum {
    TEX_SCALE_FLAGS = 0x00,  // 1 loaded, 2 valid
    TEX_SCALE_MAP = 0x04,    // per texture map: width, height (u16 each)
    TEX_SCALE_COORD = 0x24,  // per tex coord: width, height (u16 each, 8 bytes apart)
    TEX_SCALE_MAP_ID = 0x64, // per tex coord: its texture map, 0xFF none
};

#define TEX_MTX_FUNC_ENV 1u    // an environment matrix from the normal matrix
#define TEX_MTX_FUNC_NONE 2u   // no matrix: the identity loaded into 0

#define lbz(a) mem_read8_at(ctx, 0, (a))
#define lhz(a) mem_read16_at(ctx, 0, (a))
#define lwz(a) mem_read32_at(ctx, 0, (a))
#define stb(a, v) mem_write8_at(ctx, 0, (a), (u8)(v))
#define sth(a, v) mem_write16_at(ctx, 0, (a), (u16)(v))
#define stw(a, v) mem_write32_at(ctx, 0, (a), (u32)(v))
#define fifo8(v) stb(GATHER_PIPE, (v))
#define fifo16(v) sth(GATHER_PIPE, (v))
#define fifo32(v) stw(GATHER_PIPE, (v))

static inline bool plain(CPUState* ctx, u32 address, u32 size) {
    return guest_span(ctx, address, size) != NULL;
}

// --- GD writes (g3d_gpu.cpp) --------------------------------------------------

static void bp(CPUState* ctx, u32 value) {
    fifo8(0x61);
    fifo32(value);
}

static void xf_header(CPUState* ctx, u32 reg, u32 count) {
    fifo8(0x10);
    fifo16(count - 1u);
    fifo16(reg);
}

// GDSetGenMode2: nonzero counts (the tev count's minus 1 wraps into the
// register byte as the game's does), the cull mode through the SDK's table.
static void gd_set_gen_mode2(CPUState* ctx, u32 tex_gens, u32 chans, u32 tevs, u32 inds,
                             u32 cull) {
    const u32 hw_cull = lbz((u32)G3D_CULL_TABLE + cull);
    bp(ctx, 0xFE07FC3Fu);
    bp(ctx, tex_gens | ((chans << 4) & 0xFF0u) | ((tevs - 1u) << 10) | ((inds << 16) & 0xFF0000u) |
                (hw_cull << 14));
    xf_header(ctx, 0x1009, 1);
    fifo32(chans);
    xf_header(ctx, 0x103F, 1);
    fifo32(tex_gens);
}

// GDSetTexCoordScale2 for tex coord `coord`, without bias or cylindrical wrap.
static void gd_set_tex_coord_scale(CPUState* ctx, u32 coord, u32 s, u32 t) {
    bp(ctx, 0xFE03FFFFu);
    bp(ctx, ((coord * 2u + 0x30u) << 24) | (s - 1u));
    bp(ctx, ((coord * 2u + 0x31u) << 24) | (t - 1u));
}

// GDResetCurrentMtx: PNMTX0 and the identity for every tex coord.
static void gd_reset_current_mtx(CPUState* ctx) {
    fifo8(0x08);
    fifo8(0x30);
    fifo32(0x3CF3CF00u);
    fifo8(0x08);
    fifo8(0x40);
    fifo32(0x00F3CF3Cu);
    xf_header(ctx, 0x1018, 2);
    fifo32(0x3CF3CF00u);
    fifo32(0x00F3CF3Cu);
}

// GDSetCurrentMtx: the tex coords' matrices (the position's stays PNMTX0).
static void gd_set_current_mtx(CPUState* ctx, const u32 id[8]) {
    xf_header(ctx, 0x1018, 2);
    fifo32((id[0] << 6) | (id[1] << 12) | (id[2] << 18) | (id[3] << 24));
    fifo32(id[4] | (id[5] << 6) | (id[6] << 12) | (id[7] << 18));
}

// --- G3DState ------------------------------------------------------------------

// TexCoordScaleState::LoadTexCoordScale(num_tex_gens), once per material.
static void load_tex_coord_scale(CPUState* ctx, u32 count) {
    const u32 state = STATE + STATE_TEX_SCALE;
    const u32 flags = lwz(state + TEX_SCALE_FLAGS);
    if (!(flags & 2u) || (flags & 1u) || count == 0)
        return;
    for (u32 i = 0; i < count; ++i) {
        const u32 map = lbz(state + TEX_SCALE_MAP_ID + i);
        if (map == 0xFFu)
            continue;
        const u32 s = lhz(state + TEX_SCALE_MAP + map * 4u);
        const u32 t = lhz(state + TEX_SCALE_MAP + map * 4u + 2u);
        sth(state + TEX_SCALE_COORD + i * 8u, s);
        sth(state + TEX_SCALE_COORD + i * 8u + 2u, t);
        gd_set_tex_coord_scale(ctx, i, s, t);
    }
    stw(state + TEX_SCALE_FLAGS, lwz(state + TEX_SCALE_FLAGS) | 1u);
}

// GenMode2::LoadXF, when the material changed the counts.
static void load_gen_mode(CPUState* ctx) {
    const u32 state = STATE + STATE_GEN_MODE;
    const u32 flags = lwz(state + 8u);
    if (!(flags & 4u) || (flags & 3u) == 3u)
        return;
    gd_set_gen_mode2(ctx, lbz(state), lbz(state + 1u), lbz(state + 2u), lbz(state + 3u),
                     lwz(state + 4u));
    stw(state + 8u, lwz(state + 8u) | 3u);
}

// The guest's cycles per path, measured with its callees (the GX natives
// called here charge their own on top).
#define PRE_PRIMITIVE_CYCLES 122
#define PRE_PRIMITIVE_SAME_CYCLES 5
#define PRIMITIVE_CYCLES 227
#define PRIMITIVE_ENVELOPE_CYCLES 107
#define PRIMITIVE_NORMAL_CYCLES 16
#define PRIMITIVE_RESET_MTX_CYCLES 27
#define PRIMITIVE_SET_MTX_CYCLES 35
#define PRIMITIVE_ENV_MTX_CYCLES 59
#define NO_TEX_COORD_SCALE_CYCLES 6
#define TEX_COORD_SCALE_CYCLES 12
#define TEX_COORD_SCALE_COORD_CYCLES 52
#define GEN_MODE_CYCLES 45  // not seen in the scenes measured: a guess

// The cycles of load_tex_coord_scale and load_gen_mode, before they run.
static s64 state_load_cycles(CPUState* ctx) {
    const u32 flags = lwz(STATE + STATE_TEX_SCALE + TEX_SCALE_FLAGS);
    const u32 count = lbz(STATE + STATE_GEN_MODE);
    s64 cycles = 0;
    if (!(flags & 2u))
        cycles -= NO_TEX_COORD_SCALE_CYCLES;
    else if (!(flags & 1u) && count != 0)
        cycles += TEX_COORD_SCALE_CYCLES + TEX_COORD_SCALE_COORD_CYCLES * count;
    const u32 gen = lwz(STATE + STATE_GEN_MODE + 8u);
    if ((gen & 4u) && (gen & 3u) != 3u)
        cycles += GEN_MODE_CYCLES;
    return cycles;
}

// void LoadResShpPrePrimitive(ResShp shp): the shape's vertex format display
// list, after the vertex descriptor's part when the last shape had the same.
bool g3d_LoadResShpPrePrimitive(CPUState* ctx) {
    const u32 shp = lwz(ctx->gpr[3]);
    if (shp == 0) {
        ctx->downcount -= 12;
        return true;
    }
    if (lbz(STATE + STATE_SYNC) != 0)
        return false;
    s64 cycles = PRE_PRIMITIVE_CYCLES + state_load_cycles(ctx);
    load_tex_coord_scale(ctx, lbz(STATE + STATE_GEN_MODE));
    load_gen_mode(ctx);
    const u32 cache = STATE + STATE_SHP_CACHE;
    bool same = true;
    for (u32 i = 0; i < 3u; ++i)
        same = same && lwz(shp + 0xCu + i * 4u) == lwz(cache + i * 4u);
    if (!same)
        for (u32 i = 0; i < 3u; ++i)
            stw(cache + i * 4u, lwz(shp + 0xCu + i * 4u));
    stb(STATE + STATE_SYNC, 0);
    const u32 tag = shp + 0x18u, offset = lwz(tag + 8u);
    u32 list = offset != 0 ? tag + offset : 0u, size = lwz(tag + 4u);
    if (same) {
        cycles += PRE_PRIMITIVE_SAME_CYCLES;
        list += 0x20u;
        size -= 0x20u;
    }
    fifo8(0x40);
    fifo32(list);
    fifo32(size);
    ctx->downcount -= cycles;
    return true;
}

// void LoadResShpPrimitive(ResShp shp, const MTX34* view_pos, const MTX34*
// view_nrm): the shape's matrices (its node's view matrix, or the envelope's
// already in the indexed array) and its primitives.
bool g3d_LoadResShpPrimitive(CPUState* ctx) {
    const u32 handle = ctx->gpr[3], view_pos = ctx->gpr[4], view_nrm = ctx->gpr[5];
    const u32 shp = lwz(handle);
    if (shp == 0) {
        ctx->downcount -= 20;
        return true;
    }
    if (lbz(STATE + STATE_SYNC) != 0)
        return false;
    const u32 mtx = lwz(shp + 8u);
    const bool envelope = (mtx >> 31) != 0;
    const bool normals = (lwz(shp + 0x30u) & 0x400u) != 0;
    const u32 id = mtx & 0xFFFFu, nrm_array = lwz(STATE + STATE_VIEW_NRM_ARRAY);
    if (!envelope) {
        // Without the array the normal matrix is the inverse transpose of the
        // view's: rare, left to the guest.
        for (u32 i = 0; i < 8u; ++i)
            if (lwz(STATE + STATE_TEX_MTX_TYPE + i * 4u) == TEX_MTX_FUNC_ENV &&
                (nrm_array == 0 || !plain(ctx, nrm_array + id * 0x24u, 0x24u)))
                return false;
        if (view_pos != 0 &&
            (!plain(ctx, view_pos, 48u) || (normals && view_nrm != 0 && !plain(ctx, view_nrm, 48u))))
            return false;
    }
    const u32 sp = ctx->gpr[1], r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    s64 cycles = (envelope ? PRIMITIVE_ENVELOPE_CYCLES : PRIMITIVE_CYCLES) + state_load_cycles(ctx);
    load_tex_coord_scale(ctx, lbz(STATE + STATE_GEN_MODE));
    const u32 identity = STATE + STATE_IDENTITY;
    if (!envelope) {
        if (normals)
            cycles += PRIMITIVE_NORMAL_CYCLES;
        if (view_pos != 0) {
            ctx->gpr[3] = view_pos;
            ctx->gpr[4] = 0;
            rvl_GXLoadPosMtxImm(ctx);
            if (normals) {
                ctx->gpr[3] = view_nrm != 0 ? view_nrm : view_pos;
                ctx->gpr[4] = 0;
                rvl_GXLoadNrmMtxImm(ctx);
            }
        } else {
            // GXLoadPosMtxIndx(id, GX_PNMTX0), GXLoadNrmMtxIndx3x3(id, GX_PNMTX0).
            fifo8(0x20);
            fifo32((id << 16) | 0xB000u);
            if (normals) {
                fifo8(0x28);
                fifo32((id << 16) | 0x8000u | 0x400u);
            }
        }
        // PreTexMtxState::SetCurrentMtx: each tex coord's matrix.
        u32 ids[8];
        bool all_identity = true;
        for (u32 i = 0; i < 8u; ++i) {
            const u32 type = lwz(STATE + STATE_TEX_MTX_TYPE + i * 4u);
            if (type == TEX_MTX_FUNC_ENV) {
                ids[i] = 0x1Eu + i * 3u;
                // GDLoadTexMtxImm3x3(view_nrm[id], ids[i]) through the guest's
                // 3x4 copy on its stack.
                const u32 nrm = nrm_array + id * 0x24u, copy = sp - 0x60u - 0x40u + 8u;
                for (u32 row = 0; row < 3u; ++row) {
                    for (u32 col = 0; col < 3u; ++col)
                        stw(copy + row * 16u + col * 4u, lwz(nrm + row * 12u + col * 4u));
                    stw(copy + row * 16u + 12u, lwz((u32)G3D_ZERO));
                }
                ctx->gpr[3] = copy;
                ctx->gpr[4] = ids[i];
                ctx->gpr[5] = 0;  // GX_MTX3x4
                rvl_GXLoadTexMtxImm(ctx);
                cycles += PRIMITIVE_ENV_MTX_CYCLES;
                all_identity = false;
            } else if (type == TEX_MTX_FUNC_NONE) {
                ids[i] = 0;
                all_identity = false;
            } else {
                ids[i] = 0x3Cu;
            }
        }
        if (!all_identity) {
            cycles += PRIMITIVE_SET_MTX_CYCLES;
            gd_set_current_mtx(ctx, ids);
            stb(identity, 0);
        } else if (lbz(identity) == 0) {
            cycles += PRIMITIVE_RESET_MTX_CYCLES;
            gd_reset_current_mtx(ctx);
            stb(identity, 1);
        }
    } else if (lbz(identity) == 0) {
        // CurrMtxState::SetCurrMtx: the envelope's display list loads its own.
        cycles += PRIMITIVE_RESET_MTX_CYCLES;
        gd_reset_current_mtx(ctx);
        stb(identity, 1);
    }
    load_gen_mode(ctx);
    stb(STATE + STATE_SYNC, 0);
    ctx->gpr[3] = handle;
    ctx->gpr[4] = 0;
    g3d_CallPrimitiveDisplayList(ctx);
    if (envelope)
        stb(identity, 0);
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->downcount -= cycles;
    return true;
}
