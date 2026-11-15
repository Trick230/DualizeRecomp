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

typedef void (*GuestFunction)(CPUState* ctx);
GuestFunction dolrecomp_lookup(CPUState* ctx, u32 target);
extern unsigned dolrecomp_call_depth;

// Runs the rest of a guest function from `pc` on (a native that did part of
// it), on this host stack: leaving to the dispatcher instead would bring its
// callers' returns back through it, whose safepoints the guest's direct
// returns don't have. A jump native returns after it; without the function
// (or past the direct-call depth) it leaves to the dispatcher at `pc`.
static void resume_guest(CPUState* ctx, u32 pc) {
    ctx->pc = pc;
    const GuestFunction function = dolrecomp_lookup(ctx, pc);
    if (!function || dolrecomp_call_depth >= 256u)
        return;
    ++dolrecomp_call_depth;
    function(ctx);
    --dolrecomp_call_depth;
}

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
#define SKIN_PROLOGUE_CYCLES (5 + 6 + 9 + 1 + 3 + 4)
#define SKIN_LOOP_CYCLES 3
#define SKIN_EPILOGUE_CYCLES (SKIN_LOOP_CYCLES + 2 + 9 + 5)
#define SKIN_COMMAND_CYCLES (SKIN_LOOP_CYCLES + 2)
#define SKIN_WEIGHT_CYCLES(n) (19 + 37 * (s64)(n) + 11)
#define SKIN_EVPMTX_CYCLES (10 + RES_NODE_CYCLES + 7 + 1)
#define PSMTX_CONCAT_CYCLES 51

#define SKIN_MAX_COMMANDS 4096u

// The cycles of the command at `code` with PSMTXConcat's, and in `own`
// without them (PSMTXConcat charges its own).
static s64 skin_command_cost(CPUState* ctx, u32 code, s64* own) {
    if (lbz(ctx, code) == 3) {
        const s64 cycles = SKIN_COMMAND_CYCLES + SKIN_WEIGHT_CYCLES(lbz(ctx, code + 3u));
        if (own)
            *own = cycles;
        return cycles;
    }
    if (own)
        *own = SKIN_COMMAND_CYCLES + SKIN_EVPMTX_CYCLES;
    return SKIN_COMMAND_CYCLES + SKIN_EVPMTX_CYCLES + PSMTX_CONCAT_CYCLES;
}

// The cycles after the command at `code` up to the loop's next safepoint, or
// to the end when it is the last.
static s64 skin_after_cycles(CPUState* ctx, u32 code) {
    const u32 next = code + (lbz(ctx, code) == 3 ? 4u + 6u * lbz(ctx, code + 3u) : 5u);
    return lbz(ctx, next) == 1 ? SKIN_EPILOGUE_CYCLES : SKIN_LOOP_CYCLES;
}

