/*
 * Minimal ISO base media file (MP4) demuxer. See mp4.h.
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp4.h"

#define MAX_TRACKS 8

#ifdef _WIN32
#define SEEK64(f, o) _fseeki64((f), (__int64)(o), SEEK_SET)
#else
#define SEEK64(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#endif

struct mp4 {
    FILE *f;
    mp4_track tracks[MAX_TRACKS];
    int ntracks;
};

typedef struct { const uint8_t *p; size_t n; } span;

static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

/* Iterates the boxes in [p, p + n): returns the payload span and type of the next one, or 0 at the end. */
static int next_box(span *walk, uint32_t *type, span *payload)
{
    if (walk->n < 8) return 0;
    uint64_t size = rd32(walk->p);
    *type = rd32(walk->p + 4);
    size_t hdr = 8;
    if (size == 1) {
        if (walk->n < 16) return 0;
        size = rd64(walk->p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = walk->n;
    }
    if (size < hdr || size > walk->n) return 0;
    payload->p = walk->p + hdr;
    payload->n = (size_t)size - hdr;
    walk->p += size;
    walk->n -= (size_t)size;
    return 1;
}

static int find_box(span in, uint32_t want, span *out)
{
    uint32_t t;
    span w = in, pl;
    while (next_box(&w, &t, &pl)) if (t == want) { *out = pl; return 1; }
    return 0;
}

/* esds: the DecoderSpecificInfo of the ES descriptor. */
static int parse_esds(span s, mp4_track *t)
{
    const uint8_t *p = s.p + 4, *end = s.p + s.n;   /* skip version and flags */
    while (p < end) {
        const int tag = *p++;
        size_t len = 0;
        for (int i = 0; i < 4 && p < end; i++) {
            const int b = *p++;
            len = (len << 7) | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        if (tag == 3) {                 /* ES_Descriptor: ES_ID, flags, optional fields, then nested descriptors */
            if (p + 3 > end) return -1;
            const int flags = p[2];
            p += 3;
            if (flags & 0x80) p += 2;
            if (flags & 0x40) { if (p >= end) return -1; p += 1 + *p; }
            if (flags & 0x20) p += 2;
        } else if (tag == 4) {          /* DecoderConfigDescriptor: 13 bytes, then DecoderSpecificInfo */
            p += 13;
        } else if (tag == 5) {
            if (p + len > end) return -1;
            t->extra = (uint8_t *)malloc(len ? len : 1);
            if (!t->extra) return -1;
            memcpy(t->extra, p, len);
            t->extra_size = len;
            return 0;
        } else {
            p += len;
        }
    }
    return -1;
}

static int parse_stsd(span s, mp4_track *t)
{
    if (s.n < 16) return -1;
    span entries = {s.p + 8, s.n - 8};
    uint32_t type;
    span e;
    if (!next_box(&entries, &type, &e)) return -1;
    t->fourcc = type;
    if (t->type == MP4_VIDEO) {
        if (e.n < 78) return -1;
        t->width = rd16(e.p + 24);
        t->height = rd16(e.p + 26);
        span kids = {e.p + 78, e.n - 78}, c;
        if (find_box(kids, FOURCC('a', 'v', 'c', 'C'), &c)) {
            t->extra = (uint8_t *)malloc(c.n);
            if (!t->extra) return -1;
            memcpy(t->extra, c.p, c.n);
            t->extra_size = c.n;
        }
    } else if (t->type == MP4_AUDIO) {
        if (e.n < 28) return -1;
        t->channels = rd16(e.p + 16);
        t->rate = (int)(rd32(e.p + 24) >> 16);
        span kids = {e.p + 28, e.n - 28}, c;
        if (find_box(kids, FOURCC('e', 's', 'd', 's'), &c)) parse_esds(c, t);
    }
    return 0;
}

static int build_samples(mp4_track *t, span stts, span ctts, span stsc, span stsz, span stco, int co64, span stss, int64_t edit_start)
{
    if (stsz.n < 12 || stsc.n < 8 || stco.n < 8 || stts.n < 8) return -1;
    const uint32_t fixed = rd32(stsz.p + 4), n = rd32(stsz.p + 8);
    if (!fixed && stsz.n < 12 + (size_t)n * 4) return -1;
    t->nsamples = n;
    t->offset = (uint64_t *)calloc(n ? n : 1, sizeof(uint64_t));
    t->size = (uint32_t *)calloc(n ? n : 1, sizeof(uint32_t));
    t->dts = (int64_t *)calloc(n ? n : 1, sizeof(int64_t));
    t->cts = (int64_t *)calloc(n ? n : 1, sizeof(int64_t));
    t->sync = (uint8_t *)malloc(n ? n : 1);
    if (!t->offset || !t->size || !t->dts || !t->cts || !t->sync) return -1;
    for (uint32_t i = 0; i < n; i++) t->size[i] = fixed ? fixed : rd32(stsz.p + 12 + (size_t)i * 4);

    /* chunk layout */
    const uint32_t nchunks = rd32(stco.p + 4), nstsc = rd32(stsc.p + 4);
    if (stco.n < 8 + (size_t)nchunks * (co64 ? 8 : 4) || stsc.n < 8 + (size_t)nstsc * 12) return -1;
    uint32_t sample = 0;
    for (uint32_t c = 0; c < nchunks && sample < n; c++) {
        uint32_t per = 0;
        for (uint32_t e = 0; e < nstsc; e++) if (rd32(stsc.p + 8 + e * 12) <= c + 1) per = rd32(stsc.p + 8 + e * 12 + 4);
        uint64_t off = co64 ? rd64(stco.p + 8 + (size_t)c * 8) : rd32(stco.p + 8 + (size_t)c * 4);
        for (uint32_t k = 0; k < per && sample < n; k++, sample++) {
            t->offset[sample] = off;
            off += t->size[sample];
        }
    }
    /* timestamps */
    uint32_t i = 0;
    int64_t dts = 0;
    const uint32_t nstts = rd32(stts.p + 4);
    for (uint32_t e = 0; e < nstts && 8 + (size_t)(e + 1) * 8 <= stts.n; e++) {
        const uint32_t cnt = rd32(stts.p + 8 + e * 8), delta = rd32(stts.p + 8 + e * 8 + 4);
        for (uint32_t k = 0; k < cnt && i < n; k++, i++) { t->dts[i] = dts; dts += delta; }
    }
    for (i = 0; i < n; i++) t->cts[i] = t->dts[i];
    if (ctts.n >= 8) {
        i = 0;
        const uint32_t nc = rd32(ctts.p + 4);
        for (uint32_t e = 0; e < nc && 8 + (size_t)(e + 1) * 8 <= ctts.n; e++) {
            const uint32_t cnt = rd32(ctts.p + 8 + e * 8);
            const int32_t off = (int32_t)rd32(ctts.p + 8 + e * 8 + 4);
            for (uint32_t k = 0; k < cnt && i < n; k++, i++) t->cts[i] += off;
        }
    }
    for (i = 0; i < n; i++) t->cts[i] -= edit_start;
    memset(t->sync, stss.n >= 8 ? 0 : 1, n);
    if (stss.n >= 8) {
        const uint32_t ns = rd32(stss.p + 4);
        for (uint32_t e = 0; e < ns && 8 + (size_t)(e + 1) * 4 <= stss.n; e++) {
            const uint32_t s = rd32(stss.p + 8 + e * 4);
            if (s >= 1 && s <= n) t->sync[s - 1] = 1;
        }
    }
    return 0;
}

static int parse_trak(span trak, mp4_track *t, uint32_t movie_timescale)
{
    span mdia, mdhd, hdlr, minf, stbl, stsd, stts, ctts = {0, 0}, stsc, stsz, stco, stss = {0, 0}, edts, elst;
    int co64 = 0;
    int64_t edit_start = 0;
    if (!find_box(trak, FOURCC('m', 'd', 'i', 'a'), &mdia)) return 0;
    if (!find_box(mdia, FOURCC('m', 'd', 'h', 'd'), &mdhd) || !find_box(mdia, FOURCC('h', 'd', 'l', 'r'), &hdlr)) return 0;
    if (mdhd.n < 24 || hdlr.n < 12) return 0;
    t->timescale = mdhd.p[0] == 1 ? rd32(mdhd.p + 20) : rd32(mdhd.p + 12);
    const uint32_t kind = rd32(hdlr.p + 8);
    t->type = kind == FOURCC('v', 'i', 'd', 'e') ? MP4_VIDEO : kind == FOURCC('s', 'o', 'u', 'n') ? MP4_AUDIO : 0;
    if (!t->type) return 0;
    if (find_box(trak, FOURCC('e', 'd', 't', 's'), &edts) && find_box(edts, FOURCC('e', 'l', 's', 't'), &elst) && elst.n >= 8) {
        /* Empty edits at the start delay the presentation; the first real edit's media time shifts it back.
         * TODO: only that first real edit is honoured (no later edits, no rates other than 1). */
        const int v1 = elst.p[0] == 1;
        const size_t entry = v1 ? 20 : 12, count = rd32(elst.p + 4);
        int64_t delay = 0, start = 0;
        for (size_t i = 0; i < count && 8 + (i + 1) * entry <= elst.n; i++) {
            const uint8_t *e = elst.p + 8 + i * entry;
            const int64_t dur = v1 ? (int64_t)rd64(e) : (int64_t)rd32(e);
            const int64_t mt = v1 ? (int64_t)rd64(e + 8) : (int32_t)rd32(e + 4);
            if (mt >= 0) { start = mt; break; }
            delay += dur * t->timescale / (movie_timescale ? movie_timescale : 1);
        }
        edit_start = start - delay;
    }
    if (!find_box(mdia, FOURCC('m', 'i', 'n', 'f'), &minf) || !find_box(minf, FOURCC('s', 't', 'b', 'l'), &stbl)) return -1;
    if (!find_box(stbl, FOURCC('s', 't', 's', 'd'), &stsd) || !find_box(stbl, FOURCC('s', 't', 't', 's'), &stts) ||
        !find_box(stbl, FOURCC('s', 't', 's', 'c'), &stsc) || !find_box(stbl, FOURCC('s', 't', 's', 'z'), &stsz))
        return -1;
    if (!find_box(stbl, FOURCC('s', 't', 'c', 'o'), &stco)) {
        if (!find_box(stbl, FOURCC('c', 'o', '6', '4'), &stco)) return -1;
        co64 = 1;
    }
    find_box(stbl, FOURCC('c', 't', 't', 's'), &ctts);
    find_box(stbl, FOURCC('s', 't', 's', 's'), &stss);
    if (parse_stsd(stsd, t) < 0) return -1;
    return build_samples(t, stts, ctts, stsc, stsz, stco, co64, stss, edit_start);
}

mp4 *mp4_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    mp4 *m = (mp4 *)calloc(1, sizeof *m);
    uint8_t hdr[16];
    uint64_t pos = 0;
    uint8_t *moov = NULL;
    size_t moov_size = 0;
    if (!m) goto fail;
    m->f = f;
    for (;;) {
        if (SEEK64(f, pos) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint64_t size = rd32(hdr);
        const uint32_t type = rd32(hdr + 4);
        size_t h = 8;
        if (size == 1) { if (fread(hdr + 8, 1, 8, f) != 8) break; size = rd64(hdr + 8); h = 16; }
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            if (size < h || size > ((uint64_t)1 << 30)) goto fail;
            moov_size = (size_t)(size - h);
            moov = (uint8_t *)malloc(moov_size ? moov_size : 1);
            if (!moov || fread(moov, 1, moov_size, f) != moov_size) goto fail;
            break;
        }
        if (size == 0 || size < h) break;
        pos += size;
    }
    if (!moov) goto fail;
    span walk = {moov, moov_size}, pl, mvhd;
    uint32_t type, movie_timescale = 0;
    if (find_box(walk, FOURCC('m', 'v', 'h', 'd'), &mvhd) && mvhd.n >= 24) movie_timescale = rd32(mvhd.p + (mvhd.p[0] == 1 ? 20 : 12));
    while (next_box(&walk, &type, &pl)) {
        if (type != FOURCC('t', 'r', 'a', 'k') || m->ntracks >= MAX_TRACKS) continue;
        if (parse_trak(pl, &m->tracks[m->ntracks], movie_timescale) < 0) goto fail;
        if (m->tracks[m->ntracks].type) m->ntracks++;
    }
    free(moov);
    return m;
fail:
    free(moov);
    mp4_close(m);
    if (!m) fclose(f);
    return NULL;
}

void mp4_close(mp4 *m)
{
    if (!m) return;
    for (int i = 0; i < MAX_TRACKS; i++) {
        mp4_track *t = &m->tracks[i];
        free(t->offset); free(t->size); free(t->dts); free(t->cts); free(t->sync); free(t->extra);
    }
    if (m->f) fclose(m->f);
    free(m);
}

const mp4_track *mp4_find(const mp4 *m, int type)
{
    for (int i = 0; i < m->ntracks; i++) if (m->tracks[i].type == type) return &m->tracks[i];
    return NULL;
}

int mp4_read(mp4 *m, const mp4_track *t, uint32_t idx, uint8_t *buf, size_t cap)
{
    if (idx >= t->nsamples || t->size[idx] > cap) return -1;
    if (SEEK64(m->f, t->offset[idx]) != 0) return -1;
    if (fread(buf, 1, t->size[idx], m->f) != t->size[idx]) return -1;
    return (int)t->size[idx];
}

uint32_t mp4_max_sample(const mp4_track *t)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < t->nsamples; i++) if (t->size[i] > m) m = t->size[i];
    return m;
}

int mp4_reorder_depth(const mp4_track *t)
{
    int depth = 0;
    for (uint32_t i = 1; i < t->nsamples; i++) {
        int n = 0;
        for (uint32_t j = i > 16 ? i - 16 : 0; j < i; j++) if (t->cts[j] > t->cts[i]) n++;
        if (n > depth) depth = n;
    }
    return depth;
}
