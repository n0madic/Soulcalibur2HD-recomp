/*
 * Soulcalibur II HD Online -- cellSail (media player) for the port.
 *
 * The game plays its movies (opening_cs.mp4, opening_ac.mp4, ending_ac.mp4) through the SDK's cellSail: a player
 * object with an event callback, stream descriptors, "futures" it blocks on, and two adapters (sound, graphics)
 * that it polls once per frame for decoded data. The toolkit's cellSail.c is a host-API sketch with a different
 * calling convention, so the game's imports landed on stubs that never delivered an event and the movie state
 * machine stalled.
 *
 * This implements the SDK contract (calls, events, states) and plays the movie: the MP4 is decoded by the
 * built-in H.264 and AAC decoders (media/movie.cpp). The game owns the picture buffers (the graphics adapter's
 * allocation callback); pictures are converted into them as RGBA. The PCM goes into buffers of ours. Struct
 * layouts and event numbers follow RPCS3's cellSail.h (the SDK headers are not available here); how the adapters
 * are used comes from the game's own code. If a movie cannot be opened the stream still "plays" for a moment
 * with no pictures, so the game moves on.
 *
 * Handlers use the generic HLE ABI: r3..r10 as uint64_t, guest pointers as 32-bit addresses.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/movie.hpp"

extern "C" {
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
uint32_t vm_read32(uint64_t addr);
void     vm_write32(uint64_t addr, uint32_t val);
uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
extern uint8_t* vm_base;
extern const char* ppu_vfs_root;
}

namespace {

constexpr uint64_t CELL_OK                    = 0;
constexpr uint64_t CELL_SAIL_ERROR_INVALID_ARG   = 0x80610701u;
constexpr uint64_t CELL_SAIL_ERROR_INVALID_STATE = 0x80610702u;
constexpr uint64_t CELL_SAIL_ERROR_CANCEL        = 0x80610709u;
constexpr uint64_t CELL_SAIL_ERROR_MEMORY        = 0x806107F0u;

/* Guest struct sizes: CellSailAudioFormat and CellSailVideoFormat. */
constexpr uint32_t kAudioFormatBytes = 24;
constexpr uint32_t kVideoFormatBytes = 32;

/* CellSailEvent major numbers (value = major << 32 | minor). */
enum : uint32_t {
    EV_PLAYER_CALL_COMPLETED = 2,
    EV_PLAYER_STATE_CHANGED  = 3,
    EV_STREAM_OPENED         = 4,
    EV_STREAM_CLOSED         = 5,
    EV_SESSION_STARTED       = 6,
    EV_PAUSE_STATE_CHANGED   = 7,
    EV_SOURCE_EOS            = 8,
};

/* CellSailPlayerCallType (the minor of CALL_COMPLETED). */
enum : uint32_t {
    CALL_BOOT = 1, CALL_OPEN_STREAM = 2, CALL_CLOSE_STREAM = 3, CALL_START = 10, CALL_STOP = 11,
};

/* CellSailPlayerState. */
enum : int {
    ST_INITIALIZED = 0, ST_BOOT_TRANSITION = 1, ST_CLOSED = 2, ST_OPEN_TRANSITION = 3, ST_OPENED = 4,
    ST_START_TRANSITION = 5, ST_RUNNING = 6, ST_STOP_TRANSITION = 7, ST_CLOSE_TRANSITION = 8,
};

/* What GetFrame returns: the frame status (bits 8 and up) and the sync status (bits 2..7). Zero: no frame. */
constexpr uint32_t kFrameNext = (2u << 8) | (1u << 2);       /* CELL_SAIL_SYNC_NEXT | CELL_SAIL_SYNC_ON_TIME */
constexpr uint32_t kFrameRepeated = (1u << 8) | (1u << 2);   /* CELL_SAIL_SYNC_REPEATED | CELL_SAIL_SYNC_ON_TIME */

/* Pictures the graphics adapter is given buffers for, and PCM buffers handed out in turn. */
constexpr int kPictureBuffers = 6;
constexpr int kPcmBuffers = 8;
constexpr uint32_t kMaxPcmFrames = 2048;
/* How far ahead of real time the sound is handed out. */
constexpr int64_t kAudioLeadUs = 250'000;
/* The sound the game has been handed is not yet heard: its own buffering delays it by about this much (tunable with
 * SC2HD_AV_OFFSET_MS, positive delays the pictures). */
constexpr int64_t kAudioLatencyUs = 100'000;
/* How long cellSailFutureGet waits when the game passes no timeout. */
constexpr uint64_t kFutureDefaultTimeoutUs = 10'000'000;
/* A movie that cannot be decoded ends after this many frames of the game's polling. */
constexpr int kFramesBeforeEosFallback = 60;

std::mutex g_mtx;   /* guards the tables only; never held across a guest callback */
/* Held while a frame is written into the guest buffers of an open stream (outside g_mtx: a picture conversion takes
 * milliseconds). close_media takes it before it frees those buffers. Lock order: g_output_mtx, then g_mtx. */
std::mutex g_output_mtx;

struct Descriptor {
    int32_t  stream_type = 0;
    uint32_t uri_ea = 0;
};

struct Player {
    uint32_t cb = 0, cb_arg = 0;          /* current event handler */
    uint32_t alloc_fn = 0, alloc_free_fn = 0, alloc_arg = 0;   /* copy of the CellSailMemAllocator */
    int      state = ST_INITIALIZED;
    bool     paused = false;              /* SetPaused; Start keeps the clock stopped while it is set */
    bool     eos_sent = false;
    int      frames_running = 0;          /* fallback when there is no movie */
    std::vector<uint32_t> descs;          /* registered descriptors, in order */
    uint32_t sound_adapter = 0, gfx_adapter = 0;
    uint32_t gfx_free_cb = 0, gfx_free_arg = 0;   /* the graphics adapter's Free callback for the picture buffers, kept
                                                   * so they are returned even if the adapter is finalized first */

