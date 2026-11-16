// Host implementations of nw4r g3d's draw (G3DState's loads and the shape
// loads), see rvl_os.c for the contract every native follows.
//
// Each one does what the guest's function does to G3DState's caches and puts
// the same commands in the GX stream, through the same host GX calls (the
// matrix loads, the shape packet). Unlike the calc natives they don't replay
// the guest's instructions: only memory and the stream come out identical,
// the cycles charged are the guest's typical path and the volatile registers
// are left as they were. Each checks everything it will touch before it
// touches anything, and declines what it doesn't cover.

#include <math.h>
#include <string.h>

#include "core/cpu.h"

#include "ppc_natives.h"
#include "sdk/guest_memory.h"

bool dualize_os_sync(CPUState* ctx, u32 next);

bool rvl_GXLoadPosMtxImm(CPUState* ctx);
bool rvl_GXLoadNrmMtxImm(CPUState* ctx);
bool rvl_GXLoadTexMtxImm(CPUState* ctx);
bool rvl_GXLoadLightObjImm(CPUState* ctx);
bool g3d_CallPrimitiveDisplayList(CPUState* ctx);

#define GATHER_PIPE 0xCC008000u

// G3DState's statics (g3d_state.cpp), from the natives list.
#define STATE ((u32)G3D_STATE)
enum {
    STATE_SYNC = 0x000,            // sSyncGX: the next display list flushes GX state
    STATE_GEN_MODE = 0x004,        // sGenMode2: 4 counts, cull mode, dirty flags
    STATE_TEX_SCALE = 0x010,       // sTexCoordScale
    STATE_TEX_OBJ = 0x080,         // the last 8 tex objects loaded (32 bytes each)
    STATE_TEX_OBJ_VALID = 0x180,   // byte: which of them are
    STATE_TLUT = 0x1A0,            // the last 8 TLUT objects loaded (12 bytes each)
    STATE_TLUT_VALID = 0x200,      // half: which of them are
    STATE_SHP_CACHE = 0x204,       // sShp: the vertex format of the last shape
    STATE_IDENTITY = 0x210,        // the current matrices are PNMTX0 and identities
    STATE_TEX_MTX_TYPE = 0x218,    // sPreTexMtxState: 8 function types
    STATE_TEV = 0x238,             // the last tev loaded
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
#define GEN_MODE_CYCLES 42

// The cycles of load_tex_coord_scale, before it runs.
static s64 tex_scale_cycles(CPUState* ctx) {
    const u32 flags = lwz(STATE + STATE_TEX_SCALE + TEX_SCALE_FLAGS);
    const u32 count = lbz(STATE + STATE_GEN_MODE);
    s64 cycles = 0;
    if (!(flags & 2u))
        cycles -= NO_TEX_COORD_SCALE_CYCLES;
    else if (!(flags & 1u) && count != 0)
        cycles += TEX_COORD_SCALE_CYCLES + TEX_COORD_SCALE_COORD_CYCLES * count;
    return cycles;
}

// The cycles of load_gen_mode, before it runs.
static s64 gen_mode_cycles(CPUState* ctx) {
    const u32 gen = lwz(STATE + STATE_GEN_MODE + 8u);
    return (gen & 4u) && (gen & 3u) != 3u ? GEN_MODE_CYCLES : 0;
}

static s64 state_load_cycles(CPUState* ctx) { return tex_scale_cycles(ctx) + gen_mode_cycles(ctx); }

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

// --- GX ---------------------------------------------------------------------

// __GXData, through the SDK's small-data pointer to it.
#define GX_DATA lwz(ctx->gpr[2] + (u32)GX_DATA_SDA2)
enum {
    GX_DATA_BP_SENT_NOT = 0x000,  // nonzero: no flush primitive needed before a display list
    GX_DATA_SU_TS0 = 0x108,       // per tex coord: s scale and bias
    GX_DATA_SU_TS1 = 0x128,       // t
    GX_DATA_TREF = 0x150,         // per two tev stages: their tex coords and maps
    GX_DATA_TEX_FLUSH = 0x174,    // the BP that flushes the texture state
    GX_DATA_PE_CTRL = 0x22C,
    GX_DATA_TEX_REGIONS = 0x258,  // 3 x 8 regions (16 bytes each): even, odd LODs, RGBA8 mipmaps
    GX_DATA_TLUT_REGIONS = 0x3D8, // 20 x 16 bytes
    GX_DATA_GEN_MODE = 0x254,
    GX_DATA_IREF = 0x170,         // the indirect stages' tex coords and maps
    GX_DATA_TEX_MAP_SIZE = 0x564, // per tex map
    GX_DATA_TEX_MAP_WRAP = 0x584,
    GX_DATA_TEV_TEX_MAP = 0x5A4,  // per tev stage
    GX_DATA_MANUAL_COORDS = 0x5E4,
    GX_DATA_TEV_TC_ENABLED = 0x5E8,
    GX_DATA_TEX_REGION_CALLBACK = 0x518,
    GX_DATA_TLUT_REGION_CALLBACK = 0x51C,
    GX_DATA_DIRTY_STATE = 0x5FC,
};
enum { GX_DIRTY_SU_TEX = 1u };

// The guest's cycles, measured (the indirect stages' are not seen in the
// scenes measured: a guess).
#define GX_SU_TEX_REGS_CYCLES 60
#define GX_SU_TEX_REGS_IND_CYCLES 20
#define GX_SU_TEX_REGS_TEV_CYCLES 14
#define GX_SU_REGS_CYCLES 36
#define GX_CALL_DISPLAY_LIST_CYCLES 23
#define GX_DIRTY_STATE_CYCLES 35
#define PPC_SYNC_CYCLES 12

// __SetSURegs: tex coord `coord`'s scale from map `map`'s size and repeat modes.
static void gx_set_su_regs(CPUState* ctx, u32 gx, u32 map, u32 coord) {
    const u32 ts0 = gx + GX_DATA_SU_TS0 + coord * 4u, ts1 = gx + GX_DATA_SU_TS1 + coord * 4u;
    const u32 size = lwz(gx + GX_DATA_TEX_MAP_SIZE + map * 4u);
    const u32 wrap = lwz(gx + GX_DATA_TEX_MAP_WRAP + map * 4u);
    stw(ts0, (lwz(ts0) & 0xFFFE0000u) | (size & 0x3FFu) | ((u32)((wrap & 3u) == 1u) << 16));
    stw(ts1, (lwz(ts1) & 0xFFFE0000u) | ((size >> 10) & 0x3FFu) |
                 ((u32)(((wrap >> 2) & 3u) == 1u) << 16));
    bp(ctx, lwz(ts0));
    bp(ctx, lwz(ts1));
    sth(gx + 2u, 0);
}

// __GXSetSUTexRegs: the scales of the coords the indirect and tev stages
// sample, unless set by hand. Returns its cycles.
static s64 gx_set_su_tex_regs(CPUState* ctx, u32 gx) {
    const u32 manual = lwz(gx + GX_DATA_MANUAL_COORDS);
    if (manual == 0xFFu)
        return GX_SU_TEX_REGS_CYCLES;
    const u32 gen = lwz(gx + GX_DATA_GEN_MODE), iref = lwz(gx + GX_DATA_IREF);
    const u32 inds = (gen >> 16) & 7u, tevs = ((gen >> 10) & 0xFu) + 1u;
    s64 cycles = GX_SU_TEX_REGS_CYCLES + GX_SU_TEX_REGS_IND_CYCLES * inds +
                 GX_SU_TEX_REGS_TEV_CYCLES * tevs;
    for (u32 i = 0; i < inds; ++i) {
        const u32 map = (iref >> (i * 6u)) & 7u, coord = (iref >> (i * 6u + 3u)) & 7u;
        if (!(manual & (1u << coord))) {
            gx_set_su_regs(ctx, gx, map, coord);
            cycles += GX_SU_REGS_CYCLES;
        }
    }
    const u32 enabled = lwz(gx + GX_DATA_TEV_TC_ENABLED);
    for (u32 i = 0; i < tevs; ++i) {
        const u32 tref = lwz(gx + GX_DATA_TREF + (i >> 1) * 4u);
        const u32 coord = (i & 1u ? tref >> 15 : tref >> 3) & 7u;
        const u32 map = lwz(gx + GX_DATA_TEV_TEX_MAP + i * 4u) & ~0x100u;
        if (map != 0xFFu && !(manual & (1u << coord)) && (enabled & (1u << i))) {
            gx_set_su_regs(ctx, gx, map, coord);
            cycles += GX_SU_REGS_CYCLES;
        }
    }
    return cycles;
}

// Whether GXCallDisplayList can run here: no dirty state but the tex coord
// scales, and no flush primitive needed before the list.
static bool gx_call_display_list_ready(CPUState* ctx) {
    const u32 gx = GX_DATA, dirty = lwz(gx + GX_DATA_DIRTY_STATE);
    if (dirty & ~GX_DIRTY_SU_TEX)
        return false;
    // __SetSURegs clears the half at +2; the one at +0 then decides alone.
    return dirty ? lhz(gx) != 0 : lwz(gx + GX_DATA_BP_SENT_NOT) != 0;
}

// GXCallDisplayList, once gx_call_display_list_ready. Returns its cycles.
static s64 gx_call_display_list(CPUState* ctx, u32 list, u32 size) {
    const u32 gx = GX_DATA;
    s64 cycles = GX_CALL_DISPLAY_LIST_CYCLES;
    if (lwz(gx + GX_DATA_DIRTY_STATE) != 0) {
        cycles += GX_DIRTY_STATE_CYCLES + gx_set_su_tex_regs(ctx, gx);
        stw(gx + GX_DATA_DIRTY_STATE, 0);
    }
    fifo8(0x40);
    fifo32(list);
    fifo32(size);
    return cycles;
}

// The texture loads, with the SDK's region callbacks (the natives decline
// a game's own).
static bool gx_default_regions(CPUState* ctx, u32 gx) {
    return lwz(gx + GX_DATA_TEX_REGION_CALLBACK) == (u32)GX_TEX_REGION_CALLBACK &&
           lwz(gx + GX_DATA_TLUT_REGION_CALLBACK) == (u32)GX_TLUT_REGION_CALLBACK;
}

// A tex map's BP register: `reg` is map 0's (maps 4-7 at +0x20).
static u32 gx_tex_reg(u32 reg, u32 map) { return map < 4u ? reg + map : reg + 0x1Cu + map; }

// __GXDefaultTexRegionCallback: the region by format and mipmapping.
static u32 gx_tex_region(CPUState* ctx, u32 gx, u32 obj, u32 map) {
    const s32 fmt = (s32)lwz(obj + 0x14u);
    const bool mip = (lbz(obj + 0x1Fu) & 1u) != 0;
    u32 table;
    if (fmt == 6)
        table = mip ? 2u : 1u;  // RGBA8
    else if (fmt >= 8 && fmt <= 10)
        table = 0u;  // color indexed
    else
        table = mip ? 1u : 0u;
    return gx + GX_DATA_TEX_REGIONS + table * 0x80u + (map & 7u) * 16u;
}

// Whether gx_load_tex_obj can load `obj`: a TLUT it names is in the table.
static bool gx_tex_obj_ok(CPUState* ctx, u32 obj) {
    return (lbz(obj + 0x1Fu) & 2u) || lwz(obj + 0x18u) < 20u;
}

// GXLoadTexObj(obj, map): its 6 BPs (and its TLUT's when it has one), the
// size and wrap for the tex coord scales, which it dirties.
static void gx_load_tex_obj(CPUState* ctx, u32 gx, u32 obj, u32 map) {
    const u32 region = gx_tex_region(ctx, gx, obj, map);
    const u32 mode0 = (lwz(obj) & 0xFFFFFFu) | gx_tex_reg(0x80u, map) << 24;
    const u32 mode1 = (lwz(obj + 4u) & 0xFFFFFFu) | gx_tex_reg(0x84u, map) << 24;
    const u32 image0 = (lwz(obj + 8u) & 0xFFFFFFu) | gx_tex_reg(0x88u, map) << 24;
    const u32 image1 = (lwz(region) & 0xFFFFFFu) | gx_tex_reg(0x8Cu, map) << 24;
    const u32 image2 = (lwz(region + 4u) & 0xFFFFFFu) | gx_tex_reg(0x90u, map) << 24;
    const u32 image3 = (lwz(obj + 0xCu) & 0xFFFFFFu) | gx_tex_reg(0x94u, map) << 24;
    bp(ctx, mode0);
    bp(ctx, mode1);
    bp(ctx, image0);
    bp(ctx, image1);
    bp(ctx, image2);
    bp(ctx, image3);
    stw(obj, mode0);
    stw(obj + 4u, mode1);
    stw(obj + 8u, image0);
    stw(region, image1);
    stw(region + 4u, image2);
    stw(obj + 0xCu, image3);
    if (!(lbz(obj + 0x1Fu) & 2u)) {
        const u32 tlut = gx + GX_DATA_TLUT_REGIONS + lwz(obj + 0x18u) * 16u;
        stw(tlut + 4u, (lwz(tlut + 4u) & 0xFFFFFFu) | gx_tex_reg(0x98u, map) << 24);
        bp(ctx, lwz(tlut + 4u));
    }
    stw(gx + GX_DATA_TEX_MAP_SIZE + map * 4u, image0);
    stw(gx + GX_DATA_TEX_MAP_WRAP + map * 4u, mode0);
    stw(gx + GX_DATA_DIRTY_STATE, lwz(gx + GX_DATA_DIRTY_STATE) | GX_DIRTY_SU_TEX);
    sth(gx + 2u, 0);
}

// GXLoadTlut(obj, name), between two texture flushes.
static void gx_load_tlut(CPUState* ctx, u32 gx, u32 obj, u32 name) {
    const u32 region = gx + GX_DATA_TLUT_REGIONS + name * 16u;
    bp(ctx, lwz(gx + GX_DATA_TEX_FLUSH));
    sth(gx + 2u, 0);
    bp(ctx, lwz(obj + 4u));
    bp(ctx, lwz(region));
    bp(ctx, lwz(gx + GX_DATA_TEX_FLUSH));
    sth(gx + 2u, 0);
    const u32 tlut = (lwz(obj) & ~0x3FFu) | (lwz(region) & 0x3FFu);
    stw(region + 4u, tlut);
    stw(region + 8u, lwz(obj + 4u));
    stw(region + 12u, lwz(obj + 8u));
    stw(obj, tlut);
}

// --- Material loads -------------------------------------------------------------

// The guest's cycles per path, measured with their callees.
#define MAT_DL_CYCLES 39
#define MAT_IND_MTX_CYCLES 35
#define MAT_IND_MTX_DL_CYCLES 9
#define MAT_GEN_MODE_BP_CYCLES 29

// GenMode2::LoadBP: the gen mode register alone, when the material changed
// the counts and the XF part is still to come. Returns its cycles.
static s64 load_gen_mode_bp(CPUState* ctx) {
    const u32 state = STATE + STATE_GEN_MODE;
    const u32 flags = lwz(state + 8u);
    if (!(flags & 4u) || (flags & 3u) != 2u)
        return 0;
    const u32 cull = lbz((u32)G3D_GEN_MODE_CULL_TABLE + lwz(state + 4u));
    bp(ctx, 0xFE07FC3Fu);
    bp(ctx, lbz(state) | (lbz(state + 1u) << 4) | ((lbz(state + 2u) - 1u) << 10) | (cull << 14) |
                (lbz(state + 3u) << 16));
    stw(state + 8u, flags | 1u);
    return MAT_GEN_MODE_BP_CYCLES;
}

// A material display list of `size` bytes after the gen mode's BP: what the
// pixel, tev color and indirect matrix loads share. `count` 0 skips the list.
static bool load_mat_dl(CPUState* ctx, u32 size, u32 count, s64 cycles) {
    const u32 dl = lwz(ctx->gpr[3]);
    if (dl == 0) {
        ctx->downcount -= 8;
        return true;
    }
    // After a synced load (the z compare location's), GXCallDisplayList.
    const bool sync = lbz(STATE + STATE_SYNC) != 0;
    if (sync && count != 0 && !gx_call_display_list_ready(ctx))
        return false;
    cycles += load_gen_mode_bp(ctx);
    stb(STATE + STATE_SYNC, 0);
    if (count != 0) {
        if (sync) {
            cycles += gx_call_display_list(ctx, dl, size);
        } else {
            // GXFastCallDisplayList.
            fifo8(0x40);
            fifo32(dl);
            fifo32(size);
        }
    }
    ctx->downcount -= cycles;
    return true;
}

// void LoadResMatPix(ResMatPix pix): alpha compare, z mode, blend mode, dst alpha.
bool g3d_LoadResMatPix(CPUState* ctx) { return load_mat_dl(ctx, 0x20u, 1u, MAT_DL_CYCLES); }

// void LoadResMatTevColor(ResMatTevColor color): the tev's colors and konst colors.
bool g3d_LoadResMatTevColor(CPUState* ctx) { return load_mat_dl(ctx, 0x80u, 1u, MAT_DL_CYCLES); }

// void LoadResMatIndMtxAndScale(ResMatIndMtxAndScale ind): the indirect
// matrices and scales, if the material has indirect stages.
bool g3d_LoadResMatIndMtx(CPUState* ctx) {
    const u32 count = lbz(STATE + STATE_GEN_MODE + 3u);
    return load_mat_dl(ctx, 0x40u, count, MAT_IND_MTX_CYCLES + (count ? MAT_IND_MTX_DL_CYCLES : 0));
}


// The guest's cycles: per map unused, the cached object's, a load.
#define TEX_OBJ_CYCLES 149
#define TEX_OBJ_MAP_CYCLES 10
#define TEX_OBJ_SAME_CYCLES 102
#define TEX_OBJ_LOAD_CYCLES 241
#define TEX_OBJ_TLUT_CYCLES 20  // with a TLUT: not seen in the scenes measured, a guess
#define TLUT_CYCLES 144
#define TLUT_MAP_CYCLES 10
#define TLUT_SAME_CYCLES 20  // these two not seen either: guesses
#define TLUT_LOAD_CYCLES 80
#define NO_TEX_CYCLES 8

// Whether the `words` words at `obj` equal those at `cache`.
static bool same_words(CPUState* ctx, u32 obj, u32 cache, u32 words) {
    for (u32 i = 0; i < words; ++i)
        if (lwz(obj + i * 4u) != lwz(cache + i * 4u))
            return false;
    return true;
}

// void LoadResMatTlut(ResMatTlutObj tlut): the TLUTs of the material's maps
// that differ from the last loaded, which invalidates those maps' objects.
bool g3d_LoadResMatTlut(CPUState* ctx) {
    const u32 data = lwz(ctx->gpr[3]);
    if (data == 0) {
        ctx->downcount -= NO_TEX_CYCLES;
        return true;
    }
    const u32 gx = GX_DATA;
    if (!plain(ctx, data, 4u + 8u * 12u) || !gx_default_regions(ctx, gx))
        return false;
    const u32 used = lwz(data);
    s64 cycles = TLUT_CYCLES;
    for (u32 i = 0; i < 8u; ++i) {
        cycles += TLUT_MAP_CYCLES;
        if (!(used & (1u << i)))
            continue;
        const u32 obj = data + 4u + i * 12u, cache = STATE + STATE_TLUT + i * 12u;
        if ((lhz(STATE + STATE_TLUT_VALID) & (1u << i)) && same_words(ctx, obj, cache, 3u)) {
            cycles += TLUT_SAME_CYCLES;
            continue;
        }
        sth(STATE + STATE_TLUT_VALID, lhz(STATE + STATE_TLUT_VALID) | (1u << i));
        for (u32 w = 0; w < 3u; ++w)
            stw(cache + w * 4u, lwz(obj + w * 4u));
        gx_load_tlut(ctx, gx, obj, i);
        stb(STATE + STATE_TEX_OBJ_VALID, lbz(STATE + STATE_TEX_OBJ_VALID) & ~(1u << i));
        cycles += TLUT_LOAD_CYCLES;
    }
    stb(STATE + STATE_SYNC, 1);
    ctx->downcount -= cycles;
    return true;
}

// void LoadResMatTexObj(ResMatTexObj tex): the tex objects of the material's
// maps that differ from the last loaded, and their sizes for the tex coord
// scales.
bool g3d_LoadResMatTexObj(CPUState* ctx) {
    const u32 data = lwz(ctx->gpr[3]);
    if (data == 0) {
        ctx->downcount -= NO_TEX_CYCLES;
        return true;
    }
    const u32 gx = GX_DATA;
    if (!plain(ctx, data, 4u + 8u * 32u) || !gx_default_regions(ctx, gx))
        return false;
    const u32 used = lwz(data);
    for (u32 i = 0; i < 8u; ++i)
        if ((used & (1u << i)) && !gx_tex_obj_ok(ctx, data + 4u + i * 32u))
            return false;
    s64 cycles = TEX_OBJ_CYCLES;
    for (u32 i = 0; i < 8u; ++i) {
        cycles += TEX_OBJ_MAP_CYCLES;
        if (!(used & (1u << i)))
            continue;
        const u32 obj = data + 4u + i * 32u, cache = STATE + STATE_TEX_OBJ + i * 32u;
        if ((lbz(STATE + STATE_TEX_OBJ_VALID) & (1u << i)) && same_words(ctx, obj, cache, 8u)) {
            cycles += TEX_OBJ_SAME_CYCLES;
            continue;
        }
        stb(STATE + STATE_TEX_OBJ_VALID, lbz(STATE + STATE_TEX_OBJ_VALID) | (1u << i));
        for (u32 w = 0; w < 8u; ++w)
            stw(cache + w * 4u, lwz(obj + w * 4u));
        cycles += TEX_OBJ_LOAD_CYCLES + (lbz(obj + 0x1Fu) & 2u ? 0 : TEX_OBJ_TLUT_CYCLES);
        gx_load_tex_obj(ctx, gx, obj, i);
        const u32 image0 = lwz(obj + 8u), scale = STATE + STATE_TEX_SCALE;
        sth(scale + TEX_SCALE_MAP + i * 4u, (image0 & 0x3FFu) + 1u);
        sth(scale + TEX_SCALE_MAP + i * 4u + 2u, ((image0 >> 10) & 0x3FFu) + 1u);
        stw(scale + TEX_SCALE_FLAGS, (lwz(scale + TEX_SCALE_FLAGS) & ~1u) | 2u);
    }
    stb(STATE + STATE_SYNC, 1);
    ctx->downcount -= cycles;
    return true;
}

// The guest's cycles: per count or the cull mode, unchanged or not.
#define MAT_GEN_MODE_CYCLES 16
#define MAT_GEN_MODE_SAME_CYCLES 7
#define MAT_GEN_MODE_SET_CYCLES 17

// void LoadResMatGenMode(ResGenMode gen): the material's counts and cull
// mode into sGenMode2, which a change dirties (the counts its BP, the tex
// coords their scales too), with the flag that it has one.
bool g3d_LoadResMatGenMode(CPUState* ctx) {
    const u32 data = lwz(ctx->gpr[3]);
    if (data == 0) {
        ctx->downcount -= 3;
        return true;
    }
    const u32 state = STATE + STATE_GEN_MODE, flags = state + 8u;
    s64 cycles = MAT_GEN_MODE_CYCLES;
    for (u32 i = 0; i < 4u; ++i) {
        const u32 count = lbz(data + i);
        if (lbz(state + i) == count) {
            cycles += MAT_GEN_MODE_SAME_CYCLES;
            continue;
        }
        stb(state + i, count);
        stw(flags, lwz(flags) & (i < 2u ? ~3u : ~1u));
        cycles += MAT_GEN_MODE_SET_CYCLES;
    }
    const u32 cull = lwz(data + 4u);
    if (lwz(state + 4u) == cull) {
        cycles += MAT_GEN_MODE_SAME_CYCLES;
    } else {
        stw(state + 4u, cull);
        stw(flags, lwz(flags) & ~1u);
        cycles += MAT_GEN_MODE_SET_CYCLES;
    }
    if (!(lwz(flags) & 4u))
        stw(flags, (lwz(flags) & ~3u) | 4u);
    ctx->downcount -= cycles;
    return true;
}

// The guest's cycles: per color or channel control written, per second
// channel (fitted on the scenes measured; CHAN_NONE_CYCLES is a guess).
#define CHAN_CYCLES 180
#define CHAN_NONE_CYCLES 30
#define CHAN_XF_CYCLES 12
#define CHAN_SECOND_CYCLES 52
#define CHAN_ONE_CYCLES 40  // the second channel's controls cleared

static float f32_bits(u32 bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void xf_reg(CPUState* ctx, u32 reg, u32 value) {
    xf_header(ctx, reg, 1u);
    fifo32(value);
}

// A channel control (`chan` 0-1 color, 2-3 alpha) with its light mask; the
// lit form keeps the material source bit.
static void xf_chan_ctrl(CPUState* ctx, u32 chan, u32 ctrl, u32 lights, bool lit) {
    const u32 value = (ctrl & (lit ? 0xFFFF87C1u : 0xFFFF87C3u)) | (lights & 0xFu) << 2 | (lights & 0xF0u) << 7;
    xf_reg(ctx, 0x100Eu + (chan & 3u), value);
}

// The material color scaled by `scale`, a component at a time as the guest's
// floats round it.
static u32 chan_mat_color(CPUState* ctx, u32 color, u32 scale) {
    const float k = f32_bits(lwz((u32)G3D_CHAN_COLOR_SCALE)), round = f32_bits(lwz((u32)G3D_CHAN_COLOR_ROUND));
    u32 out = 0;
    for (u32 i = 0; i < 4u; ++i) {
        const float x = (float)(s32)(lbz(scale + i) * lbz(color + i));
        const float scaled = k * x;
        const float c = round + scaled;
        out = out << 8 | ((u32)(s32)c & 0xFFu);
    }
    return out;
}

// void LoadResMatChan(ResMatChan chan, u32 color0_lights, u32 alpha0_lights,
// u32 color1_lights, u32 alpha1_lights, const GXColor* scale, bool lit): the
// channels' colors and controls (the second only with two channels), then
// the gen mode's XF if it changed.
bool g3d_LoadResMatChan(CPUState* ctx) {
    const u32 data = lwz(ctx->gpr[3]);
    if (data == 0) {
        ctx->downcount -= CHAN_NONE_CYCLES;
        return true;
    }
    const u32 scale = ctx->gpr[8];
    if (!plain(ctx, data, 0x28u) || !plain(ctx, scale, 4u))
        return false;
    const bool lit = ctx->gpr[9] != 0;
    s64 cycles = CHAN_CYCLES;
    u32 flags = lwz(data);
    if (flags & 3u) {
        xf_reg(ctx, 0x100Cu, lwz(data + 4u));
        cycles += CHAN_XF_CYCLES;
    }
    const u32 mat = chan_mat_color(ctx, data + 8u, scale);
    if (flags & 0xCu) {
        xf_reg(ctx, 0x100Au, mat);
        cycles += CHAN_XF_CYCLES;
    }
    if (flags & 0x10u) {
        xf_chan_ctrl(ctx, 0u, lwz(data + 0xCu), ctx->gpr[4], lit);
        cycles += CHAN_XF_CYCLES;
    }
    if (flags & 0x20u) {
        xf_chan_ctrl(ctx, 2u, lwz(data + 0x10u), ctx->gpr[5], lit);
        cycles += CHAN_XF_CYCLES;
    }
    if (lbz(STATE + STATE_GEN_MODE + 1u) == 2u) {
        flags = lwz(data + 0x14u);
        cycles += CHAN_SECOND_CYCLES;
        if (flags & 3u) {
            xf_reg(ctx, 0x100Du, lwz(data + 0x18u));
            cycles += CHAN_XF_CYCLES;
        }
        if (flags & 0xCu) {
            xf_reg(ctx, 0x100Bu, lwz(data + 0x1Cu));
            cycles += CHAN_XF_CYCLES;
        }
        if (flags & 0x10u) {
            xf_chan_ctrl(ctx, 1u, lwz(data + 0x20u), ctx->gpr[6], lit);
            cycles += CHAN_XF_CYCLES;
        }
        if (flags & 0x20u) {
            xf_chan_ctrl(ctx, 3u, lwz(data + 0x24u), ctx->gpr[7], lit);
            cycles += CHAN_XF_CYCLES;
        }
    } else {
        xf_chan_ctrl(ctx, 1u, 0u, 0u, false);
        xf_chan_ctrl(ctx, 3u, 0u, 0u, false);
        cycles += 2 * CHAN_XF_CYCLES + CHAN_ONE_CYCLES;
    }
    cycles += gen_mode_cycles(ctx);
    load_gen_mode(ctx);
    ctx->downcount -= cycles;
    return true;
}

// LoadResTexSrt's static: which post-transform texture matrices hold the
// identity.
#define STATE_POST_TEX_IDENTITY 0x214u
#define TEX_SRT_SIZE 0x248u  // ResTexSrtData: flags, mode, 8 TexSrt, 8 TexMtxEffect
#define TEX_SRT_EFFECT 0xA8u

// The guest's cycles: per call, per texture by path, per Maya matrix made
// (counted for the ones not seen yet), GXLoadTexMtxImm's own apart.
#define TEX_SRT_CYCLES 57
#define TEX_SRT_NONE_CYCLES 19
#define TEX_SRT_IDENT_CYCLES 58
#define TEX_SRT_IDENT_LOAD_CYCLES 79
#define TEX_SRT_CALC_CYCLES 135
#define TEX_SRT_IDENTITY_MODE_CYCLES 40
static const s16 tex_srt_maya_cycles[7] = {140, 129, 22, 16, 132, 117, 16};

static u32 bits_f32(float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    return bits;
}

static float f32_at(CPUState* ctx, u32 address) { return f32_bits(lwz(address)); }

// The FPU state host float math matches the guest's in: on, round to
// nearest (the host's flush to zero follows NI), and SinCosFIdx's u16
// conversions through GQR3 unscaled.
static bool tex_srt_fpu_ok(CPUState* ctx) {
    return (ctx->msr & PPC_MSR_FP) && (ctx->fpscr & 3u) == 0 && (ctx->hid2 & PPC_HID2_LSQE) &&
           (ctx->gqr[3] & 0x3F073F07u) == 0x00050005u;
}

// SinCosFIdx(s, c, fidx) as the guest's floats compute it; false when its
// range loop would run.
static bool sin_cos_fidx(CPUState* ctx, float fidx, float* s, float* c) {
    const float index = f32_bits(bits_f32(fidx) & 0x7FFFFFFFu);
    if (!(index < f32_at(ctx, (u32)SINCOS_RANGE)))
        return false;
    const s32 whole = (s32)index;
    const float rest = index - (float)whole;
    const u32 entry = (u32)SINCOS_TABLE + (((u32)whole << 4) & 0xFF0u);
    const float ds = rest * f32_at(ctx, entry + 8u);
    float sv = f32_at(ctx, entry) + ds;
    const float dc = rest * f32_at(ctx, entry + 12u);
    const float cv = f32_at(ctx, entry + 4u) + dc;
    if (fidx < f32_at(ctx, (u32)SINCOS_ZERO))
        sv = -sv;
    *s = sv;
    *c = cv;
    return true;
}

// The Maya texture matrix functions' constants (0, 1, degrees to FIdx, 0.5,
// -0.5), then the Make functions MakeTexSrtMtx_SRT .. _S by index = the
// TexSrt flag's scale-one, rotate-zero and translate-zero bits.
enum { MAYA_ZERO = 0x0, MAYA_ONE = 0x4, MAYA_TO_FIDX = 0x8, MAYA_HALF = 0xC, MAYA_MINUS_HALF = 0x10, MAYA_MAKE = 0x14 };
// Each Make function's distance below CalcTexMtx_Maya, as the game has them.
static const u16 maya_make_offset[7] = {0x800, 0x910, 0x858, 0xA14, 0x9D4, 0xAB8, 0xAF8};

static bool maya_uses_angle(u32 index) { return !(index & 2u); }

// The first two rows MakeTexSrtMtx_<index> writes for `srt` (Su, Sv, R, Tu,
// Tv), one guest float operation per statement.
static void maya_make(CPUState* ctx, u32 index, u32 srt, float s, float c, float m[8]) {
    const u32 k = (u32)G3D_MAYA_CONST;
    const float zero = f32_at(ctx, k + MAYA_ZERO), one = f32_at(ctx, k + MAYA_ONE);
    const float half = f32_at(ctx, k + MAYA_HALF), minus_half = f32_at(ctx, k + MAYA_MINUS_HALF);
    const float su = f32_at(ctx, srt), sv = f32_at(ctx, srt + 4u);
    const float tu = f32_at(ctx, srt + 12u), tv = f32_at(ctx, srt + 16u);
    switch (index) {
    case 0: {  // SRT
        const float hs = half * s;
        const float hc = minus_half * c;
        const float a = hs - half;
        const float b = hc - a;
        const float d = hc + a;
        const float u = b - tu;
        const float v = tv + d;
        const float sv_v = sv * v;
        m[0] = su * c;
        m[1] = su * s;
        m[2] = zero;
        m[3] = su * u;
        m[4] = -sv * s;
        m[5] = sv * c;
        m[6] = zero;
        m[7] = one + sv_v;
        break;
    }
    case 1: {  // RT
        const float hc = half * c;
        const float hs = half * s;
        const float a = half - hc;
        const float b = a - hs;
        const float d = a + hs;
        m[0] = c;
        m[1] = s;
        m[2] = zero;
        m[3] = b - tu;
        m[4] = -s;
        m[5] = c;
        m[6] = zero;
        m[7] = tv + d;
        break;
    }
    case 2: {  // ST
        const float t = tv - one;
        const float a = sv * t;
        m[0] = su;
        m[1] = zero;
        m[2] = zero;
        m[3] = -su * tu;
        m[4] = zero;
        m[5] = sv;
        m[6] = zero;
        m[7] = one + a;
        break;
    }
    case 3:  // T
        m[0] = one;
        m[1] = zero;
        m[2] = zero;
        m[3] = -tu;
        m[4] = zero;
        m[5] = one;
        m[6] = zero;
        m[7] = tv;
        break;
    case 4: {  // SR
        const float svc = sv * c;
        const float svs = sv * s;
        const float suc = su * c;
        const float sus = su * s;
        const float a = svs - svc;
        const float b = sus + suc;
        const float a2 = a - sv;
        const float b2 = b - su;
        const float ha = half * a2;
        m[0] = suc;
        m[1] = sus;
        m[2] = zero;
        m[3] = minus_half * b2;
        m[4] = -svs;
        m[5] = svc;
        m[6] = zero;
        m[7] = one + ha;
        break;
    }
    case 5: {  // R
        const float hc = half * c;
        const float hs = half * s;
        const float a = half - hc;
        m[0] = c;
        m[1] = s;
        m[2] = zero;
        m[3] = a - hs;
        m[4] = -s;
        m[5] = c;
        m[6] = zero;
        m[7] = a + hs;
        break;
    }
    default:  // S
        m[0] = su;
        m[1] = zero;
        m[2] = zero;
        m[3] = zero;
        m[4] = zero;
        m[5] = sv;
        m[6] = zero;
        m[7] = one - sv;
        break;
    }
}

// GXLoadTexMtxImm(m, GX_PTTEXMTX0 + 3 * i, GX_MTX3x4) from the guest's copy
// at `address`.
static void load_post_tex_mtx_at(CPUState* ctx, u32 address, u32 i) {
    ctx->gpr[3] = address;
    ctx->gpr[4] = 0x40u + i * 3u;
    ctx->gpr[5] = 0;
    rvl_GXLoadTexMtxImm(ctx);
}

static void load_post_tex_mtx(CPUState* ctx, u32 address, const float m[12], u32 i) {
    for (u32 j = 0; j < 12u; ++j)
        stw(address + j * 4u, bits_f32(m[j]));
    load_post_tex_mtx_at(ctx, address, i);
}

// G3DState's LightState: the set loaded and its four masks, the light in
// each of GX's 8 slots, 128 LightObj (flags, then the GXLightObj), 128
// ambient colors, 128 sets (8 light ids, the ambient id).
enum {
    LIGHT_SET_LOADED = 0x10,
    LIGHT_MASK_DIFF_COLOR = 0x14,
    LIGHT_MASK_DIFF_ALPHA = 0x18,
    LIGHT_MASK_SPEC_COLOR = 0x1C,
    LIGHT_MASK_SPEC_ALPHA = 0x20,
    LIGHT_SLOT = 0x24,
    LIGHT_OBJ = 0x2C,
    LIGHT_AMB = 0x222C,
    LIGHT_SET = 0x242C,
    LIGHT_STATE_SIZE = 0x2E2C,
};
enum { LIGHT_ENABLE = 4, LIGHT_SPECULAR = 2, LIGHT_NO_COLOR = 0x10, LIGHT_NO_ALPHA = 0x20 };
#define LIGHT_OBJ_SIZE 0x44u
#define LIGHT_SET_SIZE 0xCu

// The texture map modes' functions (ScnDependentTexMtxFuncTable at STATE +
// 0x248: 256 of function, type) and the camera matrices they read
// (CameraMtxState: flags, the view index, the inverse of the view's camera
// matrix made on first use, 32 camera, projection and environment matrices).
#define STATE_TEX_MAP_FUNC 0x248u
enum {
    CAMERA_FLAGS = 0x0,  // half: 1 the inverse is made
    CAMERA_VIEW = 0x2,   // half
    CAMERA_INVERSE = 0x4,
    CAMERA_MTX = 0x34,
    CAMERA_PROJ = 0xE34,
    CAMERA_ENV = 0x1434,
    CAMERA_STATE_SIZE = 0x1A34,
};
#define CAMERA_COUNT 32
#define MTX34_SIZE 0x30u

// The guest's cycles of each mapping function by path, its natives apart.
#define TEX_SRT_MAP_CYCLES 85
#define MAP_INVERSE_CYCLES 16
#define MAP_INVERSE_MAKE_CYCLES 12
#define MAP_ENV_CAMERA_CYCLES 66
#define MAP_ENV_NONE_CYCLES 43
#define MAP_ENV_LIGHT_OFF_CYCLES 58
#define MAP_PROJ_CAMERA_CYCLES 68
#define MAP_PROJ_NONE_CYCLES 35
#define MAP_DEFAULT_CYCLES 14
#define MAP_ENV_LIGHT_CYCLES 182
#define MAP_LIGHT_DIR_CYCLES 30
#define MAP_LIGHT_POS_CYCLES 50
#define MAP_LIGHT_POS_ZERO_DIR_CYCLES 58
#define MAP_LIGHT_AT_EYE_CYCLES 3
#define MAP_VEC_IS_NEXT_CYCLES 3
#define MAP_UP_X_CYCLES 10
#define MAP_UP_Z_CYCLES 14
#define MAP_UP_POLE_CYCLES 18
#define MAP_HALF_NORMALIZE_CYCLES 45
#define MAP_HALF_COPY_CYCLES 50
#define MAP_SPEC_HALF_CYCLES 5
#define MAP_SPEC_NEG_CYCLES 15
#define MAP_SPEC_LIGHT_CYCLES 203
#define MAP_SPEC_CAMERA_CYCLES 140
#define MAP_SPEC_NONE_CYCLES 30

// The light paths' vectors, in scratch below the native's frame where the
// guest's mapping functions keep theirs; a LightObj's GXLightObj position and
// direction (after its flags word).
enum {
    V_DIR = 0x00,
    V_UP = 0x0C,
    V_A = 0x18,
    V_B = 0x24,
    V_EYE = 0x30,
    V_T = 0x3C,
    V_NA = 0x48,
    V_NB = 0x54,
    V_H = 0x60,
    V_HALF = 0x6C,
    MAP_SCRATCH = 0x78,
};
enum { GX_LIGHT_POS = 0x2C, GX_LIGHT_DIR = 0x38 };

bool rvl_PSMTXConcat(CPUState* ctx);
bool rvl_PSMTXCopy(CPUState* ctx);
bool rvl_PSMTXInverse(CPUState* ctx);
bool rvl_PSVECNormalize(CPUState* ctx);
bool rvl_PSVECCrossProduct(CPUState* ctx);
bool rvl_PSVECAdd(CPUState* ctx);
bool rvl_PSVECDotProduct(CPUState* ctx);
_Static_assert(TS_ENV_CONCAT == TS_CONCAT && TS_ENV_COPY == TS_COPY && TS_CONCAT == SKIN_PSMTX_CONCAT &&
                   TS_COPY == CV_RVL_COPY && TS_INVERSE == TS_RVL_INVERSE,
               "the mapping functions' callees with natives");
_Static_assert(TS_SPEC_CONCAT == TS_CONCAT && TS_SPEC_COPY == TS_COPY && TS_ENV_INVERSE == G3D_GET_INV_CAMERA &&
                   TS_SPEC_INVERSE == G3D_GET_INV_CAMERA && TS_NORMALIZE == RVL_VEC_NORMALIZE &&
                   TS_SPEC_NORMALIZE == RVL_VEC_NORMALIZE && TS_HALF_NORMALIZE == RVL_VEC_NORMALIZE &&
                   TS_CROSS == RVL_VEC_CROSS && TS_SPEC_CROSS == RVL_VEC_CROSS && TS_HALF_ADD == RVL_VEC_ADD &&
                   TS_HALF_DOT == RVL_VEC_DOT,
               "the light mapping functions' callees with natives");

static void map_concat(CPUState* ctx, u32 a, u32 b, u32 ab) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = ab;
    rvl_PSMTXConcat(ctx);
}

static void map_copy(CPUState* ctx, u32 src, u32 dst) {
    ctx->gpr[3] = src;
    ctx->gpr[4] = dst;
    rvl_PSMTXCopy(ctx);
}

// The light a mapping function reads, 0 when out of range.
static u32 map_light(s8 index) {
    return index >= 0 ? (u32)G3D_LIGHT_STATE + LIGHT_OBJ + (u32)index * LIGHT_OBJ_SIZE : 0u;
}

// What the natives below need: the float state PSMTXConcat's and the PSVEC
// natives' replays run in, the camera state as plain memory, and a function
// this file knows; the light paths also the light, their constants and
// scratch below the native's frame for the vectors.
static bool tex_map_ok(CPUState* ctx, u32 mode, s8 camera, s8 light, u32 scratch) {
    if (!(ctx->msr & PPC_MSR_FP) || (ctx->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)) || !(ctx->hid2 & PPC_HID2_LSQE) ||
        (ctx->fpscr & 3u) || (ctx->gqr[0] & 0x00070007u) || !plain(ctx, (u32)G3D_CAMERA_STATE, CAMERA_STATE_SIZE) ||
        lhz((u32)G3D_CAMERA_STATE + CAMERA_VIEW) >= CAMERA_COUNT)
        return false;
    const u32 func = lwz(STATE + STATE_TEX_MAP_FUNC + mode * 8u), obj = map_light(light);
    const bool env = func == (u32)G3D_ENV_MAPPING, spec = func == (u32)G3D_ENV_SPEC_MAPPING;
    if (!env && !spec)
        return func == (u32)G3D_PROJ_MAPPING || func == (u32)G3D_DEFAULT_MAPPING;
    if (env && camera >= 0)
        return true;
    if (obj && !plain(ctx, obj, LIGHT_OBJ_SIZE))
        return false;
    const bool lit = obj && (lwz(obj) & LIGHT_ENABLE);
    if (!lit && (env || camera < 0))
        return true;  // the view's environment matrix
    const u32 r2 = ctx->gpr[2];
    return plain(ctx, scratch, MAP_SCRATCH) && plain(ctx, (u32)G3D_ENV_LIGHT_MTX, MTX34_SIZE) &&
           plain(ctx, (u32)G3D_ENV_ZERO, 4) && plain(ctx, (u32)G3D_ENV_ONE, 4) && plain(ctx, (u32)G3D_ENV_EPS, 4) &&
           plain(ctx, (u32)G3D_ENV_UP, 4) && plain(ctx, r2 + (u32)SDA2_HALF, 4) &&
           plain(ctx, r2 + (u32)SDA2_THREE, 4) && plain(ctx, r2 + (u32)HALF_ANGLE_EPS, 4);
}

