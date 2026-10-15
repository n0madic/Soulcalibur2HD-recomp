/*
 * Compact H.264/AVC decoder: High profile and below, CABAC, progressive frames, 4:2:0, 8 bit. Portable C99.
 *
 * It decodes what the game's movies use (and what most encoders produce for plain progressive video): I, P and B
 * slices, multiple slices and reference frames, reference list modification, memory management operations,
 * explicit and implicit weighted prediction, spatial and temporal direct prediction, the 8x8 transform and the
 * in-loop deblocking filter. Not supported (reported as an error): CAVLC, interlaced and MBAFF streams, slice
 * groups, scaling matrices, other chroma formats and bit depths, picture order count type 1.
 *
 * Pictures come out in decoding order; putting them into display order is the caller's job (the container's
 * timestamps do that).
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SC2HD_H264_H
#define SC2HD_H264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_dec h264_dec;

typedef struct {
    const uint8_t *plane[3];   /* Y, Cb, Cr; valid until the next h264_decode() or h264_close() */
    int stride[3];
    int width, height;         /* cropped luma size; the chroma planes are half of it */
    int full_range;            /* VUI video_full_range_flag */
    int matrix;                /* VUI matrix_coefficients (1 = BT.709, 5/6 = BT.601, 2 = unspecified) */
} h264_frame;

/* `avcc` is the AVCDecoderConfigurationRecord of an MP4 file (carries the SPS and PPS and the NAL length size).
 * Returns NULL if it is malformed or the stream is not supported. */
h264_dec *h264_open(const uint8_t *avcc, size_t size);
void h264_close(h264_dec *d);

/* Decodes one access unit in length-prefixed (MP4) form. Returns 1 and fills `out` if it completed a picture,
 * 0 if there is nothing to output, a negative value on a broken or unsupported stream. */
int h264_decode(h264_dec *d, const uint8_t *packet, size_t size, h264_frame *out);

/* Size of the output pictures, known after h264_open(). */
int h264_width(const h264_dec *d);
int h264_height(const h264_dec *d);

#ifdef __cplusplus
}
#endif
#endif