// Whether CalcSkinning can run its byte code natively.
static bool skinning_ok(CPUState* ctx, u32 model, u32 attrib, u32 mdl, u32 code) {
    const u32 skin = (u32)SKIN_TEMPORARY;
    for (u32 n = 0;; ++n) {
        if (n > SKIN_MAX_COMMANDS)
            return false;
        if (!plain(ctx, code, 1))
            return false;
        const u32 op = lbz(ctx, code);
        if (op == 1)
            break;
        if (op == 3) {
            if (!plain(ctx, code, 4))
                return false;
            const u32 target = (lbz(ctx, code + 1u) << 8) + lbz(ctx, code + 2u);
            const u32 count = lbz(ctx, code + 3u);
            if (!plain(ctx, attrib + target * 4u, 4) || !plain(ctx, model + target * MTX_SIZE, MTX_SIZE))
                return false;
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
            code += 5u;
        }
    }
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
    // The frame, GetResNode's below it and PSMTXConcat's below that.
    if (!code || !fpu_ready(ctx) || !plain(ctx, frame - 0x60u, 0x98u) ||
        !skinning_ok(ctx, model, attrib, mdl, code))
        return false;
    // The guest stops at a safepoint once the downcount runs out: run the
    // commands that fit whole, and leave the rest to it.
    s64 own = SKIN_PROLOGUE_CYCLES, left = ctx->downcount - SKIN_PROLOGUE_CYCLES;
    if (lbz(ctx, code) != 1 && left - skin_command_cost(ctx, code, NULL) - skin_after_cycles(ctx, code) <= 0)
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

    for (bool first = true;; first = false) {
        if (lbz(ctx, ctx->gpr[28]) != 1) {
            s64 cmd_own;
            const s64 cycles = skin_command_cost(ctx, ctx->gpr[28], &cmd_own);
            if (!first && left - cycles - skin_after_cycles(ctx, ctx->gpr[28]) <= 0) {
                // Where the guest's loop goes on, as from its safepoint.
                ctx->downcount -= own;
                resume_guest(ctx, base + 0x19Cu);
                return true;
            }
            left -= cycles;
            own += cmd_own;
        }
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
    ctx->downcount -= own + SKIN_EPILOGUE_CYCLES;
    ctx->pc = ctx->lr & ~3u;
    return true;
}

bool rvl_DCInvalidateRange(CPUState* ctx);
bool rvl_PSMTXConcatArray(CPUState* ctx);
bool rvl_PSMTXCopy(CPUState* ctx);
bool rvl_PSMTXInvXpose(CPUState* ctx);
bool nw4r_MTX34ToMTX33(CPUState* ctx);
bool nw4r_MTX34InvTranspose33(CPUState* ctx);

// From the natives list: CalcView_LC_DMA_ModelMtx's entry, its 0.0f and
// billboard table, the billboard work matrix, and the callees it runs inline
// (the locked cache's DMA queue and transfers, the node ID lookup, the buffer
// swaps) or through their natives.
_Static_assert(CV_SAVEGPR == CV_MWCC_SAVEGPR && CV_RESTGPR == CV_MWCC_RESTGPR,
               "CalcView saves r14-r31 through the runtime's helpers");
_Static_assert(CV_RES_NODE == SKIN_RES_NODE, "CalcView calls ResMdl::GetResNode");
_Static_assert(CV_DC_INVALIDATE == CV_RVL_DC_INVALIDATE && CV_CONCAT_ARRAY == CV_RVL_CONCAT_ARRAY &&
                   CV_CONCAT == SKIN_PSMTX_CONCAT && CV_COPY == CV_RVL_COPY &&
                   CV_INV_XPOSE == CV_RVL_INV_XPOSE && CV_TO_MTX33 == CV_NW4R_TO_MTX33 &&
                   CV_INV_TRANSPOSE33 == CV_NW4R_INV_TRANSPOSE33,
               "CalcView's callees with natives");
_Static_assert(CV_QUEUE_WAIT_2 == CV_QUEUE_WAIT && CV_LOAD_2 == CV_LOAD && CV_STORE_2 == CV_STORE &&
                   CV_SWAP_2 == CV_SWAP && CV_SWAP_B && CV_QUEUE_LENGTH && CV_NODE_ID,
               "CalcView's inline callees, each matched by its code");

#define SPR_DMAU 922u
#define SPR_DMAL 923u
#define LC_BASE 0xE0000000u
#define CV_BLOCK 40u  // matrices per locked cache buffer
#define MTX33_SIZE 36u

static inline void stfs(CPUState* ctx, u8 f, u32 address) {
    mem_write32_at(ctx, 0, address, convert_to_single(f64_bits(ctx->fpr[f])));
}

// LCQueueWait(r3) with the DMA queue empty (the host's transfers are done
// when queued): 5 cycles.
static void cv_queue_wait(CPUState* ctx, u32 ret) {
    ctx->lr = ret;
    ctx->gpr[4] = (ctx->hid2 >> 24) & 0xFu;
    cmpwi(ctx, ctx->gpr[4], (s32)ctx->gpr[3]);
}

// LCLoadBlocks(r3 = cache, r4 = memory, r5 = blocks) and LCStoreBlocks(r3 =
// memory, r4 = cache, r5 = blocks), each a b to the code that writes DMAU and
// DMAL: 12 cycles, as the two mtspr go to the instruction fallback, which
// charges the interpreter's cost on top of the block's.
static void cv_dma(CPUState* ctx, u32 ret, bool load) {
    ctx->lr = ret;
    const u32 code = load ? (u32)CV_LOAD_DMA : (u32)CV_STORE_DMA;
    const u8 memory = load ? 4 : 3, cache = load ? 3 : 4;
    ctx->gpr[6] = (ctx->gpr[5] >> 2) & 0x1Fu;
    ctx->gpr[memory] &= 0x1FFFFFFFu;
    ctx->gpr[6] |= ctx->gpr[memory];
    ctx->spr_write(ctx, SPR_DMAU, ctx->gpr[6], code + 0x0Cu);
    ctx->gpr[6] = ((ctx->gpr[5] << 2) & 0xCu) | ctx->gpr[cache] | (load ? 0x12u : 0x02u);
    ctx->spr_write(ctx, SPR_DMAL, ctx->gpr[6], code + 0x1Cu);
}
#define CV_QUEUE_WAIT_CYCLES 5
#define CV_DMA_CYCLES 12

// The node ID of matrix r4, from the matrix table at *(r3): 7 cycles.
static void cv_node_id(CPUState* ctx, u32 ret) {
    ctx->lr = ret;
    ctx->gpr[5] = lwz(ctx, ctx->gpr[3]);
    ctx->gpr[0] = ctx->gpr[4] << 2;
    ctx->gpr[3] = lwz(ctx, ctx->gpr[5] + 0x24u);
    ctx->gpr[3] = ctx->gpr[5] + ctx->gpr[3];
    ctx->gpr[3] += ctx->gpr[0];
    ctx->gpr[3] = lwz(ctx, ctx->gpr[3] + 4u);
}
#define CV_NODE_ID_CYCLES 7

// Swaps the words at a and b: 5 cycles.
static void cv_swap(CPUState* ctx, u32 ret, u32 a, u32 b) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->lr = ret;
    ctx->gpr[5] = lwz(ctx, a);
    ctx->gpr[0] = lwz(ctx, b);
    stw(ctx, a, ctx->gpr[0]);
    stw(ctx, b, ctx->gpr[5]);
}
#define CV_SWAP_CYCLES 5