// GetInvCameraMtxPtr: the inverse of the view's camera matrix, made once.
static u32 map_inverse(CPUState* ctx, s64* cycles) {
    const u32 state = (u32)G3D_CAMERA_STATE;
    *cycles += MAP_INVERSE_CYCLES;
    const u32 flags = lhz(state + CAMERA_FLAGS);
    if (!(flags & 1u)) {
        ctx->gpr[3] = state + CAMERA_MTX + lhz(state + CAMERA_VIEW) * MTX34_SIZE;
        ctx->gpr[4] = state + CAMERA_INVERSE;
        rvl_PSMTXInverse(ctx);
        sth(state + CAMERA_FLAGS, flags | 1u);
        *cycles += MAP_INVERSE_MAKE_CYCLES;
    }
    return state + CAMERA_INVERSE;
}

static void vec_set(CPUState* ctx, u32 v, u32 x, u32 y, u32 z) {
    stw(v, x);
    stw(v + 4u, y);
    stw(v + 8u, z);
}

// fneg: the sign bit flipped.
static void vec_neg(CPUState* ctx, u32 dst, u32 src) {
    vec_set(ctx, dst, lwz(src) ^ 0x80000000u, lwz(src + 4u) ^ 0x80000000u, lwz(src + 8u) ^ 0x80000000u);
}

