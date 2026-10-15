/*
 * Soulcalibur II HD Online -- MP3 decoding for MultiStream without Sony's flashMP3.pic.
 *
 * The game's audio engine (MultiStream) streams its music as MPEG-1 layer III. Its SPU program loads
 * Sony's decoder, flashMP3.pic (firmware), into local store at 0x16F00 + vaddr and calls it once per
 * frame. The decoder is mpg123 compiled for the SPU; this file is a host replacement for the one
 * function MultiStream calls, found by tracing the real decoder next to a reference decoder:
 *
 *   int decodeFrame(const u8 *frame, int *used, int *pcm_bytes, u8 *heap,
 *                   float *old_block0, float *old_block1, float *buffs0, float *buffs1,
 *                   u8 *backup, u32 *buffer_loc)                          // LS 0x16F00 + 0x74A0
 *
 *   frame        LS address of one complete frame (header first), 128-byte aligned.
 *   used         out, word 0: bytes of the frame (0x180 for 128 kbit/s at 48 kHz).
 *   pcm_bytes    out, word 0: bytes of PCM produced (0x900).
 *   heap         22528-byte scratch; the PCM is left at heap + 0x900 and that address is returned
 *                (-1 on a frame it cannot decode, with both outs zero).
 *   old_block*, buffs*, backup
 *                the voice's decoder state, which MultiStream DMAs in before and out after every call
 *                (two 4992-byte blocks, LS 0x30780 and 0x31B00): overlap-add tails, synthesis history,
 *                bit reservoir. Their layout is the decoder's own business, so here they just hold a
 *                serialized dr_mp3 state.
 *   buffer_loc   in/out word inside the voice's 0x30-byte state block; the real decoder lowers it by
 *                4 (mod 16) per frame. Nothing else reads it; kept for fidelity.
 *
 * The output is one 1152-sample buffer per channel of signed 16-bit big-endian PCM: channel 0 (left, after
 * mid/side decoding) at heap + 0x900 and channel 1 at heap + 0x1900. A stereo stream needs both: MultiStream
 * reads the second buffer too, and leaving it unwritten plays stale data as constant hiss. For a mono
 * frame the second buffer gets a copy of channel 0. Verified sample by sample against the real decoder
 * (MP3_HLE=compare): within 2 LSB on both channels.
 *
 * MP3_HLE=on (default) decodes on the host, MP3_HLE=off lets the real .pic run on the SPU
 * interpreter, MP3_HLE=compare runs both and reports the difference.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "third_party/dr_mp3_core.h"

uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa);

#define MS_IMAGE_ID  3          /* MultiStream's SPU program (spu_0002_at_0042AD00) */
#define PIC_LS_BASE  0x16F00u   /* where it maps the .pic: LS = base + vaddr */
#define PIC_DECODE   (PIC_LS_BASE + 0x74A0u)   /* the .pic's entry point, decodeFrame */

#define PCM_OFFSET        0x900u    /* PCM output follows the 2304-byte bitstream window in the heap */
#define PCM_RIGHT_OFFSET 0x1000u   /* channel 1 of a stereo frame, relative to the channel 0 buffer */
#define STATE_BLOCK_BYTES 0x1380u   /* one of the two per-voice state blocks */
#define MAX_PCM_SAMPLES   1152

/* Decoder state kept between frames, serialized into the voice's state blocks. */
#define STATE_MAGIC 0x53433232u   /* "SC22" */
typedef struct {
    uint32_t magic;
    int32_t  reserv, free_format_bytes;
    uint8_t  header[4];
    uint8_t  reserv_buf[512];
    float    mdct_overlap[2][9 * 32];
    float    qmf_state[15 * 2 * 32];
} mp3_state;

_Static_assert(sizeof(mp3_state) <= 2 * STATE_BLOCK_BYTES, "decoder state must fit the voice's state blocks");

typedef struct {
    int     ok;
    int     used;                       /* bytes of the frame */
    int     samples;                    /* mono samples produced */
    int16_t pcm[MAX_PCM_SAMPLES];       /* channel 0 */
    int16_t pcm_r[MAX_PCM_SAMPLES];     /* channel 1 of a stereo frame, else a copy of channel 0 */
} frame_result;

