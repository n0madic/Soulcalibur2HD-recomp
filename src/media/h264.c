/*
 * Compact H.264/AVC decoder -- parameter sets, slice headers, reference picture management, the API.
 * The macroblock layer is in h264_mb.c, inter prediction in h264_mc.c, the loop filter in h264_dbf.c.
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "h264_int.h"

/* ---- bit reader -------------------------------------------------------------------------------------------------- */

static void br_init(bitreader *b, const uint8_t *p, size_t n) { b->buf = p; b->pos = 0; b->nbits = n * 8; }

static uint32_t br_get1(bitreader *b)
{
    uint32_t v = 0;
    if (b->pos < b->nbits) v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static uint32_t br_get(bitreader *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0) v = (v << 1) | br_get1(b);
    return v;
}

static uint32_t br_ue(bitreader *b)
{
    int zeros = 0;
    while (!br_get1(b)) {
        if (++zeros > 31 || b->pos > b->nbits) return 0;
    }
    return zeros ? (((uint32_t)1 << zeros) - 1) + br_get(b, zeros) : 0;
}

/* ue(v) as an int that stays above any limit the caller checks: values over `cap` come back as cap + 1, so a huge
 * code word can neither wrap negative nor overflow the arithmetic that follows. */
static int br_ue_cap(bitreader *b, uint32_t cap)
{
    const uint32_t v = br_ue(b);
    return (int)(v > cap ? cap + 1 : v);
}