    /* the open stream */
    std::shared_ptr<Movie> movie;
    uint32_t session = 0;
    uint32_t scratch = 0;                 /* guest block: format structs, the picture pointer array, PCM buffers */
    uint32_t pic_buf[kPictureBuffers] = {};
    int      pic_n = 0, pic_next = 0;
    int      pic_last = -1;               /* slot of the picture shown last, repeated while no newer one is due */
    uint64_t pic_last_pts = 0;
    uint32_t pic_pitch = 0;
    int      pcm_next = 0, pcm_channels = 0;
    uint32_t pcm_rate = 0;
    uint64_t pcm_delivered = 0;           /* sample frames handed to the game */
    int64_t  audio_at_us = 0;             /* the stream clock when the sound was last handed out */
    std::chrono::steady_clock::time_point origin;   /* the stream clock is now - origin while running */
    int64_t  frozen_us = 0;
    bool     running = false;
};

struct Adapter {
    uint32_t funcs = 0, arg = 0;
    uint32_t cb[5] = {};          /* copy of the callback table: the game builds it on its stack */
    uint8_t  format[kVideoFormatBytes] = {};   /* the larger of the two format structs */
};

std::map<uint32_t, Player>     g_players;
std::map<uint32_t, Descriptor> g_descs;
std::map<uint32_t, Adapter>    g_sound, g_gfx;
uint32_t g_session = 0;

bool trace_on()
{
    static int on = -1;
    if (on < 0) on = getenv("SC2HD_SAIL_TRACE") ? 1 : 0;
    return on == 1;
}
#define TRACE(...) do { if (trace_on()) { fprintf(stderr, "[sail] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

void wbe64(uint32_t ea, uint64_t v) { vm_write32(ea, (uint32_t)(v >> 32)); vm_write32(ea + 4, (uint32_t)v); }

int64_t clock_us(const Player& p)
{
    if (!p.running) return p.frozen_us;
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - p.origin).count();
}

int64_t av_offset_us()
{
    static int64_t v = [] {
        const char* e = getenv("SC2HD_AV_OFFSET_MS");
        return e ? (int64_t)atoi(e) * 1000 : -kAudioLatencyUs;
    }();
    return v;
}

/* The position the pictures are shown for. The sound is the master: the game takes it at the pace of its audio
 * output, which is not exactly the wall clock's, and pictures that follow the wall clock drift away from it. Between
 * two hand-outs of sound the position advances with the stream clock. Without sound it is the stream clock. */
int64_t video_clock_us(const Player& p)
{
    const int64_t wall = clock_us(p);
    if (!p.pcm_rate || !p.pcm_delivered) return wall;
    const int64_t v = (int64_t)(p.pcm_delivered * 1'000'000ULL / p.pcm_rate) + (wall - p.audio_at_us) + av_offset_us();
    return v < 0 ? 0 : v;
}

void clock_run(Player& p, bool run)
{
    if (run == p.running) return;
    if (run) p.origin = std::chrono::steady_clock::now() - std::chrono::microseconds(p.frozen_us);
    else p.frozen_us = clock_us(p);
    p.running = run;
}

/* Deliver one event to the player's current handler, on the calling guest thread. The handler may
 * call back into cellSail (futures, ReplaceEventHandler), so no lock is held here and the handler
 * is read afresh for every event. */
void deliver(uint32_t self, uint32_t major, uint32_t minor, uint64_t arg0, uint64_t arg1)
{
    uint32_t cb, arg;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        auto it = g_players.find(self);
        if (it == g_players.end()) return;
        cb = it->second.cb; arg = it->second.cb_arg;
    }
    if (!cb) return;
    TRACE("event %u.%u arg0=0x%llx arg1=0x%llx -> handler 0x%x", major, minor,
          (unsigned long long)arg0, (unsigned long long)arg1, cb);
    ppu_guest_call(cb, arg, ((uint64_t)major << 32) | minor, arg0, arg1, 0, 0, 0, 0);
    TRACE("  handler returned; game object 0x%x state field = %d", arg, (int)vm_read32(arg + 8));
}

void set_state(uint32_t self, int st)
{
    { std::lock_guard<std::mutex> g(g_mtx); g_players[self].state = st; }
    deliver(self, EV_PLAYER_STATE_CHANGED, 0, (uint64_t)st, 0);
}

int player_state(uint32_t self)
{
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_players.find(self);
    return it == g_players.end() ? -1 : it->second.state;
}

/* ---- futures: CellSailFuture = { mutex, cond, flags, result, userParam }, 24 bytes ---------- */

uint64_t h_future_initialize(uint64_t self, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    for (uint32_t o = 0; o < 24; o += 4) vm_write32(self + o, 0);
    return CELL_OK;
}

uint64_t h_future_set(uint64_t self, uint64_t result, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (!self) return CELL_OK;
    vm_write32(self + 12, (uint32_t)result);
    vm_write32(self + 8, vm_read32(self + 8) | 1u);
    TRACE("future 0x%llx set to %d", (unsigned long long)self, (int)(int32_t)result);
    return CELL_OK;
}

