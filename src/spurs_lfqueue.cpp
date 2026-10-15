/*
 * Soulcalibur II HD Online -- the SPURS lock-free queue calls the game makes from the PPU.
 *
 * The game creates its work queues with _cellSpursLFQueueInitialize and fills them with
 * _cellSpursLFQueuePushBody; the SPU tasks that consume them run the pop side themselves on the
 * same 128-byte queue line. The queue is an ANY2ANY CellSyncLFQueue (direction 3), the one kind
 * RPCS3 has no implementation for. Both sides speak a lock-free protocol on that line, so the PPU
 * side has to move exactly the same fields the SPU code expects. This file is that protocol, written
 * down from the behaviour of the library the real console links (reference: RPCS3's cellSync.cpp
 * for the other directions, which share the field names).
 *
 * Queue line, big-endian, 128-byte aligned (offsets in the Q_* constants):
 *
 *   +00 pop1:  h1 completed-pop pointer | h2 | h3 pop-waiter ring | h4 allocated-pop pointer
 *   +08 push1: h5 completed-push pointer | h6 completion bitmap | h7 push-waiter ring | h8 allocated-push pointer
 *   +10 entry size   +14 depth   +18 buffer (bit 0 set for ANY2ANY)   +20 bs[4]   +24 direction
 *   +28 v1   +2C init state (0 / 1 initialising / 2 ready)   +30 push2   +32 hs1[15] pop-waiter tokens
 *   +50 pop2   +52 hs2[15] push-waiter tokens   +70 eaSignal   +78 v2   +7C event queue id
 *
 * Pointers run over [0, 2 * depth) so that full and empty differ. A completion bitmap (h6) lets up to
 * 15 pushers finish out of order: bit 15 is the entry at h5, and the pointer only moves over a run of
 * completed entries. A waiter ring is a word of three 5-bit counters mod 30, A (tail: where the next
 * waiter registers), C (head of those not yet signalled) and B (head of those signalled but not yet
 * done); its 15 token slots are hs1 for consumers waiting for data and hs2 for producers waiting for
 * room. A token is (workload << 8 | task) of an SPURS task, or 0xFFFF for a PPU thread (event queue).
 *
 * Not implemented, because the game does not use it (TODO markers below): popping from the PPU,
 * the PPU2SPU / SPU2PPU / SPU2SPU push paths, and blocking on a full queue through an event queue
 * (the queue has no Lv2 event queue attached in this port, so such a push fails with STAT, as it does
 * on the console without an attached queue).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ppu_context.h"

extern "C" {
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern uint32_t g_ps3_sdk_version;
int  ppu_stdcx64(uint64_t ea, uint64_t expected, uint64_t val);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t task_id);
}

namespace {

/* ---- guest memory ----------------------------------------------------------------------------- */

inline uint8_t  rd8(uint32_t a)  { return vm_base[a]; }
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, vm_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t rd64(uint32_t a)
{
    return __builtin_bswap64(__atomic_load_n(reinterpret_cast<uint64_t*>(vm_base + a), __ATOMIC_RELAXED));
}
inline void wr8(uint32_t a, uint8_t v)   { vm_base[a] = v; }
inline void wr16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(vm_base + a, &v, 2); }
inline void wr32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(vm_base + a, &v, 4); }
inline void wr64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(vm_base + a, &v, 8); }

/* Load-reserved / store-conditional on a doubleword, through the runtime's reservation machinery so
 * that SPU atomics on the same line (GETLLAR / PUTLLC) see the store. */
inline uint64_t ll64(uint32_t ea)
{
    uint64_t v = rd64(ea);
    g_active_ctx->reserve_addr  = ea;
    g_active_ctx->reserve_value = v;
    return v;
}
inline bool sc64(uint32_t ea, uint64_t v)
{
    const bool ok = ppu_stdcx64(ea, g_active_ctx->reserve_value, v) != 0;
    g_active_ctx->reserve_addr = 0;
    return ok;
}
inline void fence() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* ---- layout and codes ------------------------------------------------------------------------- */

enum : uint32_t {
    Q_POP1 = 0x00, Q_POP_H3 = 0x04, Q_PUSH1 = 0x08, Q_SIZE = 0x10, Q_DEPTH = 0x14, Q_BUFFER = 0x18,
    Q_BS0 = 0x20, Q_BS1 = 0x21, Q_DIR = 0x24, Q_V1 = 0x28, Q_INIT = 0x2C, Q_PUSH2 = 0x30, Q_HS1 = 0x32,
    Q_POP2 = 0x50, Q_HS2 = 0x52, Q_EA_SIGNAL = 0x70, Q_V2 = 0x78, Q_EQ_ID = 0x7C,
    Q_PUSH_H7 = 0x0C,
};
constexpr uint32_t DIR_ANY2ANY = 3;

