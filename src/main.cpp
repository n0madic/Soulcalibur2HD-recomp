/*
 * ps3recomp game project -- the host: everything of the port except the lifted game code.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: load the game module (the lifted PPU and SPU code, built on the
 * user's machine from their own copy of the game), allocate the flat guest VM,
 * load the PPU ELF named on the command line, register the module's function
 * table and the HLE NID handlers, start a frame clock, and dispatch the entry
 * OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's header preamble (host build: make_host_header.py), which declares
 * ppu_context and the function table type. It comes first: everything below is
 * written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/mman.h>
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
void     cellGame_init_from_paramsfo(const char* sfo_path);   /* title id, CATEGORY, versions */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
unsigned cellGcmGetFlipStatus(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* Content installer (install.cpp): `sc2hd install ...` unpacks and decrypts packages, `sc2hd selftest` checks the crypto. */
namespace sc2hd {
int install_main(int argc, char** argv);
int selftest_main();
}

/* ---------------------------------------------------------------------------
 * The game module
 * -----------------------------------------------------------------------*/
/* The lifted code is a loadable module (dll / dylib / so) built from the user's own copy of the game, so that nothing
 * derived from it ships with the host. The runtime reads the module's function table through these.
 * A monolith build (SC2HD_MONOLITH, personal use) links the lifted code in instead and has no module. */
#ifndef SC2HD_MONOLITH
const func_entry* g_game_function_table = nullptr;
uint64_t          g_game_function_table_count = 0;

#ifndef SC2HD_HOST_ABI
#  error "SC2HD_HOST_ABI must be defined by the build (CMakeLists.txt)"
#endif

#ifdef _WIN32
static void* module_open(const char* path) { return (void*)LoadLibraryA(path); }
static void* module_sym(void* m, const char* name) { return (void*)GetProcAddress((HMODULE)m, name); }
static const char* module_error(void)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "Win32 error %lu", (unsigned long)GetLastError());
    return buf;
}
#else
static void* module_open(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void* module_sym(void* m, const char* name) { return dlsym(m, name); }
static const char* module_error(void) { const char* e = dlerror(); return e ? e : "unknown error"; }
#endif

static bool load_game_module(const char* path)
{
    void* m = module_open(path);
    if (!m) {
        fprintf(stderr, "ERROR: cannot load the game module %s: %s\n", path, module_error());
        return false;
    }
    const char* abi = (const char*)module_sym(m, "sc2hd_module_abi");
    if (!abi || strcmp(abi, SC2HD_HOST_ABI) != 0) {
        fprintf(stderr, "ERROR: %s was built for another host (module ABI %s, host ABI %s); rebuild it\n", path,
                abi ? abi : "missing", SC2HD_HOST_ABI);
        return false;
    }
    const func_entry* table = (const func_entry*)module_sym(m, "function_table");
    const uint64_t*   count = (const uint64_t*)module_sym(m, "function_table_count");
    if (!table || !count) {
        fprintf(stderr, "ERROR: %s has no function table\n", path);
        return false;
    }
    g_game_function_table = table;
    g_game_function_table_count = *count;
    printf("[boot] game module %s: %llu lifted functions\n", path, (unsigned long long)*count);
    return true;
}
#endif  // SC2HD_MONOLITH

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* On Windows a Vulkan window with D3D12 as the fallback, Metal on Apple, and on Linux a Vulkan window with the null
 * backend's headless software path as the fallback for a machine with no Vulkan device or no display -- the same
 * selection runtime/host/host_posix.c makes.
 * All of them expose the same three entry points, so the frame clock below is backend-agnostic. */
#if defined(_WIN32) && defined(PS3RECOMP_RSX_VULKAN)
/* Windows: the Vulkan window first. The D3D12 backend has only the toolkit's old live-draw path, which draws this
 * game's scene into a supersampled target it never resolves (a black picture after the intro); Vulkan runs the same
 * draw engine as Metal. D3D12 stays as the fallback for a machine without a Vulkan driver. SC2HD_D3D12=1 forces it. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
struct RsxBackend {
    const char* name;
    int  (*init)(uint32_t w, uint32_t h, const char* title);
    void (*present)(void);
    int  (*pump)(void);
};
static RsxBackend g_rsx = { "D3D12", rsx_d3d12_backend_init, rsx_d3d12_backend_present,
                            rsx_d3d12_backend_pump_messages };

static int rsx_backend_init(uint32_t w, uint32_t h, const char* title)
{
    if (!getenv("SC2HD_D3D12")) {
        const RsxBackend vulkan = { "Vulkan", rsx_vulkan_backend_init, rsx_vulkan_backend_present,
                                    rsx_vulkan_backend_pump_messages };
        if (vulkan.init(w, h, title) == 0) { g_rsx = vulkan; return 0; }
        fprintf(stderr, "[rsx] Vulkan backend unavailable, falling back to D3D12\n");
    }
    return g_rsx.init(w, h, title);
}
#  define rsx_backend_present g_rsx.present
#  define rsx_backend_pump    g_rsx.pump
#  define RSX_BACKEND_NAME    g_rsx.name
#elif defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  ifdef PS3RECOMP_RSX_VULKAN
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
#  endif
struct RsxBackend {
    const char* name;
    int  (*init)(uint32_t w, uint32_t h, const char* title);
    void (*present)(void);
    int  (*pump)(void);
};
static RsxBackend g_rsx = { "null (headless software)", rsx_null_backend_init, rsx_null_backend_present,
                            rsx_null_backend_pump_messages };

static int rsx_backend_init(uint32_t w, uint32_t h, const char* title)
{
#  ifdef PS3RECOMP_RSX_VULKAN
    // Without a display the Vulkan backend would render offscreen for nothing, so a headless run (a server, a
    // container) takes the software path; SC2HD_HEADLESS forces that on a machine with a display.
    if ((getenv("DISPLAY") || getenv("WAYLAND_DISPLAY")) && !getenv("SC2HD_HEADLESS")) {
        const RsxBackend vulkan = { "Vulkan", rsx_vulkan_backend_init, rsx_vulkan_backend_present,
                                    rsx_vulkan_backend_pump_messages };
        if (vulkan.init(w, h, title) == 0) { g_rsx = vulkan; return 0; }
        fprintf(stderr, "[rsx] Vulkan backend unavailable, falling back to the headless software path\n");
    }
#  endif
    return g_rsx.init(w, h, title);
}
#  define rsx_backend_present g_rsx.present
#  define rsx_backend_pump    g_rsx.pump
#  define RSX_BACKEND_NAME    g_rsx.name
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Process exit
 * -----------------------------------------------------------------------*/
/* Ends the process while the guest, SPU and frame-clock threads are still running. exit() would run the static
 * destructors (the HLE tables, their mutexes) under those threads' feet, so flush the output and leave directly. */
[[noreturn]] static void exit_process(int code)
{
    fflush(NULL);
    _exit(code);
}

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
    /* The flip is DONE only now: the walker has executed everything queued before
     * the guest's FLIP word, so the guest may reuse that frame's display lists.
     * Completing flips on the vblank beat alone (the template's cellGcmTickFlip
     * next to cellGcmTickVBlank) let the guest rewrite a list the walker was still
     * reading -- the title double-buffers its display lists and relies on flip
     * status for that. */
    cellGcmTickFlip();
}