// Whether v is (c, c, c), the guest's fcmpu per component until one differs.
static bool vec_is(CPUState* ctx, u32 v, float c, s64* cycles) {
    for (u32 i = 0; i < 3u; ++i) {
        if (i)
            *cycles += MAP_VEC_IS_NEXT_CYCLES;
        if (!(f32_at(ctx, v + i * 4u) == c))
            return false;
    }
    return true;
}

static void vec_normalize(CPUState* ctx, u32 src, u32 unit) {
    ctx->gpr[3] = src;
    ctx->gpr[4] = unit;
    rvl_PSVECNormalize(ctx);
}

static void vec_cross(CPUState* ctx, u32 a, u32 b, u32 axb) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = axb;
    rvl_PSVECCrossProduct(ctx);
}

// VEC3TransformNormal: fmuls and fadds as the guest pairs them, unfused.
static void vec_transform_normal(CPUState* ctx, u32 out, u32 mtx, u32 v) {
#pragma clang fp contract(off)
    const float x = f32_at(ctx, v), y = f32_at(ctx, v + 4u), z = f32_at(ctx, v + 8u);
    float r[3];
    for (u32 i = 0; i < 3u; ++i) {
        const u32 row = mtx + i * 16u;
        const float xy = f32_at(ctx, row) * x + f32_at(ctx, row + 4u) * y;
        r[i] = f32_at(ctx, row + 8u) * z + xy;
    }
    vec_set(ctx, out, bits_f32(r[0]), bits_f32(r[1]), bits_f32(r[2]));
}

