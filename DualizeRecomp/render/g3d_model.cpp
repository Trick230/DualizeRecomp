// Native nw4r g3d models (scene renderer phase 1).
//
// Every g3d model the game draws goes through DrawResMdlDirectly(ResMdl,
// viewPos, viewNrm, viewEnv, byteCodeOpa, byteCodeXlu, replacement, mode).
// With DUALIZE_G3D_SCAN set it reads each model resource once and checks its
// layout (MDL0 revision 10), logging every model it meets. Each shape's
// primitive display list reaches the host as a mesh built once (see
// g3d_CallPrimitiveDisplayList).

#include "core/cpu.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>

namespace {

constexpr u32 kMdl0 = 0x4D444C30u; // 'MDL0'
constexpr u32 kRevision = 10;

// ResMdlData offsets (revision 10) of the dictionaries the draw reads.
enum : u32 {
    kDicByteCode = 0x10,
    kDicNode = 0x14,
    kDicPos = 0x18,
    kDicNrm = 0x1C,
    kDicClr = 0x20,
    kDicTex = 0x24,
    kDicMat = 0x30,
    kDicShp = 0x38,
};

struct Reader {
    CPUState* ctx;
    u32 r32(u32 a) const { return mem_read32_at(ctx, 0, a); }
    u16 r16(u32 a) const { return mem_read16_at(ctx, 0, a); }
    u8 r8(u32 a) const { return mem_read8_at(ctx, 0, a); }
    // A resource offset field: relative to the struct that holds it, 0 = none.
    u32 ofs(u32 base, u32 field) const {
        const u32 v = r32(base + field);
        return v != 0 ? base + v : 0;
    }
    u32 dicCount(u32 dic) const { return dic != 0 ? r32(dic + 4) : 0; }
    // Entry 0 is the root of the patricia tree; data starts at entry 1.
    u32 dicData(u32 dic, u32 i) const { return dic + r32(dic + 8 + (i + 1) * 16 + 12); }
};

// A display list held by a resource (ResTagDLData): its size and where it is,
// relative to the tag.
struct TagDL {
    u32 addr = 0, size = 0;
};
TagDL tagDL(const Reader& r, u32 tag) { return {tag + r.r32(tag + 8), r.r32(tag + 4)}; }

// ResShpData offsets.
enum : u32 {
    kShpPrePrimDL = 0x18,
    kShpPrimDL = 0x24,
    kShpNumVtx = 0x40,
    kShpNumPolygon = 0x44,
};

// GX vertex attributes in CP VCD order: PNMTXIDX, TEX0..7MTXIDX, POS, NRM,
// CLR0, CLR1, TEX0..7. Each is 0 (none), 1 (direct), 2 (index8), 3 (index16).
constexpr u32 kAttrs = 21;

struct ShapeStats {
    u32 bad = 0;
    u32 vertices = 0, triangles = 0, unique = 0, draws = 0, mtxLoads = 0;
};

ShapeStats decodeShape(const Reader& r, u32 shp) {
    ShapeStats st;
    u8 desc[kAttrs] = {};
    // The pre-primitive list sets the vertex descriptor (CP 0x50 and 0x60);
    // the rest it sets (formats, array bases) the ResVtx headers also hold.
    const TagDL pre = tagDL(r, shp + kShpPrePrimDL);
    for (u32 p = pre.addr, end = pre.addr + pre.size; p < end;) {
        const u8 cmd = r.r8(p);
        if (cmd == 0x00) {
            ++p;
        } else if (cmd == 0x08) {
            const u8 reg = r.r8(p + 1);
            const u32 v = r.r32(p + 2);
            if (reg == 0x50) {
                desc[0] = v & 1u;
                for (u32 i = 0; i < 8; ++i)
                    desc[1 + i] = (v >> (1 + i)) & 1u;
                for (u32 i = 0; i < 4; ++i)
                    desc[9 + i] = (v >> (9 + i * 2)) & 3u;
            } else if (reg == 0x60) {
                for (u32 i = 0; i < 8; ++i)
                    desc[13 + i] = (v >> (i * 2)) & 3u;
            }
            p += 6;
        } else if (cmd == 0x10) {
            p += 5 + 4 * ((r.r32(p + 1) >> 16) + 1);
        } else if (cmd == 0x61) {
            p += 5;
        } else {
            st.bad |= 1u; // unexpected command
            break;
        }
    }
    u32 vtxSize = 0;
    for (u32 a = 0; a < kAttrs; ++a) {
        if (desc[a] == 1 && a >= 9)
            st.bad |= 2u; // a direct attribute needs the VAT: not seen in models
        vtxSize += desc[a] == 3 ? 2u : desc[a] != 0 ? 1u : 0u;
    }
    if (desc[9] == 0)
        st.bad |= 4u; // no positions
    std::unordered_set<std::string> unique;
    const TagDL prim = tagDL(r, shp + kShpPrimDL);
    for (u32 p = prim.addr, end = prim.addr + prim.size; p < end;) {
        const u8 cmd = r.r8(p);
        if (cmd == 0x00) {
            ++p;
        } else if (cmd >= 0x20 && cmd <= 0x38 && (cmd & 7) == 0) {
            ++st.mtxLoads; // indexed XF load: a matrix of the palette
            p += 5;
        } else if (cmd >= 0x80 && cmd < 0xC0) {
            const u8 type = cmd & 0xF8;
            const u32 n = r.r16(p + 1);
            p += 3;
            std::string key(vtxSize, '\0');
            for (u32 v = 0; v < n; ++v, p += vtxSize) {
                for (u32 b = 0; b < vtxSize; ++b)
                    key[b] = char(r.r8(p + b));
                unique.insert(key);
            }
            st.vertices += n;
            ++st.draws;
            if (type == 0x80)
                st.triangles += n / 4 * 2;
            else if (type == 0x90)
                st.triangles += n / 3;
            else if ((type == 0x98 || type == 0xA0) && n >= 3)
                st.triangles += n - 2;
            else
                st.bad |= 8u; // lines or points
        } else {
            st.bad |= 16u;
            break;
        }
    }
    st.unique = u32(unique.size());
    if (st.vertices != r.r32(shp + kShpNumVtx))
        st.bad |= 32u;
    if (st.triangles != r.r32(shp + kShpNumPolygon))
        st.bad |= 64u;
    return st;
}

u32 bytecodeDraws(const Reader& r, u32 code, u32 numMat, u32 numShp, bool& ok) {
    u32 draws = 0;
    for (u32 i = 0; code != 0 && i < 4096; ++i, code += 8) {
        const u8 op = r.r8(code);
        if (op == 1)
            return draws;
        if (op != 4) {
            ok = false;
            return draws;
        }
        const u32 mat = (u32(r.r8(code + 1)) << 8) | r.r8(code + 2);
        const u32 shp = (u32(r.r8(code + 3)) << 8) | r.r8(code + 4);
        if (mat >= numMat || shp >= numShp)
            ok = false;
        ++draws;
    }
    return draws;
}

void scan(const Reader& r, u32 mdl, u32 opa, u32 xlu) {
    u32 bad = (r.r32(mdl) != kMdl0 ? 1u : 0u) | (r.r32(mdl + 8) != kRevision ? 2u : 0u);
    bool ok = true;
    const u32 nodes = r.dicCount(r.ofs(mdl, kDicNode));
    const u32 mats = r.dicCount(r.ofs(mdl, kDicMat));
    const u32 shpDic = r.ofs(mdl, kDicShp);
    const u32 shps = r.dicCount(shpDic);
    u32 vtx = 0, polys = 0;
    for (u32 i = 0; i < shps; ++i) {
        const u32 shp = r.dicData(shpDic, i);
        if (r.ofs(shp, 4) != mdl) // toResMdlData points back at the model
            bad |= 4u;
        vtx += r.r32(shp + kShpNumVtx);
        polys += r.r32(shp + kShpNumPolygon);
        const ShapeStats st = decodeShape(r, shp);
        std::fprintf(stderr, "[g3d]   shp %08X %s %X vtx %u/%u tris %u/%u unique %u draws %u mtx %u\n", shp,
                     st.bad ? "BAD" : "ok", st.bad, st.vertices, r.r32(shp + kShpNumVtx), st.triangles,
                     r.r32(shp + kShpNumPolygon), st.unique, st.draws, st.mtxLoads);
        if (st.bad)
            bad |= 32u;
    }
    const u32 posDic = r.ofs(mdl, kDicPos);
    for (u32 i = 0; i < r.dicCount(posDic); ++i)
        if (r.ofs(r.dicData(posDic, i), 4) != mdl)
            bad |= 8u;
    const u32 drawOpa = bytecodeDraws(r, opa, mats, shps, ok);
    const u32 drawXlu = bytecodeDraws(r, xlu, mats, shps, ok);
    if (!ok)
        bad |= 16u;
    ok = bad == 0;
    std::fprintf(stderr,
                 "[g3d] mdl %08X %s %X rev %u nodes %u mats %u shps %u pos %u nrm %u clr %u tex %u vtx %u polys %u "
                 "opa %u xlu %u\n",
                 mdl, ok ? "ok" : "BAD", bad, r.r32(mdl + 8), nodes, mats, shps, posDic ? r.dicCount(posDic) : 0,
                 r.dicCount(r.ofs(mdl, kDicNrm)), r.dicCount(r.ofs(mdl, kDicClr)),
                 r.dicCount(r.ofs(mdl, kDicTex)), vtx, polys, drawOpa, drawXlu);
}

} // namespace

