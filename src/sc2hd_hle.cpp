/*
 * Soulcalibur II HD Online (NPUB31366, versions 01.00 and 01.01) -- title-specific HLE.
 *
 * Firmware imports the game uses that ps3recomp v0.15.0 does not implement:
 *   - the cellFiber scheduler API (cellFiberPpuInitializeScheduler, CreateFiber,
 *     RunFibers, WaitSignal, SendSignal, JoinFiber, FinalizeScheduler); the
 *     toolkit only has the older scheduler-less cellFiberPpuCreateFiber(fiber, ...)
 *   - _sys_spu_printf_initialize, sceNpDrmIsAvailable
 *
 * Handlers use the generic HLE ABI (runtime/ppu/ppu_hle.cpp): up to eight integer
 * arguments r3..r10 as uint64_t, result in r3. Pointers are guest addresses.
 *
 * Struct sizes come from the SDK layout as documented in RPCS3's cellFiber.h
 * (RPCS3 itself does not implement these functions, they are `todo` there).
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600   /* Darwin hides the real ucontext_t without it */
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>        /* fibers stand in for ucontext */
#else
#  include <ucontext.h>
#endif

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ppu_context.h"

extern "C" {
int32_t  cellFontInit(const void* config);                          /* libs/font/cellFont.c */
int32_t  cellFontFTInit(const void* config, void* lib);             /* libs/font/cellFontFT.c */
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
uint32_t vm_read32(uint64_t addr);
void     vm_write32(uint64_t addr, uint32_t val);
extern uint8_t* vm_base;
extern void (*g_ppu_thread_entry_trampoline)(ppu_context*);
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
}