// The light's vector in view space, toward the light: its direction when it
// has one (flags 1 or 2) that isn't zero, else from its position.
static void map_light_vec(CPUState* ctx, u32 obj, u32 inverse, u32 v, s64* cycles) {
    const float zero = f32_at(ctx, (u32)G3D_ENV_ZERO);
    const u32 flags = lwz(obj);
    *cycles += (flags & 1u) ? 4 : (flags & 2u) ? 6 : 8;  // LightObj::IsDirectional's test
    vec_neg(ctx, v, obj + GX_LIGHT_DIR);                 // GXGetLightDir
    if ((flags & 3u) && !vec_is(ctx, v, zero, cycles)) {
        vec_transform_normal(ctx, v, inverse, v);
        *cycles += MAP_LIGHT_DIR_CYCLES;
        return;
    }
    vec_set(ctx, v, lwz(obj + GX_LIGHT_POS), lwz(obj + GX_LIGHT_POS + 4u), lwz(obj + GX_LIGHT_POS + 8u));
    vec_transform_normal(ctx, v, inverse, v);
    vec_neg(ctx, v, v);
    *cycles += (flags & 3u) ? MAP_LIGHT_POS_ZERO_DIR_CYCLES : MAP_LIGHT_POS_CYCLES;
    if (vec_is(ctx, v, zero, cycles)) {
        stw(v + 4u, lwz((u32)G3D_ENV_ONE));
        *cycles += MAP_LIGHT_AT_EYE_CYCLES;
    }
}