uint64_t h_future_get(uint64_t self, uint64_t timeout_us, uint64_t result_ea, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    /* The player completes every call before it returns, so the future is normally set already.
     * A future nothing will ever set must not hang the game forever. */
    using us = std::chrono::microseconds;
    const uint64_t max_us = (uint64_t)us::max().count();   /* a larger value (~0 as "forever") would turn negative */
    const auto limit = us((int64_t)(timeout_us ? std::min(timeout_us, max_us) : kFutureDefaultTimeoutUs));
    const auto t0 = std::chrono::steady_clock::now();
    while (!(vm_read32(self + 8) & 1u)) {
        if (std::chrono::steady_clock::now() - t0 > limit) {
            TRACE("future 0x%llx timed out", (unsigned long long)self);
            return CELL_SAIL_ERROR_CANCEL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (result_ea) vm_write32(result_ea, vm_read32(self + 12));
    return CELL_OK;
}

/* ---- memory allocator: CellSailMemAllocator = { callbacks, pArg }; callbacks = { alloc, free } --- */

uint64_t h_alloc_init(uint64_t self, uint64_t callbacks, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("MemAllocatorInitialize(0x%llx callbacks=0x%llx arg=0x%llx)", (unsigned long long)self,
          (unsigned long long)callbacks, (unsigned long long)arg);
    vm_write32(self, (uint32_t)callbacks);
    vm_write32(self + 4, (uint32_t)arg);
    return CELL_OK;
}

uint32_t guest_alloc(uint32_t opd, uint32_t arg, uint32_t size)
{
    if (!opd) return 0;
    const uint64_t r = ppu_guest_call(opd, arg, 16, size, 0, 0, 0, 0, 0);
    TRACE("guest_alloc(size=0x%x) via opd 0x%x arg 0x%x -> 0x%llx", size, opd, arg, (unsigned long long)r);
    return (uint32_t)r;
}

void guest_free(uint32_t opd, uint32_t arg, uint32_t ptr)
{
    if (opd && ptr) ppu_guest_call(opd, arg, 16, ptr, 0, 0, 0, 0, 0);
}

/* ---- media: the movie behind an open stream ------------------------------------------------------- */

/* Guest path of a cell-fs URI, mapped onto the host the way the toolkit's file system maps it. */
std::string host_path(const char* uri)
{
    std::string g = uri ? uri : "";
    static const char kScheme[] = "x-cell-fs://";
    if (g.rfind(kScheme, 0) == 0) g = g.substr(sizeof kScheme - 1);
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    if (hdd0 && *hdd0 && g.rfind("/dev_hdd0/", 0) == 0) return std::string(hdd0) + "/" + g.substr(10);
    for (const char* mount : {"/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/"}) {
        if (g.rfind(mount, 0) == 0) return std::string(ppu_vfs_root) + "/" + g.substr(strlen(mount));
    }
    return std::string(ppu_vfs_root) + (g.empty() || g[0] != '/' ? "/" : "") + g;
}

/* CellSailVideoFormat (32 bytes): coding, scan, bitsPerColor, frameRate (s8 each), width, height (s16),
 * pitch, alpha (s32), colorMatrix, aspectRatio, colorRange, reserved (s8), reserved (s32), reserved (s64). */
void make_video_format(uint8_t* f, int w, int h)
{
    std::memset(f, 0xff, 32);
    f[0] = 1;            /* CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED */
    f[1] = 0;            /* progressive */
    f[2] = 8;
    f[3] = 3;            /* 30000/1001 Hz */
    f[4] = (uint8_t)(w >> 8); f[5] = (uint8_t)w;
    f[6] = (uint8_t)(h >> 8); f[7] = (uint8_t)h;
    const uint32_t pitch = (uint32_t)w * 4;
    f[8] = (uint8_t)(pitch >> 24); f[9] = (uint8_t)(pitch >> 16); f[10] = (uint8_t)(pitch >> 8); f[11] = (uint8_t)pitch;
    f[12] = f[13] = f[14] = 0; f[15] = 255;            /* alpha */
}

/* CellSailAudioFormat (24 bytes): coding, chNum (s8), sampleNum (s16), fs, chLayout, reserved (s32), reserved (s64). */
void make_audio_format(uint8_t* f, int channels, int rate)
{
    std::memset(f, 0xff, 32);
    f[0] = 1;            /* CELL_SAIL_AUDIO_CODING_LPCM_FLOAT32 */
    f[1] = (uint8_t)channels;
    f[2] = 0x04; f[3] = 0x00;   /* 1024 samples */
    f[4] = (uint8_t)(rate >> 24); f[5] = (uint8_t)(rate >> 16); f[6] = (uint8_t)(rate >> 8); f[7] = (uint8_t)rate;
    const uint32_t layout = channels == 1 ? 1 : 2;
    f[8] = (uint8_t)(layout >> 24); f[9] = (uint8_t)(layout >> 16); f[10] = (uint8_t)(layout >> 8); f[11] = (uint8_t)layout;
}

uint32_t cb_of(uint32_t funcs, int idx) { return funcs ? vm_read32(funcs + 4u * (uint32_t)idx) : 0; }

Adapter make_adapter(uint32_t funcs, uint32_t arg, int ncb)
{
    Adapter a;
    a.funcs = funcs;
    a.arg = arg;
    for (int i = 0; i < ncb; i++) a.cb[i] = cb_of(funcs, i);
    return a;
}

/* Opens the decoders' side of the stream: tells the adapters the formats, has the game allocate the picture
 * buffers, and starts the decoder thread. Runs on the guest thread that called OpenStream. */
void open_media(uint32_t self, std::shared_ptr<Movie> mv)
{
    uint32_t alloc_fn, alloc_arg, gfx_ad, snd_ad, session;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        Player& p = g_players[self];
        alloc_fn = p.alloc_fn; alloc_arg = p.alloc_arg; gfx_ad = p.gfx_adapter; snd_ad = p.sound_adapter;
        session = ++g_session;
    }
    const int channels = mv->audio_channels() ? mv->audio_channels() : 2;
    const uint32_t pcm_slot = kMaxPcmFrames * (uint32_t)channels * 4;
    const uint32_t scratch_size = 0x80 + pcm_slot * kPcmBuffers;
    const uint32_t scratch = guest_alloc(alloc_fn, alloc_arg, scratch_size);
    if (!scratch) { TRACE("no memory for the movie buffers"); return; }

    Adapter gfx{}, snd{};
    { std::lock_guard<std::mutex> g(g_mtx); if (g_gfx.count(gfx_ad)) gfx = g_gfx[gfx_ad]; if (g_sound.count(snd_ad)) snd = g_sound[snd_ad]; }

    uint32_t pic[kPictureBuffers] = {};
    int pic_n = 0;
    const uint32_t pitch = (uint32_t)mv->width() * 4;
    if (gfx.funcs) {
        uint8_t fmt[32];
        make_video_format(fmt, mv->width(), mv->height());
        std::memcpy(vm_base + scratch, fmt, 32);
        { std::lock_guard<std::mutex> g(g_mtx); std::memcpy(g_gfx[gfx_ad].format, fmt, 32); }
        if (gfx.cb[0]) ppu_guest_call(gfx.cb[0], gfx.arg, 0, 0, 0, 0, 0, 0, 0);                 /* Makeup */
        if (gfx.cb[2]) ppu_guest_call(gfx.cb[2], gfx.arg, scratch, session, 0, 0, 0, 0, 0);     /* FormatChanged */
        if (gfx.cb[3]) {                                                                                  /* Alloc */
            const uint32_t size = pitch * (uint32_t)mv->height();
            const uint64_t r = ppu_guest_call(gfx.cb[3], gfx.arg, size, kPictureBuffers, scratch + 0x40, 0, 0, 0, 0);
            TRACE("gfx alloc(size=0x%x num=%d) -> %d", size, kPictureBuffers, (int)(int32_t)r);
            if ((int32_t)r >= 0) {
                for (int i = 0; i < kPictureBuffers; i++) pic[i] = vm_read32(scratch + 0x40 + 4u * (uint32_t)i);
                pic_n = kPictureBuffers;
                for (int i = 0; i < pic_n; i++) TRACE("  picture buffer %d at 0x%x", i, pic[i]);
            }
        }
    }
    const bool sound = snd.funcs && mv->has_audio();
    if (sound) {
        uint8_t fmt[32];
        make_audio_format(fmt, channels, mv->audio_rate());
        std::memcpy(vm_base + scratch + 0x20, fmt, 32);
        { std::lock_guard<std::mutex> g(g_mtx); std::memcpy(g_sound[snd_ad].format, fmt, 32); }
        if (snd.cb[0]) ppu_guest_call(snd.cb[0], snd.arg, 0, 0, 0, 0, 0, 0, 0);
        if (snd.cb[2]) ppu_guest_call(snd.cb[2], snd.arg, scratch + 0x20, session, 0, 0, 0, 0, 0);
    }
    /* A track nobody takes is not decoded: its queue would fill up and the movie would never end. */
    mv->set_outputs(pic_n > 0, sound);
    {
        std::lock_guard<std::mutex> g(g_mtx);
        Player& p = g_players[self];
        p.movie = mv;
        p.session = session;
        p.scratch = scratch;
        std::memcpy(p.pic_buf, pic, sizeof pic);
        p.pic_n = pic_n; p.pic_next = 0; p.pic_last = -1; p.pic_pitch = pitch;
        p.gfx_free_cb = pic_n ? gfx.cb[4] : 0; p.gfx_free_arg = gfx.arg;
        p.pcm_next = 0; p.pcm_channels = channels; p.pcm_rate = sound ? (uint32_t)mv->audio_rate() : 0;
        p.pcm_delivered = 0; p.audio_at_us = 0;
        p.frozen_us = 0; p.running = false;
    }
    mv->start();
}

void close_media(uint32_t self)
{
    std::shared_ptr<Movie> mv;
    uint32_t alloc_free, alloc_arg, scratch, gfx_free, gfx_arg;
    int pic_n;
    {
        /* Detached under g_output_mtx: once it is released no frame is being written into the buffers freed below. */
        std::lock_guard<std::mutex> out(g_output_mtx);
        std::lock_guard<std::mutex> g(g_mtx);
        auto it = g_players.find(self);
        if (it == g_players.end()) return;
        Player& p = it->second;
        mv = std::move(p.movie);
        scratch = p.scratch; p.scratch = 0;
        pic_n = p.pic_n; p.pic_n = 0;
        alloc_free = p.alloc_free_fn; alloc_arg = p.alloc_arg; gfx_free = p.gfx_free_cb; gfx_arg = p.gfx_free_arg;
        p.running = false;
    }
    if (!mv) return;
    mv.reset();     /* joins the decoder thread */
    if (scratch && pic_n && gfx_free) ppu_guest_call(gfx_free, gfx_arg, (uint64_t)pic_n, scratch + 0x40, 0, 0, 0, 0, 0);
    guest_free(alloc_free, alloc_arg, scratch);
}

/* ---- player ---------------------------------------------------------------------------------- */

uint64_t h_player_initialize2(uint64_t self, uint64_t alloc, uint64_t cb, uint64_t cb_arg, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("Initialize2(player=0x%llx cb=0x%llx)", (unsigned long long)self, (unsigned long long)cb);
    {
        std::lock_guard<std::mutex> g(g_mtx);
        Player p;
        /* The game builds the allocator and its function table on its stack and drops them once
         * Initialize2 returns, so they are copied here, like the SDK does. */
        const uint32_t funcs = alloc ? vm_read32(alloc) : 0;
        if (funcs) { p.alloc_fn = vm_read32(funcs); p.alloc_free_fn = vm_read32(funcs + 4); }
        p.alloc_arg = alloc ? vm_read32(alloc + 4) : 0;
        p.cb = (uint32_t)cb; p.cb_arg = (uint32_t)cb_arg;
        g_players[(uint32_t)self] = p;
    }
    deliver((uint32_t)self, EV_PLAYER_STATE_CHANGED, 0, ST_INITIALIZED, 0);
    return CELL_OK;
}

uint64_t h_player_finalize(uint64_t self, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("Finalize(0x%llx)", (unsigned long long)self);
    close_media((uint32_t)self);
    std::lock_guard<std::mutex> g(g_mtx);
    g_players.erase((uint32_t)self);
    return CELL_OK;
}

uint64_t h_player_replace_handler(uint64_t self, uint64_t cb, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_players.find((uint32_t)self);
    if (it == g_players.end()) return CELL_SAIL_ERROR_INVALID_ARG;
    it->second.cb = (uint32_t)cb; it->second.cb_arg = (uint32_t)arg;
    TRACE("ReplaceEventHandler(0x%llx -> 0x%llx)", (unsigned long long)self, (unsigned long long)cb);
    return CELL_OK;
}

uint64_t h_player_boot(uint64_t self, uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("Boot(0x%llx user=0x%llx)", (unsigned long long)self, (unsigned long long)user);
    if (player_state((uint32_t)self) < 0) return CELL_SAIL_ERROR_INVALID_STATE;
    set_state((uint32_t)self, ST_BOOT_TRANSITION);
    set_state((uint32_t)self, ST_CLOSED);
    deliver((uint32_t)self, EV_PLAYER_CALL_COMPLETED, CALL_BOOT, 0, user);
    return CELL_OK;
}

uint64_t h_player_create_descriptor(uint64_t self, uint64_t type, uint64_t media_info, uint64_t uri, uint64_t pp_desc, uint64_t, uint64_t, uint64_t)
{
    (void)media_info;
    TRACE("CreateDescriptor enter (player=0x%llx type=%d)", (unsigned long long)self, (int)type);
    uint32_t alloc_fn, alloc_arg;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        auto it = g_players.find((uint32_t)self);
        if (it == g_players.end()) return CELL_SAIL_ERROR_INVALID_ARG;
        alloc_fn = it->second.alloc_fn; alloc_arg = it->second.alloc_arg;
    }
    const uint32_t desc = guest_alloc(alloc_fn, alloc_arg, 0x40);
    if (!desc) { TRACE("CreateDescriptor: allocator returned 0"); return CELL_SAIL_ERROR_MEMORY; }
    for (uint32_t o = 0; o < 0x40; o += 4) vm_write32(desc + o, 0);
    { std::lock_guard<std::mutex> g(g_mtx); g_descs[desc] = Descriptor{(int32_t)type, (uint32_t)uri}; }
    vm_write32(pp_desc, desc);
    TRACE("CreateDescriptor(type=%d uri='%s') -> 0x%x", (int)type, uri ? (const char*)(vm_base + uri) : "", desc);
    return CELL_OK;
}