namespace {

constexpr uint64_t CELL_OK                = 0;
constexpr uint64_t CELL_FIBER_ERROR_INVAL = 0x80760002u;
constexpr uint64_t CELL_FIBER_ERROR_NOMEM = 0x80760004u;
constexpr uint64_t CELL_FIBER_ERROR_STAT  = 0x8076000Fu;

/* SDK struct sizes (bytes). */
constexpr uint32_t SIZEOF_SCHED_ATTR = 256;
constexpr uint32_t SIZEOF_FIBER_ATTR = 256;

/* Host stack for each fiber. The guest stack (eaStack) is separate: it holds
 * the guest's own frames, this one the host frames of the lifted C++ code. */
constexpr size_t FIBER_HOST_STACK = 4u * 1024 * 1024;

enum FiberState { F_READY, F_RUNNING, F_WAIT_SIGNAL, F_WAIT_JOIN, F_DONE };

struct Sched;

struct Fiber {
    uint32_t  ea = 0;
    uint32_t  opd = 0;
    uint32_t  priority = 0;
    uint32_t  stack_ea = 0;
    uint32_t  stack_size = 0;
    uint64_t  arg = 0;
    uint64_t  seq = 0;            /* creation order, FIFO among equal priorities */
    int       state = F_READY;
    bool      signal_pending = false;
    Fiber*    join_target = nullptr;  /* F_WAIT_JOIN: the fiber this one waits for */
    int32_t   status = 0;
    Sched*    sched = nullptr;
#ifdef _WIN32
    void*     host_fiber = nullptr;       /* the Win32 fiber, with its own host stack */
#else
    ucontext_t uc;
    std::unique_ptr<char[]> host_stack;
#endif
    ppu_context ctx;
};

struct Sched {
    uint32_t ea = 0;
    std::vector<Fiber*> fibers;
#ifdef _WIN32
    void*      back = nullptr;    /* RunFibers' fiber, valid while running */
#else
    ucontext_t back;              /* RunFibers' context, valid while running */
#endif
    bool running = false;
};

std::mutex                         g_mx;
std::condition_variable            g_cv;
std::unordered_map<uint32_t, Sched*> g_scheds;
std::unordered_map<uint32_t, Fiber*> g_fibers;
uint64_t                           g_seq = 0;

/* The fiber executing on this host thread, if any. */
thread_local Fiber* t_cur = nullptr;

void zero_guest(uint32_t ea, uint32_t bytes)
{
    if (ea) memset(vm_base + ea, 0, bytes);
}

Fiber* pick_runnable(Sched* s)
{
    Fiber* best = nullptr;
    for (Fiber* f : s->fibers) {
        if (f->state != F_READY) continue;
        if (!best || f->priority < best->priority ||
            (f->priority == best->priority && f->seq < best->seq))
            best = f;
    }
    return best;
}

/* Every fiber of the scheduler has ended (or it has none). */
bool all_done(const Sched* s)
{
    for (const Fiber* f : s->fibers)
        if (f->state != F_DONE) return false;
    return true;
}

/* Host context switching: ucontext on POSIX, fibers on Windows. The scheduler (RunFibers' thread) is the "back"
 * context every fiber returns to when it yields or ends. */
void fiber_body(Fiber* f);

#ifdef _WIN32
VOID CALLBACK fiber_entry(void* p)
{
    fiber_body(static_cast<Fiber*>(p));
}

bool host_create(Fiber* f)
{
    f->host_fiber = CreateFiberEx(0, FIBER_HOST_STACK, 0, fiber_entry, f);
    return f->host_fiber != nullptr;
}

/* Scheduler -> fiber. The calling thread becomes a fiber itself on first use. */
void host_resume(Sched* s, Fiber* f)
{
    if (!IsThreadAFiber()) ConvertThreadToFiber(nullptr);
    s->back = GetCurrentFiber();
    SwitchToFiber(f->host_fiber);
}

void host_yield(Fiber* f)   { SwitchToFiber(f->sched->back); }
[[noreturn]] void host_exit(Fiber* f)
{
    SwitchToFiber(f->sched->back);
    abort();                 /* a finished fiber is never resumed */
}
#else
void fiber_entry(unsigned hi, unsigned lo)
{
    fiber_body(reinterpret_cast<Fiber*>((static_cast<uintptr_t>(hi) << 32) | lo));
}

bool host_create(Fiber* f)
{
    f->host_stack.reset(new char[FIBER_HOST_STACK]);
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp   = f->host_stack.get();
    f->uc.uc_stack.ss_size = FIBER_HOST_STACK;
    f->uc.uc_link          = nullptr;
    auto p = reinterpret_cast<uintptr_t>(f);
    makecontext(&f->uc, reinterpret_cast<void (*)()>(fiber_entry), 2,
                static_cast<unsigned>(p >> 32), static_cast<unsigned>(p & 0xFFFFFFFFu));
    return true;
}

void host_resume(Sched* s, Fiber* f) { swapcontext(&s->back, &f->uc); }
void host_yield(Fiber* f)            { swapcontext(&f->uc, &f->sched->back); }
[[noreturn]] void host_exit(Fiber* f)
{
    setcontext(&f->sched->back);
    abort();
}
#endif

/* Fiber body, on its own host stack. */
void fiber_body(Fiber* f)
{
    f->ctx.gpr[1] = (f->stack_ea + f->stack_size - 0x100u) & ~0xFull;
    f->ctx.gpr[3] = f->arg;
    f->ctx.cia    = f->opd;
    g_ppu_thread_entry_trampoline(&f->ctx);   /* runs the entry OPD to completion */

    {
        std::lock_guard<std::mutex> lk(g_mx);
        f->status = static_cast<int32_t>(f->ctx.gpr[3]);
        f->state  = F_DONE;
        for (auto& kv : g_fibers)                  /* fibers that joined this one run again */
            if (kv.second->state == F_WAIT_JOIN && kv.second->join_target == f) kv.second->state = F_READY;
    }
    g_cv.notify_all();
    host_exit(f);                             /* never returns */
}

uint64_t h_fiber_nop(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t)
{
    return CELL_OK;
}

/* int _cellFiberPpuSchedulerAttributeInitialize(attr, sdkVersion) */
uint64_t h_sched_attr_init(uint64_t attr, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (!attr) return CELL_FIBER_ERROR_INVAL;
    zero_guest(static_cast<uint32_t>(attr), SIZEOF_SCHED_ATTR);
    return CELL_OK;
}

/* int _cellFiberPpuAttributeInitialize(attr, sdkVersion) */
uint64_t h_fiber_attr_init(uint64_t attr, uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t)
{
    if (!attr) return CELL_FIBER_ERROR_INVAL;
    zero_guest(static_cast<uint32_t>(attr), SIZEOF_FIBER_ATTR);
    return CELL_OK;
}

/* int cellFiberPpuInitializeScheduler(scheduler, attr) */
uint64_t h_init_scheduler(uint64_t sched, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t, uint64_t)
{
    if (!sched) return CELL_FIBER_ERROR_INVAL;
    std::lock_guard<std::mutex> lk(g_mx);
    auto& slot = g_scheds[static_cast<uint32_t>(sched)];
    if (!slot) slot = new Sched();
    slot->ea = static_cast<uint32_t>(sched);
    return CELL_OK;
}

/* int cellFiberPpuFinalizeScheduler(scheduler) */
uint64_t h_finalize_scheduler(uint64_t sched, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_scheds.find(static_cast<uint32_t>(sched));
    if (it == g_scheds.end()) return CELL_FIBER_ERROR_INVAL;
    if (it->second->running) return CELL_FIBER_ERROR_STAT;
    for (Fiber* f : it->second->fibers) g_fibers.erase(f->ea);
    /* Fibers and their host stacks are leaked on purpose: a joiner may still
     * hold a pointer, and the game finalizes once at shutdown. */
    g_scheds.erase(it);
    return CELL_OK;
}

/* int cellFiberPpuCreateFiber(scheduler, fiber, entry, arg, priority,
 *                             eaStack, sizeStack, attr) */
uint64_t h_create_fiber(uint64_t sched, uint64_t fiber, uint64_t entry, uint64_t arg,
                        uint64_t priority, uint64_t ea_stack, uint64_t size_stack,
                        uint64_t)
{
    if (!sched || !fiber || !entry || !ea_stack || size_stack < 0x400)
        return CELL_FIBER_ERROR_INVAL;

    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_scheds.find(static_cast<uint32_t>(sched));
    if (it == g_scheds.end()) return CELL_FIBER_ERROR_INVAL;

    auto* f = new Fiber();
    f->ea         = static_cast<uint32_t>(fiber);
    f->opd        = static_cast<uint32_t>(entry);
    f->arg        = arg;
    f->priority   = static_cast<uint32_t>(priority);
    f->stack_ea   = static_cast<uint32_t>(ea_stack);
    f->stack_size = static_cast<uint32_t>(size_stack);
    f->seq        = g_seq++;
    f->sched      = it->second;

    memset(&f->ctx, 0, sizeof f->ctx);
    ppu_context* parent = g_active_ctx;
    f->ctx.gpr[13]   = parent ? parent->gpr[13] : 0;   /* r13 0 -> trampoline picks a TLS block */
    f->ctx.thread_id = parent ? parent->thread_id : 0x7FFF0000ull;

    if (!host_create(f)) {
        delete f;
        return CELL_FIBER_ERROR_NOMEM;
    }

    it->second->fibers.push_back(f);
    g_fibers[f->ea] = f;
    g_cv.notify_all();
    return CELL_OK;
}

/* int cellFiberPpuRunFibers(scheduler): runs the fibers until every one has ended. A fiber waiting for a signal (or a
 * join) that another thread will deliver is waited for on g_cv, not abandoned. */
uint64_t h_run_fibers(uint64_t sched_ea, uint64_t, uint64_t, uint64_t, uint64_t,
                      uint64_t, uint64_t, uint64_t)
{
    std::unique_lock<std::mutex> lk(g_mx);
    auto it = g_scheds.find(static_cast<uint32_t>(sched_ea));
    if (it == g_scheds.end()) return CELL_FIBER_ERROR_INVAL;
    Sched* s = it->second;
    if (s->running) return CELL_FIBER_ERROR_STAT;
    s->running = true;

    for (;;) {
        Fiber* f = pick_runnable(s);
        if (!f) {
            if (all_done(s)) break;
            g_cv.wait(lk);
            continue;
        }
        f->state = F_RUNNING;
        lk.unlock();

        ppu_context* saved_active = g_active_ctx;
        Fiber* saved_cur = t_cur;
        t_cur = f;
        g_active_ctx = &f->ctx;
        host_resume(s, f);
        g_active_ctx = saved_active;
        t_cur = saved_cur;

        lk.lock();
        if (f->state == F_RUNNING) f->state = F_READY;   /* defensive */
    }
    s->running = false;
    return CELL_OK;
}

/* int cellFiberPpuWaitSignal(void): called by a fiber, blocks it until SendSignal. */
uint64_t h_wait_signal(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t)
{
    Fiber* f = t_cur;
    if (!f) return CELL_FIBER_ERROR_STAT;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (f->signal_pending) { f->signal_pending = false; return CELL_OK; }
        f->state = F_WAIT_SIGNAL;
    }
    host_yield(f);   /* resumed by RunFibers once signalled */
    return CELL_OK;
}

