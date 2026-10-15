/*
 * Compact MPEG-4 AAC-LC decoder (mono and stereo, 1024-sample frames). Portable C99.
 *
 * Not supported (reported as an error): main profile prediction, LTP, SSR, gain control, coupling channels,
 * intensity stereo, perceptual noise substitution, SBR/PS, more than two channels. HE-AAC streams (SBR/PS signalled
 * implicitly or explicitly) decode to their AAC-LC core: half the rate, and mono for PS.
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SC2HD_AAC_H
#define SC2HD_AAC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aac_dec aac_dec;

#define AAC_FRAME_SAMPLES 1024

/* `asc` is the AudioSpecificConfig (the MP4 'esds' decoder specific info). NULL if unsupported. */
aac_dec *aac_open(const uint8_t *asc, size_t size);
void aac_close(aac_dec *d);
int aac_channels(const aac_dec *d);
int aac_rate(const aac_dec *d);

/* Decodes one raw_data_block (an MP4 sample) into AAC_FRAME_SAMPLES interleaved float samples per channel,
 * full scale = +-1.0. Returns the samples per channel, or a negative value on error. */
int aac_decode(aac_dec *d, const uint8_t *data, size_t size, float *out);

#ifdef __cplusplus
}
#endif
#endif
