/*
 * Compact MPEG-4 AAC-LC decoder. See aac.h.
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aac.h"
#include "aac_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { ONLY_LONG = 0, LONG_START = 1, EIGHT_SHORT = 2, LONG_STOP = 3 };
enum { ZERO_HCB = 0, ESC_HCB = 11, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15 };

/* ---- bit reader -------------------------------------------------------------------------------------------------- */

typedef struct { const uint8_t *buf; size_t pos, nbits; } br_t;

static unsigned br_get1(br_t *b)
{
    unsigned v = 0;
    if (b->pos < b->nbits) v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static unsigned br_get(br_t *b, int n)
{
    unsigned v = 0;
    while (n-- > 0) v = (v << 1) | br_get1(b);
    return v;
}

/* ---- Huffman trees ------------------------------------------------------------------------------------------------ */

typedef struct { int16_t child[2]; } hnode;   /* >= 0: another node; < 0: leaf, symbol -child - 1; 0 = absent */
typedef struct { hnode *nodes; int count; } htree;

static int htree_build(htree *t, int nsym, const void *codes, int code_is32, const uint8_t *bits)
{
    t->nodes = (hnode *)calloc((size_t)nsym + 1, sizeof(hnode));
    if (!t->nodes) return -1;
    t->count = 1;
    for (int s = 0; s < nsym; s++) {
        const uint32_t code = code_is32 ? ((const uint32_t *)codes)[s] : ((const uint16_t *)codes)[s];
        int node = 0;
        for (int i = bits[s] - 1; i >= 0; i--) {
            const int bit = (code >> i) & 1;
            if (i == 0) { t->nodes[node].child[bit] = (int16_t)(-s - 1); break; }
            if (!t->nodes[node].child[bit]) t->nodes[node].child[bit] = (int16_t)t->count++;
            node = t->nodes[node].child[bit];
        }
    }
    return 0;
}

static int huff(br_t *b, const htree *t)
{
    int node = 0;
    for (int i = 0; i < 24; i++) {
        const int n = t->nodes[node].child[br_get1(b)];
        if (n < 0) return -n - 1;
        if (n == 0) return -1;
        node = n;
    }
    return -1;
}

/* ---- FFT and IMDCT ------------------------------------------------------------------------------------------------ */

typedef struct {
    int m;                    /* number of MDCT coefficients (half the window length) */
    int h;                    /* complex FFT size: m / 2 */
    float *tw_re, *tw_im;     /* FFT twiddles, h / 2 entries */
    float *rot_re, *rot_im;   /* exp(-i pi (k + 1/8) / m), h entries: the pre- and the post-twiddle */
    uint16_t *rev;
} mdct_t;

static int mdct_init(mdct_t *c, int m)
{
    c->m = m;
    c->h = m / 2;
    c->tw_re = (float *)malloc(sizeof(float) * (size_t)c->h);
    c->tw_im = (float *)malloc(sizeof(float) * (size_t)c->h);
    c->rot_re = (float *)malloc(sizeof(float) * (size_t)c->h);
    c->rot_im = (float *)malloc(sizeof(float) * (size_t)c->h);
    c->rev = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)c->h);
    if (!c->tw_re || !c->tw_im || !c->rot_re || !c->rot_im || !c->rev) return -1;
    for (int k = 0; k < c->h / 2; k++) {
        c->tw_re[k] = (float)cos(-2.0 * M_PI * k / c->h);
        c->tw_im[k] = (float)sin(-2.0 * M_PI * k / c->h);
    }
    for (int k = 0; k < c->h; k++) {
        c->rot_re[k] = (float)cos(M_PI * (k + 0.125) / m);
        c->rot_im[k] = (float)-sin(M_PI * (k + 0.125) / m);
    }
    int bits = 0;
    while ((1 << bits) < c->h) bits++;
    for (int i = 0; i < c->h; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        c->rev[i] = (uint16_t)r;
    }
    return 0;
}

static void mdct_free(mdct_t *c)
{
    free(c->tw_re); free(c->tw_im); free(c->rot_re); free(c->rot_im); free(c->rev);
}

/* In place radix-2 FFT of c->h points held as separate re / im arrays. */
static void fft(const mdct_t *c, float *re, float *im)
{
    const int n = c->h;
    for (int i = 0; i < n; i++) {
        const int j = c->rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                const float wr = c->tw_re[k * step], wi = c->tw_im[k * step];
                const int a = i + k, b = a + half;
                const float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
        }
    }
}