uint64_t h_player_add_descriptor(uint64_t self, uint64_t desc, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("AddDescriptor(0x%llx, 0x%llx)", (unsigned long long)self, (unsigned long long)desc);
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_players.find((uint32_t)self);
    if (it == g_players.end() || !g_descs.count((uint32_t)desc)) return CELL_SAIL_ERROR_INVALID_ARG;
    it->second.descs.push_back((uint32_t)desc);
    return CELL_OK;
}

uint64_t h_player_destroy_descriptor(uint64_t self, uint64_t desc, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_players.find((uint32_t)self);
    if (it != g_players.end()) {
        auto& v = it->second.descs;
        for (size_t i = 0; i < v.size(); i++) if (v[i] == (uint32_t)desc) { v.erase(v.begin() + i); break; }
    }
    g_descs.erase((uint32_t)desc);   /* the guest block is left to the allocator's owner */
    return CELL_OK;
}

uint64_t h_player_get_descriptor_count(uint64_t self, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_players.find((uint32_t)self);
    const uint64_t n = it == g_players.end() ? 0 : it->second.descs.size();
    TRACE("GetDescriptorCount(0x%llx) -> %llu", (unsigned long long)self, (unsigned long long)n);
    return n;
}

uint64_t h_descriptor_get_uri(uint64_t desc, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    auto it = g_descs.find((uint32_t)desc);
    return it == g_descs.end() ? 0 : it->second.uri_ea;
}

