// Host implementations of Revolution SDK OS functions, compiled into a game's
// recompiled module and wired up by its natives list (src/app/natives.h).
//
// Each one either performs the whole call and returns true, or returns false
// before touching guest state so the recompiled SDK code runs instead. The
// structure offsets below belong to the Revolution SDK; where the game's
// linker put things comes from the natives list's defines (ppc_natives.h).
// The list pins every entry to the exact guest bytes it replaces, so a
// different build is refused at recompile time instead of corrupting memory
// here.
//
// Interrupt masking is elided: the module delivers interrupts only between
// dispatches, never inside a call, so the guest's disable/restore pair around
// these bodies has no observable effect. Volatile registers other than the
// return value are left as they are; the SDK is compiled to the PPC EABI and
// no caller reads them after a call.

#include "core/cpu.h"

#include "ppc_natives.h"

// Lomem
#define OS_CURRENT_THREAD 0x800000E4u

// OSThread
#define THREAD_PRIORITY 720u
#define THREAD_BASE 724u
#define THREAD_MUTEX 752u
#define THREAD_MUTEX_QUEUE_HEAD 756u
#define THREAD_MUTEX_QUEUE_TAIL 760u
#define THREAD_SIZE 764u

// OSMutex
#define MUTEX_QUEUE_HEAD 0u
#define MUTEX_THREAD 8u
#define MUTEX_COUNT 12u
#define MUTEX_LINK_NEXT 16u
#define MUTEX_LINK_PREV 20u
#define MUTEX_SIZE 24u

// From the natives list: __OSRunQueueHint's small-data offset
// (RUN_QUEUE_HINT_SDA_OFFSET), and OSDisableInterrupts and the end of its
// mtmsr window (OS_DISABLE_INTERRUPTS, OS_DISABLE_INTERRUPTS_END).

static inline u32 load(const u8* base, u32 offset) {
    return read_be32(base + offset);
}

static inline void store(u8* base, u32 offset, u32 value) {
    write_be32(base + offset, value);
}

// Host view of a guest object that can be read and written in place, or NULL
// when any byte of it needs the out-of-line memory path.
static inline u8* object(CPUState* ctx, u32 address, u32 size) {
    return address ? ppc_fast_write_ptr(ctx, address, size) : NULL;
}

static inline u8* current_thread(CPUState* ctx, u32* address) {
    const u8* slot = ppc_fast_ptr(ctx->fast_read, OS_CURRENT_THREAD, 4);
    if (!slot)
        return NULL;
    *address = read_be32(slot);
    return object(ctx, *address, THREAD_SIZE);
}

static inline u32 msr_ee(u32 msr) {
    return (msr >> 15) & 1u;
}

static inline void set_cr0_compare_zero(CPUState* ctx, s32 value) {
    u32 bits = value < 0 ? 0x8u : value > 0 ? 0x4u : 0x2u;
    bits |= (ctx->xer >> 31) & 1u;
    ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (bits << 28);
}

// BOOL OSDisableInterrupts(void)
bool rvl_OSDisableInterrupts(CPUState* ctx) {
    const u32 msr = ctx->msr;
    ctx->gpr[4] = msr & ~PPC_MSR_EE;
    ctx->msr = ctx->gpr[4];
    ctx->gpr[3] = msr_ee(msr);
    ctx->downcount -= 5;
    return true;
}

// BOOL OSEnableInterrupts(void)
bool rvl_OSEnableInterrupts(CPUState* ctx) {
    const u32 msr = ctx->msr;
    ctx->gpr[4] = msr | PPC_MSR_EE;
    ctx->msr = ctx->gpr[4];
    ctx->gpr[3] = msr_ee(msr);
    if (ctx->msr_written)
        ctx->msr_written(ctx);
    ctx->downcount -= 5;
    return true;
}

// BOOL OSRestoreInterrupts(BOOL level)
bool rvl_OSRestoreInterrupts(CPUState* ctx) {
    const u32 msr = ctx->msr;
    set_cr0_compare_zero(ctx, (s32)ctx->gpr[3]);
    ctx->gpr[4] = msr;
    ctx->gpr[5] = ctx->gpr[3] ? msr | PPC_MSR_EE : msr & ~PPC_MSR_EE;
    ctx->msr = ctx->gpr[5];
    ctx->gpr[3] = msr_ee(msr);
    if (ctx->msr_written)
        ctx->msr_written(ctx);
    ctx->downcount -= 7;
    return true;
}