static uint32_t ls_be32(const spu_context* c, uint32_t a) {
    a &= SPU_LS_MASK;
    return (uint32_t)c->ls[a] << 24 | (uint32_t)c->ls[a + 1] << 16 | (uint32_t)c->ls[a + 2] << 8 | c->ls[a + 3];
}

static void ls_put_be32(spu_context* c, uint32_t a, uint32_t v) {
    a &= SPU_LS_MASK;
    c->ls[a] = (uint8_t)(v >> 24); c->ls[a + 1] = (uint8_t)(v >> 16);
    c->ls[a + 2] = (uint8_t)(v >> 8); c->ls[a + 3] = (uint8_t)v;
}

static void state_load(ma_dr_mp3dec* dec, const mp3_state* s) {
    memset(dec, 0, sizeof *dec);
    if (s->magic != STATE_MAGIC) return;      /* a fresh voice: all zero is the decoder's initial state */
    dec->reserv = s->reserv;
    dec->free_format_bytes = s->free_format_bytes;
    memcpy(dec->header, s->header, sizeof dec->header);
    memcpy(dec->reserv_buf, s->reserv_buf, sizeof dec->reserv_buf);
    memcpy(dec->mdct_overlap, s->mdct_overlap, sizeof dec->mdct_overlap);
    memcpy(dec->qmf_state, s->qmf_state, sizeof dec->qmf_state);
}

static void state_store(mp3_state* s, const ma_dr_mp3dec* dec) {
    memset(s, 0, sizeof *s);
    s->magic = STATE_MAGIC;
    s->reserv = dec->reserv;
    s->free_format_bytes = dec->free_format_bytes;
    memcpy(s->header, dec->header, sizeof s->header);
    memcpy(s->reserv_buf, dec->reserv_buf, sizeof dec->reserv_buf);
    memcpy(s->mdct_overlap, dec->mdct_overlap, sizeof s->mdct_overlap);
    memcpy(s->qmf_state, dec->qmf_state, sizeof s->qmf_state);
}

/* Decode the frame at `frame` (avail bytes readable). The decoder state in `s` advances on success. */
static void decode_frame(mp3_state* s, const uint8_t* frame, uint32_t avail, frame_result* r) {
    ma_dr_mp3dec dec;
    ma_dr_mp3dec_frame_info info;
    int16_t pcm[MA_DR_MP3_MAX_SAMPLES_PER_FRAME];

    r->ok = 0; r->used = 0; r->samples = 0;
    if (avail < MA_DR_MP3_HDR_SIZE || !ma_dr_mp3_hdr_valid(frame)) return;
    state_load(&dec, s);
    memset(pcm, 0, sizeof pcm);     /* a frame that cannot be restored from the reservoir leaves it untouched */
    memset(&info, 0, sizeof info);
    int bytes = ma_dr_mp3_hdr_frame_bytes(frame, dec.free_format_bytes) + ma_dr_mp3_hdr_padding(frame);
    if (bytes <= 0 || (uint32_t)bytes > avail) return;

    /* Exactly one frame is offered, so the decoder takes it without looking for a following header. */
    int samples = ma_dr_mp3dec_decode_frame(&dec, frame, bytes, pcm, &info);
    /* A frame whose main data begins in a reservoir this voice never filled (it started mid-stream) decodes to 0
     * samples, but the decoder accepted it (dec.header holds its header; a rejected frame resets the state) and saved
     * its bytes into the reservoir. It plays as silence and the state advances, so the next frames can be restored. */
    if (samples == 0 && dec.header[0] == 0xFF && info.channels >= 1) samples = (int)ma_dr_mp3_hdr_frame_samples(frame);
    if (samples <= 0 || info.channels < 1 || samples > MAX_PCM_SAMPLES) return;
    for (int i = 0; i < samples; i++) {
        r->pcm[i] = pcm[i * info.channels];                                  /* channel 0 */
        r->pcm_r[i] = pcm[i * info.channels + (info.channels > 1)];
    }
    r->ok = 1; r->used = info.frame_bytes ? info.frame_bytes : bytes; r->samples = samples;
    state_store(s, &dec);
}