uint64_t h_player_open_stream(uint64_t self, uint64_t, uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    const int st = player_state((uint32_t)self);
    TRACE("OpenStream(0x%llx) state=%d", (unsigned long long)self, st);
    if (st != ST_CLOSED) return CELL_SAIL_ERROR_INVALID_STATE;
    uint32_t desc = 0, uri_ea = 0;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        auto& p = g_players[(uint32_t)self];
        if (!p.descs.empty()) desc = p.descs.front();
        auto d = g_descs.find(desc);
        if (d != g_descs.end()) uri_ea = d->second.uri_ea;
        p.eos_sent = false; p.frames_running = 0; p.paused = false;
    }
    std::shared_ptr<Movie> mv;
    if (uri_ea) {
        const std::string path = host_path((const char*)(vm_base + uri_ea));
        std::string err;
        mv = Movie::open(path, &err);
        if (!mv) fprintf(stderr, "[sail] cannot play '%s': %s\n", path.c_str(), err.c_str());
        else TRACE("playing '%s' (%dx%d, audio %d Hz x%d)", path.c_str(), mv->width(), mv->height(), mv->audio_rate(), mv->audio_channels());
    }
    set_state((uint32_t)self, ST_OPEN_TRANSITION);
    deliver((uint32_t)self, EV_STREAM_OPENED, 0, desc, 0);
    if (mv) open_media((uint32_t)self, mv);
    set_state((uint32_t)self, ST_OPENED);
    deliver((uint32_t)self, EV_PLAYER_CALL_COMPLETED, CALL_OPEN_STREAM, 0, user);
    return CELL_OK;
}

uint64_t h_player_close_stream(uint64_t self, uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    const int st = player_state((uint32_t)self);
    TRACE("CloseStream(0x%llx) state=%d", (unsigned long long)self, st);
    if (st != ST_OPENED) return CELL_SAIL_ERROR_INVALID_STATE;
    uint32_t desc = 0;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        auto& p = g_players[(uint32_t)self];
        if (!p.descs.empty()) desc = p.descs.front();
    }
    set_state((uint32_t)self, ST_CLOSE_TRANSITION);
    close_media((uint32_t)self);
    deliver((uint32_t)self, EV_STREAM_CLOSED, 0, desc, 0);
    set_state((uint32_t)self, ST_CLOSED);
    deliver((uint32_t)self, EV_PLAYER_CALL_COMPLETED, CALL_CLOSE_STREAM, 0, user);
    return CELL_OK;
}