// The up vector for the unit v: y, or z by y's sign when v is along y.
static void map_up(CPUState* ctx, u32 up, u32 v, s64* cycles) {
    const u32 zero = lwz((u32)G3D_ENV_ZERO);
    const float eps = f32_at(ctx, (u32)G3D_ENV_EPS);
    if (!(fabsf(f32_at(ctx, v)) < eps)) {
        vec_set(ctx, up, zero, lwz((u32)G3D_ENV_UP), zero);
        *cycles += MAP_UP_X_CYCLES;
    } else if (!(fabsf(f32_at(ctx, v + 8u)) < eps)) {
        vec_set(ctx, up, zero, lwz((u32)G3D_ENV_UP), zero);
        *cycles += MAP_UP_Z_CYCLES;
    } else {
        const bool down = f32_at(ctx, v + 4u) <= f32_bits(zero);
        vec_set(ctx, up, zero, zero, lwz(down ? (u32)G3D_ENV_ONE : (u32)G3D_ENV_UP));
        *cycles += MAP_UP_POLE_CYCLES;
    }
}

// C_VECHalfAngle(a, b, half): the unit half vector of -a and -b, or their sum
// when it's (nearly) zero.
static void vec_half_angle(CPUState* ctx, u32 a, u32 b, u32 half, u32 scratch, s64* cycles) {
    const u32 na = scratch + V_NA, nb = scratch + V_NB, h = scratch + V_H;
    vec_neg(ctx, na, a);
    vec_neg(ctx, nb, b);
    vec_normalize(ctx, na, na);
    vec_normalize(ctx, nb, nb);
    ctx->gpr[3] = na;
    ctx->gpr[4] = nb;
    ctx->gpr[5] = h;
    rvl_PSVECAdd(ctx);
    ctx->gpr[3] = h;
    ctx->gpr[4] = h;
    rvl_PSVECDotProduct(ctx);
    if (ctx->fpr[1] > (f64)f32_at(ctx, ctx->gpr[2] + (u32)HALF_ANGLE_EPS)) {
        vec_normalize(ctx, h, half);
        *cycles += MAP_HALF_NORMALIZE_CYCLES;
    } else {
        vec_set(ctx, half, lwz(h), lwz(h + 4u), lwz(h + 8u));
        *cycles += MAP_HALF_COPY_CYCLES;
    }
}