// OSThread* OSGetCurrentThread(void)
bool rvl_OSGetCurrentThread(CPUState* ctx) {
    const u8* slot = ppc_fast_ptr(ctx->fast_read, OS_CURRENT_THREAD, 4);
    if (!slot)
        return false;
    ctx->gpr[3] = read_be32(slot);
    ctx->downcount -= 3;
    return true;
}

// Takes `mutex` for the current thread: a free one is queued on the thread,
// an owned one recurses. Returns 1 when it was taken, 0 when another thread
// holds it, and -1 when guest memory needs the slow path (nothing written).
static int try_lock_mutex(CPUState* ctx, u32 mutex_address) {
    u32 thread_address;
    u8* thread = current_thread(ctx, &thread_address);
    u8* mutex = object(ctx, mutex_address, MUTEX_SIZE);
    if (!thread || !mutex)
        return -1;

    const u32 owner = load(mutex, MUTEX_THREAD);
    if (owner == thread_address) {
        store(mutex, MUTEX_COUNT, load(mutex, MUTEX_COUNT) + 1u);
        ctx->downcount -= 30;
        return 1;
    }
    if (owner != 0)
        return 0;

    // EnqueueTail(&thread->queueMutex, mutex, link)
    const u32 tail_address = load(thread, THREAD_MUTEX_QUEUE_TAIL);
    u8* tail = NULL;
    if (tail_address && !(tail = object(ctx, tail_address, MUTEX_SIZE)))
        return -1;
    store(mutex, MUTEX_THREAD, thread_address);
    store(mutex, MUTEX_COUNT, load(mutex, MUTEX_COUNT) + 1u);
    if (tail)
        store(tail, MUTEX_LINK_NEXT, mutex_address);
    else
        store(thread, THREAD_MUTEX_QUEUE_HEAD, mutex_address);
    store(mutex, MUTEX_LINK_PREV, tail_address);
    store(mutex, MUTEX_LINK_NEXT, 0);
    store(thread, THREAD_MUTEX_QUEUE_TAIL, mutex_address);
    ctx->downcount -= 40;
    return 1;
}

// void OSLockMutex(OSMutex* mutex)
//
// Contention sleeps the thread, which only the guest scheduler can do.
bool rvl_OSLockMutex(CPUState* ctx) {
    return try_lock_mutex(ctx, ctx->gpr[3]) > 0;
}

// BOOL OSTryLockMutex(OSMutex* mutex)
bool rvl_OSTryLockMutex(CPUState* ctx) {
    const int taken = try_lock_mutex(ctx, ctx->gpr[3]);
    if (taken < 0)
        return false;
    if (!taken)
        ctx->downcount -= 25;
    ctx->gpr[3] = (u32)taken;
    return true;
}

// void OSUnlockMutex(OSMutex* mutex)
//
// Handles unlocks that wake nobody: a recursive release, or a final release
// with no waiter, no inherited priority to drop and no reschedule pending.
bool rvl_OSUnlockMutex(CPUState* ctx) {
    const u32 mutex_address = ctx->gpr[3];
    u32 thread_address;
    u8* thread = current_thread(ctx, &thread_address);
    u8* mutex = object(ctx, mutex_address, MUTEX_SIZE);
    if (!thread || !mutex)
        return false;

    if (load(mutex, MUTEX_THREAD) != thread_address) {
        ctx->downcount -= 25;
        return true;
    }
    const u32 count = load(mutex, MUTEX_COUNT) - 1u;
    if (count != 0) {
        store(mutex, MUTEX_COUNT, count);
        ctx->downcount -= 30;
        return true;
    }

    const u8* hint = ppc_fast_ptr(ctx->fast_read,
                                  ctx->gpr[13] + (u32)RUN_QUEUE_HINT_SDA_OFFSET, 4);
    if (!hint || read_be32(hint) != 0 || load(mutex, MUTEX_QUEUE_HEAD) != 0 ||
        (s32)load(thread, THREAD_PRIORITY) < (s32)load(thread, THREAD_BASE))
        return false;

    // DequeueItem(&thread->queueMutex, mutex, link)
    const u32 next_address = load(mutex, MUTEX_LINK_NEXT);
    const u32 prev_address = load(mutex, MUTEX_LINK_PREV);
    u8* next = NULL;
    u8* prev = NULL;
    if ((next_address && !(next = object(ctx, next_address, MUTEX_SIZE))) ||
        (prev_address && !(prev = object(ctx, prev_address, MUTEX_SIZE))))
        return false;
    store(mutex, MUTEX_COUNT, 0);
    if (next)
        store(next, MUTEX_LINK_PREV, prev_address);
    else
        store(thread, THREAD_MUTEX_QUEUE_TAIL, prev_address);
    if (prev)
        store(prev, MUTEX_LINK_NEXT, next_address);
    else
        store(thread, THREAD_MUTEX_QUEUE_HEAD, next_address);
    store(mutex, MUTEX_THREAD, 0);
    ctx->downcount -= 60;
    return true;
}