uint64_t h_player_start(uint64_t self, uint64_t, uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    const int st = player_state((uint32_t)self);
    TRACE("Start(0x%llx) state=%d", (unsigned long long)self, st);
    if (st != ST_OPENED) return CELL_SAIL_ERROR_INVALID_STATE;
    set_state((uint32_t)self, ST_START_TRANSITION);
    deliver((uint32_t)self, EV_SESSION_STARTED, 0, 1, 0);
    { std::lock_guard<std::mutex> g(g_mtx); Player& p = g_players[(uint32_t)self]; clock_run(p, !p.paused); }
    set_state((uint32_t)self, ST_RUNNING);
    deliver((uint32_t)self, EV_PLAYER_CALL_COMPLETED, CALL_START, 0, user);
    return CELL_OK;
}

uint64_t h_player_stop(uint64_t self, uint64_t user, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    const int st = player_state((uint32_t)self);
    TRACE("Stop(0x%llx) state=%d", (unsigned long long)self, st);
    if (st != ST_RUNNING) return CELL_SAIL_ERROR_INVALID_STATE;
    set_state((uint32_t)self, ST_STOP_TRANSITION);
    { std::lock_guard<std::mutex> g(g_mtx); clock_run(g_players[(uint32_t)self], false); }
    set_state((uint32_t)self, ST_OPENED);
    deliver((uint32_t)self, EV_PLAYER_CALL_COMPLETED, CALL_STOP, 0, user);
    return CELL_OK;
}

uint64_t h_player_set_paused(uint64_t self, uint64_t paused, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    const bool p = (paused & 0xff) != 0;
    TRACE("SetPaused(0x%llx, %d)", (unsigned long long)self, (int)p);
    {
        std::lock_guard<std::mutex> g(g_mtx);
        auto it = g_players.find((uint32_t)self);
        if (it == g_players.end()) return CELL_SAIL_ERROR_INVALID_ARG;
        it->second.paused = p;
        if (it->second.state == ST_RUNNING) clock_run(it->second, !p);
    }
    deliver((uint32_t)self, EV_PAUSE_STATE_CHANGED, 0, p ? 1 : 0, 0);
    return CELL_OK;
}

uint64_t h_player_set_adapter_sound(uint64_t self, uint64_t, uint64_t adapter, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    g_players[(uint32_t)self].sound_adapter = (uint32_t)adapter;
    return CELL_OK;
}

uint64_t h_player_set_adapter_gfx(uint64_t self, uint64_t, uint64_t adapter, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    g_players[(uint32_t)self].gfx_adapter = (uint32_t)adapter;
    return CELL_OK;
}

/* ---- adapters: the game polls them once per frame ------------------------------------------- */

uint64_t h_sound_init(uint64_t self, uint64_t funcs, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("SoundAdapterInitialize(0x%llx funcs=0x%llx arg=0x%llx): makeup 0x%x cleanup 0x%x formatChanged 0x%x",
          (unsigned long long)self, (unsigned long long)funcs, (unsigned long long)arg, cb_of((uint32_t)funcs, 0),
          cb_of((uint32_t)funcs, 1), cb_of((uint32_t)funcs, 2));
    std::lock_guard<std::mutex> g(g_mtx);
    g_sound[(uint32_t)self] = make_adapter((uint32_t)funcs, (uint32_t)arg, 3);
    return CELL_OK;
}

uint64_t h_gfx_init(uint64_t self, uint64_t funcs, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    TRACE("GraphicsAdapterInitialize(0x%llx funcs=0x%llx arg=0x%llx): makeup 0x%x cleanup 0x%x formatChanged 0x%x alloc 0x%x free 0x%x",
          (unsigned long long)self, (unsigned long long)funcs, (unsigned long long)arg, cb_of((uint32_t)funcs, 0),
          cb_of((uint32_t)funcs, 1), cb_of((uint32_t)funcs, 2), cb_of((uint32_t)funcs, 3), cb_of((uint32_t)funcs, 4));
    std::lock_guard<std::mutex> g(g_mtx);
    g_gfx[(uint32_t)self] = make_adapter((uint32_t)funcs, (uint32_t)arg, 5);
    return CELL_OK;
}

uint64_t h_sound_finalize(uint64_t self, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    g_sound.erase((uint32_t)self);
    return CELL_OK;
}

uint64_t h_gfx_finalize(uint64_t self, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    g_gfx.erase((uint32_t)self);
    return CELL_OK;
}

uint64_t h_gfx_set_preferred_format(uint64_t self, uint64_t fmt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::lock_guard<std::mutex> g(g_mtx);
    Adapter& a = g_gfx[(uint32_t)self];
    for (uint32_t i = 0; i < sizeof a.format; i++) a.format[i] = vm_base[fmt + i];
    TRACE("GraphicsAdapterSetPreferredFormat: coding %d scan %d bits %d rate %d w %d h %d", (int8_t)a.format[0], (int8_t)a.format[1],
          (int8_t)a.format[2], (int8_t)a.format[3], (int16_t)((a.format[4] << 8) | a.format[5]), (int16_t)((a.format[6] << 8) | a.format[7]));
    return CELL_OK;
}

/* Copies the adapter's format into the game's struct, which has `Bytes` bytes (Adapter::format holds the larger one). */
template <std::map<uint32_t, Adapter>& Table, uint32_t Bytes>
uint64_t h_adapter_get_format(uint64_t self, uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    static_assert(Bytes <= sizeof(Adapter::format), "format buffer too small");
    std::lock_guard<std::mutex> g(g_mtx);
    Adapter& a = Table[(uint32_t)self];
    for (uint32_t i = 0; i < Bytes; i++) vm_base[out + i] = a.format[i];
    return CELL_OK;
}

/* The player whose graphics (or sound) adapter is `adapter`, or null. Call with g_mtx held. */
Player* player_of_gfx(uint32_t adapter)
{
    for (auto& kv : g_players) if (kv.second.gfx_adapter == adapter) return &kv.second;
    return nullptr;
}
Player* player_of_sound(uint32_t adapter)
{
    for (auto& kv : g_players) if (kv.second.sound_adapter == adapter) return &kv.second;
    return nullptr;
}