static void mtx_rows(CPUState* ctx, u32 m, u32 a, u32 b, u32 c) {
    for (u32 i = 0; i < 3u; ++i) {
        stw(m + i * 4u, lwz(a + i * 4u));
        stw(m + 0x10u + i * 4u, lwz(b + i * 4u));
        stw(m + 0x20u + i * 4u, lwz(c + i * 4u));
    }
}

// m = outer * (m * inverse) with m's translation zeroed in between.
static void map_finish(CPUState* ctx, u32 m, u32 inverse, u32 outer) {
    map_concat(ctx, m, inverse, m);
    const u32 zero = lwz((u32)G3D_ENV_ZERO);
    stw(m + 0x2Cu, zero);
    stw(m + 0x1Cu, zero);
    stw(m + 0x0Cu, zero);
    map_concat(ctx, outer, m, m);
}

// The mode's function into m (ScnDependentMtxFunc: EnvironmentMapping,
// EnvironmentSpecularMapping, ProjectionMapping, DefaultMapping), as
// tex_map_ok allows it.
static void tex_map(CPUState* ctx, u32 mode, u32 m, s8 camera, s8 light, u32 scratch, s64* cycles) {
    const u32 state = (u32)G3D_CAMERA_STATE, func = lwz(STATE + STATE_TEX_MAP_FUNC + mode * 8u);
    const u32 view = lhz(state + CAMERA_VIEW) * MTX34_SIZE, env = state + CAMERA_ENV + view;
    const u32 obj = map_light(light);
    const bool lit = obj && (lwz(obj) & LIGHT_ENABLE);
    if (func == (u32)G3D_ENV_MAPPING) {
        if (camera >= 0) {
            const u32 inverse = map_inverse(ctx, cycles);
            map_concat(ctx, state + CAMERA_MTX + (u32)camera * MTX34_SIZE, inverse, m);
            const u32 zero = lwz((u32)G3D_ENV_ZERO);
            stw(m + 0x2Cu, zero);
            stw(m + 0x1Cu, zero);
            stw(m + 0x0Cu, zero);
            map_concat(ctx, env, m, m);
            *cycles += MAP_ENV_CAMERA_CYCLES;
        } else if (lit) {
            // The light's vector as -z, x and y from the up vector.
            const u32 v = scratch + V_DIR, up = scratch + V_UP, a = scratch + V_A, b = scratch + V_B;
            const u32 inverse = map_inverse(ctx, cycles);
            map_light_vec(ctx, obj, inverse, v, cycles);
            vec_normalize(ctx, v, v);
            map_up(ctx, up, v, cycles);
            vec_cross(ctx, v, up, a);
            vec_normalize(ctx, a, a);
            vec_cross(ctx, a, v, b);
            vec_neg(ctx, up, v);
            mtx_rows(ctx, m, a, b, up);
            map_finish(ctx, m, inverse, (u32)G3D_ENV_LIGHT_MTX);
            *cycles += MAP_ENV_LIGHT_CYCLES;
        } else {
            *cycles += obj ? MAP_ENV_LIGHT_OFF_CYCLES : MAP_ENV_NONE_CYCLES;
            map_copy(ctx, env, m);
        }
    } else if (func == (u32)G3D_ENV_SPEC_MAPPING) {
        // The half vector between the eye (the view's -z) and the light, or
        // the camera's -z, as z.
        const u32 inverse = map_inverse(ctx, cycles);
        const u32 eye = scratch + V_EYE, h = scratch + V_HALF, up = scratch + V_UP, a = scratch + V_A,
                  b = scratch + V_B;
        if (lit) {
            const u32 v = scratch + V_DIR;
            vec_neg(ctx, eye, state + CAMERA_MTX + view + 0x20u);
            map_light_vec(ctx, obj, inverse, v, cycles);
            if (!(lwz(obj) & LIGHT_SPECULAR)) {
                vec_half_angle(ctx, eye, v, h, scratch, cycles);
                *cycles += MAP_SPEC_HALF_CYCLES;
            } else {
                vec_neg(ctx, h, v);
                *cycles += MAP_SPEC_NEG_CYCLES;
            }
            map_up(ctx, up, h, cycles);
            *cycles += MAP_SPEC_LIGHT_CYCLES;
        } else if (camera >= 0) {
            const u32 cam = state + CAMERA_MTX + (u32)camera * MTX34_SIZE, t = scratch + V_T;
            vec_neg(ctx, eye, state + CAMERA_MTX + view + 0x20u);
            vec_set(ctx, up, lwz(cam + 0x10u), lwz(cam + 0x14u), lwz(cam + 0x18u));
            vec_neg(ctx, t, cam + 0x20u);
            vec_half_angle(ctx, eye, t, h, scratch, cycles);
            *cycles += MAP_SPEC_CAMERA_CYCLES;
        } else {
            map_copy(ctx, env, m);
            *cycles += MAP_SPEC_NONE_CYCLES;
            return;
        }
        vec_normalize(ctx, h, h);
        vec_cross(ctx, up, h, a);
        vec_normalize(ctx, a, a);
        vec_cross(ctx, h, a, b);
        mtx_rows(ctx, m, a, b, h);
        map_finish(ctx, m, inverse, env);
    } else if (func == (u32)G3D_PROJ_MAPPING) {
        if (camera >= 0) {
            const u32 inverse = map_inverse(ctx, cycles);
            map_concat(ctx, state + CAMERA_MTX + (u32)camera * MTX34_SIZE, inverse, m);
            map_concat(ctx, state + CAMERA_PROJ + (u32)camera * MTX34_SIZE, m, m);
            *cycles += MAP_PROJ_CAMERA_CYCLES;
        } else {
            map_copy(ctx, state + CAMERA_PROJ + view, m);
            *cycles += MAP_PROJ_NONE_CYCLES;
        }
    } else {
        *cycles += MAP_DEFAULT_CYCLES;  // m already holds the identity: PSMTXIdentity
    }
}