// OSContext
#define CONTEXT_GPR 0u
#define CONTEXT_CR 128u
#define CONTEXT_LR 132u
#define CONTEXT_CTR 136u
#define CONTEXT_XER 140u
#define CONTEXT_SRR0 408u
#define CONTEXT_SRR1 412u
#define CONTEXT_STATE 418u  // u16
#define CONTEXT_GQR 420u
#define CONTEXT_SIZE 452u   // through gqr[7]; the FPU half is saved lazily

#define CONTEXT_STATE_EXC 0x2u  // saved by an exception: r5-r12 are live too

// OSDisableInterrupts' mtmsr window (OS_DISABLE_INTERRUPTS to _END): a
// context interrupted inside it resumes at the function start so the disable
// is redone.

// u32 OSSaveContext(OSContext* context)
//
// Returns 0 now; a later OSLoadContext of this context returns 1 from here.
bool rvl_OSSaveContext(CPUState* ctx) {
    u8* context = object(ctx, ctx->gpr[3], CONTEXT_SIZE);
    if (!context)
        return false;
    for (u32 i = 13; i < 32; ++i)
        store(context, CONTEXT_GPR + 4u * i, ctx->gpr[i]);
    for (u32 i = 1; i < 8; ++i)
        store(context, CONTEXT_GQR + 4u * i, ctx->gqr[i]);
    store(context, CONTEXT_CR, ctx->cr);
    store(context, CONTEXT_LR, ctx->lr);
    store(context, CONTEXT_SRR0, ctx->lr);
    store(context, CONTEXT_SRR1, ctx->msr);
    store(context, CONTEXT_CTR, ctx->ctr);
    store(context, CONTEXT_XER, ctx->xer);
    store(context, CONTEXT_GPR + 4u, ctx->gpr[1]);
    store(context, CONTEXT_GPR + 8u, ctx->gpr[2]);
    store(context, CONTEXT_GPR + 12u, 1);
    ctx->gpr[0] = 1;
    ctx->gpr[3] = 0;
    ctx->downcount -= 32;
    return true;
}

static inline u16 load16(const u8* base, u32 offset) {
    return (u16)(base[offset] << 8 | base[offset + 1]);
}

static inline void store16(u8* base, u32 offset, u16 value) {
    base[offset] = (u8)(value >> 8);
    base[offset + 1] = (u8)value;
}

// The runtime (cpu/cpu.cpp): a context was loaded, which switches host fibers
// when it is another thread's and returns once this one's is loaded again;
// and the rest of a guest call, run up to its return to `pc`.
void dualize_context_loaded(CPUState* ctx, u32 context);
void dualize_run_to(CPUState* ctx, u32 pc);