// DrawResMdlDirectly: r3 = &ResMdl, r7/r8 = the opaque/translucent draw
// bytecode. Declines always: the game draws the model.
extern "C" bool g3d_DrawResMdlDirectly(CPUState* ctx) {
    static const bool enabled = std::getenv("DUALIZE_G3D_SCAN") != nullptr;
    if (!enabled)
        return false;
    static std::unordered_set<u32> seen;
    const Reader r{ctx};
    const u32 mdl = r.r32(ctx->gpr[3]);
    if (seen.insert(mdl).second)
        scan(r, mdl, ctx->gpr[7], ctx->gpr[8]);
    return false;
}

// ResShp::CallPrimitiveDisplayList(sync): r3 = &ResShp. The unsynced call
// writes GX_CMD_CALL_DL for the shape's primitive list straight into the FIFO;
// this writes the host shape packet in its place (aurora gx/recomp.hpp
// HostShapeOpcode), which draws the list from a mesh built once. The synced
// call flushes GX state first: the game's code runs it.
extern "C" bool g3d_CallPrimitiveDisplayList(CPUState* ctx) {
    if (ctx->gpr[4] != 0)
        return false;
    const Reader r{ctx};
    const u32 tag = r.r32(ctx->gpr[3]) + kShpPrimDL;
    const u32 ofs = r.r32(tag + 8);
    mem_write8_at(ctx, 0, 0xCC008000u, 0x50u);
    mem_write32_at(ctx, 0, 0xCC008000u, ofs != 0 ? tag + ofs : 0u);
    mem_write32_at(ctx, 0, 0xCC008000u, r.r32(tag + 4));
    return true;
}