/* CellSailGraphicsFrameInfo / CellSailSoundFrameInfo: pBuffer, sessionId, tag, status (u32 each), pts (u64). */
void write_frame_info(uint32_t info, uint32_t buffer, uint32_t session, uint32_t tag, uint32_t status, uint64_t pts)
{
    vm_write32(info, buffer);
    vm_write32(info + 4, session);
    vm_write32(info + 8, tag);
    vm_write32(info + 12, status);
    wbe64(info + 16, pts);
}

/* Hands out the picture due now, converted into one of the game's buffers. 0: no picture. */
uint64_t h_gfx_get_frame(uint64_t self, uint64_t info, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::shared_ptr<Movie> mv;
    std::lock_guard<std::mutex> out(g_output_mtx);   /* the picture buffers stay allocated while this runs */
    uint32_t frame = 0, session = 0, pitch = 0, slot = 0;
    int64_t now_us = 0;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        Player* p = player_of_gfx((uint32_t)self);
        if (p && p->movie && p->pic_n > 0 && p->running) {
            mv = p->movie;
            slot = (uint32_t)(p->pic_next % p->pic_n);
            frame = p->pic_buf[slot];
            session = p->session;
            pitch = p->pic_pitch;
            now_us = video_clock_us(*p);
        }
    }
    int64_t pts = 0;
    if (mv && frame && mv->pop_video_rgba(now_us, vm_base + frame, (int)pitch, &pts)) {
        {
            std::lock_guard<std::mutex> g(g_mtx);
            if (Player* p = player_of_gfx((uint32_t)self)) { p->pic_next++; p->pic_last = (int)slot; p->pic_last_pts = (uint64_t)(pts * 9 / 100); }
        }
        static int n = 0;
        if (n++ < 5 || n % 150 == 0) TRACE("gfx GetFrame #%d -> buffer 0x%x pts %lld us at clock %lld us", n, frame, (long long)pts, (long long)now_us);
        write_frame_info((uint32_t)info, frame, session, slot + 1, kFrameNext, (uint64_t)(pts * 9 / 100));
        return kFrameNext;
    }
    /* Nothing newer is due: show the last picture again (the game polls faster than the movie's frame rate and
     * draws nothing when told there is no frame). */
    {
        std::lock_guard<std::mutex> g(g_mtx);
        Player* p = player_of_gfx((uint32_t)self);
        if (p && p->movie && p->pic_last >= 0) {   /* also while paused or stopped: the screen must not go blank */
            write_frame_info((uint32_t)info, p->pic_buf[p->pic_last], p->session, (uint32_t)p->pic_last + 1, kFrameRepeated, p->pic_last_pts);
            return kFrameRepeated;
        }
    }
    write_frame_info((uint32_t)info, 0, session, 0, 0, 0);
    return 0;
}

/* Hands out `samples` frames of PCM, never running ahead of the stream clock by more than kAudioLeadUs. */
uint64_t h_sound_get_frame(uint64_t self, uint64_t samples, uint64_t info, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::shared_ptr<Movie> mv;
    std::lock_guard<std::mutex> out(g_output_mtx);   /* the PCM buffers stay allocated while this runs */
    uint32_t session = 0, scratch = 0, slot = 0, rate = 0;
    int channels = 0;
    uint64_t delivered = 0;
    int64_t now_us = 0;
    {
        std::lock_guard<std::mutex> g(g_mtx);
        Player* p = player_of_sound((uint32_t)self);
        if (p && p->movie && p->movie->has_audio() && p->running && p->scratch) {
            mv = p->movie;
            session = p->session; scratch = p->scratch; channels = p->pcm_channels; rate = p->pcm_rate;
            slot = (uint32_t)(p->pcm_next % kPcmBuffers);
            delivered = p->pcm_delivered;
            now_us = clock_us(*p);
        }
    }
    if (!mv || samples == 0 || samples > kMaxPcmFrames) {
        write_frame_info((uint32_t)info, 0, session, 0, 0, 0);
        return samples > kMaxPcmFrames ? CELL_SAIL_ERROR_INVALID_ARG : 0;
    }
    const int64_t played_us = (int64_t)(delivered * 1'000'000ULL / rate);
    const size_t avail = mv->audio_available();
    const bool last = mv->audio_finished();
    if (played_us > now_us + kAudioLeadUs || avail == 0 || (avail < samples && !last)) {
        write_frame_info((uint32_t)info, 0, session, 0, 0, 0);
        return 0;
    }
    std::vector<float> pcm((size_t)samples * (size_t)channels, 0.0f);
    int64_t pts = 0;
    mv->read_audio(pcm.data(), (size_t)samples, &pts);
    const uint32_t buf = scratch + 0x80 + slot * (kMaxPcmFrames * (uint32_t)channels * 4);
    for (size_t i = 0; i < pcm.size(); i++) {
        uint32_t bits;
        std::memcpy(&bits, &pcm[i], 4);
        vm_write32(buf + (uint32_t)i * 4, bits);
    }
    {
        std::lock_guard<std::mutex> g(g_mtx);
        if (Player* p = player_of_sound((uint32_t)self)) { p->pcm_next++; p->pcm_delivered += samples; p->audio_at_us = clock_us(*p); }
    }
    static int n = 0;
    if (n++ < 5 || n % 400 == 0) TRACE("sound GetFrame(%llu) -> buffer 0x%x pts %lld us at clock %lld us", (unsigned long long)samples, buf, (long long)pts, (long long)now_us);
    write_frame_info((uint32_t)info, buf, session, slot + 1, kFrameNext, (uint64_t)(pts * 9 / 100));
    return kFrameNext;
}

/* TODO: convert the PTS (90 kHz) into the stream's time position; every position reads as 0 for now. */
uint64_t h_adapter_pts_to_time(uint64_t, uint64_t, uint64_t out, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    if (out) { vm_write32(out, 0); vm_write32(out + 4, 0); }
    return CELL_OK;
}

/* The graphics adapter's AV sync is the game's per-frame beat while a movie is up: when the movie has been
 * played out, end the stream so the movie scene moves on. */