// Resumes `context` the way OSLoadContext's closing rfi does: at its SRR0
// with its SRR1, never at the caller.
static void resume_context(CPUState* ctx, u8* context, u32 address) {
    u32 srr0 = load(context, CONTEXT_SRR0);
    if (srr0 > OS_DISABLE_INTERRUPTS && srr0 < OS_DISABLE_INTERRUPTS_END) {
        srr0 = OS_DISABLE_INTERRUPTS;
        store(context, CONTEXT_SRR0, srr0);
    }
    const u32 state = load16(context, CONTEXT_STATE);
    u32 first = 13;
    if (state & CONTEXT_STATE_EXC) {
        store16(context, CONTEXT_STATE, (u16)(state & ~CONTEXT_STATE_EXC));
        first = 5;
    }
    ctx->gpr[0] = load(context, CONTEXT_GPR);
    ctx->gpr[1] = load(context, CONTEXT_GPR + 4u);
    ctx->gpr[2] = load(context, CONTEXT_GPR + 8u);
    for (u32 i = first; i < 32; ++i)
        ctx->gpr[i] = load(context, CONTEXT_GPR + 4u * i);
    for (u32 i = 1; i < 8; ++i)
        ctx->gqr[i] = load(context, CONTEXT_GQR + 4u * i);
    ctx->cr = load(context, CONTEXT_CR);
    ctx->lr = load(context, CONTEXT_LR);
    ctx->ctr = load(context, CONTEXT_CTR);
    ctx->xer = load(context, CONTEXT_XER);
    ctx->srr0 = srr0;
    ctx->srr1 = load(context, CONTEXT_SRR1);
    ctx->gpr[4] = load(context, CONTEXT_GPR + 16u);
    ctx->gpr[3] = load(context, CONTEXT_GPR + 12u);

    // mtmsr (EE and RI off), then rfi.
    const u32 msr = ctx->msr & ~(PPC_MSR_EE | PPC_MSR_RI);
    ctx->msr = ((msr & ~PPC_MSR_RFI_MASK) | (ctx->srr1 & PPC_MSR_RFI_MASK)) & ~PPC_MSR_POW;
    ctx->pc = srr0 & ~3u;
    dualize_context_loaded(ctx, address);
}

// void OSLoadContext(OSContext* context)
//
// A jump native: it ends where the loaded context resumes.
bool rvl_OSLoadContext(CPUState* ctx) {
    u8* context = object(ctx, ctx->gpr[3], CONTEXT_SIZE);
    if (!context || (ctx->msr & PPC_MSR_PR))
        return false;
    resume_context(ctx, context, ctx->gpr[3]);
    ctx->downcount -= 50;
    return true;
}

// Lomem, as offsets from OS_CURRENT_CONTEXT_PHYSICAL
#define OS_CURRENT_CONTEXT_PHYSICAL 0x800000C0u
#define OS_CURRENT_CONTEXT_OFFSET 0x14u
#define OS_FPU_CONTEXT_OFFSET 0x18u
#define OS_CURRENT_THREAD_OFFSET 0x24u
#define OS_LOMEM_SIZE 0x28u

// OSThread scheduling fields
#define THREAD_STATE 712u  // u16
#define THREAD_STATE_READY 1u
#define THREAD_STATE_RUNNING 2u
#define THREAD_QUEUE 732u
#define THREAD_LINK_NEXT 736u
#define THREAD_LINK_PREV 740u

// RunQueue[32], each an OSThreadQueue {head, tail}
#define RUN_QUEUE 0x8068F5B0u
#define RUN_QUEUE_SIZE 256u

// Small-data scheduler globals
#define RUN_QUEUE_BITS_SDA_OFFSET (-27528)
#define RESCHEDULE_SDA_OFFSET (-27536)
#define SWITCH_THREAD_CALLBACK_SDA_OFFSET (-29752)

#define DEFAULT_SWITCH_THREAD_CALLBACK 0x80455CDCu  // the SDK's empty one
#define SELECT_THREAD_SAVE_RETURN 0x804563E0u       // after its OSSaveContext call
#define SELECT_THREAD_FRAME 16u

#define SLEEP_THREAD_SELECT_RETURN 0x804570BCu      // after OSSleepThread's SelectThread call
#define SLEEP_THREAD_FRAME 16u
#define THREAD_STATE_WAITING 4u
#define THREAD_SUSPEND 716u
#define THREAD_QUEUE_WALK_MAX 64u  // bound on a thread queue walk