/* Safety net for the flip rule above: when the walker resynchronises after a bad command it can
 * drop the guest's FLIP word, and the guest would then wait for a completion that never comes.
 * A flip outstanding for longer than FLIP_STALE_MS (several frames, far beyond a normal one) is
 * completed anyway. */
#define FLIP_STALE_MS 100
static void complete_stale_flip(void)
{
    static ULONGLONG waiting_since = 0;
    if (cellGcmGetFlipStatus() != 1u) { waiting_since = 0; return; }   /* 1 = WAITING */
    const ULONGLONG now = GetTickCount64();
    if (!waiting_since) { waiting_since = now; return; }
    if (now - waiting_since >= FLIP_STALE_MS) {
        cellGcmTickFlip();
        waiting_since = 0;
    }
}

/* The RSX has to be running when the guest starts: the real one consumes commands from the first
 * frame, while here the walker does not start until the window exists (0.2-0.7 s). A guest that ran
 * meanwhile queued a dozen frames and overwrote their double-buffered display lists before the walker
 * got to them, which lost one-off state sent in an early list (the 2D vertex-program matrix: every
 * later UI frame came out black, about every second launch). */
static volatile int g_rsx_started = 0;
static void wait_for_rsx_start(void)
{
    for (int ms = 0; !g_rsx_started && ms < 10000; ms++) Sleep(1);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK" : "FAILED");
    g_rsx_started = 1;

    unsigned  last_flip = 0;
    ULONGLONG next_tick = GetTickCount64();
    /* At most one guest frame per vblank, as on a PS3 driving a 60 Hz output. Presenting completes the flip, and
     * the game renders its next frame as soon as it does: on a 120 Hz display the Vulkan swapchain let it flip about
     * 110 times a second. The Metal path is left as it was. */
#ifdef __APPLE__
    const int flip_per_vblank = 0;
#else
    const int flip_per_vblank = 1;
#endif
    unsigned vblanks = 0, presented_vblank = ~0u;
#define FLIP_ALLOWED() (!flip_per_vblank || presented_vblank != vblanks)

    for (;;) {
        /* 1 ms, not 4: this thread also drains the FIFO, and a frame the guest finished while it slept waited out the
         * rest of the sleep before being drained and flipped (about 5 ms a frame of a 60 fps fight). The ticks below
         * keep their own 16 ms clock. */
        Sleep(1);
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            vblanks++;
            complete_stale_flip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && FLIP_ALLOWED() && cellGcm_take_flip_pending()) {
                present_guest_frame();
                presented_vblank = vblanks;
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* SC2HD_FPS=1: guest frames presented per second, every 5 s. */
        { static int on = -1; static ULONGLONG t0; static LONG f0;
          if (on < 0) { on = getenv("SC2HD_FPS") ? 1 : 0; t0 = now; f0 = g_frames_presented; }
          if (on && now - t0 >= 5000) {
              const LONG f = g_frames_presented;
              fprintf(stderr, "[fps] %.1f\n", (f - f0) * 1000.0 / (double)(now - t0));
              t0 = now; f0 = f;
          } }

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (FLIP_ALLOWED() && cellGcm_take_flip_pending()) {
                present_guest_frame();
                presented_vblank = vblanks;
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            if (rsx_backend_pump() != 0) {
                /* Window closed: the game has no exit of its own to run, and its threads keep the process
                 * alive, so leave directly. */
                fprintf(stderr, "[boot] window closed -- exiting\n");
                exit_process(0);
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip && FLIP_ALLOWED()) {
                present_guest_frame();
                presented_vblank = vblanks;
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
#undef FLIP_ALLOWED
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) {
        char* s = strrchr(s_vfs_root, '/');
        if (!s) { strcpy(s_vfs_root, "."); break; }        /* a relative path ran out: the root is the current dir */
        if (s == s_vfs_root) { s[1] = 0; break; }          /* reached the file system root */
        *s = 0;
    }
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. mmap, not calloc: the runtime protects pages of the arena
     * (vm_commit), which needs a page-aligned base, and glibc's calloc of this size returns a pointer 16 bytes
     * into a page. MAP_NORESERVE keeps the strict-overcommit modes of Linux from refusing 3.5 GB up front. */
    void* arena = mmap(nullptr, 0xE0000000u, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = arena == MAP_FAILED ? nullptr : (uint8_t*)arena;
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

#ifdef __APPLE__
#include <pthread.h>
/* Host stack of the guest main thread. The lifted code recurses on the host
 * stack as deep as the guest does on its own. */
#define GUEST_HOST_STACK (256ull * 1024 * 1024)

struct GuestRun { uint32_t entry; int rc; };

static void* guest_main_thread(void* arg)
{
    GuestRun* r = (GuestRun*)arg;
    wait_for_rsx_start();
    r->rc = ppu_run(r->entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", r->rc);
    /* A guest that called sys_process_exit exits the host from inside ppu_run.
     * Getting here means the entry function returned instead. */
    exit_process(g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0);
}
#endif

/* Sets an environment variable the runtime reads, unless the user already set it. */
static void set_default_env(const char* name, const char* value)
{
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "install") == 0) return sc2hd::install_main(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) return sc2hd::selftest_main();

#ifdef SC2HD_MONOLITH
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }
#else
    const char* module_path = getenv("SC2HD_MODULE");
    if (argc >= 3 && strcmp(argv[1], "--module") == 0) {
        module_path = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2 || !module_path) {
        printf("usage: %s [--module <game module>] <PPU ELF>\n"
               "       the module can also be named by SC2HD_MODULE\n", argv[0]);
        return 2;
    }
#endif

    /* The game double-buffers its display lists and keeps writing the next frame's list while the
     * walker may still be reading the previous one (patches/0006): walk a private copy of each
     * called list. */
    set_default_env("GCM_SNAPSHOT_CALLS", "1");

    /* SPU channel reads block until data arrives (a read of an empty mailbox otherwise returns 0 at once,
     * which ends MultiStream's SPU program), and code that is neither lifted nor a registered overlay runs
     * on the interpreter instead of ending the job. */
    static const char* const spu_env[] = { "SPU_CH_BLOCK", "SPU_INTERP_UNLIFTED" };
    for (const char* name : spu_env) set_default_env(name, "1");

    /* First boot has no save: the game must see cellSaveDataAutoLoad2 fail with ERROR_CBRESULT (its
     * stat callback answered ERR_NODATA) and offer to create one. Reported as CELL_OK it decodes a
     * zeroed buffer instead (brightness step 0 = a tenth of the picture); as ERROR_NODATA it shows
     * its "save file is corrupted" dialog. */
    set_default_env("PS3_SAVEDATA_NODATA_OK", "2");

#ifdef PS3RECOMP_RSX_VULKAN
    /* The Vulkan backend renders offscreen and shows the frame in an SDL window only on request, and draws with
     * the game's own shaders only on request: both are what a player wants. */
    set_default_env("PS3RECOMP_VK_WINDOW", "1");
    set_default_env("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
#endif

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

#ifndef SC2HD_MONOLITH
    if (!load_game_module(module_path)) return 1;
#endif

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Title id, game category and versions come from PARAM.SFO. Without it
     * cellGameBootCheck reports a disc game "BLES00000" and the title aborts. */
    {
        static char sfo[1100];
        const char* env = getenv("PS3_PARAM_SFO");
        snprintf(sfo, sizeof sfo, "%s", (env && *env) ? env : "PARAM.SFO");
        cellGame_init_from_paramsfo(sfo);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

#ifdef __APPLE__
    /* AppKit and the Metal backend create and pump their window on the process
     * main thread (rsx_metal_backend.m dispatch_sync()s onto the main queue).
     * If the main thread is busy inside ppu_run, that call never returns and
     * the frame clock -- which drains the RSX FIFO the guest's label waits
     * depend on -- never starts. So the guest runs on a secondary thread and
     * the main thread becomes the frame clock. */
    static GuestRun run = { entry, 0 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GUEST_HOST_STACK);
    pthread_t guest;
    if (pthread_create(&guest, &attr, guest_main_thread, &run) != 0) {
        fprintf(stderr, "ERROR: could not start the guest main thread\n");
        return 1;
    }
    frame_clock(NULL);       /* runs until the process exits */
    return 0;
#else
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    wait_for_rsx_start();

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    exit_process(g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0);
#endif
}