/* Arguments of decodeFrame as the SPU ABI passes them: r3..r12, preferred slot. */
typedef struct {
    uint32_t frame, used, pcm_bytes, heap, old0, old1, buffs0, buffs1, backup, loc;
} call_args;

static call_args read_args(const spu_context* c) {
    uint32_t a[10];
    for (int i = 0; i < 10; i++) a[i] = c->gpr[3 + i]._u32[0] & SPU_LS_MASK;
    call_args g = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9] };
    return g;
}

/* The two 4992-byte state blocks sit back to back: buffs0 | old_block0 | backup | buffs1 | old_block1. */
static int layout_ok(const call_args* g) {
    return g->old0 == g->buffs0 + 0x880 && g->backup == g->buffs0 + 0x1180 &&
           g->buffs1 == g->buffs0 + STATE_BLOCK_BYTES && g->old1 == g->buffs1 + 0x880 &&
           g->buffs0 + 2 * STATE_BLOCK_BYTES <= SPU_LS_SIZE;
}

static void return_to_caller(spu_context* c, int32_t r3) {
    for (int i = 0; i < 4; i++) c->gpr[3]._u32[i] = (uint32_t)r3;
    c->pc = c->gpr[0]._u32[0] & SPU_LS_MASK;
    c->status = SPU_STATUS_RUNNING;
}

static void apply_result(spu_context* c, const call_args* g, const frame_result* r) {
    ls_put_be32(c, g->used, 0);
    ls_put_be32(c, g->pcm_bytes, 0);
    if (!r->ok) { return_to_caller(c, -1); return; }
    uint8_t* out = c->ls + ((g->heap + PCM_OFFSET) & SPU_LS_MASK);
    uint8_t* out_r = c->ls + ((g->heap + PCM_OFFSET + PCM_RIGHT_OFFSET) & SPU_LS_MASK);
    for (int i = 0; i < r->samples; i++) {
        out[2 * i] = (uint8_t)((uint16_t)r->pcm[i] >> 8);
        out[2 * i + 1] = (uint8_t)r->pcm[i];
        out_r[2 * i] = (uint8_t)((uint16_t)r->pcm_r[i] >> 8);
        out_r[2 * i + 1] = (uint8_t)r->pcm_r[i];
    }
    ls_put_be32(c, g->used, (uint32_t)r->used);
    ls_put_be32(c, g->pcm_bytes, (uint32_t)r->samples * 2);
    ls_put_be32(c, g->loc, (ls_be32(c, g->loc) + 12) & 15);
    return_to_caller(c, (int32_t)(g->heap + PCM_OFFSET));
}

static mp3_state* voice_state(spu_context* c, const call_args* g) {
    static int warned;
    static mp3_state shared;
    if (layout_ok(g)) return (mp3_state*)(void*)(c->ls + g->buffs0);
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[mp3] unexpected state buffer layout (buffs0=0x%05X); using one shared state\n", g->buffs0);
    }
    return &shared;
}

/* MP3_DUMP=<prefix>: append every frame offered (<prefix>.mp3) and the PCM returned (<prefix>.pcm, big-endian
 * 16-bit mono), to check the decoder against a reference one offline. */
static void dump_frame(const spu_context* c, const call_args* g, const frame_result* r) {
    static FILE *fi, *fo;
    static int tried;
    if (!tried) {
        tried = 1;
        const char* prefix = getenv("MP3_DUMP");
        if (!prefix) return;
        char name[1024];
        snprintf(name, sizeof name, "%s.mp3", prefix); fi = fopen(name, "wb");
        snprintf(name, sizeof name, "%s.pcm", prefix); fo = fopen(name, "wb");
    }
    if (!fi || !fo || !r->ok) return;
    fwrite(c->ls + g->frame, 1, (size_t)r->used, fi);
    fwrite(c->ls + ((g->heap + PCM_OFFSET) & SPU_LS_MASK), 1, (size_t)r->samples * 2, fo);
    fflush(fi); fflush(fo);
}

