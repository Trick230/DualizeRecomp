// Host implementations of nw4r g3d's matrix calculation (see rvl_os.c for the
// contract every native follows).
//
// Like rvl_mtx.c, each one replays the guest's instruction sequence, callees
// and stack traffic included, through the helpers the recompiled code uses, so
// memory, registers and FPSCR come out identical. Each checks everything it
// will touch before it touches anything, and declines unless the FPU is on
// with its exceptions masked, quantized loads and stores are enabled, every
// operand lies in plain guest memory and the call would not run out of cycles
// (the guest's returns and loops are safepoints).

#include "core/cpu.h"

#include "ppc_natives.h"
#include "sdk/guest_memory.h"

bool rvl_PSMTXConcat(CPUState* ctx);

// From the natives list: CalcSkinning's entry, its 0.0f and skinning work
// matrices, and its callees, ResMdl::GetResNode(u32) (with ResMdl's dictionary
// and ResDic's entry lookups) and PSMTXConcat.
_Static_assert(SKIN_CONCAT == SKIN_PSMTX_CONCAT, "CalcSkinning calls PSMTXConcat");
_Static_assert(SKIN_SAVEGPR == SKIN_MWCC_SAVEGPR && SKIN_RESTGPR == SKIN_MWCC_RESTGPR,
               "CalcSkinning saves r24-r31 through the runtime's helpers");

#define MTX_SIZE 48u

static inline bool fpu_ready(CPUState* ctx) {
    return (ctx->msr & PPC_MSR_FP) && !(ctx->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)) &&
           (ctx->hid2 & PPC_HID2_LSQE) && (ctx->gqr[0] & 0x00070007u) == 0;
}

static inline bool plain(CPUState* ctx, u32 address, u32 size) {
    return guest_span(ctx, address, size) != NULL;
}

#define LOAD(f, base, offset) ppc_psq_load_inline(ctx, f, (base) + (offset), false, 0, false, 0)
#define STORE(f, base, offset) ppc_psq_store_inline(ctx, f, (base) + (offset), false, 0, false, 0)

static inline u32 lbz(CPUState* ctx, u32 address) {
    return mem_read8_at(ctx, 0, address);
}

static inline u32 lwz(CPUState* ctx, u32 address) {
    return mem_read32_at(ctx, 0, address);
}

static inline void stw(CPUState* ctx, u32 address, u32 value) {
    mem_write32_at(ctx, 0, address, value);
}

static inline void lfs(CPUState* ctx, u8 f, u32 address) {
    const f64 value = f64_value(convert_to_double(lwz(ctx, address)));
    ctx->fpr[f] = value;
    ctx->ps1[f] = value;
}

// What lis loads for an address whose low half goes with a signed offset.
static inline u32 high_half(u32 address) {
    return (address + 0x8000u) & 0xFFFF0000u;
}

static inline void cr0(CPUState* ctx, bool lt, bool gt) {
    const u32 bits = (lt ? 0x8u : gt ? 0x4u : 0x2u) | (ctx->xer >> 31);
    ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (bits << 28);
}

static inline void cmpwi(CPUState* ctx, u32 a, s32 b) {
    cr0(ctx, (s32)a < b, (s32)a > b);
}

static inline void cmplwi(CPUState* ctx, u32 a, u32 b) {
    cr0(ctx, a < b, a > b);
}

// ResMdl::GetResNode(u32 index) for the ResMdl at `mdl` (a pointer to its
// data): the node, or 0 when the model has no node dictionary or no such
// node. Reads only.
static u32 res_node(CPUState* ctx, u32 mdl, u32 index) {
    if (!plain(ctx, mdl, 4))
        return 0;
    const u32 data = lwz(ctx, mdl);
    if (!plain(ctx, data + 0x14u, 4))
        return 0;
    const u32 offset = lwz(ctx, data + 0x14u);
    if (!offset)
        return 0;
    const u32 dic = data + offset, entry = dic + ((index + 1u) << 4);
    if (!plain(ctx, entry + 0x14u, 4))
        return 0;
    const u32 node = lwz(ctx, entry + 0x14u);
    return node ? dic + node : 0;
}

// GetResNode's cycles when it finds the node.
#define RES_NODE_CYCLES 34