#define MWCC_GPR14_CYCLES 19  // _savegpr_14, _restgpr_14

// DCInvalidateRange's cycles (rvl_cache.c).
static s64 dc_invalidate_cycles(u32 start, u32 bytes) {
    if (!bytes)
        return 2;
    return 2 + 6 + 7 * (s64)((bytes + (start & 31u) + 31u) >> 5) + 1;
}

// What CalcView costs: its own cycles (charged at the end) up to the first
// block and for each block, and the same with its natives' (an upper bound: a
// singular matrix's inverse takes fewer), to know how much of it runs before
// the guest would stop at a safepoint.
#define CV_MAX_BLOCKS 64u
#define CV_PROLOGUE_CYCLES (17 + MWCC_GPR14_CYCLES)
#define CV_EPILOGUE_CYCLES (4 + MWCC_GPR14_CYCLES + 5)
typedef struct {
    s64 setup_own, setup_total;
    s64 block_own[CV_MAX_BLOCKS], block_total[CV_MAX_BLOCKS];
} CvCost;

// Whether CalcView can run natively, and its costs. Billboards and nodes
// whose world matrix comes from their parent's are declined.
static bool calc_view_ok(CPUState* ctx, CvCost* cost) {
    const u32 sp = ctx->gpr[1], num = ctx->gpr[7], pos = ctx->gpr[3], nrm = ctx->gpr[4];
    const u32 attrib = ctx->gpr[6], view = ctx->gpr[8], mdl = ctx->gpr[9], tex = ctx->gpr[10];
    // Its frame and the callees' below it, and the caller's LR slot, which
    // GetResNode writes.
    if (!fpu_ready(ctx) || !ctx->cache_range || !ctx->spr_write || (ctx->msr & PPC_MSR_PR) ||
        ((ctx->hid2 >> 24) & 0xFu) != 0 || num > CV_MAX_BLOCKS * CV_BLOCK || !plain(ctx, sp - 0x140u, 0x148u))
        return false;
    s64 cycles = CV_PROLOGUE_CYCLES, natives = 0;
    u32 ids = 0;
    if (num) {
        if (!plain(ctx, view, MTX_SIZE) || !plain(ctx, (u32)CV_ZERO, 4) || !plain(ctx, mdl, 4) ||
            !plain(ctx, ctx->gpr[13] + (u32)MTX_ARRAY_UNIT_SDA, 8) || !plain(ctx, MTX_UNIT_PAIR, 8))
            return false;
        const u32 data = lwz(ctx, mdl), table = data + 0x48u;
        if (!plain(ctx, table + 0x24u, 4))
            return false;
        ids = table + lwz(ctx, table + 0x24u) + 4u;
        const u32 pos_bytes = (num * MTX_SIZE + 31u) & ~31u;
        cycles += 23 + 3 + 1 + 3 + 3 + CV_QUEUE_WAIT_CYCLES + 5 + (num <= CV_BLOCK) + 1 +
                  CV_DMA_CYCLES + 2 + (num > 20 ? 1 + 1 + 3 + 3 : 2 + CV_QUEUE_WAIT_CYCLES) + 11 + 2;
        natives += dc_invalidate_cycles(pos, pos_bytes);
        if (nrm) {
            cycles += 8 + 2;
            natives += dc_invalidate_cycles(nrm, (num * MTX33_SIZE + 31u) & ~31u);
            if (tex) {
                cycles += 4;
                natives += dc_invalidate_cycles(tex, pos_bytes);
            }
        }
    }
    cost->setup_own = cycles;
    cost->setup_total = cycles + natives;
    for (u32 base = 0, block = 0; base < num; base += CV_BLOCK, ++block) {
        const u32 remain = num - base, count = remain > CV_BLOCK ? CV_BLOCK : remain;
        cycles = 4 + (remain > CV_BLOCK ? 5 : 11) + 2 + CV_QUEUE_WAIT_CYCLES + 3;
        natives = 0;
        if (base + CV_BLOCK < num)
            cycles += 8 + (remain - CV_BLOCK > CV_BLOCK ? 2 + 1 : 6) + CV_DMA_CYCLES;
        cycles += 2;
        if (count > 1) {
            cycles += 5 + 1;
            natives += 67 + 36 * (s64)(count - 1);
        } else {
            cycles += 4;
            natives += PSMTX_CONCAT_CYCLES;
        }
        cycles += 8;
        for (u32 i = 0; i < count; ++i) {
            const u32 g = base + i;
            if (!plain(ctx, attrib + 4u * g, 4) || !plain(ctx, ids + 4u * g, 4))
                return false;
            const u32 flags = lwz(ctx, attrib + 4u * g);
            if (flags & 0xFFu)
                return false;
            cycles += 2 + 4 + 7 + CV_NODE_ID_CYCLES + 3 + 3;
            const u32 id = lwz(ctx, ids + 4u * g);
            if ((s32)id >= 0) {
                const u32 node = res_node(ctx, mdl, id);
                if (!node || !plain(ctx, node + 0x14u, 4) || (lwz(ctx, node + 0x14u) & 0x400u))
                    return false;
                cycles += 2 + RES_NODE_CYCLES + 2 + 3;
            }
            if (nrm) {
                cycles += 2 + 9 + 6;
                if (flags & 0x20000000u) {
                    cycles += 2 + (tex ? 4 + 9 : 0) + 3 + 1;
                    natives += (tex ? 13 : 0) + 13;
                } else {
                    cycles += 2 + (tex ? 4 + 4 + 1 : 3);
                    natives += (tex ? 13 : 0) + 27 + 21;
                }
            }
        }
        cycles += 2 + 5 + CV_DMA_CYCLES + 2 + (nrm ? 6 + 2 : 0) + 2;
        if (nrm)
            cycles += 5 + CV_DMA_CYCLES + 2 + (tex ? 4 + CV_DMA_CYCLES : 0);
        cycles += 4 * (3 + CV_SWAP_CYCLES) + 8 + 2;
        cost->block_own[block] = cycles;
        cost->block_total[block] = cycles + natives;
    }
    return true;
}