/* MP3_STATS=1: count the frames offered and refused, and report each distinct header once. */
static void stats_frame(const spu_context* c, const call_args* g, const frame_result* r) {
    static unsigned total, failed;
    static uint32_t seen[32];
    static int nseen;
    const uint8_t* f = c->ls + g->frame;
    uint32_t h = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | f[3];
    h &= 0xFFFFFC0Fu | 0x00000300u;       /* drop padding/private/mode-extension/copyright bits */
    total++;
    if (!r->ok) {
        failed++;
        if (failed <= 20) fprintf(stderr, "[mp3] frame %u refused: header %02X%02X%02X%02X avail %u\n", total,
                                  f[0], f[1], f[2], f[3], SPU_LS_SIZE - g->frame);
    }
    for (int i = 0; i < nseen; i++) if (seen[i] == h) goto counted;
    if (nseen < 32) {
        seen[nseen++] = h;
        fprintf(stderr, "[mp3] header %08X: version %u layer %u bitrate idx %u rate idx %u mode %u, ok=%d\n", h,
                (h >> 19) & 3, (h >> 17) & 3, (h >> 12) & 15, (h >> 10) & 3, (h >> 6) & 3, r->ok);
    }
counted:
    if (total % 1000 == 0) fprintf(stderr, "[mp3] %u frames, %u refused\n", total, failed);
}

static int hook_on(spu_context* c) {
    call_args g = read_args(c);
    frame_result r;
    static int stats = -1;
    if (stats < 0) stats = getenv("MP3_STATS") != NULL;
    decode_frame(voice_state(c, &g), c->ls + g.frame, SPU_LS_SIZE - g.frame, &r);
    if (stats) stats_frame(c, &g, &r);
    apply_result(c, &g, &r);
    dump_frame(c, &g, &r);
    return 1;
}

/* Run the real decoder and ours side by side, on the same input, and report the difference.
 * Voices are told apart by a hash of the real decoder's state blocks: the state a voice leaves
 * is the state it presents on its next call. */