// Everything a switching SelectThread touches, checked up front so the
// switch itself cannot fail halfway.
typedef struct SelectPlan {
    u8* bits_slot;
    u8* hint_slot;
    u8* lomem;
    u8* queues;
    u8* current;
    u32 current_address;
    u32 bits;       // run-queue bits after any requeue
    u32 priority;   // the current thread's
    bool requeue;   // the current thread goes back on its run queue
    u8* old_tail;   // RunQueue[priority]'s tail before the requeue
    u32 index;      // the run queue the next thread comes from
    u8* next;
    u32 next_address;
    u8* after;      // the thread behind `next` in its queue
    u32 after_address;
} SelectPlan;

// Plans SelectThread(yield), for a current thread in `state` or, when that is
// negative, in the state it is in now; false when that call would not switch
// or needs the guest.
static bool plan_select(CPUState* ctx, SelectPlan* plan, u32 yield, s32 state) {
    if (ctx->msr & PPC_MSR_PR)
        return false;
    const u32 sda = ctx->gpr[13];
    plan->bits_slot = object(ctx, sda + (u32)RUN_QUEUE_BITS_SDA_OFFSET, 4);
    plan->hint_slot = object(ctx, sda + (u32)RUN_QUEUE_HINT_SDA_OFFSET, 4);
    const u8* reschedule_slot =
        ppc_fast_ptr(ctx->fast_read, sda + (u32)RESCHEDULE_SDA_OFFSET, 4);
    const u8* callback_slot =
        ppc_fast_ptr(ctx->fast_read, sda + (u32)SWITCH_THREAD_CALLBACK_SDA_OFFSET, 4);
    plan->lomem = object(ctx, OS_CURRENT_CONTEXT_PHYSICAL, OS_LOMEM_SIZE);
    plan->queues = object(ctx, RUN_QUEUE, RUN_QUEUE_SIZE);
    if (!plan->bits_slot || !plan->hint_slot || !reschedule_slot || !callback_slot ||
        !plan->lomem || !plan->queues || (s32)read_be32(reschedule_slot) > 0 ||
        read_be32(callback_slot) != DEFAULT_SWITCH_THREAD_CALLBACK)
        return false;

    plan->current_address = load(plan->lomem, OS_CURRENT_THREAD_OFFSET);
    plan->current = object(ctx, plan->current_address, THREAD_SIZE);
    if (!plan->current || load(plan->lomem, OS_CURRENT_CONTEXT_OFFSET) != plan->current_address)
        return false;

    // A running thread goes back on its run queue when yielding or outranked.
    u8* const current = plan->current;
    u32 bits = load(plan->bits_slot, 0);
    plan->priority = load(current, THREAD_PRIORITY);
    if (state < 0)
        state = load16(current, THREAD_STATE);
    plan->requeue = state == THREAD_STATE_RUNNING;
    plan->old_tail = NULL;
    if (plan->requeue) {
        const s32 highest = bits ? __builtin_clz(bits) : 32;
        if (plan->priority > 31u || (!yield && (s32)plan->priority <= highest))
            return false;  // nothing outranks it: no switch
        const u32 tail_address = load(plan->queues, 8u * plan->priority + 4u);
        if (tail_address && !(plan->old_tail = object(ctx, tail_address, THREAD_SIZE)))
            return false;
        bits |= 1u << (31u - plan->priority);
    }
    if (bits == 0)
        return false;  // the idle wait
    plan->bits = bits;

    // The thread that runs next. Its queue is empty now only when the requeue
    // is what fills it.
    plan->index = (u32)__builtin_clz(bits);
    plan->next_address = load(plan->queues, 8u * plan->index);
    if (!plan->next_address) {
        if (!plan->requeue || plan->index != plan->priority)
            return false;
        plan->next_address = plan->current_address;
    }
    plan->next = plan->next_address == plan->current_address
                     ? current
                     : object(ctx, plan->next_address, THREAD_SIZE);
    if (!plan->next)
        return false;
    plan->after_address = plan->next == current ? 0 : load(plan->next, THREAD_LINK_NEXT);
    if (plan->requeue && !plan->after_address && plan->next != current &&
        plan->index == plan->priority)
        plan->after_address = plan->current_address;
    plan->after = plan->after_address == plan->current_address ? current
                  : plan->after_address ? object(ctx, plan->after_address, THREAD_SIZE)
                                        : NULL;
    return !plan->after_address || plan->after;
}