/* int cellFiberPpuSendSignal(fiber, uint32_t* numWorker) */
uint64_t h_send_signal(uint64_t fiber_ea, uint64_t num_worker, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t)
{
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_fibers.find(static_cast<uint32_t>(fiber_ea));
        if (it == g_fibers.end()) return CELL_FIBER_ERROR_INVAL;
        Fiber* f = it->second;
        if (f->state == F_WAIT_SIGNAL) f->state = F_READY;
        else if (f->state != F_DONE)   f->signal_pending = true;
    }
    if (num_worker) vm_write32(num_worker, 0);
    g_cv.notify_all();
    return CELL_OK;
}

/* int cellFiberPpuJoinFiber(fiber, int32_t* status): waits for the fiber to end. Called from a fiber it yields to the
 * scheduler (blocking the host thread would stop the scheduler that has to run the fiber joined); from a plain PPU
 * thread it blocks. */
uint64_t h_join_fiber(uint64_t fiber_ea, uint64_t status_ea, uint64_t, uint64_t,
                      uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::unique_lock<std::mutex> lk(g_mx);
    auto it = g_fibers.find(static_cast<uint32_t>(fiber_ea));
    if (it == g_fibers.end()) return CELL_FIBER_ERROR_INVAL;
    Fiber* f = it->second;
    Fiber* self = t_cur;
    if (self && self != f && f->state != F_DONE) {
        self->state = F_WAIT_JOIN;
        self->join_target = f;
        lk.unlock();
        host_yield(self);   /* resumed by RunFibers once f has ended */
        lk.lock();
        self->join_target = nullptr;
    }
    g_cv.wait(lk, [f] { return f->state == F_DONE; });
    if (status_ea) vm_write32(status_ea, static_cast<uint32_t>(f->status));
    return CELL_OK;
}