/* y[0 .. 2m-1] = sum_k X[k] cos(pi / m * (n + 1/2 + m / 2) * (k + 1/2)), via a DCT-IV of size m. */
static void imdct(const mdct_t *c, const float *X, float *y)
{
    const int m = c->m, h = c->h;
    float re[512], im[512], u[1024];
    for (int k = 0; k < h; k++) {
        const float a = X[2 * k], b = X[m - 1 - 2 * k];
        re[k] = a * c->rot_re[k] - b * c->rot_im[k];
        im[k] = a * c->rot_im[k] + b * c->rot_re[k];
    }
    fft(c, re, im);
    for (int k = 0; k < h; k++) {
        const float xr = re[k] * c->rot_re[k] - im[k] * c->rot_im[k];
        const float xi = re[k] * c->rot_im[k] + im[k] * c->rot_re[k];
        u[2 * k] = xr;
        u[m - 1 - 2 * k] = -xi;
    }
    for (int n = 0; n < m / 2; n++) y[n] = u[n + m / 2];
    for (int n = m / 2; n < 3 * m / 2; n++) y[n] = -u[3 * m / 2 - 1 - n];
    for (int n = 3 * m / 2; n < 2 * m; n++) y[n] = -u[n - 3 * m / 2];
}

/* ---- decoder state ------------------------------------------------------------------------------------------------ */

typedef struct {
    int window_seq, window_shape, max_sfb, num_windows, num_groups;
    uint8_t group_len[8];
    const uint16_t *swb;
    int num_swb, tns_max_bands;
} ics_t;

typedef struct {
    int n_filt[8], length[8][3], order[8][3], direction[8][3];
    float coef[8][3][20];
    int present;
} tns_t;

typedef struct {
    ics_t ics;
    uint8_t bt[8 * 64];
    int sf[8 * 64];
    int q[1024];
    float spec[1024];
    float overlap[1024];
    int prev_shape;
    tns_t tns;
    int ms_used[8 * 64];
} chan_t;