// Pushes a guest frame of `size` bytes saving lr, r31 and r30 as the SDK's
// prologues do (stwu r1,-size(r1); stw r0,size+4(r1); stw r31,size-4(r1);
// stw r30,size-8(r1)).
static void push_frame(u8* frame, CPUState* ctx, u32 size) {
    store(frame, 0, ctx->gpr[1]);
    store(frame, size + 4u, ctx->lr);
    store(frame, size - 4u, ctx->gpr[31]);
    store(frame, size - 8u, ctx->gpr[30]);
    ctx->gpr[1] -= size;
}

// Runs a planned SelectThread from its entry: the current thread is saved to
// resume inside it, and the next one is loaded.
static void commit_select(CPUState* ctx, const SelectPlan* plan, u8* frame, u32 yield) {
    push_frame(frame, ctx, SELECT_THREAD_FRAME);
    ctx->gpr[30] = yield;
    u8* const current = plan->current;

    if (plan->requeue) {
        // EnqueueTail(&RunQueue[priority], current, link)
        u8* queue = plan->queues + 8u * plan->priority;
        store16(current, THREAD_STATE, THREAD_STATE_READY);
        store(current, THREAD_QUEUE, RUN_QUEUE + 8u * plan->priority);
        if (plan->old_tail)
            store(plan->old_tail, THREAD_LINK_NEXT, plan->current_address);
        else
            store(queue, 0, plan->current_address);
        store(current, THREAD_LINK_PREV, load(queue, 4));
        store(current, THREAD_LINK_NEXT, 0);
        store(queue, 4, plan->current_address);
        store(plan->bits_slot, 0, plan->bits);
        store(plan->hint_slot, 0, 1);
    }

    if (!(load16(current, CONTEXT_STATE) & CONTEXT_STATE_EXC)) {
        // OSSaveContext(current) from the call before SELECT_THREAD_SAVE_RETURN,
        // where cr0 holds the EQ of the rlwinm. that tested the state.
        ctx->cr = (ctx->cr & 0x0FFFFFFFu) | (0x2u | (ctx->xer >> 31)) << 28;
        ctx->lr = SELECT_THREAD_SAVE_RETURN;
        ctx->gpr[3] = plan->current_address;
        rvl_OSSaveContext(ctx);
    }

    // Dequeue the head of RunQueue[index].
    u8* queue = plan->queues + 8u * plan->index;
    store(plan->hint_slot, 0, 0);
    if (plan->after)
        store(plan->after, THREAD_LINK_PREV, 0);
    else
        store(queue, 4, 0);
    store(queue, 0, plan->after_address);
    if (!plan->after)
        store(plan->bits_slot, 0, plan->bits & ~(1u << (31u - plan->index)));
    store(plan->next, THREAD_QUEUE, 0);
    store16(plan->next, THREAD_STATE, THREAD_STATE_RUNNING);

    // OSSetCurrentContext(next): its SRR1 keeps FP only if it owns the FPU.
    store(plan->lomem, OS_CURRENT_THREAD_OFFSET, plan->next_address);
    store(plan->lomem, OS_CURRENT_CONTEXT_OFFSET, plan->next_address);
    store(plan->lomem, 0, plan->next_address & 0x3FFFFFFFu);
    const u32 srr1 = load(plan->next, CONTEXT_SRR1);
    store(plan->next, CONTEXT_SRR1,
          load(plan->lomem, OS_FPU_CONTEXT_OFFSET) == plan->next_address
              ? srr1 | PPC_MSR_FP
              : srr1 & ~PPC_MSR_FP);

    resume_context(ctx, plan->next, plan->next_address);
    ctx->downcount -= 150;
}

// After commit_select: back on this thread (no switch, or its host fiber
// resumed) at its saved SelectThread with the frame at `sp`, the guest call
// runs on to its return to `caller`, where the caller's host frame goes on.
static void finish_select(CPUState* ctx, u32 sp, u32 caller) {
    if (ctx->pc == SELECT_THREAD_SAVE_RETURN && ctx->gpr[1] == sp)
        dualize_run_to(ctx, caller);
}