#define COMPARE_VOICES 32
static uint64_t state_hash(const spu_context* c, const call_args* g) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = c->ls + g->buffs0;
    for (uint32_t i = 0; i < 2 * STATE_BLOCK_BYTES; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int hook_compare(spu_context* c) {
    static struct { uint64_t key; uint64_t stamp; mp3_state st; } voices[COMPARE_VOICES];
    static uint64_t clock_, zero_key;
    static unsigned frames, bad, shown, fresh_start;
    static int worst;
    call_args g = read_args(c);
    if (!layout_ok(&g)) return 0;
    if (!zero_key) { uint8_t z[2 * STATE_BLOCK_BYTES] = {0}; spu_context fake; fake.ls = z; call_args f = { 0 }; zero_key = state_hash(&fake, &f); }

    uint64_t pre = state_hash(c, &g);
    int slot = -1, victim = 0, slot_known;
    for (int i = 0; i < COMPARE_VOICES; i++) {
        if (voices[i].key == pre) slot = i;
        if (voices[i].stamp < voices[victim].stamp) victim = i;
    }
    slot_known = slot >= 0 || pre == zero_key;
    mp3_state st;
    memset(&st, 0, sizeof st);
    if (slot >= 0) st = voices[slot].st;
    else if (pre != zero_key) fresh_start++;       /* a voice we have not seen decode: starts mid-stream */

    frame_result mine;
    decode_frame(&st, c->ls + g.frame, SPU_LS_SIZE - g.frame, &mine);

    spu_interp_run(c, c->pc);               /* the real .pic, from LS */

    uint64_t post = state_hash(c, &g);
    if (slot < 0) slot = victim;
    voices[slot].key = post; voices[slot].stamp = ++clock_; voices[slot].st = st;

    int32_t ret = (int32_t)c->gpr[3]._u32[0];
    int diff = 0;
    if (ret == -1 || !mine.ok) {
        diff = (ret == -1) == !mine.ok ? 0 : 99999;
    } else if ((uint32_t)ret != g.heap + PCM_OFFSET || ls_be32(c, g.pcm_bytes) != (uint32_t)mine.samples * 2 ||
               ls_be32(c, g.used) != (uint32_t)mine.used) {
        diff = 99998;
    } else {
        const uint8_t* real = c->ls + (uint32_t)ret;
        for (int i = 0; i < mine.samples; i++) {
            int d = (int16_t)((real[2 * i] << 8) | real[2 * i + 1]) - mine.pcm[i];
            int dr = (int16_t)((real[PCM_RIGHT_OFFSET + 2 * i] << 8) | real[PCM_RIGHT_OFFSET + 2 * i + 1]) - mine.pcm_r[i];
            if (d < 0) d = -d;
            if (dr < 0) dr = -dr;
            if (d > diff) diff = d;
            if (dr > diff) diff = dr;
        }
    }
    {   /* report each distinct header layout once: version/layer/bitrate/rate/mode, not padding or mode extension */
        static uint32_t seen[16]; static int nseen;
        uint32_t h = ls_be32(c, g.frame) & 0xFFFFF0CFu;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == h;
        if (!known && nseen < 16) {
            seen[nseen++] = h;
            fprintf(stderr, "[mp3] new header layout %08X: ret %d, frame bytes %u, pcm bytes %u\n", h, ret,
                    ls_be32(c, g.used), ls_be32(c, g.pcm_bytes));
        }
    }
    {   /* MP3_PROBE=<prefix>: write the whole LS after the real decoder ran, with both channels of our decode */
        static int probes;
        const char* pre_ = getenv("MP3_PROBE");
        if (pre_ && mine.ok && probes < 3 && frames > 300 && (frames % 97) == 0) {
            char name[1024];
            snprintf(name, sizeof name, "%s_%d.ls", pre_, probes);
            FILE* f = fopen(name, "wb"); if (f) { fwrite(c->ls, 1, SPU_LS_SIZE, f); fclose(f); }
            snprintf(name, sizeof name, "%s_%d.pcm", pre_, probes);
            f = fopen(name, "wb");
            if (f) { fwrite(mine.pcm, 2, (size_t)mine.samples, f); fwrite(mine.pcm_r, 2, (size_t)mine.samples, f); fclose(f); }
            fprintf(stderr, "[mp3] probe %d: heap 0x%X ret 0x%X frame 0x%X used 0x%X pcm 0x%X buffs0 0x%X loc 0x%X\n", probes, g.heap, ret, g.frame, g.used, g.pcm_bytes, g.buffs0, g.loc);
            probes++;
        }
    }
    frames++;
    if (diff > worst) worst = diff;
    if (diff > 4) bad++;
    if (diff > 4 && shown++ < 24) {
        int pm = 0, pr = 0;
        if (mine.ok && ret != -1)
            for (int i = 0; i < mine.samples; i++) {
                int m = mine.pcm[i], r = (int16_t)((c->ls[ret + 2 * i] << 8) | c->ls[ret + 2 * i + 1]);
                if (m < 0) m = -m;
                if (r < 0) r = -r;
                if (m > pm) pm = m;
                if (r > pr) pr = r;
            }
        fprintf(stderr, "[mp3] frame %u differs by %d (peak mine %d real %d, hdr %08X, voice %s)\n", frames, diff, pm, pr,
                ls_be32(c, g.frame), slot_known ? "known" : "new");
    }
    if (frames % 500 == 0)
        fprintf(stderr, "[mp3] compare: %u frames, %u differ by more than 4 LSB, worst %d, %u frames of unseen voices\n",
                frames, bad, worst, fresh_start);
    return 1;
}

static int hook_off(spu_context* c) {
    (void)c;
    return 0;       /* not handled: the dispatcher interprets the code in LS */
}

__attribute__((constructor)) static void register_hooks(void) {
    const char* m = getenv("MP3_HLE");
    int (*fn)(spu_context*) = hook_on;
    if (m && (!strcmp(m, "off") || !strcmp(m, "0"))) fn = hook_off;
    else if (m && !strcmp(m, "compare")) fn = hook_compare;
    spu_register_ls_hook(MS_IMAGE_ID, PIC_DECODE, fn);
}