// The camera a texture maps from: ids past the 32 cameras are none.
static s8 tex_map_camera(CPUState* ctx, u32 effect) {
    const s8 camera = (s8)lbz(effect);
    return camera < CAMERA_COUNT ? camera : -1;
}

// void LoadResTexSrt(ResTexSrt srt): each texture's post-transform matrix,
// from its TexSrt through the Maya functions (any other mode makes the
// identity), the identity loaded once while it stays; a texture mapped from
// the camera or a light loads its mode's matrix. Declines effect matrices
// that aren't the identity, mappings on a TexSrt that isn't, and those
// tex_map_ok doesn't take.
bool g3d_LoadResTexSrt(CPUState* ctx) {
    const u32 srt = lwz(ctx->gpr[3]);
    const u32 frame = ctx->gpr[1] - 0xE0u, work = frame + 0x68u, ident = frame + 0x8u;
    if (!plain(ctx, frame, 0xE8u) || (srt && !plain(ctx, srt, TEX_SRT_SIZE)) || !tex_srt_fpu_ok(ctx))
        return false;
    const u32 maya = (u32)G3D_MAYA_CONST, maya_entry = (u32)G3D_CALC_TEX_MTX_MAYA;
    for (u32 i = 0; i < 7u; ++i)
        if (lwz(maya + MAYA_MAKE + i * 4u) != maya_entry - maya_make_offset[i])
            return false;
    const u32 flags = srt ? lwz(srt) : 0u, mode = srt ? lwz(srt + 4u) : 0u;
    // Everything it can't do, before touching anything.
    for (u32 i = 0; i < 8u; ++i) {
        if (!(flags >> (i * 4u) & 1u))
            continue;
        const u32 effect = srt + TEX_SRT_EFFECT + i * 0x34u, nibble = flags >> (i * 4u) & 0xFu;
        const u32 map = lbz(effect + 2u);
        if (!(lbz(effect + 3u) & 1u) ||
            (map != 0 && (nibble != 0xFu || !tex_map_ok(ctx, map, tex_map_camera(ctx, effect), (s8)lbz(effect + 1u),
                                                       frame - MAP_SCRATCH))))
            return false;
        if (nibble == 0xFu || mode != 0)
            continue;
        const u32 index = nibble >> 1 & 7u;
        float s, c;
        if (maya_uses_angle(index) &&
            !sin_cos_fidx(ctx, f32_at(ctx, maya + MAYA_TO_FIDX) * f32_at(ctx, srt + 8u + i * 0x14u + 8u), &s, &c))
            return false;
    }
    stw(ctx->gpr[1] + 4u, ctx->lr);  // its LR save into the caller's frame
    s64 cycles = TEX_SRT_CYCLES;
    u32 post_identity = lwz(STATE + STATE_POST_TEX_IDENTITY);
    for (u32 i = 0; i < 8u; ++i) {
        stw(STATE + STATE_TEX_MTX_TYPE + i * 4u, 0);
        if (!(flags >> (i * 4u) & 1u)) {
            cycles += TEX_SRT_NONE_CYCLES;
            continue;
        }
        const u32 nibble = flags >> (i * 4u) & 0xFu;
        float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const u32 effect = srt + TEX_SRT_EFFECT + i * 0x34u, map = lbz(effect + 2u);
        if (map != 0) {
            // The mode's matrix made over the identity and loaded as it is.
            for (u32 j = 0; j < 12u; ++j)
                stw(work + j * 4u, bits_f32(m[j]));
            cycles += TEX_SRT_MAP_CYCLES;
            tex_map(ctx, map, work, tex_map_camera(ctx, effect), (s8)lbz(effect + 1u), frame - MAP_SCRATCH, &cycles);
            stw(STATE + STATE_TEX_MTX_TYPE + i * 4u, lwz(STATE + STATE_TEX_MAP_FUNC + map * 8u + 4u));
            post_identity &= ~(1u << i);
            stw(STATE + STATE_POST_TEX_IDENTITY, post_identity);
            load_post_tex_mtx_at(ctx, work, i);
            continue;
        }
        if (nibble == 0xFu) {
            // The identity: loaded only if the last matrix there wasn't.
            if (post_identity & (1u << i)) {
                cycles += TEX_SRT_IDENT_CYCLES;
                continue;
            }
            post_identity |= 1u << i;
            stw(STATE + STATE_POST_TEX_IDENTITY, post_identity);
            cycles += TEX_SRT_IDENT_LOAD_CYCLES;
            load_post_tex_mtx(ctx, ident, m, i);
            continue;
        }
        cycles += TEX_SRT_CALC_CYCLES;
        const u32 index = nibble >> 1 & 7u;
        if (mode != 0) {
            cycles += TEX_SRT_IDENTITY_MODE_CYCLES;
        } else if (index != 7u) {
            const u32 data = srt + 8u + i * 0x14u;
            float s = 0, c = 0;
            if (maya_uses_angle(index)) {
                const float fidx = f32_at(ctx, maya + MAYA_TO_FIDX) * f32_at(ctx, data + 8u);
                sin_cos_fidx(ctx, fidx, &s, &c);
                if (fidx < 0.0f)
                    ++cycles;
            }
            maya_make(ctx, index, data, s, c, m);
            m[8] = f32_at(ctx, maya + MAYA_ZERO);
            m[9] = m[8];
            m[10] = f32_at(ctx, maya + MAYA_ONE);
            m[11] = m[8];
            cycles += tex_srt_maya_cycles[index];
        }
        post_identity &= ~(1u << i);
        stw(STATE + STATE_POST_TEX_IDENTITY, post_identity);
        load_post_tex_mtx(ctx, work, m, i);
    }
    ctx->downcount -= cycles;
    return true;
}

// Cycles beyond the instructions counted below: both functions' frames and
// GPR saves.
#define LIGHT_SET_CYCLES 28

// std::find over s8 [first, first + 8) as the guest's loop runs it: the
// index, 8 when absent, and its cycles.
static u32 light_find(CPUState* ctx, u32 first, s32 value, s64* cycles) {
    for (u32 i = 0; i < 8u; ++i)
        if ((s8)lbz(first + i) == value) {
            *cycles += 9 + 9 * i;
            return i;
        }
    *cycles += 74;
    return 8;
}

// The mask pointers' stores: 2 per test, 2 per store (the last one's branch
// 3 on the same-set path).
static void light_store_masks(CPUState* ctx, const u32 out[4], u32 light, s64* cycles, bool same) {
    for (u32 i = 0; i < 4u; ++i) {
        *cycles += 2;
        if (out[i]) {
            stw(out[i], lwz(light + LIGHT_MASK_DIFF_COLOR + i * 4u));
            *cycles += 2 + (same && i == 3u);
        }
    }
}