/* ---- fonts and text conversion: the *WithRevision entry points the SDK emits ------------- */

/* int cellFontInitializeWithRevision(uint64_t revisionFlags, CellFontConfig* config) */
uint64_t h_font_init_rev(uint64_t, uint64_t config, uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t)
{
    return static_cast<uint64_t>(static_cast<int64_t>(cellFontInit(reinterpret_cast<const void*>(config))));
}

/* int cellFontInitLibraryFreeTypeWithRevision(uint64_t revisionFlags, config, CellFontLibrary** lib) */
uint64_t h_font_ft_init_rev(uint64_t, uint64_t config, uint64_t lib, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t)
{
    return static_cast<uint64_t>(static_cast<int64_t>(
        cellFontFTInit(reinterpret_cast<const void*>(config), reinterpret_cast<void*>(lib))));
}

/* int SBCSstoUTF8s(const uint8_t* src, uint32_t* srcLen, uint8_t* dst, uint32_t* dstLen, uint32_t codePage)
 * Single-byte text to UTF-8. The game feeds Latin text (ASCII / ISO-8859-1 / Windows-1252 range, code
 * page 0x1D); bytes >= 0x80 are mapped as ISO-8859-1. *srcLen returns the bytes consumed, *dstLen the
 * bytes written. With dst == NULL nothing is written and *dstLen returns the size the conversion needs
 * (the game sizes its buffer that way). The result is an L10nResult, not a CELL error code. */