#define AT(offset) (base + (offset))
#define FRAME(offset) (sp + (offset))

// One block of up to 40 matrices: their view matrices, then the normal (and
// texture) matrices made from those, in locked cache buffers the DMA filled
// and empties.
static void calc_view_block(CPUState* ctx, u32 base, u32 sp) {
    u32* const r = ctx->gpr;
    r[14] = r[17] - r[26];
    cmplwi(ctx, r[14], CV_BLOCK);
    r[25] = r[14];
    if (r[14] > CV_BLOCK) {
        r[0] = 0x2D;
        r[25] = CV_BLOCK;
        stw(ctx, FRAME(0x70), r[0]);
        r[24] = 0x3C;
    } else {
        r[3] = r[14] * 0x24u;
        r[4] = r[14] * 0x30u;
        r[0] = r[3] + 0x1Fu;
        r[0] >>= 5;
        r[4] += 0x1Fu;
        stw(ctx, FRAME(0x70), r[0]);
        r[24] = r[4] >> 5;
    }
    r[3] = r[27];
    cv_queue_wait(ctx, AT(0x19C));
    r[0] = r[26] + CV_BLOCK;
    cmplwi(ctx, r[0], r[17]);
    if (r[0] < r[17]) {
        // The next block's model matrices
        r[5] = r[14] - CV_BLOCK;
        r[3] = lwz(ctx, FRAME(0x2C));
        r[0] *= 0x30u;
        cmplwi(ctx, r[5], CV_BLOCK);
        r[4] = r[16] + r[0];
        if (r[5] > CV_BLOCK) {
            r[5] = 0x3C;
        } else {
            r[5] *= 0x30u;
            r[0] = r[5] + 0x1Fu;
            r[5] = r[0] >> 5;
        }
        cv_dma(ctx, AT(0x1D8), true);
    }
    cmplwi(ctx, r[25], 1);
    r[4] = lwz(ctx, FRAME(0x3C));
    r[3] = r[18];
    r[5] = lwz(ctx, FRAME(0x38));
    // Each native call below is checked by calc_view_ok: it runs.
    if (r[25] > 1) {
        r[6] = r[25];
        ctx->lr = AT(0x1F4);
        rvl_PSMTXConcatArray(ctx);
    } else {
        ctx->lr = AT(0x208);
        rvl_PSMTXConcat(ctx);
    }
    r[3] = lwz(ctx, FRAME(0xC));
    r[23] = 0;
    r[0] = lwz(ctx, FRAME(0x7C));
    r[14] = 0;
    r[21] = 0;
    r[0] = r[3] + r[0];
    stw(ctx, FRAME(0x74), r[0]);
    for (;;) {
        cmplwi(ctx, r[23], r[25]);
        if (r[23] >= r[25])
            break;
        r[3] = lwz(ctx, FRAME(0x74));
        r[8] = lwz(ctx, r[3] + r[21]);
        r[0] = r[8] & 0xFFu;  // 0: no billboard
        cmpwi(ctx, r[0], 0);
        r[5] = lwz(ctx, r[19]);
        r[22] = r[26] + r[23];
        r[4] = r[22];
        r[3] = FRAME(0x10);
        r[0] = r[5] + 0x48u;
        stw(ctx, FRAME(0x10), r[0]);
        cv_node_id(ctx, AT(0x318));
        cmpwi(ctx, r[3], 0);
        r[4] = r[3];
        if ((s32)r[3] >= 0) {
            r[3] = r[19];
            run_res_node(ctx, AT(0x32C));
            cmpwi(ctx, r[3], 0);
            r[0] = lwz(ctx, r[3] + 0x14u) & 0x400u;  // 0: the node's own matrix
            cmpwi(ctx, r[0], 0);
        }
        r[23] += 1;
        r[14] += 0x30u;
        r[21] += 4u;
    }
    r[0] = lwz(ctx, FRAME(8));
    r[5] = r[24];
    r[4] = lwz(ctx, FRAME(0x38));
    r[3] = r[0] + r[31];
    cv_dma(ctx, AT(0x3A4), false);
    cmpwi(ctx, r[15], 0);
    if (r[15]) {
        r[0] = 0;
        r[23] = 0;
        stw(ctx, FRAME(0x78), r[0]);
        r[30] = 0;
        r[14] = 0;
        for (;;) {
            cmplwi(ctx, r[23], r[25]);
            if (r[23] >= r[25])
                break;
            r[3] = lwz(ctx, FRAME(0x74));
            r[4] = lwz(ctx, FRAME(0x38));
            r[0] = lwz(ctx, r[3] + r[14]);
            r[3] = lwz(ctx, FRAME(0x34));
            r[22] = r[4] + r[30];
            r[0] &= 0x20000000u;  // uniform scale
            cmpwi(ctx, r[0], 0);
            const bool uniform = r[0] != 0;
            r[0] = lwz(ctx, FRAME(0x78));
            r[21] = r[3] + r[0];
            cmpwi(ctx, r[20], 0);
            if (uniform) {
                if (r[20]) {
                    r[0] = lwz(ctx, FRAME(0x30));
                    r[3] = r[22];
                    r[4] = r[0] + r[30];
                    ctx->lr = AT(0x400);
                    rvl_PSMTXCopy(ctx);
                    // The texture matrix's translation: 0
                    for (u32 row = 3; row-- > 0;) {
                        r[0] = lwz(ctx, FRAME(0x30));
                        r[3] = r[0] + r[30];
                        stfs(ctx, 31, r[3] + 0x0Cu + 0x10u * row);
                    }
                }
                r[3] = r[21];
                r[4] = r[22];
                ctx->lr = AT(0x430);
                nw4r_MTX34ToMTX33(ctx);
            } else if (r[20]) {
                r[0] = lwz(ctx, FRAME(0x30));
                r[3] = r[22];
                r[4] = r[0] + r[30];
                ctx->lr = AT(0x44C);
                rvl_PSMTXInvXpose(ctx);
                r[0] = lwz(ctx, FRAME(0x30));
                r[3] = r[21];
                r[4] = r[0] + r[30];
                ctx->lr = AT(0x45C);
                nw4r_MTX34ToMTX33(ctx);
            } else {
                r[3] = r[21];
                r[4] = r[22];
                ctx->lr = AT(0x46C);
                nw4r_MTX34InvTranspose33(ctx);
            }
            r[3] = lwz(ctx, FRAME(0x78));
            r[23] += 1;
            r[30] += 0x30u;
            r[14] += 4u;
            r[3] += 0x24u;
            stw(ctx, FRAME(0x78), r[3]);
        }
    }
    cmpwi(ctx, r[15], 0);
    if (r[15]) {
        r[0] = lwz(ctx, FRAME(0x80));
        r[4] = lwz(ctx, FRAME(0x34));
        r[5] = lwz(ctx, FRAME(0x70));
        r[3] = r[15] + r[0];
        cv_dma(ctx, AT(0x4A8), false);
        cmpwi(ctx, r[20], 0);
        if (r[20]) {
            r[4] = lwz(ctx, FRAME(0x30));
            r[5] = r[24];
            r[3] = r[20] + r[31];
            cv_dma(ctx, AT(0x4C0), false);
        }
    }
    cv_swap(ctx, AT(0x4CC), FRAME(0x3C), FRAME(0x2C));
    cv_swap(ctx, AT(0x4D8), FRAME(0x38), FRAME(0x28));
    cv_swap(ctx, AT(0x4E4), FRAME(0x34), FRAME(0x24));
    cv_swap(ctx, AT(0x4F0), FRAME(0x30), FRAME(0x20));
    r[3] = lwz(ctx, FRAME(0x80));
    r[26] += CV_BLOCK;
    r[31] += 0x780u;
    r[3] += 0x5A0u;
    stw(ctx, FRAME(0x80), r[3]);
    r[3] = lwz(ctx, FRAME(0x7C));
    r[3] += 0xA0u;
    stw(ctx, FRAME(0x7C), r[3]);
}