constexpr int32_t SYNC_ERROR_AGAIN = int32_t(0x80410101), SYNC_ERROR_INVAL = int32_t(0x80410102),
                  SYNC_ERROR_PERM = int32_t(0x80410109), SYNC_ERROR_STAT = int32_t(0x8041010F),
                  SYNC_ERROR_ALIGN = int32_t(0x80410110), SYNC_ERROR_NULL = int32_t(0x80410111);
constexpr int32_t SPURS_ERROR_INVAL = int32_t(0x80410902), SPURS_ERROR_SRCH = int32_t(0x80410905),
                  SPURS_ERROR_PERM = int32_t(0x80410909), SPURS_ERROR_STAT = int32_t(0x8041090F), SPURS_ERROR_ALIGN = int32_t(0x80410910),
                  SPURS_ERROR_NULL = int32_t(0x80410911), SPURS_ERROR_SIGNAL = int32_t(0x80410914);

constexpr int RING = 30;                  /* waiter ring counters run mod 30, over 15 token slots */
inline uint32_t ring_slot(uint32_t counter) { return counter > 14 ? counter - 15 : counter; }
inline uint32_t ring_diff(uint32_t from, uint32_t to) { return to >= from ? to - from : to + RING - from; }  /* to - from mod 30 */

/* Sync errors become Spurs errors by their low byte. */
inline int32_t to_spurs(int32_t rc) { return rc < 0 ? int32_t((uint32_t(rc) & 0xFF) | 0x80410900u) : rc; }

/* ---- delivering a signal ---------------------------------------------------------------------- */

using SignalFn = int32_t (*)(uint32_t ea_signal, uint32_t token);

/* Wake the task named by `token` = workload << 8 | task. `ea_signal` is what the queue was created
 * with: a SPURS instance with bit 0 set (the workload's taskset is recorded in the instance), or a
 * taskset address (bit 0 clear). */
int32_t send_signal(uint32_t ea_signal, uint32_t token)
{
    const uint32_t base = ea_signal & ~0xFu;
    const uint32_t task = token & 0xFF;
    uint32_t taskset = base;
    if ((ea_signal & 0xF) == 1) {
        const uint32_t wid = token >> 8;
        constexpr uint32_t WKL_ENABLED = 0xB0, WKL_INFO1 = 0xB00, WKL_INFO_SIZE = 0x20;
        if (wid >= 16 || !(rd32(base + WKL_ENABLED) & (0x80000000u >> wid))) return SPURS_ERROR_SRCH;
        taskset = uint32_t(rd64(base + WKL_INFO1 + wid * WKL_INFO_SIZE + 8));
        if (!taskset) return SPURS_ERROR_SRCH;
    }
    if (taskset & 0x7F) return SPURS_ERROR_ALIGN;
    if (task > 0x7F) return SPURS_ERROR_INVAL;
    spu_taskset_signal_task(taskset, task);
    return 0;
}

/* Deliver the tokens collected by a completion. 0xFFxx names a PPU thread blocked on the queue's event
 * queue; nothing blocks that way here, so only the error for an unattached queue is reproduced. */
int32_t send_tokens(uint32_t q, const uint32_t* tokens, unsigned n, SignalFn sig)
{
    for (unsigned i = 0; i < n; i++) {
        const uint32_t t = tokens[i];
        if ((t & 0xFF00) == 0xFF00) {
            const uint32_t port = (t & 0xFF) == 1 ? rd32(q + Q_V1) : (uint32_t(rd16(q + Q_PUSH2)) << 16 | rd16(q + Q_POP2));
            if (port == 0xFFFFFFFFu) return SYNC_ERROR_PERM;
            /* TODO: sys_event_port_send(port) for a queue with an attached event queue. */
            continue;
        }
        if (int32_t rc = sig(rd32(q + Q_EA_SIGNAL + 4), t)) return rc;
    }
    return 0;
}

/* ---- push: allocate an entry ------------------------------------------------------------------ */