uint64_t h_gfx_update_av_sync(uint64_t self, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    uint32_t player = 0;
    {
        static int n = 0;
        if (n++ < 3) TRACE("gfx UpdateAvSync(0x%llx)", (unsigned long long)self);
    }
    {
        std::lock_guard<std::mutex> g(g_mtx);
        for (auto& kv : g_players) {
            Player& p = kv.second;
            if (p.gfx_adapter != (uint32_t)self || p.state != ST_RUNNING || p.eos_sent) continue;
            bool done;
            if (p.movie) {
                const int64_t audio_end = p.pcm_rate ? (int64_t)(p.pcm_delivered * 1'000'000ULL / p.pcm_rate) : 0;
                const int64_t end = std::max(p.movie->video_end_us(), audio_end);
                done = p.movie->finished() && video_clock_us(p) >= end + 200'000;
            } else {
                done = ++p.frames_running >= kFramesBeforeEosFallback;
            }
            if (done) { p.eos_sent = true; player = kv.first; }
        }
    }
    if (player) {
        /* At the end of the stream the player stops itself and falls back to OPENED; the game's
         * movie scene then closes the stream, which is what ends the scene. */
        TRACE("end of stream");
        { std::lock_guard<std::mutex> g(g_mtx); clock_run(g_players[player], false); }
        deliver(player, EV_SOURCE_EOS, 0, 0, 0);
        set_state(player, ST_STOP_TRANSITION);
        set_state(player, ST_OPENED);
    }
    return CELL_OK;
}

uint64_t h_sound_update_av_sync(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    return CELL_OK;
}

}  // namespace

extern "C" void sc2hd_sail_register(void)
{
    struct E { uint32_t nid; const char* name; void* fn; };
    typedef uint64_t (*H)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    static const E table[] = {
        { 0x346EBBA3u, "cellSailMemAllocatorInitialize",         (void*)(H)h_alloc_init },
        { 0x4CC54F8Eu, "cellSailFutureInitialize",               (void*)(H)h_future_initialize },
        { 0xA37FED15u, "cellSailFutureSet",                      (void*)(H)h_future_set },
        { 0x3A2D806Cu, "cellSailFutureGet",                      (void*)(H)h_future_get },
        { 0x23654375u, "cellSailPlayerInitialize2",              (void*)(H)h_player_initialize2 },
        { 0x18B4629Du, "cellSailPlayerFinalize",                 (void*)(H)h_player_finalize },
        { 0x47632810u, "cellSailPlayerReplaceEventHandler",      (void*)(H)h_player_replace_handler },
        { 0xBDF21B0Fu, "cellSailPlayerBoot",                     (void*)(H)h_player_boot },
        { 0xD7938B8Du, "cellSailPlayerCreateDescriptor",         (void*)(H)h_player_create_descriptor },
        { 0x7C8DFF3Bu, "cellSailPlayerAddDescriptor",            (void*)(H)h_player_add_descriptor },
        { 0xFC839BD4u, "cellSailPlayerDestroyDescriptor",        (void*)(H)h_player_destroy_descriptor },
        { 0x752F8585u, "cellSailPlayerGetDescriptorCount",       (void*)(H)h_player_get_descriptor_count },
        { 0x4C191088u, "cellSailDescriptorGetUri",               (void*)(H)h_descriptor_get_uri },
        { 0x34ECC1B9u, "cellSailPlayerOpenStream",               (void*)(H)h_player_open_stream },
        { 0x85BEFFCCu, "cellSailPlayerCloseStream",              (void*)(H)h_player_close_stream },
        { 0xE535B0D3u, "cellSailPlayerStart",                    (void*)(H)h_player_start },
        { 0xEBA8D4ECu, "cellSailPlayerStop",                     (void*)(H)h_player_stop },
        { 0xD1D55A90u, "cellSailPlayerSetPaused",                (void*)(H)h_player_set_paused },
        { 0x1139A206u, "cellSailPlayerSetSoundAdapter",          (void*)(H)h_player_set_adapter_sound },
        { 0x18BCD21Bu, "cellSailPlayerSetGraphicsAdapter",       (void*)(H)h_player_set_adapter_gfx },
        { 0x3D0D3B72u, "cellSailSoundAdapterInitialize",         (void*)(H)h_sound_init },
        { 0x1C983864u, "cellSailGraphicsAdapterInitialize",      (void*)(H)h_gfx_init },
        { 0xD1462438u, "cellSailSoundAdapterFinalize",           (void*)(H)h_sound_finalize },
        { 0x76488BB1u, "cellSailGraphicsAdapterFinalize",        (void*)(H)h_gfx_finalize },
        { 0x2E3CCB5Eu, "cellSailGraphicsAdapterSetPreferredFormat", (void*)(H)h_gfx_set_preferred_format },
        { 0xF25F197Du, "cellSailSoundAdapterGetFormat",          (void*)(H)h_adapter_get_format<g_sound, kAudioFormatBytes> },
        { 0xFFD58AA4u, "cellSailGraphicsAdapterGetFormat",       (void*)(H)h_adapter_get_format<g_gfx, kVideoFormatBytes> },
        { 0x7EB8D6B5u, "cellSailSoundAdapterGetFrame",           (void*)(H)h_sound_get_frame },
        { 0x0247C69Eu, "cellSailGraphicsAdapterGetFrame",        (void*)(H)h_gfx_get_frame },
        { 0x4AE979DFu, "cellSailSoundAdapterPtsToTimePosition",  (void*)(H)h_adapter_pts_to_time },
        { 0x1872331Bu, "cellSailGraphicsAdapterPtsToTimePosition", (void*)(H)h_adapter_pts_to_time },
        { 0xEEC22809u, "cellSailSoundAdapterUpdateAvSync",       (void*)(H)h_sound_update_av_sync },
        { 0x44A20E79u, "cellSailGraphicsAdapterUpdateAvSync",    (void*)(H)h_gfx_update_av_sync },
    };
    for (const E& e : table) ps3_hle_register(e.nid, e.name, e.fn);
}