uint64_t h_sbcs_to_utf8(uint64_t src, uint64_t src_len_ea, uint64_t dst, uint64_t dst_len_ea,
                        uint64_t, uint64_t, uint64_t, uint64_t)
{
    constexpr uint64_t L10N_CONVERSION_OK = 0, L10N_SRC_ILLEGAL = 1, L10N_DST_EXHAUSTED = 2;   /* L10nResult */
    if (!src || !src_len_ea || !dst_len_ea) return L10N_SRC_ILLEGAL;
    const uint32_t in_len  = vm_read32(src_len_ea);
    const uint32_t out_cap = vm_read32(dst_len_ea);
    uint32_t in = 0, out = 0;
    uint64_t result = L10N_CONVERSION_OK;
    while (in < in_len) {
        const uint8_t c = vm_base[src + in];
        const uint32_t need = c < 0x80 ? 1 : 2;
        if (dst && out + need > out_cap) { result = L10N_DST_EXHAUSTED; break; }
        if (dst) {
            if (c < 0x80) {
                vm_base[dst + out] = c;
            } else {
                vm_base[dst + out]     = static_cast<uint8_t>(0xC0 | (c >> 6));
                vm_base[dst + out + 1] = static_cast<uint8_t>(0x80 | (c & 0x3F));
            }
        }
        out += need;
        in++;
    }
    vm_write32(src_len_ea, in);
    vm_write32(dst_len_ea, out);
    return result;
}

}  // namespace

/* NIDs are SHA-1(name + suffix) as in tools/nid_database.py; they are written
 * out here so the table does not depend on the generator. */
extern "C" void sc2hd_sail_register(void);   /* sail_hle.cpp */
extern "C" void sc2hd_lfqueue_register(void);   /* spurs_lfqueue.cpp */

extern "C" void sc2hd_hle_register(void)
{
    struct E { uint32_t nid; const char* name;
               uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t); };
    static const E table[] = {
        { 0x55870804u, "_cellFiberPpuInitialize",                   h_fiber_nop },
        { 0x9E25C72Du, "_cellFiberPpuSchedulerAttributeInitialize", h_sched_attr_init },
        { 0xC11F8056u, "_cellFiberPpuAttributeInitialize",          h_fiber_attr_init },
        { 0xEE3B604Du, "cellFiberPpuInitializeScheduler",           h_init_scheduler },
        { 0x8B6BAA01u, "cellFiberPpuFinalizeScheduler",             h_finalize_scheduler },
        { 0x7C2F4034u, "cellFiberPpuCreateFiber",                   h_create_fiber },
        { 0x12B1ACF0u, "cellFiberPpuRunFibers",                     h_run_fibers },
        { 0x6C164B3Bu, "cellFiberPpuWaitSignal",                    h_wait_signal },
        { 0x8AFB8356u, "cellFiberPpuSendSignal",                    h_send_signal },
        { 0xA6004249u, "cellFiberPpuJoinFiber",                     h_join_fiber },
        { 0xF03DCC29u, "cellFontInitializeWithRevision",            h_font_init_rev },
        { 0x7A0A83C4u, "cellFontInitLibraryFreeTypeWithRevision",   h_font_ft_init_rev },
        { 0x72632E53u, "SBCSstoUTF8s",                              h_sbcs_to_utf8 },
        /* sysPrxForUser: registers the SPU printf callbacks; nothing prints. */
        { 0x45FE2FCEu, "_sys_spu_printf_initialize",                h_fiber_nop },
        /* sceNp: the content is served already decrypted, see README. */
        { 0xAD218FAFu, "sceNpDrmIsAvailable",                       h_fiber_nop },
        /* The queue's Lv2 event queue is HLE-only (patches/0002): nothing to detach. */
        { 0x73E06F91u, "cellSpursLFQueueDetachLv2EventQueue",       h_fiber_nop },
    };
    for (const E& e : table)
        ps3_hle_register(e.nid, e.name, reinterpret_cast<void*>(e.fn));
    sc2hd_sail_register();
    sc2hd_lfqueue_register();
}