// void CalcView_LC_DMA_ModelMtx(MTX34* viewPos, MTX33* viewNrm, const MTX34*
// model, const u32* attrib, u32 num, const MTX34* view, const ResMdl* mdl,
// MTX34* viewTex): the view matrices of a model's nodes (and their normal and
// texture matrices when asked for), 40 at a time through the locked cache.
bool g3d_CalcViewLC(CPUState* ctx) {
    CvCost cost;
    if (!calc_view_ok(ctx, &cost))
        return false;
    // The guest stops at a safepoint once the downcount runs out: run the
    // setup and the blocks that fit whole, and leave the rest to it.
    const u32 blocks = (ctx->gpr[7] + CV_BLOCK - 1u) / CV_BLOCK;
    s64 own = cost.setup_own;
    s64 left = ctx->downcount - cost.setup_total - (blocks ? cost.block_total[0] : 0) -
               (blocks <= 1 ? CV_EPILOGUE_CYCLES : 0);
    if (left <= 0)
        return false;
    u32* const r = ctx->gpr;
    const u32 base = (u32)CV_BASE, caller = r[1], sp = caller - 0xE0u;

    stw(ctx, sp, caller);
    r[1] = sp;
    r[0] = ctx->lr;
    stw(ctx, FRAME(0xE4), r[0]);
    mem_write64(ctx, FRAME(0xD0), f64_bits(ctx->fpr[31]));
    STORE(31, sp, 0xD8u);
    r[11] = FRAME(0xD0);
    ctx->lr = AT(0x1C);  // _savegpr_14
    for (u32 i = 0; i < 18; ++i)
        stw(ctx, r[11] - 0x48u + 4u * i, r[14 + i]);
    cmpwi(ctx, r[7], 0);
    stw(ctx, FRAME(8), r[3]);
    r[15] = r[4];
    r[16] = r[5];
    stw(ctx, FRAME(0xC), r[6]);
    r[17] = r[7];
    r[18] = r[8];
    r[19] = r[9];
    r[20] = r[10];

    if (r[17]) {
        r[4] = r[7] * 0x30u;
        r[5] = LC_BASE;
        stw(ctx, FRAME(0x3C), r[5]);
        r[11] = r[5] + 0x800u;
        r[10] = r[5] + 0x1000u;
        r[9] = r[5] + 0x1800u;
        r[8] = r[5] + 0x2000u;
        r[7] = r[5] + 0x2800u;
        r[6] = r[5] + 0x3000u;
        r[5] = r[5] + 0x3800u;
        r[0] = r[4] + 0x1Fu;
        stw(ctx, FRAME(0x38), r[11]);
        r[14] = r[0] & ~0x1Fu;
        stw(ctx, FRAME(0x34), r[10]);
        r[4] = r[14];
        stw(ctx, FRAME(0x30), r[9]);
        stw(ctx, FRAME(0x2C), r[8]);
        stw(ctx, FRAME(0x28), r[7]);
        stw(ctx, FRAME(0x24), r[6]);
        stw(ctx, FRAME(0x20), r[5]);
        ctx->lr = AT(0x98);
        rvl_DCInvalidateRange(ctx);
        cmpwi(ctx, r[15], 0);
        r[27] = 1;
        if (r[15]) {
            r[4] = r[17] * 0x24u;
            r[3] = r[15];
            r[27] = 2;
            r[0] = r[4] + 0x1Fu;
            r[4] = r[0] & ~0x1Fu;
            ctx->lr = AT(0xBC);
            rvl_DCInvalidateRange(ctx);
            cmpwi(ctx, r[20], 0);
            if (r[20]) {
                r[3] = r[20];
                r[4] = r[14];
                r[27] = 3;
                ctx->lr = AT(0xD4);
                rvl_DCInvalidateRange(ctx);
            }
        }
        ctx->lr = AT(0xD8);  // the billboard work matrix
        r[3] = (u32)CV_BB_TEMP;
        r[28] = r[3];
        r[3] = 0;
        cv_queue_wait(ctx, AT(0xE4));
        cmplwi(ctx, r[17], CV_BLOCK);
        r[3] = lwz(ctx, FRAME(0x3C));
        r[4] = r[16];
        r[5] = 0x3C;
        if (r[17] <= CV_BLOCK)
            r[5] = r[14] >> 5;
        cv_dma(ctx, AT(0x100), true);
        cmplwi(ctx, r[17], 20);
        if (r[17] > 20) {
            ctx->lr = AT(0x114);  // the queue's length
            r[4] = ctx->hid2;
            r[3] = (r[4] >> 24) & 0xFu;
            cmpwi(ctx, r[3], 0);
        } else {
            r[3] = 0;
            cv_queue_wait(ctx, AT(0x128));
        }
        r[0] = 0;
        r[3] = high_half((u32)CV_ZERO);
        stw(ctx, FRAME(0x80), r[0]);
        r[29] = high_half((u32)CV_BB_TABLE);
        lfs(ctx, 31, (u32)CV_ZERO);
        stw(ctx, FRAME(0x7C), r[0]);
        r[29] = (u32)CV_BB_TABLE;
        r[26] = 0;
        r[31] = 0;
        for (u32 block = 0;; ++block) {
            cmplwi(ctx, r[26], r[17]);
            if (r[26] >= r[17])
                break;
            if (block) {
                const s64 need = cost.block_total[block] + (block + 1u == blocks ? CV_EPILOGUE_CYCLES : 0);
                if (left - need <= 0) {
                    // Where the guest's loop goes on, as from its safepoint.
                    ctx->downcount -= own;
                    resume_guest(ctx, AT(0x154));
                    return true;
                }
                left -= need;
            }
            own += cost.block_own[block];
            calc_view_block(ctx, base, sp);
        }
    }

    LOAD(31, sp, 0xD8u);
    r[11] = FRAME(0xD0);
    ctx->fpr[31] = f64_value(mem_read64(ctx, FRAME(0xD0)));
    ctx->lr = AT(0x528);  // _restgpr_14
    for (u32 i = 0; i < 18; ++i)
        r[14 + i] = lwz(ctx, r[11] - 0x48u + 4u * i);
    r[0] = lwz(ctx, FRAME(0xE4));
    ctx->lr = r[0];
    r[1] = caller;
    ctx->downcount -= own + CV_EPILOGUE_CYCLES;
    ctx->pc = ctx->lr & ~3u;
    return true;
}

#undef AT
#undef FRAME