// static OSThread* SelectThread(BOOL yield)
//
// A jump native for the switching path: the current thread gives way to the
// head of the highest-priority run queue, which resumes wherever it was saved.
// Calls that switch nothing, the idle wait on an empty run queue and a
// non-default switch callback stay with the guest.
bool rvl_SelectThread(CPUState* ctx) {
    SelectPlan plan;
    const u32 yield = ctx->gpr[3];
    if (!plan_select(ctx, &plan, yield, -1))
        return false;
    u8* frame = object(ctx, ctx->gpr[1] - SELECT_THREAD_FRAME, SELECT_THREAD_FRAME + 8u);
    if (!frame)
        return false;
    const u32 caller = ctx->lr;
    const u32 sp = ctx->gpr[1] - SELECT_THREAD_FRAME;
    commit_select(ctx, &plan, frame, yield);
    finish_select(ctx, sp, caller);
    return true;
}

// void OSSleepThread(OSThreadQueue* queue)
//
// A jump native: the current thread waits on `queue`, in priority order, and
// the trailing SelectThread switches away from it. When nothing else can run
// (the idle wait) or rescheduling is held off, the guest does it all.
bool rvl_OSSleepThread(CPUState* ctx) {
    const u32 queue_address = ctx->gpr[3];
    u8* wait_queue = object(ctx, queue_address, 8);
    SelectPlan plan;
    if (!wait_queue || !plan_select(ctx, &plan, 0, THREAD_STATE_WAITING))
        return false;
    u8* const current = plan.current;
    const u32 current_address = plan.current_address;
    const s32 priority = (s32)plan.priority;

    // The first waiter of lower priority, which the thread goes in front of.
    u32 before_address = load(wait_queue, 0);
    u8* before = NULL;
    for (u32 count = 0; before_address; ++count) {
        if (count == THREAD_QUEUE_WALK_MAX || before_address == current_address ||
            !(before = object(ctx, before_address, THREAD_SIZE)))
            return false;
        if ((s32)load(before, THREAD_PRIORITY) > priority)
            break;
        before_address = load(before, THREAD_LINK_NEXT);
        before = NULL;
    }
    const u32 prev_address = before ? load(before, THREAD_LINK_PREV) : load(wait_queue, 4);
    u8* prev = prev_address ? object(ctx, prev_address, THREAD_SIZE) : NULL;
    u8* frames = object(ctx, ctx->gpr[1] - SLEEP_THREAD_FRAME - SELECT_THREAD_FRAME,
                        SLEEP_THREAD_FRAME + SELECT_THREAD_FRAME + 8u);
    if ((prev_address && !prev) || !frames)
        return false;

    // Prologue, OSDisableInterrupts (r31 keeps the old EE), then the thread
    // waits: state, queue and its place in it.
    const u32 sp = ctx->gpr[1] - SLEEP_THREAD_FRAME - SELECT_THREAD_FRAME;
    push_frame(frames + SELECT_THREAD_FRAME, ctx, SLEEP_THREAD_FRAME);
    ctx->gpr[30] = queue_address;
    ctx->gpr[31] = msr_ee(ctx->msr);
    ctx->msr &= ~PPC_MSR_EE;
    store16(current, THREAD_STATE, THREAD_STATE_WAITING);
    store(current, THREAD_QUEUE, queue_address);
    store(current, THREAD_LINK_NEXT, before_address);
    store(current, THREAD_LINK_PREV, prev_address);
    if (before)
        store(before, THREAD_LINK_PREV, current_address);
    else
        store(wait_queue, 4, current_address);
    if (prev)
        store(prev, THREAD_LINK_NEXT, current_address);
    else
        store(wait_queue, 0, current_address);
    store(plan.hint_slot, 0, 1);

    // SelectThread(FALSE), returning into OSSleepThread's epilogue.
    const u32 caller = ctx->lr;
    ctx->lr = SLEEP_THREAD_SELECT_RETURN;
    commit_select(ctx, &plan, frames, 0);
    ctx->downcount -= 40;
    finish_select(ctx, sp, caller);
    return true;
}