struct aac_dec {
    int channels, rate, rate_idx;
    htree sf_tree, spec_tree[11];
    mdct_t mdct_long, mdct_short;
    float win_long[2][2048], win_short[2][256];   /* [0] sine, [1] Kaiser-Bessel derived */
    float pow43[8192];
    float gain[256 + 64];
    chan_t ch[2];
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static void build_windows(aac_dec *d)
{
    for (int n = 0; n < 2048; n++) d->win_long[0][n] = (float)sin(M_PI * (n + 0.5) / 2048.0);
    for (int n = 0; n < 256; n++) d->win_short[0][n] = (float)sin(M_PI * (n + 0.5) / 256.0);
    for (int kind = 0; kind < 2; kind++) {
        const int half = kind ? 128 : 1024;
        const double alpha = kind ? 6.0 : 4.0;
        double w[1025], total = 0;
        for (int n = 0; n <= half; n++) {
            const double r = 2.0 * n / half - 1.0;
            w[n] = bessel_i0(M_PI * alpha * sqrt(1.0 - r * r));
            total += w[n];
        }
        double acc = 0;
        float *dst = kind ? d->win_short[1] : d->win_long[1];
        for (int n = 0; n < half; n++) {
            acc += w[n];
            dst[n] = dst[2 * half - 1 - n] = (float)sqrt(acc / total);
        }
    }
}

static int read_aot(br_t *b)
{
    const int aot = (int)br_get(b, 5);
    return aot == 31 ? 32 + (int)br_get(b, 6) : aot;
}

aac_dec *aac_open(const uint8_t *asc, size_t size)
{
    if (size < 2) return NULL;
    br_t b = {asc, 0, size * 8};
    int aot = read_aot(&b);
    const int rate_idx = (int)br_get(&b, 4);
    static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    if (rate_idx >= 13) return NULL;
    const int chan_cfg = (int)br_get(&b, 4);
    if (aot == 5 || aot == 29) {                  /* explicit SBR (and PS) signalling: the core is described next */
        if (br_get(&b, 4) == 15) br_get(&b, 24);  /* extensionSamplingFrequency(Index), not used */
        aot = read_aot(&b);
    }
    if (aot != 2 || (chan_cfg != 1 && chan_cfg != 2)) return NULL;   /* TODO: other object types (SBR, PS) and more than two channels */
    if (br_get1(&b)) return NULL;                 /* frame_length_flag: 960 */
    aac_dec *d = (aac_dec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->channels = chan_cfg;
    d->rate = rates[rate_idx];
    d->rate_idx = rate_idx;
    if (htree_build(&d->sf_tree, 121, aac_sf_codes, 1, aac_sf_bits) < 0) goto fail;
    static const struct { const uint16_t *codes; const uint8_t *bits; int n; } cb[11] = {
        {aac_cb1_codes, aac_cb1_bits, 81}, {aac_cb2_codes, aac_cb2_bits, 81}, {aac_cb3_codes, aac_cb3_bits, 81},
        {aac_cb4_codes, aac_cb4_bits, 81}, {aac_cb5_codes, aac_cb5_bits, 81}, {aac_cb6_codes, aac_cb6_bits, 81},
        {aac_cb7_codes, aac_cb7_bits, 64}, {aac_cb8_codes, aac_cb8_bits, 64}, {aac_cb9_codes, aac_cb9_bits, 169},
        {aac_cb10_codes, aac_cb10_bits, 169}, {aac_cb11_codes, aac_cb11_bits, 289}};
    for (int i = 0; i < 11; i++) if (htree_build(&d->spec_tree[i], cb[i].n, cb[i].codes, 0, cb[i].bits) < 0) goto fail;
    if (mdct_init(&d->mdct_long, 1024) < 0 || mdct_init(&d->mdct_short, 128) < 0) goto fail;
    build_windows(d);
    for (int i = 0; i < 8192; i++) d->pow43[i] = (float)pow((double)i, 4.0 / 3.0);
    for (int i = 0; i < 256 + 64; i++) d->gain[i] = (float)pow(2.0, 0.25 * (i - 100));
    return d;
fail:
    aac_close(d);
    return NULL;
}

void aac_close(aac_dec *d)
{
    if (!d) return;
    free(d->sf_tree.nodes);
    for (int i = 0; i < 11; i++) free(d->spec_tree[i].nodes);
    mdct_free(&d->mdct_long);
    mdct_free(&d->mdct_short);
    free(d);
}

int aac_channels(const aac_dec *d) { return d->channels; }
int aac_rate(const aac_dec *d) { return d->rate; }

/* ---- bitstream elements -------------------------------------------------------------------------------------------- */

static int parse_ics_info(aac_dec *d, br_t *b, ics_t *ics)
{
    memset(ics, 0, sizeof *ics);
    br_get1(b);
    ics->window_seq = (int)br_get(b, 2);
    ics->window_shape = (int)br_get1(b);
    if (ics->window_seq == EIGHT_SHORT) {
        ics->max_sfb = (int)br_get(b, 4);
        const unsigned grouping = br_get(b, 7);
        ics->num_windows = 8;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
        for (int i = 0; i < 7; i++) {
            if (grouping & (1u << (6 - i))) ics->group_len[ics->num_groups - 1]++;
            else ics->group_len[ics->num_groups++] = 1;
        }
        ics->swb = aac_swb_128[d->rate_idx];
        ics->num_swb = aac_num_swb_128[d->rate_idx];
        ics->tns_max_bands = aac_tns_max_bands_128[d->rate_idx];
    } else {
        ics->max_sfb = (int)br_get(b, 6);
        if (br_get1(b)) return -2;                /* TODO: predictor_data_present (AAC Main profile, LTP) */
        ics->num_windows = 1;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
        ics->swb = aac_swb_1024[d->rate_idx];
        ics->num_swb = aac_num_swb_1024[d->rate_idx];
        ics->tns_max_bands = aac_tns_max_bands_1024[d->rate_idx];
    }
    return ics->max_sfb <= ics->num_swb ? 0 : -1;
}

static int parse_tns(br_t *b, const ics_t *ics, tns_t *t)
{
    const int is8 = ics->window_seq == EIGHT_SHORT;
    static const float *const maps[4] = {aac_tns_map_0_3, aac_tns_map_0_4, aac_tns_map_1_3, aac_tns_map_1_4};
    for (int w = 0; w < ics->num_windows; w++) {
        t->n_filt[w] = (int)br_get(b, 2 - is8);
        if (!t->n_filt[w]) continue;
        const int res = (int)br_get1(b);
        for (int f = 0; f < t->n_filt[w]; f++) {
            t->length[w][f] = (int)br_get(b, 6 - 2 * is8);
            t->order[w][f] = (int)br_get(b, 5 - 2 * is8);
            if (t->order[w][f] > (is8 ? 7 : 12)) return -1;
            if (!t->order[w][f]) continue;
            t->direction[w][f] = (int)br_get1(b);
            const int compress = (int)br_get1(b);
            const int len = res + 3 - compress;
            for (int i = 0; i < t->order[w][f]; i++) t->coef[w][f][i] = maps[2 * compress + res][br_get(b, len)];
        }
    }
    t->present = 1;
    return 0;
}

/* Reads one individual_channel_stream and dequantises its spectrum into c->spec. */
static int decode_ics(aac_dec *d, br_t *b, chan_t *c, int common_window)
{
    const int global_gain = (int)br_get(b, 8);
    int r;
    if (!common_window && (r = parse_ics_info(d, b, &c->ics)) < 0) return r;
    ics_t *ics = &c->ics;
    memset(&c->tns, 0, sizeof c->tns);
    memset(c->q, 0, sizeof c->q);
    memset(c->spec, 0, sizeof c->spec);
    const int sect_bits = ics->window_seq == EIGHT_SHORT ? 3 : 5, sect_esc = (1 << sect_bits) - 1;

    for (int g = 0; g < ics->num_groups; g++) {
        int k = 0;
        while (k < ics->max_sfb) {
            const int cb = (int)br_get(b, 4);
            if (cb == 12) return -1;
            int len = 0, inc;
            do { inc = (int)br_get(b, sect_bits); len += inc; } while (inc == sect_esc && b->pos <= b->nbits);
            if (k + len > ics->max_sfb) return -1;
            for (int s = k; s < k + len; s++) c->bt[g * 64 + s] = (uint8_t)cb;
            k += len;
        }
    }
    int sf = global_gain;
    for (int g = 0; g < ics->num_groups; g++) {
        for (int s = 0; s < ics->max_sfb; s++) {
            const int cb = c->bt[g * 64 + s];
            if (cb == ZERO_HCB) { c->sf[g * 64 + s] = 0; continue; }
            if (cb == NOISE_HCB || cb == INTENSITY_HCB || cb == INTENSITY_HCB2) return -3;   /* TODO: PNS and intensity stereo (not used by the game's movies) */
            const int delta = huff(b, &d->sf_tree);
            if (delta < 0) return -1;
            sf += delta - 60;
            if (sf < 0 || sf > 255) return -1;
            c->sf[g * 64 + s] = sf;
        }
    }
    int pulse_n = 0, pulse_pos[4], pulse_amp[4];
    if (br_get1(b)) {                              /* pulse_data */
        if (ics->window_seq == EIGHT_SHORT) return -1;
        pulse_n = (int)br_get(b, 2) + 1;
        const int start_sfb = (int)br_get(b, 6);
        if (start_sfb >= ics->num_swb) return -1;
        int pos = ics->swb[start_sfb];
        for (int i = 0; i < pulse_n; i++) {
            pos += (int)br_get(b, 5);
            pulse_pos[i] = pos;
            pulse_amp[i] = (int)br_get(b, 4);
        }
        if (pos >= 1024) return -1;
    }
    if (br_get1(b) && (r = parse_tns(b, ics, &c->tns)) < 0) return r;
    if (br_get1(b)) return -3;                     /* TODO: gain control (SSR profile) */

    /* spectral data */
    int win = 0;
    for (int g = 0; g < ics->num_groups; g++) {
        for (int s = 0; s < ics->max_sfb; s++) {
            const int cb = c->bt[g * 64 + s];
            if (cb == ZERO_HCB) continue;
            const int off = ics->swb[s], width = ics->swb[s + 1] - off;
            const int dim = cb < 5 ? 4 : 2;
            const int is_signed = cb == 1 || cb == 2 || cb == 5 || cb == 6;
            const int base = cb <= 4 ? 3 : cb <= 6 ? 9 : cb <= 8 ? 8 : cb <= 10 ? 13 : 17;
            for (int w = 0; w < ics->group_len[g]; w++) {
                int *q = c->q + (win + w) * (ics->window_seq == EIGHT_SHORT ? 128 : 0) + off;
                for (int k = 0; k < width; k += dim) {
                    int idx = huff(b, &d->spec_tree[cb - 1]);
                    if (idx < 0) return -1;
                    int v[4];
                    for (int i = dim - 1; i >= 0; i--) { v[i] = idx % base; idx /= base; }
                    if (is_signed) {
                        const int bias = base == 3 ? 1 : 4;
                        for (int i = 0; i < dim; i++) v[i] -= bias;
                    } else {
                        for (int i = 0; i < dim; i++) if (v[i] && br_get1(b)) v[i] = -v[i];
                        if (cb == ESC_HCB) {
                            for (int i = 0; i < 2; i++) {
                                if (abs(v[i]) != 16) continue;
                                int n = 4;
                                while (br_get1(b)) { if (++n > 12) return -1; }
                                const int esc = (1 << n) + (int)br_get(b, n);
                                v[i] = v[i] < 0 ? -esc : esc;
                            }
                        }
                    }
                    for (int i = 0; i < dim; i++) q[k + i] = v[i];
                }
            }
        }
        win += ics->group_len[g];
    }
    for (int i = 0; i < pulse_n; i++) c->q[pulse_pos[i]] += c->q[pulse_pos[i]] > 0 ? pulse_amp[i] : -pulse_amp[i];
    if (b->pos > b->nbits) return -1;

    /* dequantisation */
    win = 0;
    for (int g = 0; g < ics->num_groups; g++) {
        for (int s = 0; s < ics->max_sfb; s++) {
            if (c->bt[g * 64 + s] == ZERO_HCB) continue;
            const float gain = d->gain[c->sf[g * 64 + s]];
            for (int w = 0; w < ics->group_len[g]; w++) {
                const int base = (win + w) * (ics->window_seq == EIGHT_SHORT ? 128 : 0);
                for (int i = ics->swb[s]; i < ics->swb[s + 1]; i++) {
                    const int q = c->q[base + i], a = abs(q) < 8192 ? abs(q) : 8191;
                    c->spec[base + i] = (q < 0 ? -d->pow43[a] : d->pow43[a]) * gain;
                }
            }
        }
        win += ics->group_len[g];
    }
    return 0;
}

static void apply_tns(chan_t *c)
{
    const ics_t *ics = &c->ics;
    const int mmm = ics->tns_max_bands < ics->max_sfb ? ics->tns_max_bands : ics->max_sfb;
    if (!c->tns.present || !mmm) return;
    for (int w = 0; w < ics->num_windows; w++) {
        int bottom = ics->num_swb;
        for (int f = 0; f < c->tns.n_filt[w]; f++) {
            const int top = bottom;
            bottom = top - c->tns.length[w][f] > 0 ? top - c->tns.length[w][f] : 0;
            const int order = c->tns.order[w][f];
            if (!order) continue;
            float lpc[20];
            for (int i = 0; i < order; i++) {
                const float r = -c->tns.coef[w][f][i];
                lpc[i] = r;
                for (int j = 0; j < (i + 1) >> 1; j++) {
                    const float a = lpc[j], bb = lpc[i - 1 - j];
                    lpc[j] = a + r * bb;
                    lpc[i - 1 - j] = bb + r * a;
                }
            }
            int start = ics->swb[bottom < mmm ? bottom : mmm], end = ics->swb[top < mmm ? top : mmm];
            const int size = end - start;
            if (size <= 0) continue;
            int inc = 1;
            if (c->tns.direction[w][f]) { inc = -1; start = end - 1; }
            start += w * 128;
            for (int m = 0; m < size; m++, start += inc)
                for (int i = 1; i <= (m < order ? m : order); i++) c->spec[start] -= c->spec[start - i * inc] * lpc[i - 1];
        }
    }
}

/* Inverse transform, windowing and overlap-add of one channel: writes 1024 samples. */
static void synthesize(aac_dec *d, chan_t *c, float *out, int stride)
{
    const ics_t *ics = &c->ics;
    float buf[2048], y[2048];
    const float scale_long = 1.0f / (1024.0f * 32768.0f), scale_short = 1.0f / (128.0f * 32768.0f);
    const int prev = c->prev_shape, cur = ics->window_shape;
    memset(buf, 0, sizeof buf);
    if (ics->window_seq == EIGHT_SHORT) {
        for (int w = 0; w < 8; w++) {
            imdct(&d->mdct_short, c->spec + w * 128, y);
            const float *wl = d->win_short[w == 0 ? prev : cur], *wr = d->win_short[cur];
            for (int n = 0; n < 128; n++) buf[448 + 128 * w + n] += y[n] * wl[n] * scale_short;
            for (int n = 128; n < 256; n++) buf[448 + 128 * w + n] += y[n] * wr[n] * scale_short;
        }
    } else {
        imdct(&d->mdct_long, c->spec, y);
        const float *ll = d->win_long[prev], *lr = d->win_long[cur];
        for (int n = 0; n < 1024; n++) {
            float wl, wr;
            if (ics->window_seq == LONG_STOP) wl = n < 448 ? 0.0f : n < 576 ? d->win_short[prev][n - 448] : 1.0f;
            else wl = ll[n];
            if (ics->window_seq == LONG_START) wr = n < 448 ? 1.0f : n < 576 ? d->win_short[cur][128 + n - 448] : 0.0f;
            else wr = lr[1024 + n];
            buf[n] = y[n] * wl * scale_long;
            buf[1024 + n] = y[1024 + n] * wr * scale_long;
        }
    }
    for (int n = 0; n < 1024; n++) {
        out[n * stride] = buf[n] + c->overlap[n];
        c->overlap[n] = buf[1024 + n];
    }
    c->prev_shape = cur;
}

int aac_decode(aac_dec *d, const uint8_t *data, size_t size, float *out)
{
    br_t b = {data, 0, size * 8};
    int nch = 0, ret = 0, got[2] = {0, 0};
    for (;;) {
        if (b.pos + 3 > b.nbits) break;
        const int id = (int)br_get(&b, 3);
        if (id == 7) break;
        if (id == 0 || id == 3) {                  /* SCE, LFE */
            br_get(&b, 4);
            if (nch >= d->channels) return -1;
            if ((ret = decode_ics(d, &b, &d->ch[nch], 0)) < 0) return ret;
            got[nch++] = 1;
        } else if (id == 1) {                      /* CPE */
            br_get(&b, 4);
            if (d->channels != 2 || nch != 0) return -1;
            const int common = (int)br_get1(&b);
            int ms_mode = 0;
            if (common) {
                if ((ret = parse_ics_info(d, &b, &d->ch[0].ics)) < 0) return ret;
                d->ch[1].ics = d->ch[0].ics;
                ms_mode = (int)br_get(&b, 2);
                if (ms_mode == 3) return -1;
                if (ms_mode == 1) {
                    for (int g = 0; g < d->ch[0].ics.num_groups; g++)
                        for (int s = 0; s < d->ch[0].ics.max_sfb; s++) d->ch[0].ms_used[g * 64 + s] = (int)br_get1(&b);
                } else if (ms_mode == 2) {
                    for (int i = 0; i < 8 * 64; i++) d->ch[0].ms_used[i] = 1;
                }
            }
            if ((ret = decode_ics(d, &b, &d->ch[0], common)) < 0) return ret;
            if ((ret = decode_ics(d, &b, &d->ch[1], common)) < 0) return ret;
            if (ms_mode) {
                const ics_t *ics = &d->ch[0].ics;
                int win = 0;
                for (int g = 0; g < ics->num_groups; g++) {
                    for (int s = 0; s < ics->max_sfb; s++) {
                        if (!d->ch[0].ms_used[g * 64 + s]) continue;
                        for (int w = 0; w < ics->group_len[g]; w++) {
                            const int base = (win + w) * (ics->window_seq == EIGHT_SHORT ? 128 : 0);
                            for (int i = ics->swb[s]; i < ics->swb[s + 1]; i++) {
                                const float m = d->ch[0].spec[base + i], sd = d->ch[1].spec[base + i];
                                d->ch[0].spec[base + i] = m + sd;
                                d->ch[1].spec[base + i] = m - sd;
                            }
                        }
                    }
                    win += ics->group_len[g];
                }
            }
            got[0] = got[1] = 1;
            nch = 2;
        } else if (id == 4) {                      /* DSE */
            br_get(&b, 4);
            const int align = (int)br_get1(&b);
            int cnt = (int)br_get(&b, 8);
            if (cnt == 255) cnt += (int)br_get(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~(size_t)7;
            b.pos += (size_t)cnt * 8;
        } else if (id == 6) {                      /* FIL */
            int cnt = (int)br_get(&b, 4);
            if (cnt == 15) cnt += (int)br_get(&b, 8) - 1;
            b.pos += (size_t)cnt * 8;
        } else {
            return -3;                             /* TODO: program config and coupling channel elements */
        }
        if (b.pos > b.nbits) return -1;
    }
    for (int c = 0; c < d->channels; c++) {
        if (!got[c]) {
            for (int n = 0; n < AAC_FRAME_SAMPLES; n++) out[n * d->channels + c] = 0.0f;
            continue;
        }
        apply_tns(&d->ch[c]);
        synthesize(d, &d->ch[c], out + c, d->channels);
    }
    return AAC_FRAME_SAMPLES;
}