// void LoadLightSet(int id, u32* diffColor, u32* diffAlpha, u32* specColor,
// u32* specAlpha, AmbLightObj* amb): the set's ambient color and the masks of
// its enabled lights, loading into GX's slots those not there yet (into a
// free one, else over one the set doesn't use); ids out of range clear the
// masks and give a white ambient. Declines sets with no slot left.
bool g3d_LoadLightSet(CPUState* ctx) {
    const u32 light = (u32)G3D_LIGHT_STATE;
    const s32 id = (s32)ctx->gpr[3];
    const u32 out[4] = {ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]}, amb = ctx->gpr[8];
    if (!plain(ctx, light, LIGHT_STATE_SIZE) || (amb && !plain(ctx, amb, 4)) || !plain(ctx, ctx->gpr[1] + 4u, 4))
        return false;
    for (u32 i = 0; i < 4u; ++i)
        if (out[i] && !plain(ctx, out[i], 4))
            return false;
    // The wrapper's LR save into the caller's frame.
    stw(ctx->gpr[1] + 4u, ctx->lr);
    if (id < 0 || id >= 0x80) {
        // The wrapper, the entry through the id tests, the mask and ambient
        // tests and stores, the exit.
        s64 cycles = LIGHT_SET_CYCLES + 23 + (id < 0 ? 13 : 15) + 4 * 2 + 2 + 6;
        for (u32 i = 0; i < 4u; ++i)
            if (out[i]) {
                stw(out[i], 0);
                cycles += 2;
            }
        if (amb) {
            for (u32 i = 0; i < 4u; ++i)
                stb(amb + 3u - i, 0xFF);
            cycles += 6;
        }
        ctx->downcount -= cycles;
        return true;
    }
    const u32 set = light + LIGHT_SET + (u32)id * LIGHT_SET_SIZE;
    const bool same = (s32)lwz(light + LIGHT_SET_LOADED) == id;
    if (!same) {
        // Replay the slot choices to find a set that runs out of slots.
        u8 slot[8];
        for (u32 j = 0; j < 8u; ++j)
            slot[j] = lbz(light + LIGHT_SLOT + j);
        for (u32 i = 0; i < 8u; ++i) {
            const s8 index = (s8)lbz(set + i);
            if (index < 0 || !(lwz(light + LIGHT_OBJ + (u32)index * LIGHT_OBJ_SIZE) & LIGHT_ENABLE))
                continue;
            u32 j = 0;
            while (j < 8u && (s8)slot[j] != index)
                ++j;
            if (j < 8u)
                continue;
            j = 0;
            while (j < 8u && (s8)slot[j] != -1)
                ++j;
            if (j == 8u) {
                j = 0;
                for (; j < 8u; ++j) {
                    u32 k = 0;
                    while (k < 8u && (s8)lbz(set + k) != (s8)slot[j])
                        ++k;
                    if (k == 8u)
                        break;
                }
                if (j == 8u)
                    return false;
            }
            slot[j] = (u8)index;
        }
    }
    // The wrapper, then the entry, the id tests and the ambient test.
    s64 cycles = LIGHT_SET_CYCLES + 23 + 15 + 2;
    if (amb) {
        const s8 index = (s8)lbz(set + 8u);
        cycles += 7;
        if (index < 0) {
            for (u32 i = 0; i < 4u; ++i)
                stb(amb + 3u - i, 0xFF);
            cycles += 6;
        } else {
            const u32 from = light + LIGHT_AMB + (u32)index * 4u;
            for (u32 i = 0; i < 4u; ++i)
                stb(amb + i, lbz(from + i));
            cycles += 5 + 9;
        }
    }
    cycles += 3;
    if (same) {
        light_store_masks(ctx, out, light, &cycles, true);
        ctx->downcount -= cycles + 6;
        return true;
    }
    u32 mask[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < 4u; ++i)
        stw(light + LIGHT_MASK_DIFF_COLOR + i * 4u, 0);
    stw(light + LIGHT_SET_LOADED, (u32)id);
    cycles += 15;
    for (u32 i = 0; i < 8u; ++i) {
        const s8 index = (s8)lbz(set + i);
        cycles += 4 + 4;
        if (index < 0)
            continue;
        const u32 obj = light + LIGHT_OBJ + (u32)index * LIGHT_OBJ_SIZE;
        cycles += 7;
        if (!(lwz(obj) & LIGHT_ENABLE))
            continue;
        cycles += 4 + 1 + 3;
        u32 j = light_find(ctx, light + LIGHT_SLOT, index, &cycles);
        if (j < 8u) {
            cycles += 3;
        } else {
            cycles += 3;
            j = 0;
            while (j < 8u && (s8)lbz(light + LIGHT_SLOT + j) != -1) {
                cycles += 7;
                ++j;
            }
            cycles += j < 8u ? 6 : 2;
            cycles += 3;
            if (j == 8u) {
                cycles += 2;
                for (j = 0;; ++j) {
                    cycles += 5 + 1 + 2;
                    if (light_find(ctx, set, (s8)lbz(light + LIGHT_SLOT + j), &cycles) == 8u)
                        break;
                    cycles += 3;
                }
                cycles += 14;
            } else {
                cycles += 15;
            }
            ctx->gpr[3] = obj + 4u;
            ctx->gpr[4] = 1u << j;
            rvl_GXLoadLightObjImm(ctx);
            stb(light + LIGHT_SLOT + j, (u8)index);
        }
        const u32 bit = 1u << j, flags = lwz(obj);
        const u32 kind = flags & LIGHT_SPECULAR ? 2u : 0u;
        cycles += 9 + 2;
        if (!(flags & LIGHT_NO_COLOR)) {
            mask[kind] |= bit;
            stw(light + LIGHT_MASK_DIFF_COLOR + kind * 4u, mask[kind]);
            cycles += 3;
        }
        cycles += 3;
        if (!(flags & LIGHT_NO_ALPHA)) {
            mask[kind + 1u] |= bit;
            stw(light + LIGHT_MASK_DIFF_COLOR + (kind + 1u) * 4u, mask[kind + 1u]);
            cycles += kind ? 4 : 3;
        }
    }
    light_store_masks(ctx, out, light, &cycles, false);
    ctx->downcount -= cycles + 6;
    return true;
}

#define ZCOMPLOC_CYCLES 26
#define ZCOMPLOC_SET_CYCLES 23
#define TEV_SAME_CYCLES 25
#define TEV_CYCLES 107
#define TEV_SU_TEX_CYCLES 4  // the tex coord scales after it: the mean of a data-dependent 0-9
#define TEX_COORD_GEN_CYCLES 53
#define TEX_COORD_GEN_4_CYCLES 1  // the size ladder: 2 or more coords
#define TEX_COORD_GEN_8_CYCLES 3  // 4 or more

// void LoadResMatMisc's z compare location (ResMatMisc misc): GXSetZCompLoc
// when it changed, which makes the next display list a synced one.
bool g3d_LoadResMatZCompLoc(CPUState* ctx) {
    const u32 misc = lwz(ctx->gpr[3]);
    if (misc == 0) {
        ctx->downcount -= 8;
        return true;
    }
    const u32 cache = (u32)G3D_ZCOMPLOC_CACHE, loc = lbz(misc);
    s64 cycles = ZCOMPLOC_CYCLES;
    if (!(lwz(cache) & 1u) || lbz(cache + 4u) != loc) {
        stb(cache + 4u, loc);
        stw(cache, lwz(cache) | 1u);
        // GXSetZCompLoc.
        const u32 gx = GX_DATA, pe = (lwz(gx + GX_DATA_PE_CTRL) & ~0x40u) | ((loc & 1u) << 6);
        stw(gx + GX_DATA_PE_CTRL, pe);
        bp(ctx, pe);
        sth(gx + 2u, 0);
        stb(STATE + STATE_SYNC, 1);
        cycles += ZCOMPLOC_SET_CYCLES;
    }
    ctx->downcount -= cycles;
    return true;
}

// void LoadResMatTev(ResTev tev): the tev stages' display list when the
// material's differs from the last one, and the tex coord scales from the
// texture maps its coords sample.
bool g3d_LoadResMatTev(CPUState* ctx) {
    const u32 tev = lwz(ctx->gpr[3]);
    const u32 last = STATE + STATE_TEV;
    if (tev == 0 || tev == lwz(last)) {
        ctx->downcount -= TEV_SAME_CYCLES;
        return true;
    }
    // A synced load: PPCSync first.
    const bool sync = lbz(STATE + STATE_SYNC) != 0;
    if (!gx_call_display_list_ready(ctx) || (sync && !dualize_os_sync(ctx, (u32)PPC_SYNC + 4u)))
        return false;
    if (sync)
        ctx->downcount -= PPC_SYNC_CYCLES;
    stw(last, tev);
    const bool dirty = lwz(GX_DATA + GX_DATA_DIRTY_STATE) != 0;
    s64 cycles = TEV_CYCLES + load_gen_mode_bp(ctx);
    stb(STATE + STATE_SYNC, 0);
    // ResTev::CallDisplayList.
    cycles += gx_call_display_list(ctx, tev + 0x20u,
                                   lwz((u32)G3D_TEV_DL_SIZE + (lbz(tev + 0xCu) - 1u) * 4u));
    const u32 scale = STATE + STATE_TEX_SCALE, flags = lwz(scale + TEX_SCALE_FLAGS);
    const u32 map0 = lwz(tev + 0x10u), map1 = lwz(tev + 0x14u);
    if (!(flags & 2u) || map0 != lwz(scale + TEX_SCALE_MAP_ID) ||
        map1 != lwz(scale + TEX_SCALE_MAP_ID + 4u)) {
        stw(scale + TEX_SCALE_MAP_ID, map0);
        stw(scale + TEX_SCALE_MAP_ID + 4u, map1);
        stw(scale + TEX_SCALE_FLAGS, (lwz(scale + TEX_SCALE_FLAGS) | 2u) & ~1u);
    }
    cycles += tex_scale_cycles(ctx);
    if (dirty)
        cycles += TEV_SU_TEX_CYCLES;
    load_tex_coord_scale(ctx, lbz(STATE + STATE_GEN_MODE));
    ctx->downcount -= cycles;
    return true;
}

// void LoadResMatTexCoordGen(ResMatTexCoordGen gen): the tex coord
// generators' display list (sized by their count), then the gen mode's XF part.
bool g3d_LoadResMatTexCoordGen(CPUState* ctx) {
    const u32 dl = lwz(ctx->gpr[3]);
    if (dl == 0) {
        ctx->downcount -= 8;
        return true;
    }
    // After a synced load, GXCallDisplayList.
    const u32 count = lbz(STATE + STATE_GEN_MODE);
    const bool sync = lbz(STATE + STATE_SYNC) != 0;
    if (sync && count != 0 && !gx_call_display_list_ready(ctx))
        return false;
    s64 cycles = TEX_COORD_GEN_CYCLES + gen_mode_cycles(ctx);
    stb(STATE + STATE_SYNC, 0);
    if (count >= 2u)
        cycles += TEX_COORD_GEN_4_CYCLES + (count >= 4u ? TEX_COORD_GEN_8_CYCLES : 0);
    if (count != 0) {
        const u32 size = count < 2u ? 0x20u : count < 4u ? 0x40u : count < 8u ? 0x80u : 0xA0u;
        if (sync) {
            cycles += gx_call_display_list(ctx, dl, size);
        } else {
            fifo8(0x40);
            fifo32(dl);
            fifo32(size);
        }
    }
    load_gen_mode(ctx);
    ctx->downcount -= cycles;
    return true;
}