/* Write 0xFFFF into one halfword of a waiter ring, as a compare-and-swap on the aligned word around it. */
void mark_waiter_slot(uint32_t ea)
{
    const uint32_t word = ea & ~3u;
    const uint32_t mask = (ea & 2) ? 0x0000FFFFu : 0xFFFF0000u;
    auto* p = reinterpret_cast<uint32_t*>(vm_base + word);
    uint32_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(p, &cur, __builtin_bswap32(__builtin_bswap32(cur) | mask), false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}

/* Reserve the next entry of an ANY2ANY queue for a push. On success *pointer is its position in
 * [0, 2 * depth). The first attempt of the console's loop: a producer that finds the queue full and
 * may block would register in the push-waiter ring and sleep on the event queue; without an attached
 * event queue that ends in STAT before anything is committed. */
int32_t get_push_pointer2(uint32_t q, int32_t* pointer, bool blocking, bool use_event_queue)
{
    const int32_t depth = int32_t(rd32(q + Q_DEPTH)), depth2 = 2 * depth;
    for (;;) {
        const uint64_t push1 = ll64(q + Q_PUSH1);
        const uint64_t pop1  = rd64(q + Q_POP1);

        const int32_t  next   = int16_t(push1);                     /* h8: next entry to allocate */
        const int32_t  done   = int32_t(push1 >> 48);               /* h5: entries up to here are complete */
        const int32_t  popped = int32_t(pop1 >> 48);                /* h1 */
        const uint32_t ring   = uint32_t(push1 >> 16) & 0xFFFF;     /* h7 */
        const uint32_t tail = ring & 31, head = (ring >> 10) & 31, signalled = (ring >> 5) & 31;

        const int32_t  entries   = next - popped + (next < popped ? depth2 : 0);
        const int32_t  in_flight = next - done + (next < done ? depth2 : 0);
        const uint32_t waiting   = ring_diff(head, tail);            /* registered, not yet signalled */
        const uint32_t woken     = ring_diff(signalled, head);       /* signalled, entry not yet taken */

        const bool full     = !(depth > entries + int32_t(woken));
        const bool too_many = in_flight + int32_t(woken) > 15;
        const bool cannot   = full || too_many || waiting != 0;      /* waiters go first */

        if (!cannot) {
            const int32_t advanced = next + 1 < depth2 ? next + 1 : 0;
            if (!sc64(q + Q_PUSH1, (push1 & ~uint64_t(0xFFFF)) | uint16_t(advanced))) continue;
            *pointer = next;
            return 0;
        }
        if (!use_event_queue) return SYNC_ERROR_AGAIN;
        const bool ring_full = waiting == 15 || woken == 15;
        if (ring_full || !blocking) {
            if (!sc64(q + Q_PUSH1, push1)) continue;                  /* the answer holds for a consistent snapshot */
            return SYNC_ERROR_AGAIN;
        }
        if (rd8(q + Q_BS1) == 0xFF) {
            /* No event queue attached. The console library has already marked the slot it would register
             * in (0xFFFF, a PPU thread) by then, and leaves the mark behind. */
            mark_waiter_slot(q + Q_HS2 + 2 * ring_slot(tail));
            if (ll64(q + Q_PUSH1) != push1) continue;
            fence();
            if (rd64(q + Q_POP1) != pop1) continue;
            return SYNC_ERROR_STAT;
        }
        /* TODO: register in the push-waiter ring (hs2) and sleep on the event queue. */
        return SYNC_ERROR_AGAIN;
    }
}

/* ---- push: publish an entry ------------------------------------------------------------------- */

/* The entry at `pointer` is written. Mark it complete, move the completed-push pointer over the run of
 * completed entries, and wake as many waiting consumers (and producers, once room is free) as that
 * allows, handing each its token. Waking a consumer rewrites the pop word as well, so that case runs
 * as two steps on two words (the `slow` path): pop word first, then push word, which must not have
 * changed in between. */
int32_t complete_push_pointer2(uint32_t q, int32_t pointer, SignalFn sig)
{
    const int32_t depth = int32_t(rd32(q + Q_DEPTH)), depth2 = 2 * depth;
    /* Tokens taken so far, kept across retries. A retry of the slow path takes the producers' tokens again (their ring
     * advance is in the push word that was not stored), so the count is capped at the array: a waiter left over stays
     * registered and is woken by a later completion. */
    constexpr unsigned kMaxTokens = 64;
    uint32_t tokens[kMaxTokens];
    unsigned ntok = 0;
    bool slow = false;

    for (;;) {
        uint64_t push1, pop1;
        if (slow) { pop1 = ll64(q + Q_POP1); fence(); push1 = rd64(q + Q_PUSH1); }
        else      { push1 = ll64(q + Q_PUSH1); fence(); pop1 = rd64(q + Q_POP1); }

        const int32_t  done      = int32_t(push1 >> 48);              /* h5 */
        const uint32_t bitmap    = uint32_t(push1 >> 32) & 0xFFFF;    /* h6 */
        uint32_t       pring     = uint32_t(push1 >> 16) & 0xFFFF;    /* h7: producers' ring */
        const int32_t  next      = int16_t(push1);                    /* h8 */
        const int32_t  popped    = int32_t(pop1 >> 48);               /* h1 */
        uint32_t       cring     = uint32_t(pop1 >> 16) & 0xFFFF;     /* h3: consumers' ring */
        const int32_t  pop_alloc = int16_t(pop1);                     /* h4 */

        const int32_t dist = pointer - done + (done > pointer ? depth2 : 0);

        const uint32_t c_tail = cring & 31, c_head = (cring >> 10) & 31, c_signalled = (cring >> 5) & 31;
        const int32_t  pops_in_flight = pop_alloc - popped + (popped > pop_alloc ? depth2 : 0);
        int32_t        c_budget = pops_in_flight + int32_t(ring_diff(c_signalled, c_head));

        const uint32_t p_tail = pring & 31, p_head = (pring >> 10) & 31, p_signalled = (pring >> 5) & 31;
        const int32_t  occupied = next - popped + (popped > next ? depth2 : 0);
        int32_t        p_filled = occupied + int32_t(ring_diff(p_signalled, p_head));

        /* Entries done = the run of 1 bits from the top of the bitmap once this entry is marked. */
        const uint32_t sh     = uint32_t(15 - dist);
        const uint32_t marked = bitmap | ((sh & 0x20) ? 0u : (1u << (sh & 31)));
        const uint32_t unmarked = ~marked & 0xFFFF;
        const uint32_t k = unmarked ? uint32_t(__builtin_clz(unmarked)) - 16 : 16;

        int32_t new_done = done + int32_t(k);
        if (new_done >= depth2) new_done -= depth2;

        int32_t c_wakeable = c_budget > 15 ? 0 : int32_t(ring_diff(c_head, c_tail));
        int32_t p_wakeable = depth > p_filled ? int32_t(ring_diff(p_head, p_tail)) : 0;

        unsigned round = 0;                                           /* tokens taken in this attempt */
        bool restart_slow = false;
        for (int32_t remaining = int32_t(k) - 1; remaining >= 0; remaining--) {
            if (ntok + round < kMaxTokens && !(int32_t(ntok) > remaining) && c_wakeable != 0 && c_budget <= 15) {
                const uint32_t head = (cring >> 10) & 31;
                const uint32_t token = rd16(q + Q_HS1 + 2 * ring_slot(head));
                tokens[ntok + round] = token == 0xFFFF ? 0xFF01u : token;
                if (!slow) { restart_slow = true; break; }            /* the pop word needs its own reservation */
                cring = (cring & ~0x7C00u) | ((head == RING - 1 ? 0 : head + 1) << 10);
                round++; c_budget++; c_wakeable--;
            }
            if (ntok + round < kMaxTokens && p_wakeable != 0 && p_filled < depth) {
                const uint32_t head = (pring >> 10) & 31;
                const uint32_t token = rd16(q + Q_HS2 + 2 * ring_slot(head));
                tokens[ntok + round] = token == 0xFFFF ? 0xFF02u : token;
                pring = (pring & ~0x7C00u) | ((head == RING - 1 ? 0 : head + 1) << 10);
                round++; p_filled++; p_wakeable--;
            }
        }
        if (restart_slow) { slow = true; continue; }

        uint64_t new_push1 = push1;
        new_push1 = (new_push1 & 0x0000FFFFFFFFFFFFull) | (uint64_t(uint32_t(new_done) & 0xFFFF) << 48);
        new_push1 = (new_push1 & ~(uint64_t(0xFFFF) << 32)) | (uint64_t((marked << k) & 0xFFFF) << 32);
        new_push1 = (new_push1 & ~(uint64_t(0xFFFF) << 16)) | (uint64_t(pring & 0xFFFF) << 16);
        const uint64_t new_pop1 = (pop1 & ~(uint64_t(0xFFFF) << 16)) | (uint64_t(cring & 0xFFFF) << 16);

        if (!sc64(slow ? q + Q_POP1 : q + Q_PUSH1, slow ? new_pop1 : new_push1)) continue;
        ntok += round;
        if (slow) {
            /* Second word: only if neither the push word nor the consumers' ring changed meanwhile. */
            bool again = false;
            for (;;) {
                if (ll64(q + Q_PUSH1) != push1) { again = true; break; }
                fence();
                if (rd16(q + Q_POP_H3) != (cring & 0xFFFF)) { again = true; break; }
                if (sc64(q + Q_PUSH1, new_push1)) break;
            }
            if (again) continue;                                      /* tokens so far stay counted */
        }
        return send_tokens(q, tokens, ntok, sig);
    }
}

/* ---- creating a queue ------------------------------------------------------------------------- */

/* Field initialisation of a fresh queue (the part below the validation). */
void init_fields(uint32_t q, uint32_t buffer, uint32_t size, uint32_t depth, uint32_t direction, uint32_t ea_signal)
{
    wr16(q + 0x00, 0); wr16(q + 0x02, 0); wr16(q + 0x06, 0);
    wr16(q + 0x08, 0); wr16(q + 0x0A, 0); wr16(q + 0x0E, 0);
    wr32(q + Q_SIZE, size);
    wr32(q + Q_DEPTH, depth);
    wr64(q + Q_BUFFER, buffer);
    wr32(q + Q_DIR, direction);
    wr16(q + Q_PUSH2, 0);
    for (uint32_t i = 0; i < 15; i++) wr16(q + Q_HS1 + 2 * i, 0);
    wr16(q + Q_POP2, 0);
    for (uint32_t i = 0; i < 15; i++) wr16(q + Q_HS2 + 2 * i, 0);
    wr64(q + Q_EA_SIGNAL, ea_signal);
    if (direction == DIR_ANY2ANY) {
        wr16(q + Q_POP_H3, 0);
        wr64(q + Q_BUFFER, rd64(q + Q_BUFFER) | 1);
        wr8(q + Q_BS0, 0xFF); wr8(q + Q_BS1, 0xFF);
        wr16(q + Q_PUSH_H7, 0);
        wr32(q + Q_V1, 0xFFFFFFFFu);
        wr16(q + Q_PUSH2, 0xFFFF); wr16(q + Q_POP2, 0xFFFF);
        wr32(q + Q_V2, 0); wr32(q + Q_EQ_ID, 0);
    } else {
        wr32(q + Q_V1, 0);
        wr64(q + Q_V2, 0);
        wr32(q + Q_BS0, 0xFFFFFFFFu);
    }
}

/* The SDK version the title was built with decides how strict the checks are. */
uint32_t sdk_version()
{
    return g_ps3_sdk_version == 0xFFFFFFFFu ? 0x493000u : g_ps3_sdk_version;
}

int32_t sync_queue_initialize(uint32_t q, uint32_t buffer, uint32_t size, uint32_t depth, uint32_t direction,
                              uint32_t ea_signal)
{
    if (!q) return SYNC_ERROR_NULL;
    if (size) {
        if (!buffer) return SYNC_ERROR_NULL;
        if (size > 0x4000 || (size & 0xF)) return SYNC_ERROR_INVAL;
    }
    if (!depth || (depth & 0xFFFF8000u) || direction > DIR_ANY2ANY) return SYNC_ERROR_INVAL;
    if ((q & 0x7F) || (buffer & 0xF)) return SYNC_ERROR_ALIGN;

    const bool strict = sdk_version() > 0x17FFFF;
    uint32_t old;
    for (;;) {                                                       /* claim the init word */
        const uint32_t state = rd32(q + Q_INIT);
        uint32_t want;
        if (state) {
            if (strict && state != 2) return SYNC_ERROR_STAT;        /* someone else is initialising */
            old = state; want = state;
        } else {
            if (strict)
                for (uint32_t off = 0; off < 128; off += 8)
                    if (rd64(q + off)) return SYNC_ERROR_STAT;       /* the line must be zeroed */
            old = 1; want = 1;
        }
        uint32_t expected = __builtin_bswap32(state), desired = __builtin_bswap32(want);
        if (__atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(vm_base + q + Q_INIT), &expected, desired, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            break;
    }
    fence();
    if (old == 2) {                                                  /* already created: the parameters must agree */
        if (rd32(q + Q_SIZE) != size || rd32(q + Q_DEPTH) != depth || rd64(q + Q_BUFFER) != buffer)
            return SYNC_ERROR_INVAL;
        if (strict && (rd64(q + Q_EA_SIGNAL) != ea_signal || rd32(q + Q_DIR) != direction)) return SYNC_ERROR_INVAL;
        return 0;
    }
    init_fields(q, buffer, size, depth, direction, ea_signal);
    fence();
    wr32(q + Q_INIT, 2);
    fence();
    return 0;
}

/* ---- the calls -------------------------------------------------------------------------------- */

/* s32 _cellSpursLFQueueInitialize(void* tasksetOrSpurs, CellSyncLFQueue* queue, const void* buffer,
 *                                 u32 size, u32 depth, u32 direction)
 * The first argument is a taskset, or a SPURS instance with bit 0 set. */
int32_t lfqueue_initialize(uint32_t ea_signal, uint32_t queue, uint32_t buffer, uint32_t size, uint32_t depth,
                           uint32_t direction)
{
    if ((ea_signal & 0xF) == 0 && rd32(ea_signal + 0x74) > 0x1F) return SPURS_ERROR_INVAL;
    return to_spurs(sync_queue_initialize(queue, buffer, size, depth, direction, ea_signal));
}

/* s32 _cellSpursLFQueuePushBody(CellSyncLFQueue* queue, const void* data, u32 isBlocking) */
int32_t lfqueue_push_body(uint32_t q, uint32_t data, uint32_t blocking, SignalFn sig)
{
    if (q & 0x7F) return SPURS_ERROR_ALIGN;
    if (!q || !data) return SPURS_ERROR_NULL;
    if (rd32(q + Q_DIR) != DIR_ANY2ANY) {
        /* TODO: the other directions (PPU2SPU and so on); the game only creates ANY2ANY queues. */
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[lfqueue] PushBody on a queue of direction %u is not implemented\n", rd32(q + Q_DIR)); }
        return SPURS_ERROR_PERM;
    }

    int32_t pointer = 0;
    if (int32_t rc = get_push_pointer2(q, &pointer, blocking != 0, true)) return to_spurs(rc);

    const int32_t depth = int32_t(rd32(q + Q_DEPTH));
    const int32_t slot = pointer >= depth ? pointer - depth : pointer;
    const uint32_t size = rd32(q + Q_SIZE);
    const uint32_t buffer = uint32_t(rd64(q + Q_BUFFER)) & ~1u;
    memcpy(vm_base + buffer + size * uint32_t(slot), vm_base + data, size);

    const int32_t rc = complete_push_pointer2(q, pointer, sig);
    if (rc == SPURS_ERROR_INVAL || rc == SPURS_ERROR_STAT) return SPURS_ERROR_SIGNAL;   /* a signal that was refused */
    return to_spurs(rc);
}

uint64_t h_lfqueue_initialize(uint64_t a_spurs, uint64_t a_queue, uint64_t a_buffer, uint64_t a_size,
                              uint64_t a_depth, uint64_t a_direction, uint64_t, uint64_t)
{
    return uint32_t(lfqueue_initialize(uint32_t(a_spurs), uint32_t(a_queue), uint32_t(a_buffer), uint32_t(a_size),
                                       uint32_t(a_depth), uint32_t(a_direction)));
}

uint64_t h_lfqueue_push_body(uint64_t a_queue, uint64_t a_data, uint64_t a_blocking, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t)
{
    return uint32_t(lfqueue_push_body(uint32_t(a_queue), uint32_t(a_data), uint32_t(a_blocking), send_signal));
}

}  // namespace

extern "C" void sc2hd_lfqueue_register(void)
{
    ps3_hle_register(0x011EE38Bu, "_cellSpursLFQueueInitialize", reinterpret_cast<void*>(h_lfqueue_initialize));
    ps3_hle_register(0x8A85674Du, "_cellSpursLFQueuePushBody", reinterpret_cast<void*>(h_lfqueue_push_body));
}

/* For the comparison against the console library (lfq_selftest.cpp). */
extern "C" int32_t sc2hd_lfqueue_test_initialize(uint32_t ea_signal, uint32_t q, uint32_t buffer, uint32_t size,
                                                 uint32_t depth, uint32_t direction)
{
    return lfqueue_initialize(ea_signal, q, buffer, size, depth, direction);
}
extern "C" int32_t sc2hd_lfqueue_test_push(uint32_t q, uint32_t data, uint32_t blocking, int32_t (*sig)(uint32_t, uint32_t))
{
    return lfqueue_push_body(q, data, blocking, sig);
}