// void OSWakeupThread(OSThreadQueue* queue)
//
// Readies every thread waiting on `queue`. When that leaves nothing that
// outranks the current thread, the trailing SelectThread switches nothing
// and the whole call is done here; otherwise the guest runs it.
bool rvl_OSWakeupThread(CPUState* ctx) {
    const u32 queue_address = ctx->gpr[3];
    const u32 sda = ctx->gpr[13];
    u8* wait_queue = object(ctx, queue_address, 8);
    u8* bits_slot = object(ctx, sda + (u32)RUN_QUEUE_BITS_SDA_OFFSET, 4);
    u8* hint_slot = object(ctx, sda + (u32)RUN_QUEUE_HINT_SDA_OFFSET, 4);
    const u8* reschedule_slot =
        ppc_fast_ptr(ctx->fast_read, sda + (u32)RESCHEDULE_SDA_OFFSET, 4);
    u8* queues = object(ctx, RUN_QUEUE, RUN_QUEUE_SIZE);
    u32 current_address;
    u8* current = current_thread(ctx, &current_address);
    if (!wait_queue || !bits_slot || !hint_slot || !reschedule_slot || !queues || !current)
        return false;

    // Check the waiters and what readying them does to the run queue.
    u8* waiters[THREAD_QUEUE_WALK_MAX];
    u32 count = 0;
    u32 bits = load(bits_slot, 0);
    bool readied = false;
    for (u32 address = load(wait_queue, 0); address; ++count) {
        u8* thread = count < THREAD_QUEUE_WALK_MAX ? object(ctx, address, THREAD_SIZE) : NULL;
        if (!thread)
            return false;
        if ((s32)load(thread, THREAD_SUSPEND) <= 0) {
            const u32 priority = load(thread, THREAD_PRIORITY);
            const u32 tail_address = load(queues, 8u * priority + 4u);
            if (priority > 31u || (tail_address && !object(ctx, tail_address, THREAD_SIZE)))
                return false;
            bits |= 1u << (31u - priority);
            readied = true;
        }
        waiters[count] = thread;
        address = load(thread, THREAD_LINK_NEXT);
    }

    // SelectThread(FALSE) switches nothing when rescheduling is held off or
    // the current thread still outranks every ready one.
    if ((readied || load(hint_slot, 0) != 0) && (s32)read_be32(reschedule_slot) <= 0) {
        const u8* lomem =
            ppc_fast_ptr(ctx->fast_read, OS_CURRENT_CONTEXT_PHYSICAL, OS_LOMEM_SIZE);
        if (!lomem || read_be32(lomem + OS_CURRENT_CONTEXT_OFFSET) == current_address) {
            const s32 highest = bits ? __builtin_clz(bits) : 32;
            if (!lomem || load16(current, THREAD_STATE) != THREAD_STATE_RUNNING ||
                (s32)load(current, THREAD_PRIORITY) > highest)
                return false;
        }
    }

    for (u32 i = 0; i < count; ++i) {
        u8* thread = waiters[i];
        const u32 thread_address = load(wait_queue, 0);
        // DequeueHead(queue, thread, link)
        const u32 next_address = load(thread, THREAD_LINK_NEXT);
        if (next_address)
            store(waiters[i + 1], THREAD_LINK_PREV, 0);
        else
            store(wait_queue, 4, 0);
        store(wait_queue, 0, next_address);
        store16(thread, THREAD_STATE, THREAD_STATE_READY);
        if ((s32)load(thread, THREAD_SUSPEND) > 0)
            continue;
        // EnqueueTail(&RunQueue[priority], thread, link)
        const u32 priority = load(thread, THREAD_PRIORITY);
        u8* queue = queues + 8u * priority;
        const u32 tail_address = load(queue, 4);
        store(thread, THREAD_QUEUE, RUN_QUEUE + 8u * priority);
        if (tail_address)
            store(object(ctx, tail_address, THREAD_SIZE), THREAD_LINK_NEXT, thread_address);
        else
            store(queue, 0, thread_address);
        store(thread, THREAD_LINK_PREV, tail_address);
        store(thread, THREAD_LINK_NEXT, 0);
        store(queue, 4, thread_address);
        store(bits_slot, 0, load(bits_slot, 0) | 1u << (31u - priority));
        store(hint_slot, 0, 1);
    }
    ctx->downcount -= 40 + 30 * (s32)count;
    return true;
}