// The guest's GetResNode(r3 = &mdl, r4 = index) with its callees, entered with
// lr = `ret`, for a node res_node found.
static void run_res_node(CPUState* ctx, u32 ret) {
    ctx->lr = ret;
    const u32 sp = ctx->gpr[1], frame = sp - 0x20u;
    stw(ctx, frame, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[0] = ctx->lr;
    ctx->gpr[5] = lwz(ctx, ctx->gpr[3]);
    stw(ctx, frame + 0x24u, ctx->gpr[0]);
    stw(ctx, frame + 0x1Cu, ctx->gpr[31]);
    ctx->gpr[31] = ctx->gpr[4];
    ctx->gpr[4] = lwz(ctx, ctx->gpr[5] + 0x14u);
    // ResMdl's node dictionary
    ctx->lr = (u32)SKIN_RES_NODE + 0x20u;
    cmpwi(ctx, ctx->gpr[4], 0);
    ctx->gpr[0] = lwz(ctx, ctx->gpr[3]);
    ctx->gpr[3] = ctx->gpr[0] + ctx->gpr[4];
    stw(ctx, frame + 8u, ctx->gpr[3]);
    ctx->gpr[4] = ctx->gpr[31];
    ctx->gpr[3] = frame + 8u;
    // ResDic's entry
    ctx->lr = (u32)SKIN_RES_NODE + 0x30u;
    ctx->gpr[5] = lwz(ctx, ctx->gpr[3]);
    cmpwi(ctx, ctx->gpr[5], 0);
    ctx->gpr[0] = (ctx->gpr[4] + 1u) << 4;
    ctx->gpr[3] = ctx->gpr[5] + ctx->gpr[0];
    ctx->gpr[0] = lwz(ctx, ctx->gpr[3] + 0x14u);
    cmpwi(ctx, ctx->gpr[0], 0);
    ctx->gpr[3] = ctx->gpr[5] + ctx->gpr[0];
    ctx->gpr[0] = lwz(ctx, frame + 0x24u);
    ctx->gpr[31] = lwz(ctx, frame + 0x1Cu);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] = sp;
}

// CalcSkinning's cycles outside its commands, and each command's (WEIGHT's
// with n matrices: 19 + 37n + 11; EVPMTX's PSMTXConcat charges its own).
#define SKIN_CYCLES (5 + 6 + 9 + 1 + 3 + 4 + 3 + 2 + 9 + 5)
#define SKIN_COMMAND_CYCLES (3 + 2)
#define SKIN_WEIGHT_CYCLES(n) (19 + 37 * (s64)(n) + 11)
#define SKIN_EVPMTX_CYCLES (10 + RES_NODE_CYCLES + 7 + 1)
#define PSMTX_CONCAT_CYCLES 51

// Whether CalcSkinning can run its byte code natively, and the cycles it takes
// (with PSMTXConcat's) and leaves to charge (without them).
static bool skinning_ok(CPUState* ctx, u32 model, u32 attrib, u32 mdl, u32 code,
                        s64* total, s64* own) {
    const u32 skin = (u32)SKIN_TEMPORARY;
    s64 cycles = SKIN_CYCLES, concats = 0;
    for (;;) {
        if (!plain(ctx, code, 1))
            return false;
        const u32 op = lbz(ctx, code);
        if (op == 1)
            break;
        cycles += SKIN_COMMAND_CYCLES;
        if (op == 3) {
            if (!plain(ctx, code, 4))
                return false;
            const u32 target = (lbz(ctx, code + 1u) << 8) + lbz(ctx, code + 2u);
            const u32 count = lbz(ctx, code + 3u);
            if (!plain(ctx, attrib + target * 4u, 4) || !plain(ctx, model + target * MTX_SIZE, MTX_SIZE))
                return false;
            cycles += SKIN_WEIGHT_CYCLES(count);
            code += 4u;
            for (u32 i = 0; i < count; ++i, code += 6u) {
                if (!plain(ctx, code, 6))
                    return false;
                const u32 id = (lbz(ctx, code) << 8) + lbz(ctx, code + 1u);
                if (!plain(ctx, attrib + id * 4u, 4) || !plain(ctx, skin + id * MTX_SIZE, MTX_SIZE))
                    return false;
            }
        } else {
            if (!plain(ctx, code, 5))
                return false;
            const u32 id = (lbz(ctx, code + 1u) << 8) + lbz(ctx, code + 2u);
            const u32 node = res_node(ctx, mdl, (lbz(ctx, code + 3u) << 8) + lbz(ctx, code + 4u));
            if (!node || !plain(ctx, model + id * MTX_SIZE, MTX_SIZE) ||
                !plain(ctx, node + 0xA0u, MTX_SIZE) || !plain(ctx, skin + id * MTX_SIZE, MTX_SIZE))
                return false;
            cycles += SKIN_EVPMTX_CYCLES + PSMTX_CONCAT_CYCLES;
            ++concats;
            code += 5u;
        }
        if (cycles >= ctx->downcount)
            return false;
    }
    *total = cycles;
    *own = cycles - PSMTX_CONCAT_CYCLES * concats;
    return true;
}