static int32_t br_se(bitreader *b)
{
    uint32_t k = br_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

/* True while there is payload before the rbsp_trailing_bits. */
static int br_more_data(const bitreader *b)
{
    size_t n = b->nbits >> 3;
    while (n > 0 && b->buf[n - 1] == 0) n--;
    if (n == 0) return 0;
    uint8_t last = b->buf[n - 1];
    int tz = 0;
    while (!((last >> tz) & 1)) tz++;
    return b->pos < (n - 1) * 8 + (7 - tz);
}

/* Copies a NAL unit payload without the emulation prevention bytes. */
static size_t unescape(h264_dec *d, const uint8_t *src, size_t n)
{
    if (d->rbsp_cap < n + 8) {
        uint8_t *p = (uint8_t *)realloc(d->rbsp, n + 64);
        if (!p) return 0;
        d->rbsp = p;
        d->rbsp_cap = n + 64;
    }
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        d->rbsp[o++] = src[i];
        zeros = src[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

/* ---- parameter sets ---------------------------------------------------------------------------------------------- */

static int parse_vui(bitreader *b, h264_sps *s)
{
    s->matrix = 2;
    if (br_get1(b) && br_get(b, 8) == 255) { br_get(b, 16); br_get(b, 16); }   /* aspect ratio */
    if (br_get1(b)) br_get1(b);                                                 /* overscan */
    if (br_get1(b)) {                                                           /* video signal type */
        br_get(b, 3);
        s->full_range = (int)br_get1(b);
        if (br_get1(b)) { br_get(b, 8); br_get(b, 8); s->matrix = (int)br_get(b, 8); }
    }
    return 0;
}

static int parse_sps(h264_dec *d, bitreader *b)
{
    h264_sps s;
    memset(&s, 0, sizeof s);
    s.profile = (int)br_get(b, 8);
    br_get(b, 8);
    s.level = (int)br_get(b, 8);
    uint32_t id = br_ue(b);
    if (id > 31) return -1;
    switch (s.profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135: {
        uint32_t cf = br_ue(b);
        if (cf == 3) br_get1(b);
        uint32_t bdl = br_ue(b), bdc = br_ue(b);
        int bypass = (int)br_get1(b), scaling = (int)br_get1(b);
        /* TODO: other chroma formats and bit depths, lossless (transform bypass), scaling matrices. */
        if (cf != 1 || bdl || bdc || bypass || scaling) return -1;
        break;
    }
    default: break;
    }
    s.log2_max_frame_num = br_ue_cap(b, 12) + 4;
    s.poc_type = br_ue_cap(b, 2);
    if (s.poc_type == 0) s.log2_max_poc_lsb = br_ue_cap(b, 12) + 4;
    else if (s.poc_type != 2) return -1;   /* TODO: picture order count type 1 */
    if (s.log2_max_frame_num > 16 || s.log2_max_poc_lsb > 16) return -1;
    s.max_num_ref_frames = br_ue_cap(b, 16);
    br_get1(b);                                           /* gaps_in_frame_num_value_allowed_flag */
    uint32_t w = br_ue(b) + 1, h = br_ue(b) + 1;
    s.frame_mbs_only = (int)br_get1(b);
    /* TODO: interlaced coding (field pictures, MBAFF). */
    if (!s.frame_mbs_only || w > 512 || h > 512 || w * h > H264_MAX_MBS || s.max_num_ref_frames > 16) return -1;
    s.mb_w = (int)w;
    s.mb_h = (int)h;
    s.direct_8x8_inference = (int)br_get1(b);
    if (br_get1(b)) {                                     /* frame cropping, in units of 2 samples for 4:2:0 */
        s.crop_l = br_ue_cap(b, 8192) * 2;
        s.crop_r = br_ue_cap(b, 8192) * 2;
        s.crop_t = br_ue_cap(b, 8192) * 2;
        s.crop_b = br_ue_cap(b, 8192) * 2;
        if (s.crop_l + s.crop_r >= s.mb_w * 16 || s.crop_t + s.crop_b >= s.mb_h * 16) return -1;
    }
    s.matrix = 2;
    if (br_get1(b)) parse_vui(b, &s);
    s.valid = 1;
    d->sps[id] = s;
    return 0;
}

static int parse_pps(h264_dec *d, bitreader *b)
{
    h264_pps p;
    memset(&p, 0, sizeof p);
    uint32_t id = br_ue(b), sps_id = br_ue(b);
    if (id > 255 || sps_id > 31) return -1;
    p.sps_id = (int)sps_id;
    if (!br_get1(b)) return -1;                           /* TODO: CAVLC (entropy_coding_mode_flag = 0) */
    p.bottom_poc_present = (int)br_get1(b);
    if (br_ue(b) != 0) return -1;                         /* TODO: slice groups (FMO) */
    p.num_ref_default[0] = br_ue_cap(b, 32) + 1;
    p.num_ref_default[1] = br_ue_cap(b, 32) + 1;
    p.weighted_pred = (int)br_get1(b);
    p.weighted_bipred = (int)br_get(b, 2);
    p.init_qp = 26 + br_se(b);
    p.init_qs = 26 + br_se(b);
    p.chroma_qp_offset[0] = br_se(b);
    p.deblocking_control = (int)br_get1(b);
    p.constrained_intra = (int)br_get1(b);
    p.redundant_pic_cnt = (int)br_get1(b);
    p.chroma_qp_offset[1] = p.chroma_qp_offset[0];
    if (br_more_data(b)) {
        p.transform_8x8 = (int)br_get1(b);
        if (br_get1(b)) return -1;                        /* TODO: picture scaling matrices */
        p.chroma_qp_offset[1] = br_se(b);
    }
    if (p.num_ref_default[0] > 32 || p.num_ref_default[1] > 32) return -1;
    p.valid = 1;
    d->pps[id] = p;
    return 0;
}

static int handle_param_nal(h264_dec *d, const uint8_t *nal, size_t n)
{
    int type = nal[0] & 31;
    size_t m = unescape(d, nal + 1, n - 1);
    bitreader b;
    br_init(&b, d->rbsp, m);
    return type == 7 ? parse_sps(d, &b) : parse_pps(d, &b);
}

/* ---- pictures ---------------------------------------------------------------------------------------------------- */

static void free_pic(h264_pic *p)
{
    free(p->buf);
    for (int l = 0; l < 2; l++) { free(p->mv[l]); free(p->refi[l]); free(p->refid[l]); }
    free(p->mbintra);
    memset(p, 0, sizeof *p);
}

static void free_geometry(h264_dec *d)
{
    for (int i = 0; i < H264_MAX_PICS; i++) free_pic(&d->pics[i]);
    free(d->mbi); free(d->nnz_l); free(d->nnz_c[0]); free(d->nnz_c[1]); free(d->ipm); free(d->mvd[0]); free(d->mvd[1]);
    d->mbi = NULL; d->nnz_l = d->nnz_c[0] = d->nnz_c[1] = NULL; d->ipm = NULL; d->mvd[0] = d->mvd[1] = NULL;
    d->cur = NULL;
}

static int setup_geometry(h264_dec *d, const h264_sps *s)
{
    const int npics = h264_clip3(3, H264_MAX_PICS, s->max_num_ref_frames + 2);
    if (d->mb_w == s->mb_w && d->mb_h == s->mb_h && d->npics == npics && d->mbi) return 0;
    free_geometry(d);
    d->npics = npics;
    d->mb_w = s->mb_w;
    d->mb_h = s->mb_h;
    d->nmbs = d->mb_w * d->mb_h;
    d->w4 = d->mb_w * 4;
    d->h4 = d->mb_h * 4;
    d->w8 = d->mb_w * 2;
    const size_t nb4 = (size_t)d->w4 * d->h4, nb8 = (size_t)d->w8 * d->mb_h * 2;
    d->mbi = (h264_mbinfo *)calloc(d->nmbs, sizeof(h264_mbinfo));
    d->nnz_l = (uint8_t *)calloc(nb4, 1);
    d->nnz_c[0] = (uint8_t *)calloc(nb8, 1);
    d->nnz_c[1] = (uint8_t *)calloc(nb8, 1);
    d->ipm = (int8_t *)calloc(nb4, 1);
    d->mvd[0] = (uint8_t *)calloc(nb4 * 2, 1);
    d->mvd[1] = (uint8_t *)calloc(nb4 * 2, 1);
    if (!d->mbi || !d->nnz_l || !d->nnz_c[0] || !d->nnz_c[1] || !d->ipm || !d->mvd[0] || !d->mvd[1]) return -1;
    const int ys = d->mb_w * 16, cs = d->mb_w * 8, yh = d->mb_h * 16, ch = d->mb_h * 8;
    for (int i = 0; i < d->npics; i++) {
        h264_pic *p = &d->pics[i];
        p->buf = (uint8_t *)calloc((size_t)ys * yh + 2 * (size_t)cs * ch, 1);
        p->stride[0] = ys; p->stride[1] = p->stride[2] = cs;
        p->p[0] = p->buf;
        p->p[1] = p->buf + (size_t)ys * yh;
        p->p[2] = p->p[1] + (size_t)cs * ch;
        for (int l = 0; l < 2; l++) {
            p->mv[l] = (int16_t *)calloc(nb4 * 2, sizeof(int16_t));
            p->refi[l] = (int8_t *)calloc(nb8, 1);
            p->refid[l] = (int32_t *)calloc(nb8, sizeof(int32_t));
            if (!p->mv[l] || !p->refi[l] || !p->refid[l]) return -1;
        }
        p->mbintra = (uint8_t *)calloc(d->nmbs, 1);
        if (!p->buf || !p->mbintra) return -1;
    }
    return 0;
}

static h264_pic *alloc_pic(h264_dec *d)
{
    for (int i = 0; i < d->npics; i++) {
        if (!d->pics[i].busy) {
            h264_pic *p = &d->pics[i];
            p->busy = 1;
            p->ref = 0;
            p->long_idx = 0;
            p->id = ++d->next_id;
            return p;
        }
    }
    return NULL;
}

/* ---- slice header ------------------------------------------------------------------------------------------------ */

static int parse_slice_header(h264_dec *d, bitreader *b, int nal_type, int nal_ref_idc, h264_slice *sl)
{
    memset(sl, 0, sizeof *sl);
    const uint32_t first_mb = br_ue(b);
    uint32_t t = br_ue(b);
    if (t > 9) return -1;
    t %= 5;
    if (t >= 3) return -1;                                /* TODO: SP and SI slices */
    sl->type = (int)t;                                    /* 0 P, 1 B, 2 I */
    uint32_t pps_id = br_ue(b);
    if (pps_id > 255 || !d->pps[pps_id].valid) return -1;
    d->cpps = &d->pps[pps_id];
    if (!d->sps[d->cpps->sps_id].valid) return -1;
    d->csps = &d->sps[d->cpps->sps_id];
    const h264_sps *sps = d->csps;
    const h264_pps *pps = d->cpps;
    sl->pps_id = (int)pps_id;
    sl->nal_ref_idc = nal_ref_idc;
    if (first_mb >= (uint32_t)(sps->mb_w * sps->mb_h)) return -1;
    sl->first_mb = (int)first_mb;
    sl->frame_num = (int)br_get(b, sps->log2_max_frame_num);
    sl->idr = nal_type == 5;
    if (sl->idr) sl->idr_pic_id = br_ue_cap(b, 65535);
    if (sps->poc_type == 0) {
        sl->poc_lsb = (int)br_get(b, sps->log2_max_poc_lsb);
        if (pps->bottom_poc_present) sl->delta_poc_bottom = br_se(b);
    }
    if (pps->redundant_pic_cnt && br_ue(b) > 0) return 1;     /* a redundant slice: ignore */
    const int is_b = sl->type == 1, is_i = sl->type == 2;
    if (is_b) sl->direct_spatial = (int)br_get1(b);
    if (!is_i) {
        sl->num_ref[0] = pps->num_ref_default[0];
        sl->num_ref[1] = is_b ? pps->num_ref_default[1] : 0;
        if (br_get1(b)) {
            sl->num_ref[0] = br_ue_cap(b, 32) + 1;
            if (is_b) sl->num_ref[1] = br_ue_cap(b, 32) + 1;
        }
        if (sl->num_ref[0] > 16 || sl->num_ref[1] > 16) return -1;
        for (int l = 0; l < (is_b ? 2 : 1); l++) {
            if (!br_get1(b)) continue;
            for (;;) {
                uint32_t op = br_ue(b);
                if (op == 3) break;
                if (op > 5 || sl->mod_n[l] >= H264_MAX_REFS || b->pos > b->nbits) return -1;
                sl->mod[l][sl->mod_n[l]].op = (int)op;
                sl->mod[l][sl->mod_n[l]].val = br_ue_cap(b, 65536);
                sl->mod_n[l]++;
            }
        }
    }
    if ((pps->weighted_pred && sl->type == 0) || (pps->weighted_bipred == 1 && is_b)) {
        sl->luma_denom = br_ue_cap(b, 7);
        sl->chroma_denom = br_ue_cap(b, 7);
        if (sl->luma_denom > 7 || sl->chroma_denom > 7) return -1;
        for (int l = 0; l < (is_b ? 2 : 1); l++) {
            for (int i = 0; i < sl->num_ref[l]; i++) {
                sl->lw[l][i] = 1 << sl->luma_denom;
                sl->lo[l][i] = 0;
                if (br_get1(b)) { sl->lw[l][i] = br_se(b); sl->lo[l][i] = br_se(b); }
                for (int c = 0; c < 2; c++) { sl->cw[l][i][c] = 1 << sl->chroma_denom; sl->co[l][i][c] = 0; }
                if (br_get1(b)) {
                    for (int c = 0; c < 2; c++) { sl->cw[l][i][c] = br_se(b); sl->co[l][i][c] = br_se(b); }
                }
            }
        }
    }
    if (nal_ref_idc) {
        if (sl->idr) {
            br_get1(b);                                   /* no_output_of_prior_pics_flag */
            sl->long_term_reference = (int)br_get1(b);
        } else if ((sl->adaptive_marking = (int)br_get1(b)) != 0) {
            for (;;) {
                uint32_t op = br_ue(b);
                if (op == 0) break;
                if (op > 6 || sl->mmco_n >= 34 || b->pos > b->nbits) return -1;
                sl->mmco[sl->mmco_n].op = (int)op;
                if (op == 1 || op == 2 || op == 3 || op == 4 || op == 6) sl->mmco[sl->mmco_n].a = br_ue_cap(b, 65536);
                if (op == 3) sl->mmco[sl->mmco_n].b = br_ue_cap(b, 65536);
                sl->mmco_n++;
            }
        }
    }
    if (!is_i) {
        sl->cabac_init_idc = br_ue_cap(b, 2);
        if (sl->cabac_init_idc > 2) return -1;
    }
    sl->qp = pps->init_qp + br_se(b);
    if (sl->qp < 0 || sl->qp > 51) return -1;
    if (pps->deblocking_control) {
        sl->dbf_idc = br_ue_cap(b, 2);
        if (sl->dbf_idc > 2) return -1;
        if (sl->dbf_idc != 1) { sl->dbf_alpha = br_se(b) * 2; sl->dbf_beta = br_se(b) * 2; }
    }
    return 0;
}

/* ---- reference picture lists ------------------------------------------------------------------------------------- */

static int cmp_desc_wrap(const void *a, const void *b)
{
    return (*(h264_pic *const *)b)->frame_num_wrap - (*(h264_pic *const *)a)->frame_num_wrap;
}
static int cmp_asc_long(const void *a, const void *b)
{
    return (*(h264_pic *const *)a)->long_idx - (*(h264_pic *const *)b)->long_idx;
}
static int cmp_poc_desc(const void *a, const void *b) { return (*(h264_pic *const *)b)->poc - (*(h264_pic *const *)a)->poc; }
static int cmp_poc_asc(const void *a, const void *b) { return (*(h264_pic *const *)a)->poc - (*(h264_pic *const *)b)->poc; }

static void build_ref_lists(h264_dec *d, h264_slice *sl)
{
    h264_pic *shorts[H264_MAX_PICS], *longs[H264_MAX_PICS];
    int ns = 0, nl = 0;
    for (int i = 0; i < H264_MAX_PICS; i++) {
        h264_pic *p = &d->pics[i];
        if (!p->busy || p == d->cur) continue;
        if (p->ref == 1) {
            p->frame_num_wrap = p->frame_num > sl->frame_num ? p->frame_num - d->max_frame_num : p->frame_num;
            shorts[ns++] = p;
        } else if (p->ref == 2) {
            longs[nl++] = p;
        }
    }
    qsort(longs, nl, sizeof *longs, cmp_asc_long);
    h264_pic *init[2][H264_MAX_PICS * 2];
    int n[2] = {0, 0};
    if (sl->type == 0) {
        qsort(shorts, ns, sizeof *shorts, cmp_desc_wrap);
        for (int i = 0; i < ns; i++) init[0][n[0]++] = shorts[i];
        for (int i = 0; i < nl; i++) init[0][n[0]++] = longs[i];
    } else if (sl->type == 1) {
        h264_pic *before[H264_MAX_PICS], *after[H264_MAX_PICS];
        int nb = 0, na = 0;
        for (int i = 0; i < ns; i++) (shorts[i]->poc < d->cur->poc ? before : after)[shorts[i]->poc < d->cur->poc ? nb++ : na++] = shorts[i];
        qsort(before, nb, sizeof *before, cmp_poc_desc);
        qsort(after, na, sizeof *after, cmp_poc_asc);
        for (int i = 0; i < nb; i++) init[0][n[0]++] = before[i];
        for (int i = 0; i < na; i++) init[0][n[0]++] = after[i];
        for (int i = 0; i < na; i++) init[1][n[1]++] = after[i];
        for (int i = 0; i < nb; i++) init[1][n[1]++] = before[i];
        for (int i = 0; i < nl; i++) { init[0][n[0]++] = longs[i]; init[1][n[1]++] = longs[i]; }
        if (n[1] > 1 && n[0] == n[1]) {
            int same = 1;
            for (int i = 0; i < n[0]; i++) if (init[0][i] != init[1][i]) { same = 0; break; }
            if (same) { h264_pic *t = init[1][0]; init[1][0] = init[1][1]; init[1][1] = t; }
        }
    }
    for (int l = 0; l < (sl->type == 1 ? 2 : sl->type == 0 ? 1 : 0); l++) {
        const int want = sl->num_ref[l];
        h264_pic *list[H264_MAX_REFS + 1];
        int cnt = n[l] < want ? n[l] : want;
        for (int i = 0; i < cnt; i++) list[i] = init[l][i];
        for (int i = cnt; i <= H264_MAX_REFS && i < want + 1; i++) list[i] = NULL;
        /* modification */
        int pred = sl->frame_num, lt_pred = 0;
        (void)lt_pred;
        int idx = 0;
        for (int m = 0; m < sl->mod_n[l]; m++) {
            const int op = sl->mod[l][m].op, val = sl->mod[l][m].val;
            h264_pic *target = NULL;
            if (op == 0 || op == 1) {
                const int delta = val + 1;
                if (op == 0) { pred -= delta; if (pred < 0) pred += d->max_frame_num; }
                else { pred += delta; if (pred >= d->max_frame_num) pred -= d->max_frame_num; }
                const int pic_num = pred > sl->frame_num ? pred - d->max_frame_num : pred;
                for (int i = 0; i < ns; i++) if (shorts[i]->frame_num_wrap == pic_num) target = shorts[i];
            } else if (op == 2) {
                for (int i = 0; i < nl; i++) if (longs[i]->long_idx == val) target = longs[i];
            }
            if (!target || idx >= want) continue;
            int k;
            for (k = want; k > idx; k--) list[k] = list[k - 1];
            list[idx++] = target;
            int o = idx;
            for (k = idx; k <= want; k++) if (list[k] != target) list[o++] = list[k];
        }
        sl->list_n[l] = want;
        for (int i = 0; i < want; i++) sl->list[l][i] = list[i];
    }
    /* A broken stream may leave holes; point them at some picture so decoding can go on. */
    h264_pic *any = NULL;
    for (int i = 0; i < H264_MAX_PICS && !any; i++) if (d->pics[i].busy && d->pics[i].ref && &d->pics[i] != d->cur) any = &d->pics[i];
    if (!any) any = d->cur;
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < sl->list_n[l]; i++) if (!sl->list[l][i]) sl->list[l][i] = any;
}

/* Implicit bi-prediction weights and temporal direct scale factors depend on picture distances only. */
static void build_distances(h264_dec *d, h264_slice *sl)
{
    if (sl->type != 1) return;
    const int cur = d->cur->poc;
    for (int i = 0; i < sl->list_n[0]; i++) {
        const h264_pic *p0 = sl->list[0][i];
        const int p0poc = p0->poc;
        {
            const h264_pic *p1 = sl->list[1][0];
            const int tb = h264_clip3(-128, 127, cur - p0poc), td = h264_clip3(-128, 127, p1->poc - p0poc);
            int dsf = 256;
            if (td != 0 && p0->ref != 2) {
                const int tx = (16384 + abs(td / 2)) / td;
                dsf = h264_clip3(-1024, 1023, (tb * tx + 32) >> 6);
            }
            sl->dist_scale[i] = dsf;
        }
        for (int j = 0; j < sl->list_n[1]; j++) {
            const h264_pic *p1 = sl->list[1][j];
            const int tb = h264_clip3(-128, 127, cur - p0poc), td = h264_clip3(-128, 127, p1->poc - p0poc);
            int w1 = 32;
            if (td != 0 && p0->ref != 2 && p1->ref != 2) {
                const int tx = (16384 + abs(td / 2)) / td;
                const int dsf = h264_clip3(-1024, 1023, (tb * tx + 32) >> 6);
                if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128) w1 = dsf >> 2;
            }
            sl->implicit_w1[i][j] = w1;
        }
    }
}

/* ---- picture order count and reference marking ------------------------------------------------------------------- */

static void compute_poc(h264_dec *d, const h264_slice *sl)
{
    const h264_sps *s = d->csps;
    int top, bottom;
    if (s->poc_type == 0) {
        const int max_lsb = 1 << s->log2_max_poc_lsb;
        int prev_msb = d->prev_poc_msb, prev_lsb = d->prev_poc_lsb;
        if (sl->idr) { prev_msb = 0; prev_lsb = 0; }
        int msb;
        if (sl->poc_lsb < prev_lsb && prev_lsb - sl->poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
        else if (sl->poc_lsb > prev_lsb && sl->poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
        else msb = prev_msb;
        top = msb + sl->poc_lsb;
        bottom = top + sl->delta_poc_bottom;
        if (sl->nal_ref_idc) { d->prev_poc_msb = msb; d->prev_poc_lsb = sl->poc_lsb; }
    } else {
        int offset;
        if (sl->idr) offset = 0;
        else if (d->prev_frame_num > sl->frame_num) offset = d->prev_frame_num_offset + d->max_frame_num;
        else offset = d->prev_frame_num_offset;
        d->prev_frame_num_offset = offset;
        top = bottom = sl->idr ? 0 : 2 * (offset + sl->frame_num) - (sl->nal_ref_idc ? 0 : 1);
    }
    d->cur->poc = top < bottom ? top : bottom;
}

static void unmark_all(h264_dec *d)
{
    for (int i = 0; i < H264_MAX_PICS; i++) {
        if (d->pics[i].busy && &d->pics[i] != d->cur) { d->pics[i].ref = 0; d->pics[i].busy = 0; }
    }
}

static void release(h264_pic *p) { p->ref = 0; p->busy = 0; }

static void mark_current(h264_dec *d, const h264_slice *sl)
{
    h264_pic *cur = d->cur;
    int long_cur = 0;
    if (sl->idr) {
        unmark_all(d);
        if (sl->long_term_reference) { cur->ref = 2; cur->long_idx = 0; long_cur = 1; }
    } else if (sl->adaptive_marking) {
        for (int m = 0; m < sl->mmco_n; m++) {
            const int a = sl->mmco[m].a, b = sl->mmco[m].b;
            switch (sl->mmco[m].op) {
            case 1: case 3: {
                const int pic_num = sl->frame_num - (a + 1);
                for (int i = 0; i < H264_MAX_PICS; i++) {
                    h264_pic *p = &d->pics[i];
                    if (!p->busy || p == cur || p->ref != 1) continue;
                    p->frame_num_wrap = p->frame_num > sl->frame_num ? p->frame_num - d->max_frame_num : p->frame_num;
                    if (p->frame_num_wrap != pic_num) continue;
                    if (sl->mmco[m].op == 1) {
                        release(p);
                    } else {
                        for (int j = 0; j < H264_MAX_PICS; j++)
                            if (d->pics[j].busy && d->pics[j].ref == 2 && d->pics[j].long_idx == b) release(&d->pics[j]);
                        p->ref = 2;
                        p->long_idx = b;
                    }
                }
                break;
            }
            case 2:
                for (int i = 0; i < H264_MAX_PICS; i++)
                    if (d->pics[i].busy && d->pics[i].ref == 2 && d->pics[i].long_idx == a) release(&d->pics[i]);
                break;
            case 4:
                for (int i = 0; i < H264_MAX_PICS; i++)
                    if (d->pics[i].busy && d->pics[i].ref == 2 && d->pics[i].long_idx > a - 1 && &d->pics[i] != cur) release(&d->pics[i]);
                break;
            case 5:
                unmark_all(d);
                d->prev_mmco5 = 1;
                break;
            case 6:
                for (int j = 0; j < H264_MAX_PICS; j++)
                    if (&d->pics[j] != cur && d->pics[j].busy && d->pics[j].ref == 2 && d->pics[j].long_idx == a) release(&d->pics[j]);
                cur->ref = 2;
                cur->long_idx = a;
                long_cur = 1;
                break;
            default: break;
            }
        }
    } else {
        /* sliding window */
        int n = 0;
        for (int i = 0; i < H264_MAX_PICS; i++) if (d->pics[i].busy && d->pics[i].ref && &d->pics[i] != cur) n++;
        const int max = d->psps.max_num_ref_frames > 0 ? d->psps.max_num_ref_frames : 1;
        while (n >= max) {
            h264_pic *oldest = NULL;
            for (int i = 0; i < H264_MAX_PICS; i++) {
                h264_pic *p = &d->pics[i];
                if (!p->busy || p == cur || p->ref != 1) continue;
                p->frame_num_wrap = p->frame_num > sl->frame_num ? p->frame_num - d->max_frame_num : p->frame_num;
                if (!oldest || p->frame_num_wrap < oldest->frame_num_wrap) oldest = p;
            }
            if (!oldest) break;
            release(oldest);
            n--;
        }
    }
    if (!long_cur) cur->ref = 1;
}

/* ---- picture life cycle ------------------------------------------------------------------------------------------ */

static int start_picture(h264_dec *d, const h264_slice *sl)
{
    if (setup_geometry(d, d->csps) < 0) return -1;
    d->psps = *d->csps;
    d->max_frame_num = 1 << d->psps.log2_max_frame_num;
    d->cur = alloc_pic(d);
    if (!d->cur) return -1;
    d->cur->frame_num = sl->frame_num;
    compute_poc(d, sl);
    for (int i = 0; i < d->nmbs; i++) d->mbi[i].slice = -1;
    memset(d->nnz_l, 0, (size_t)d->w4 * d->h4);
    d->slice_count = 0;
    d->have_picture = 1;
    d->prev_mmco5 = 0;
    return 0;
}

static void finish_picture(h264_dec *d, const h264_slice *last, h264_frame *out, int *have_out)
{
    h264_pic *cur = d->cur;
    h264_deblock_picture(d);
    if (last->nal_ref_idc) {
        mark_current(d, last);
        if (d->prev_mmco5) {
            cur->frame_num = 0;
            cur->poc = 0;
            d->prev_poc_msb = 0;
            d->prev_poc_lsb = 0;
            d->prev_frame_num_offset = 0;
        }
    }
    d->prev_frame_num = cur->frame_num;
    /* TODO: gaps in frame_num (lost frames, gaps_in_frame_num_value_allowed_flag) are not concealed; the reference lists of
     * the pictures after a gap are then wrong. */
    const h264_sps *s = &d->psps;
    out->width = s->mb_w * 16 - s->crop_l - s->crop_r;
    out->height = s->mb_h * 16 - s->crop_t - s->crop_b;
    out->plane[0] = cur->p[0] + (size_t)s->crop_t * cur->stride[0] + s->crop_l;
    out->plane[1] = cur->p[1] + (size_t)(s->crop_t / 2) * cur->stride[1] + s->crop_l / 2;
    out->plane[2] = cur->p[2] + (size_t)(s->crop_t / 2) * cur->stride[2] + s->crop_l / 2;
    for (int i = 0; i < 3; i++) out->stride[i] = cur->stride[i];
    out->full_range = s->full_range;
    out->matrix = s->matrix;
    *have_out = 1;
    if (!cur->ref) cur->busy = 0;   /* a non-reference picture is free again, but its data stays valid until the next call */
    d->cur = NULL;
    d->have_picture = 0;
}

/* Does this slice start another picture (7.4.1.2.4)? */
static int starts_new_picture(const h264_slice *a, const h264_slice *b)
{
    return a->frame_num != b->frame_num || a->pps_id != b->pps_id || (a->nal_ref_idc == 0) != (b->nal_ref_idc == 0) ||
           a->poc_lsb != b->poc_lsb || a->delta_poc_bottom != b->delta_poc_bottom || a->idr != b->idr ||
           (a->idr && a->idr_pic_id != b->idr_pic_id);
}

/* ---- API --------------------------------------------------------------------------------------------------------- */

h264_dec *h264_open(const uint8_t *avcc, size_t size)
{
    if (size < 7 || avcc[0] != 1) return NULL;
    h264_dec *d = (h264_dec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->nal_len = (avcc[4] & 3) + 1;
    size_t pos = 6;
    int n_sps = avcc[5] & 31;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n_sps; i++) {
            if (pos + 2 > size) goto fail;
            const size_t len = ((size_t)avcc[pos] << 8) | avcc[pos + 1];
            pos += 2;
            if (len == 0 || pos + len > size) goto fail;
            handle_param_nal(d, avcc + pos, len);
            pos += len;
        }
        if (pass == 0) {
            if (pos >= size) goto fail;
            n_sps = avcc[pos++];
        }
    }
    /* Decodable only with a PPS this decoder accepts (CABAC, no FMO, ...) on top of an accepted SPS; size the pictures
     * by that SPS. */
    for (int i = 0; i < 256; i++) {
        if (d->pps[i].valid && d->sps[d->pps[i].sps_id].valid) {
            d->csps = &d->sps[d->pps[i].sps_id];
            d->max_frame_num = 1 << d->csps->log2_max_frame_num;
            if (setup_geometry(d, d->csps) < 0) goto fail;
            return d;
        }
    }
fail:
    h264_close(d);
    return NULL;
}

void h264_close(h264_dec *d)
{
    if (!d) return;
    free_geometry(d);
    free(d->rbsp);
    free(d);
}

int h264_width(const h264_dec *d) { return d->csps->mb_w * 16 - d->csps->crop_l - d->csps->crop_r; }
int h264_height(const h264_dec *d) { return d->csps->mb_h * 16 - d->csps->crop_t - d->csps->crop_b; }

int h264_decode(h264_dec *d, const uint8_t *pkt, size_t size, h264_frame *out)
{
    int have_out = 0, ret = 0;
    size_t pos = 0;
    while (pos + d->nal_len <= size) {
        size_t len = 0;
        for (int i = 0; i < d->nal_len; i++) len = (len << 8) | pkt[pos + i];
        pos += d->nal_len;
        if (len == 0 || pos + len > size) break;
        const uint8_t *nal = pkt + pos;
        pos += len;
        const int type = nal[0] & 31, ref_idc = (nal[0] >> 5) & 3;
        if (type == 7 || type == 8) {
            if (d->have_picture) { finish_picture(d, &d->sl, out, &have_out); }
            handle_param_nal(d, nal, len);
            continue;
        }
        if (type != 1 && type != 5) continue;
        size_t m = unescape(d, nal + 1, len - 1);
        if (!m) { ret = -1; continue; }
        bitreader b;
        br_init(&b, d->rbsp, m);
        h264_slice sl;
        int r = parse_slice_header(d, &b, type, ref_idc, &sl);
        if (r > 0) continue;
        if (r < 0) { ret = -1; continue; }
        if (d->have_picture && starts_new_picture(&d->sl, &sl)) finish_picture(d, &d->sl, out, &have_out);
        if (!d->have_picture) {
            if (start_picture(d, &sl) < 0) { ret = -1; continue; }
        }
        sl.number = d->slice_count++;
        build_ref_lists(d, &sl);
        build_distances(d, &sl);
        d->sl = sl;
        if (h264_decode_slice_data(d, d->rbsp, m, b.pos) < 0) ret = -1;
    }
    if (d->have_picture) finish_picture(d, &d->sl, out, &have_out);
    if (have_out) return 1;
    return ret;
}
