/*
 * Minimal ISO base media file (MP4) demuxer: the sample tables of the video and audio tracks, random access reads.
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SC2HD_MP4_H
#define SC2HD_MP4_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MP4_VIDEO = 1, MP4_AUDIO = 2 };

typedef struct {
    int type;                    /* MP4_VIDEO, MP4_AUDIO or 0 for anything else */
    uint32_t fourcc;             /* sample entry: 'avc1', 'mp4a', ... */
    uint32_t timescale;
    uint32_t nsamples;
    uint64_t *offset;            /* per sample: file offset */
    uint32_t *size;
    int64_t *dts, *cts;          /* in timescale units; cts is already shifted by the edit list */
    uint8_t *sync;               /* per sample: 1 for a random access point (all 1 when the file has no sync table) */
    uint8_t *extra;              /* avcC record (video) or AudioSpecificConfig (audio) */
    size_t extra_size;
    int width, height;           /* video */
    int channels, rate;          /* audio, from the sample entry */
} mp4_track;

typedef struct mp4 mp4;

mp4 *mp4_open(const char *path);
void mp4_close(mp4 *m);
const mp4_track *mp4_find(const mp4 *m, int type);     /* first track of that type or NULL */
/* Reads sample `idx` of the track into buf (capacity cap). Returns its size, or -1. */
int mp4_read(mp4 *m, const mp4_track *t, uint32_t idx, uint8_t *buf, size_t cap);
/* Size of the largest sample of the track. */
uint32_t mp4_max_sample(const mp4_track *t);
/* The largest number of samples decoded before a sample that is presented after it (the reorder depth). */
int mp4_reorder_depth(const mp4_track *t);

#ifdef __cplusplus
}
#endif
#endif