// void CalcSkinning(MTX34* model, u32* attrib, const ResMdl* mdl, const u8* code)
// Runs the model's NodeMix byte code: WEIGHT blends skinning matrices into a
// model matrix (and ANDs their attributes into its own), EVPMTX makes a
// skinning matrix from a model matrix and its node's inverse bind matrix. The
// code comes from the caller (the lookup by name when it is null is declined).
bool g3d_CalcSkinning(CPUState* ctx) {
    const u32 sp = ctx->gpr[1], frame = sp - 0x30u;
    const u32 model = ctx->gpr[3], attrib = ctx->gpr[4], mdl = ctx->gpr[5], code = ctx->gpr[6];
    s64 total, own;
    // The frame, GetResNode's below it and PSMTXConcat's below that.
    if (!code || !fpu_ready(ctx) || !plain(ctx, frame - 0x60u, 0x98u) ||
        !skinning_ok(ctx, model, attrib, mdl, code, &total, &own) || ctx->downcount <= total)
        return false;
    const u32 base = (u32)SKIN_BASE;

    stw(ctx, frame, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[0] = ctx->lr;
    stw(ctx, frame + 0x34u, ctx->gpr[0]);
    ctx->gpr[11] = frame + 0x30u;
    ctx->lr = base + 0x14u;  // _savegpr_24
    for (u32 i = 0; i < 8; ++i)
        stw(ctx, ctx->gpr[11] - 32u + 4u * i, ctx->gpr[24 + i]);
    cmpwi(ctx, ctx->gpr[6], 0);
    ctx->gpr[25] = ctx->gpr[3];
    ctx->gpr[26] = ctx->gpr[4];
    ctx->gpr[27] = ctx->gpr[5];
    ctx->gpr[28] = ctx->gpr[6];
    ctx->lr = base + 0x4Cu;  // GetSkinningMtxTemporary
    ctx->gpr[3] = (u32)SKIN_TEMPORARY;
    ctx->gpr[29] = ctx->gpr[3];
    ctx->gpr[30] = 0xF0000000u;
    ctx->gpr[31] = high_half((u32)SKIN_ZERO);

    for (;;) {
        ctx->gpr[0] = lbz(ctx, ctx->gpr[28]);
        cmplwi(ctx, ctx->gpr[0], 1);
        if (ctx->gpr[0] == 1)
            break;
        cmplwi(ctx, ctx->gpr[0], 3);
        const u32 p = ctx->gpr[28];
        if (ctx->gpr[0] == 3) {
            // WEIGHT
            ctx->gpr[3] = lbz(ctx, p + 1u);
            lfs(ctx, 0, (u32)SKIN_ZERO);
            ctx->gpr[0] = lbz(ctx, p + 2u);
            ctx->gpr[3] = (ctx->gpr[3] << 8) & 0xFFFFFF00u;
            ctx->ps1[0] = ctx->fpr[0];
            ctx->gpr[4] = lbz(ctx, p + 3u);
            ctx->gpr[9] = ctx->gpr[3] + ctx->gpr[0];
            ctx->gpr[8] = ctx->gpr[9] << 2;
            for (u8 f = 1; f <= 5; ++f) {
                ctx->fpr[f] = ctx->fpr[0];
                ctx->ps1[f] = ctx->fpr[0];
            }
            stw(ctx, ctx->gpr[26] + ctx->gpr[8], ctx->gpr[30]);
            ctx->ctr = ctx->gpr[4];
            cmplwi(ctx, ctx->gpr[4], 0);
            ctx->gpr[28] = p + 4u;
            if (ctx->gpr[4] != 0) {
                do {
                    const u32 q = ctx->gpr[28];
                    ctx->gpr[3] = lbz(ctx, q);
                    ctx->gpr[0] = lbz(ctx, q + 1u);
                    ctx->gpr[4] = (ctx->gpr[3] << 8) & 0xFFFFFF00u;
                    ctx->gpr[3] = lbz(ctx, q + 3u);
                    ctx->gpr[10] = ctx->gpr[4] + ctx->gpr[0];
                    ctx->gpr[7] = lbz(ctx, q + 4u);
                    ctx->gpr[6] = lbz(ctx, q + 5u);
                    ctx->gpr[4] = (ctx->gpr[3] << 16) & 0xFFFF0000u;
                    ctx->gpr[5] = lbz(ctx, q + 2u);
                    ctx->gpr[0] = ctx->gpr[10] * 0x30u;
                    ctx->gpr[6] = (ctx->gpr[6] & ~0xFF00u) | ((ctx->gpr[7] << 8) & 0xFF00u);
                    ctx->gpr[3] = lwz(ctx, ctx->gpr[26] + ctx->gpr[8]);
                    ctx->gpr[4] = (ctx->gpr[4] & 0x00FFFFFFu) | (ctx->gpr[5] << 24);
                    ctx->gpr[7] = ctx->gpr[29] + ctx->gpr[0];
                    ctx->gpr[28] = q + 6u;
                    ctx->gpr[4] |= ctx->gpr[6];
                    ctx->gpr[0] = ctx->gpr[10] << 2;
                    stw(ctx, ctx->gpr[1] + 8u, ctx->gpr[4]);
                    ctx->gpr[0] = lwz(ctx, ctx->gpr[26] + ctx->gpr[0]);
                    const u32 t = ctx->gpr[7];
                    LOAD(7, t, 0x00);
                    lfs(ctx, 6, ctx->gpr[1] + 8u);
                    ctx->gpr[0] &= ctx->gpr[3];
                    LOAD(8, t, 0x08);
                    LOAD(9, t, 0x10);
                    ppc_ps_madds_inline(ctx, 0, 7, 6, 0, false);
                    LOAD(7, t, 0x18);
                    ppc_ps_madds_inline(ctx, 1, 8, 6, 1, false);
                    LOAD(8, t, 0x20);
                    ppc_ps_madds_inline(ctx, 2, 9, 6, 2, false);
                    LOAD(9, t, 0x28);
                    ppc_ps_madds_inline(ctx, 3, 7, 6, 3, false);
                    ppc_ps_madds_inline(ctx, 4, 8, 6, 4, false);
                    stw(ctx, ctx->gpr[26] + ctx->gpr[8], ctx->gpr[0]);
                    ppc_ps_madds_inline(ctx, 5, 9, 6, 5, false);
                } while (--ctx->ctr != 0);
            }
            ctx->gpr[0] = ctx->gpr[9] * 0x30u;
            ctx->gpr[3] = ctx->gpr[25] + ctx->gpr[0];
            for (u8 f = 0; f <= 5; ++f)
                STORE(f, ctx->gpr[3], 8u * f);
        } else {
            // EVPMTX
            ctx->gpr[0] = lbz(ctx, p + 1u);
            ctx->gpr[3] = ctx->gpr[27];
            ctx->gpr[4] = lbz(ctx, p + 3u);
            ctx->gpr[6] = (ctx->gpr[0] << 8) & 0xFFFFFF00u;
            ctx->gpr[5] = lbz(ctx, p + 2u);
            ctx->gpr[0] = lbz(ctx, p + 4u);
            ctx->gpr[4] = (ctx->gpr[4] << 8) & 0xFFFFFF00u;
            ctx->gpr[24] = ctx->gpr[6] + ctx->gpr[5];
            ctx->gpr[4] += ctx->gpr[0];
            run_res_node(ctx, base + 0x184u);
            ctx->gpr[0] = ctx->gpr[24] * 0x30u;
            ctx->gpr[4] = ctx->gpr[3] + 0xA0u;
            ctx->gpr[3] = ctx->gpr[25] + ctx->gpr[0];
            ctx->gpr[5] = ctx->gpr[29] + ctx->gpr[0];
            ctx->lr = base + 0x198u;
            rvl_PSMTXConcat(ctx);  // checked by skinning_ok: it runs
            ctx->gpr[28] = p + 5u;
        }
    }

    ctx->gpr[11] = ctx->gpr[1] + 0x30u;
    ctx->lr = base + 0x1B0u;  // _restgpr_24
    for (u32 i = 0; i < 8; ++i)
        ctx->gpr[24 + i] = lwz(ctx, ctx->gpr[11] - 32u + 4u * i);
    ctx->gpr[0] = lwz(ctx, ctx->gpr[1] + 0x34u);
    ctx->lr = ctx->gpr[0];
    ctx->gpr[1] = sp;
    ctx->downcount -= own;
    return true;
}
