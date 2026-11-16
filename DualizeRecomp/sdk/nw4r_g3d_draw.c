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

#include <string.h>

#include "core/cpu.h"

#include "ppc_natives.h"
#include "sdk/guest_memory.h"

bool dualize_os_sync(CPUState* ctx, u32 next);

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
