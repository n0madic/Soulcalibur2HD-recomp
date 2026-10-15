/*
 * Compact H.264/AVC decoder -- CABAC and the macroblock layer: syntax, motion vector prediction, direct modes, intra
 * prediction, residual decoding and reconstruction. Inter prediction is in h264_mc.c.
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "h264_int.h"
#include "h264_tables.h"

#define clip3 h264_clip3
#define clip1 h264_clip_pel
#define imin h264_min
#define imax h264_max

/* A dequantised coefficient is kept within +-2^26 so that the transforms cannot overflow on a hostile stream. */
static int32_t clamp_coef(int64_t v) { return (int32_t)(v < -(1 << 26) ? -(1 << 26) : v > (1 << 26) ? (1 << 26) : v); }

/* ---- scans and dequantisation tables ----------------------------------------------------------------------------- */

static const uint8_t zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static uint8_t zz8[64];

static void init_scans(void)
{
    static int done;
    if (done) return;
    int n = 0;
    for (int s = 0; s <= 14; s++) {
        const int lo = imax(0, s - 7), hi = imin(s, 7);
        if (s & 1) { for (int x = hi; x >= lo; x--) zz8[n++] = (uint8_t)((s - x) * 8 + x); }
        else       { for (int x = lo; x <= hi; x++) zz8[n++] = (uint8_t)((s - x) * 8 + x); }
    }
    done = 1;
}

static const uint8_t norm4[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};
static const uint8_t norm8[6][6] = {
    {20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26}, {26, 23, 42, 24, 33, 31},
    {28, 25, 45, 26, 35, 33}, {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43}};

static int class4(int i, int j) { return ((i | j) & 1) == 0 ? 0 : ((i & j) & 1) ? 1 : 2; }
static int class8(int i, int j)
{
    if ((i & 3) == 0 && (j & 3) == 0) return 0;
    if ((i & 1) && (j & 1)) return 1;
    if ((i & 3) == 2 && (j & 3) == 2) return 2;
    if (((i & 3) == 0 && (j & 1)) || ((i & 1) && (j & 3) == 0)) return 3;
    if (((i & 3) == 0 && (j & 3) == 2) || ((i & 3) == 2 && (j & 3) == 0)) return 4;
    return 5;
}

/* Block index (z-order) <-> position in 4x4 units. */
static int zidx(int x, int y) { return ((y >> 1) << 3) | ((x >> 1) << 2) | ((y & 1) << 1) | (x & 1); }
static int blk_x(int b) { return ((b >> 2) & 1) * 2 + (b & 1); }
static int blk_y(int b) { return (b >> 3) * 2 + ((b >> 1) & 1); }

/* ---- CABAC -------------------------------------------------------------------------------------------------------- */

static const uint8_t lps_next[64] = {
    0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9, 11, 11, 12, 13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33, 33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};

static int cabac_bit(h264_cabac *c)
{
    int v = 0;
    if (c->pos < c->nbits) v = (c->buf[c->pos >> 3] >> (7 - (c->pos & 7))) & 1;
    c->pos++;
    return v;
}

static void cabac_start(h264_cabac *c, const uint8_t *buf, size_t byte_pos, size_t size)
{
    c->buf = buf;
    c->nbits = size * 8;
    c->pos = byte_pos * 8;
    c->range = 510;
    c->offset = 0;
    for (int i = 0; i < 9; i++) c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
}

static int decide(h264_cabac *c, uint8_t *s)
{
    int st = *s >> 1, mps = *s & 1, bin;
    const uint32_t lps = h264_range_lps[st * 4 + ((c->range >> 6) & 3)];
    c->range -= lps;
    if (c->offset >= c->range) {
        bin = !mps;
        c->offset -= c->range;
        c->range = lps;
        if (st == 0) mps = !mps;
        st = lps_next[st];
    } else {
        bin = mps;
        if (st < 62) st++;
    }
    *s = (uint8_t)((st << 1) | mps);
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    }
    return bin;
}

static int bypass(h264_cabac *c)
{
    c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    if (c->offset >= c->range) { c->offset -= c->range; return 1; }
    return 0;
}

static int terminate(h264_cabac *c)
{
    c->range -= 2;
    if (c->offset >= c->range) return 1;
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | (uint32_t)cabac_bit(c);
    }
    return 0;
}

#define DEC(d, i) decide(&(d)->cabac, &(d)->ctx[i])

static void init_contexts(h264_dec *d)
{
    const int8_t(*tab)[2] = h264_cabac_init[d->sl.type == 2 ? 0 : 1 + d->sl.cabac_init_idc];
    const int qp = clip3(0, 51, d->sl.qp);
    for (int i = 0; i < H264_CABAC_CTX; i++) {
        const int pre = clip3(1, 126, ((tab[i][0] * qp) >> 4) + tab[i][1]);
        d->ctx[i] = (uint8_t)(pre <= 63 ? ((63 - pre) << 1) : (((pre - 64) << 1) | 1));
    }
}

/* ---- neighbours ---------------------------------------------------------------------------------------------------- */

static h264_mbinfo *nb_mb(h264_dec *d, int dx, int dy)
{
    const int x = d->mb_x + dx, y = d->mb_y + dy;
    if (x < 0 || y < 0 || x >= d->mb_w || y >= d->mb_h) return NULL;
    h264_mbinfo *m = &d->mbi[y * d->mb_w + x];
    return m->slice == d->sl.number ? m : NULL;
}

/* The macroblock containing luma 4x4 block (gx, gy) if it is available to the current one. */
static h264_mbinfo *mb_of_blk(h264_dec *d, int gx, int gy)
{
    if (gx < 0 || gy < 0 || gx >= d->w4 || gy >= d->h4) return NULL;
    h264_mbinfo *m = &d->mbi[(gy >> 2) * d->mb_w + (gx >> 2)];
    return m->slice == d->sl.number ? m : NULL;
}

/* ---- motion vector prediction ------------------------------------------------------------------------------------- */

typedef struct { int avail, ref, mv[2]; } nbinfo;

static void get_nb(h264_dec *d, int list, int gx, int gy, int cur_z, nbinfo *o)
{
    o->avail = 0; o->ref = -1; o->mv[0] = o->mv[1] = 0;
    if (gx < 0 || gy < 0 || gx >= d->w4 || gy >= d->h4) return;
    const int addr = (gy >> 2) * d->mb_w + (gx >> 2);
    if (addr == d->mb_addr) {
        if (zidx(gx & 3, gy & 3) >= cur_z) return;
    } else if (d->mbi[addr].slice != d->sl.number) {
        return;
    }
    o->avail = 1;
    const int r = d->cur->refi[list][(gy >> 1) * d->w8 + (gx >> 1)];
    if (r < 0) return;
    o->ref = r;
    o->mv[0] = d->cur->mv[list][(gy * d->w4 + gx) * 2];
    o->mv[1] = d->cur->mv[list][(gy * d->w4 + gx) * 2 + 1];
}

static int median3(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }

/* shape: 0 any, 1 16x8, 2 8x16; part: index of the partition. gx, gy: the partition's first 4x4 block (global). */
static void mv_pred(h264_dec *d, int list, int ref, int gx, int gy, int w4, int shape, int part, int out[2])
{
    const int z = zidx(gx & 3, gy & 3);
    nbinfo A, B, C;
    get_nb(d, list, gx - 1, gy, z, &A);
    get_nb(d, list, gx, gy - 1, z, &B);
    get_nb(d, list, gx + w4, gy - 1, z, &C);
    if (!C.avail) get_nb(d, list, gx - 1, gy - 1, z, &C);
    if (shape == 1) {
        if (part == 0 && B.ref == ref) { out[0] = B.mv[0]; out[1] = B.mv[1]; return; }
        if (part == 1 && A.ref == ref) { out[0] = A.mv[0]; out[1] = A.mv[1]; return; }
    } else if (shape == 2) {
        if (part == 0 && A.ref == ref) { out[0] = A.mv[0]; out[1] = A.mv[1]; return; }
        if (part == 1 && C.ref == ref) { out[0] = C.mv[0]; out[1] = C.mv[1]; return; }
    }
    if (!B.avail && !C.avail && A.avail) { B = A; C = A; }
    const int ma = A.ref == ref, mb = B.ref == ref, mc = C.ref == ref;
    if (ma + mb + mc == 1) {
        const nbinfo *n = ma ? &A : mb ? &B : &C;
        out[0] = n->mv[0]; out[1] = n->mv[1];
        return;
    }
    out[0] = median3(A.mv[0], B.mv[0], C.mv[0]);
    out[1] = median3(A.mv[1], B.mv[1], C.mv[1]);
}

static void set_ref(h264_dec *d, int list, int gx8, int gy8, int w8, int h8, int ref)
{
    for (int y = 0; y < h8; y++)
        for (int x = 0; x < w8; x++) d->cur->refi[list][(gy8 + y) * d->w8 + gx8 + x] = (int8_t)ref;
}

static void set_mv(h264_dec *d, int list, int gx, int gy, int w4, int h4, int mx, int my)
{
    for (int y = 0; y < h4; y++)
        for (int x = 0; x < w4; x++) {
            const int i = ((gy + y) * d->w4 + gx + x) * 2;
            d->cur->mv[list][i] = (int16_t)mx;
            d->cur->mv[list][i + 1] = (int16_t)my;
        }
}

static void set_mvd(h264_dec *d, int list, int gx, int gy, int w4, int h4, int ax, int ay)
{
    for (int y = 0; y < h4; y++)
        for (int x = 0; x < w4; x++) {
            const int i = ((gy + y) * d->w4 + gx + x) * 2;
            d->mvd[list][i] = (uint8_t)ax;
            d->mvd[list][i + 1] = (uint8_t)ay;
        }
}

/* ---- direct prediction -------------------------------------------------------------------------------------------- */

static int min_positive(int a, int b) { return (a >= 0 && b >= 0) ? imin(a, b) : imax(a, b); }

/* Colocated block of the first list 1 picture: its list, reference index, picture id and motion vector. */
static void col_info(h264_dec *d, int bx, int by, int *ref, int *refid, int mv[2])
{
    const h264_pic *col = d->sl.list[1][0];
    const int addr = (d->mb_y * d->mb_w) + d->mb_x;
    *ref = -1; *refid = -1; mv[0] = mv[1] = 0;
    if (col->mbintra[addr]) return;
    const int gx = d->mb_x * 4 + bx, gy = d->mb_y * 4 + by;
    const int b8 = (gy >> 1) * d->w8 + (gx >> 1);
    const int l = col->refi[0][b8] >= 0 ? 0 : 1;
    if (col->refi[l][b8] < 0) return;
    *ref = col->refi[l][b8];
    *refid = col->refid[l][b8];
    mv[0] = col->mv[l][(gy * d->w4 + gx) * 2];
    mv[1] = col->mv[l][(gy * d->w4 + gx) * 2 + 1];
}

/* Derives the motion of the quadrants in `mask` (bit per 8x8) of the current macroblock in direct mode. */
static void direct_motion(h264_dec *d, int mask)
{
    const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4;
    const int inference = d->csps->direct_8x8_inference;
    int ref[2] = {0, 0}, mvp[2][2] = {{0, 0}, {0, 0}};
    if (d->sl.direct_spatial) {
        for (int l = 0; l < 2; l++) {
            nbinfo A, B, C;
            get_nb(d, l, gx0 - 1, gy0, 0, &A);
            get_nb(d, l, gx0, gy0 - 1, 0, &B);
            get_nb(d, l, gx0 + 4, gy0 - 1, 0, &C);
            if (!C.avail) get_nb(d, l, gx0 - 1, gy0 - 1, 0, &C);
            ref[l] = min_positive(A.ref, min_positive(B.ref, C.ref));
        }
        if (ref[0] < 0 && ref[1] < 0) {
            ref[0] = ref[1] = 0;
        } else {
            for (int l = 0; l < 2; l++)
                if (ref[l] >= 0) mv_pred(d, l, ref[l], gx0, gy0, 4, 0, 0, mvp[l]);
        }
    }
    const int short_col = d->sl.list[1][0]->ref != 2;
    for (int q = 0; q < 4; q++) {
        if (!(mask & (1 << q))) continue;
        const int qx = (q & 1) * 2, qy = (q >> 1) * 2;
        if (d->sl.direct_spatial) {
            for (int l = 0; l < 2; l++) set_ref(d, l, gx0 / 2 + (q & 1), gy0 / 2 + (q >> 1), 1, 1, ref[l]);
            for (int s = 0; s < 4; s++) {
                const int bx = qx + (s & 1), by = qy + (s >> 1);
                int cr, cid, cmv[2];
                if (inference) col_info(d, (q & 1) * 3, (q >> 1) * 3, &cr, &cid, cmv);
                else col_info(d, bx, by, &cr, &cid, cmv);
                const int col_zero = short_col && cr == 0 && cmv[0] >= -1 && cmv[0] <= 1 && cmv[1] >= -1 && cmv[1] <= 1;
                for (int l = 0; l < 2; l++) {
                    if (ref[l] < 0) { set_mv(d, l, gx0 + bx, gy0 + by, 1, 1, 0, 0); continue; }
                    const int zero = ref[l] == 0 && col_zero;
                    set_mv(d, l, gx0 + bx, gy0 + by, 1, 1, zero ? 0 : mvp[l][0], zero ? 0 : mvp[l][1]);
                }
            }
        } else {
            for (int s = 0; s < 4; s++) {
                const int bx = qx + (s & 1), by = qy + (s >> 1);
                int cr, cid, cmv[2];
                if (inference) col_info(d, (q & 1) * 3, (q >> 1) * 3, &cr, &cid, cmv);
                else col_info(d, bx, by, &cr, &cid, cmv);
                int r0 = 0;
                if (cr >= 0) {
                    r0 = -1;
                    for (int i = 0; i < d->sl.list_n[0]; i++) if (d->sl.list[0][i]->id == cid) { r0 = i; break; }
                    if (r0 < 0) r0 = 0;
                }
                const int dsf = d->sl.dist_scale[r0];
                const int m0x = (dsf * cmv[0] + 128) >> 8, m0y = (dsf * cmv[1] + 128) >> 8;
                if (s == 0) {
                    set_ref(d, 0, gx0 / 2 + (q & 1), gy0 / 2 + (q >> 1), 1, 1, r0);
                    set_ref(d, 1, gx0 / 2 + (q & 1), gy0 / 2 + (q >> 1), 1, 1, 0);
                }
                set_mv(d, 0, gx0 + bx, gy0 + by, 1, 1, m0x, m0y);
                set_mv(d, 1, gx0 + bx, gy0 + by, 1, 1, m0x - cmv[0], m0y - cmv[1]);
            }
        }
        for (int l = 0; l < 2; l++) set_mvd(d, l, gx0 + qx, gy0 + qy, 2, 2, 0, 0);
    }
}

/* ---- syntax elements ---------------------------------------------------------------------------------------------- */

static int read_intra_mb_type(h264_dec *d, int base, int intra_slice)
{
    int s = base;
    if (intra_slice) {
        int inc = 0;
        const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
        if (a && (a->flags & (MBF_I16 | MBF_PCM))) inc++;
        if (b && (b->flags & (MBF_I16 | MBF_PCM))) inc++;
        if (!DEC(d, s + inc)) return 0;
        s += 2;
    } else if (!DEC(d, s)) {
        return 0;
    }
    if (terminate(&d->cabac)) return 25;
    int t = 1;
    t += 12 * DEC(d, s + 1);
    if (DEC(d, s + 2)) t += 4 + 4 * DEC(d, s + 2 + intra_slice);
    t += 2 * DEC(d, s + 3 + intra_slice);
    t += DEC(d, s + 3 + 2 * intra_slice);
    return t;
}

static int read_b_mb_type(h264_dec *d)
{
    int inc = 0;
    const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
    if (a && !(a->flags & MBF_DIRECT)) inc++;
    if (b && !(b->flags & MBF_DIRECT)) inc++;
    if (!DEC(d, 27 + inc)) return 0;
    if (!DEC(d, 27 + 3)) return 1 + DEC(d, 27 + 5);
    int bits = DEC(d, 27 + 4) << 3;
    bits += DEC(d, 27 + 5) << 2;
    bits += DEC(d, 27 + 5) << 1;
    bits += DEC(d, 27 + 5);
    if (bits < 8) return bits + 3;
    if (bits == 13) return 23 + read_intra_mb_type(d, 32, 0);
    if (bits == 14) return 11;
    if (bits == 15) return 22;
    bits = (bits << 1) + DEC(d, 27 + 5);
    return bits - 4;
}

static int read_p_sub_type(h264_dec *d)
{
    if (DEC(d, 21)) return 0;
    if (!DEC(d, 22)) return 1;
    return DEC(d, 23) ? 2 : 3;
}

static int read_b_sub_type(h264_dec *d)
{
    if (!DEC(d, 36)) return 0;
    if (!DEC(d, 37)) return 1 + DEC(d, 39);
    int t = 3;
    if (DEC(d, 38)) {
        if (DEC(d, 39)) return 11 + DEC(d, 39);
        t += 4;
    }
    t += 2 * DEC(d, 39);
    t += DEC(d, 39);
    return t;
}

static int read_ref_idx(h264_dec *d, int list, int gx8, int gy8)
{
    int inc = 0;
    for (int n = 0; n < 2; n++) {
        const int x = gx8 - (n == 0), y = gy8 - (n == 1);
        if (x < 0 || y < 0) continue;
        const h264_mbinfo *m = mb_of_blk(d, x * 2, y * 2);
        if (!m) continue;
        const int ref = d->cur->refi[list][y * d->w8 + x];
        if (ref <= 0) continue;
        const int q = (x & 1) + ((y & 1) << 1);
        if (m->direct8 & (1 << q)) continue;
        inc += n == 0 ? 1 : 2;
    }
    int ref = 0, ctx = 54 + inc;
    while (DEC(d, ctx)) {
        ref++;
        ctx = 54 + ((ctx - 54) >> 2) + 4;
        if (ref >= 32) return -1;
    }
    return ref;
}

static int read_mvd_comp(h264_dec *d, int base, int sum)
{
    if (!DEC(d, base + (sum > 2) + (sum > 32))) return 0;
    int v = 1, c = base + 3;
    while (v < 9 && DEC(d, c)) {
        if (v < 4) c++;
        v++;
    }
    if (v >= 9) {
        int k = 3;
        while (bypass(&d->cabac)) {
            v += 1 << k;
            k++;
            if (k > 24) return 0;
        }
        while (k--) v += bypass(&d->cabac) << k;
    }
    return bypass(&d->cabac) ? -v : v;
}

static void read_mvd(h264_dec *d, int list, int gx, int gy, int mvd[2])
{
    int sum[2] = {0, 0};
    const int xs[2] = {gx - 1, gx}, ys[2] = {gy, gy - 1};
    for (int n = 0; n < 2; n++) {
        if (!mb_of_blk(d, xs[n], ys[n])) continue;
        sum[0] += d->mvd[list][(ys[n] * d->w4 + xs[n]) * 2];
        sum[1] += d->mvd[list][(ys[n] * d->w4 + xs[n]) * 2 + 1];
    }
    mvd[0] = read_mvd_comp(d, 40, sum[0]);
    mvd[1] = read_mvd_comp(d, 47, sum[1]);
}

/* Reads one partition's mvds and sets its motion. */
static void parse_partition_mv(h264_dec *d, int list, int gx, int gy, int w4, int h4, int shape, int part)
{
    const int ref = d->cur->refi[list][(gy >> 1) * d->w8 + (gx >> 1)];
    int mvp[2], mvd[2];
    mv_pred(d, list, ref, gx, gy, w4, shape, part, mvp);
    read_mvd(d, list, gx, gy, mvd);
    set_mv(d, list, gx, gy, w4, h4, mvp[0] + mvd[0], mvp[1] + mvd[1]);
    set_mvd(d, list, gx, gy, w4, h4, imin(abs(mvd[0]), 70), imin(abs(mvd[1]), 70));
}

static int read_cbp(h264_dec *d)
{
    const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
    const int ca = a ? a->cbp : 0x0F, cb = b ? b->cbp : 0x0F;
    int cbp = 0;
    cbp |= DEC(d, 73 + (!(ca & 2)) + 2 * (!(cb & 4)));
    cbp |= DEC(d, 73 + (!(cbp & 1)) + 2 * (!(cb & 8))) << 1;
    cbp |= DEC(d, 73 + (!(ca & 8)) + 2 * (!(cbp & 1))) << 2;
    cbp |= DEC(d, 73 + (!(cbp & 4)) + 2 * (!(cbp & 2))) << 3;
    const int la = a ? (a->cbp >> 4) : 0, lb = b ? (b->cbp >> 4) : 0;
    if (DEC(d, 77 + (la > 0) + 2 * (lb > 0))) cbp |= (1 + DEC(d, 77 + 4 + (la == 2) + 2 * (lb == 2))) << 4;
    return cbp;
}

/* ---- residual blocks ----------------------------------------------------------------------------------------------- */

static const int sig_base[6] = {105 + 0, 105 + 15, 105 + 29, 105 + 44, 105 + 47, 402};
static const int last_base[6] = {166 + 0, 166 + 15, 166 + 29, 166 + 44, 166 + 47, 417};
static const int abs_base[6] = {227 + 0, 227 + 10, 227 + 20, 227 + 30, 227 + 39, 426};
static const int cbf_base[5] = {85 + 0, 85 + 4, 85 + 8, 85 + 12, 85 + 16};

/* Decodes the coefficients of one block (coded_block_flag already known to be set) into out[scan[i]]. */
static int residual_coeffs(h264_dec *d, int cat, int ncoef, const uint8_t *scan, int32_t *out)
{
    int idx[64], n = 0, last = 0;
    for (int i = 0; i < ncoef - 1; i++) {
        int sinc, linc;
        if (cat == 5) { sinc = h264_sig8x8[i]; linc = h264_last8x8[i]; }
        else if (cat == 3) { sinc = linc = imin(i, 2); }
        else { sinc = linc = i; }
        if (DEC(d, sig_base[cat] + sinc)) {
            idx[n++] = i;
            if (DEC(d, last_base[cat] + linc)) { last = 1; break; }
        }
    }
    if (!last) idx[n++] = ncoef - 1;
    int eq1 = 0, gt1 = 0;
    for (int k = n - 1; k >= 0; k--) {
        const int ctx0 = abs_base[cat] + (gt1 ? 0 : imin(4, 1 + eq1));
        int a = 1;
        if (DEC(d, ctx0)) {
            const int ctxn = abs_base[cat] + 5 + imin(4 - (cat == 3), gt1);
            a = 2;
            while (a < 15 && DEC(d, ctxn)) a++;
            if (a >= 15) {
                int j = 0;
                while (bypass(&d->cabac) && j < 23) j++;
                int v = 1;
                while (j--) v += v + bypass(&d->cabac);
                a = v + 14;
            }
            gt1++;
        } else {
            eq1++;
        }
        out[scan[idx[k]]] = bypass(&d->cabac) ? -a : a;
    }
    return n;
}

/* coded_block_flag of a block: inc = condTermFlagA + 2 * condTermFlagB. */
static int read_cbf(h264_dec *d, int cat, int inc) { return DEC(d, cbf_base[cat] + inc); }

static int cond_intra_default(const h264_dec *d)
{
    /* an unavailable neighbour counts as coded for intra macroblocks and as not coded for inter ones */
    return (d->mbi[d->mb_addr].flags & MBF_INTRA) ? 1 : 0;
}

static int luma_cbf_inc(h264_dec *d, int gx, int gy)
{
    int inc = 0;
    for (int n = 0; n < 2; n++) {
        const int x = gx - (n == 0), y = gy - (n == 1);
        const h264_mbinfo *m = mb_of_blk(d, x, y);
        const int c = m ? d->nnz_l[y * d->w4 + x] != 0 : cond_intra_default(d);
        inc += c << n;
    }
    return inc;
}

static int chroma_ac_cbf_inc(h264_dec *d, int comp, int cx, int cy)
{
    int inc = 0;
    const int w = d->mb_w * 2;
    for (int n = 0; n < 2; n++) {
        const int x = cx - (n == 0), y = cy - (n == 1);
        const h264_mbinfo *m = mb_of_blk(d, x * 2, y * 2);
        const int c = (x < 0 || y < 0) ? cond_intra_default(d) : m ? d->nnz_c[comp][y * w + x] != 0 : cond_intra_default(d);
        inc += c << n;
    }
    return inc;
}

static int dc_cbf_inc(h264_dec *d, int bit)
{
    const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
    const int ca = a ? (a->cbf_dc >> bit) & 1 : cond_intra_default(d);
    const int cb = b ? (b->cbf_dc >> bit) & 1 : cond_intra_default(d);
    return ca + 2 * cb;
}

/* ---- transforms ---------------------------------------------------------------------------------------------------- */

static void add_idct4(uint8_t *dst, int stride, int32_t *c)
{
    int32_t t[16];
    for (int i = 0; i < 4; i++) {
        const int32_t d0 = c[i * 4], d1 = c[i * 4 + 1], d2 = c[i * 4 + 2], d3 = c[i * 4 + 3];
        const int32_t e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        t[i * 4] = e0 + e3; t[i * 4 + 1] = e1 + e2; t[i * 4 + 2] = e1 - e2; t[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        const int32_t d0 = t[j], d1 = t[4 + j], d2 = t[8 + j], d3 = t[12 + j];
        const int32_t e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        const int32_t r[4] = {e0 + e3, e1 + e2, e1 - e2, e0 - e3};
        for (int i = 0; i < 4; i++) dst[i * stride + j] = clip1(dst[i * stride + j] + ((r[i] + 32) >> 6));
    }
}

static void idct8_1d(const int32_t *in, int s, int32_t *out, int os)
{
    const int32_t d0 = in[0], d1 = in[s], d2 = in[2 * s], d3 = in[3 * s], d4 = in[4 * s], d5 = in[5 * s], d6 = in[6 * s], d7 = in[7 * s];
    const int32_t a0 = d0 + d4, a4 = d0 - d4, a2 = (d2 >> 1) - d6, a6 = d2 + (d6 >> 1);
    const int32_t b0 = a0 + a6, b2 = a4 + a2, b4 = a4 - a2, b6 = a0 - a6;
    const int32_t a1 = -d3 + d5 - d7 - (d7 >> 1), a3 = d1 + d7 - d3 - (d3 >> 1);
    const int32_t a5 = -d1 + d7 + d5 + (d5 >> 1), a7 = d3 + d5 + d1 + (d1 >> 1);
    const int32_t b1 = a1 + (a7 >> 2), b7 = a7 - (a1 >> 2), b3 = a3 + (a5 >> 2), b5 = (a3 >> 2) - a5;
    out[0] = b0 + b7; out[os] = b2 + b5; out[2 * os] = b4 + b3; out[3 * os] = b6 + b1;
    out[4 * os] = b6 - b1; out[5 * os] = b4 - b3; out[6 * os] = b2 - b5; out[7 * os] = b0 - b7;
}

static void add_idct8(uint8_t *dst, int stride, int32_t *c)
{
    int32_t t[64], u[64];
    for (int i = 0; i < 8; i++) idct8_1d(c + i * 8, 1, t + i * 8, 1);
    for (int j = 0; j < 8; j++) idct8_1d(t + j, 8, u + j, 8);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) dst[i * stride + j] = clip1(dst[i * stride + j] + ((u[i * 8 + j] + 32) >> 6));
}

/* Scales the levels of a 4x4 block in place; `skip_dc` leaves coefficient 0 (already scaled) alone. */
static void dequant4(int32_t *c, int qp, int skip_dc)
{
    const int q6 = qp / 6, qm = qp % 6;
    for (int i = skip_dc; i < 16; i++) {
        if (c[i]) c[i] = clamp_coef(((int64_t)c[i] * norm4[qm][class4(i >> 2, i & 3)]) << q6);
    }
}

static void dequant8(int32_t *c, int qp)
{
    const int q6 = qp / 6, qm = qp % 6;
    for (int i = 0; i < 64; i++) {
        if (!c[i]) continue;
        const int ls = 16 * norm8[qm][class8(i >> 3, i & 7)];
        c[i] = clamp_coef(qp >= 36 ? ((int64_t)c[i] * ls) << (q6 - 6) : ((int64_t)c[i] * ls + (1 << (5 - q6))) >> (6 - q6));
    }
}

/* ---- intra prediction ----------------------------------------------------------------------------------------------- */

/* Generic NxN directional prediction (N = 4 or 8). top[0..2N-1], left[0..N-1], tl; availability flags as given. */
static void pred_nxn(uint8_t *dst, int stride, int n, int mode, const int *top, const int *left, int tl, int have_t, int have_l)
{
#define P(x, y) ((y) < 0 ? ((x) < 0 ? tl : top[x]) : left[y])   /* P(x, -1) = top, P(-1, y) = left, P(-1, -1) = tl */
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int v = 0;
            switch (mode) {
            case 0: v = top[x]; break;
            case 1: v = left[y]; break;
            case 2: {
                int st = 0, sl = 0;
                for (int i = 0; i < n; i++) { st += top[i]; sl += left[i]; }
                const int sh = n == 4 ? 2 : 3;
                v = (have_t && have_l) ? (st + sl + n) >> (sh + 1) : have_t ? (st + (n >> 1)) >> sh : have_l ? (sl + (n >> 1)) >> sh : 128;
                break;
            }
            case 3:
                v = (x == n - 1 && y == n - 1) ? (top[2 * n - 2] + 3 * top[2 * n - 1] + 2) >> 2
                                               : (top[x + y] + 2 * top[x + y + 1] + top[x + y + 2] + 2) >> 2;
                break;
            case 4:
                if (x > y) v = (P(x - y - 2, -1) + 2 * P(x - y - 1, -1) + top[x - y] + 2) >> 2;
                else if (x < y) v = (P(-1, y - x - 2) + 2 * P(-1, y - x - 1) + left[y - x] + 2) >> 2;
                else v = (top[0] + 2 * tl + left[0] + 2) >> 2;
                break;
            case 5: {
                const int z = 2 * x - y;
                if (z >= 0 && !(z & 1)) v = (P(x - (y >> 1) - 1, -1) + top[x - (y >> 1)] + 1) >> 1;
                else if (z >= 0) v = (P(x - (y >> 1) - 2, -1) + 2 * P(x - (y >> 1) - 1, -1) + top[x - (y >> 1)] + 2) >> 2;
                else if (z == -1) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
                else v = (P(-1, y - 2 * x - 1) + 2 * P(-1, y - 2 * x - 2) + P(-1, y - 2 * x - 3) + 2) >> 2;
                break;
            }
            case 6: {
                const int z = 2 * y - x;
                if (z >= 0 && !(z & 1)) v = (P(-1, y - (x >> 1) - 1) + left[y - (x >> 1)] + 1) >> 1;
                else if (z >= 0) v = (P(-1, y - (x >> 1) - 2) + 2 * P(-1, y - (x >> 1) - 1) + left[y - (x >> 1)] + 2) >> 2;
                else if (z == -1) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
                else v = (P(x - 2 * y - 1, -1) + 2 * P(x - 2 * y - 2, -1) + P(x - 2 * y - 3, -1) + 2) >> 2;
                break;
            }
            case 7:
                if (!(y & 1)) v = (top[x + (y >> 1)] + top[x + (y >> 1) + 1] + 1) >> 1;
                else v = (top[x + (y >> 1)] + 2 * top[x + (y >> 1) + 1] + top[x + (y >> 1) + 2] + 2) >> 2;
                break;
            default: {
                const int z = x + 2 * y;
                if (z > 2 * n - 3) v = left[n - 1];
                else if (z == 2 * n - 3) v = (left[n - 2] + 3 * left[n - 1] + 2) >> 2;
                else if (!(z & 1)) v = (left[y + (x >> 1)] + left[y + (x >> 1) + 1] + 1) >> 1;
                else v = (left[y + (x >> 1)] + 2 * left[y + (x >> 1) + 1] + left[y + (x >> 1) + 2] + 2) >> 2;
                break;
            }
            }
            dst[y * stride + x] = (uint8_t)v;
        }
    }
#undef P
}

/* Is block (gx, gy) usable as an intra prediction neighbour of the block at z-order position `cur_z`? */
static int intra_avail(h264_dec *d, int gx, int gy, int cur_z)
{
    if (gx < 0 || gy < 0 || gx >= d->w4 || gy >= d->h4) return 0;
    const int addr = (gy >> 2) * d->mb_w + (gx >> 2);
    if (addr == d->mb_addr) return zidx(gx & 3, gy & 3) < cur_z;
    const h264_mbinfo *m = &d->mbi[addr];
    if (m->slice != d->sl.number) return 0;
    return !d->cpps->constrained_intra || (m->flags & MBF_INTRA);
}

/* Predicts the NxN block at 4x4 position (bx, by) of the macroblock; n = 4 or 8; reads/writes the current picture. */
static void intra_nxn(h264_dec *d, int bx, int by, int n, int mode)
{
    h264_pic *cur = d->cur;
    const int stride = cur->stride[0];
    const int gx = d->mb_x * 4 + bx, gy = d->mb_y * 4 + by, w4 = n / 4;
    const int z = zidx(bx, by);
    const int ht = intra_avail(d, gx, gy - 1, z), hl = intra_avail(d, gx - 1, gy, z);
    const int htl = intra_avail(d, gx - 1, gy - 1, z), htr = intra_avail(d, gx + w4, gy - 1, z);
    uint8_t *p = cur->p[0] + (size_t)(gy * 4) * stride + gx * 4;
    int top[17] = {0}, left[8] = {0}, tl = 128;
    if (ht) {
        for (int i = 0; i < n; i++) top[i] = p[-stride + i];
        for (int i = n; i < 2 * n; i++) top[i] = htr ? p[-stride + i] : top[n - 1];
    }
    if (hl) for (int i = 0; i < n; i++) left[i] = p[i * stride - 1];
    if (htl) tl = p[-stride - 1];
    if (n == 8) {
        int ft[16], fl[8], ftl;
        if (ht) {
            ft[0] = htl ? (tl + 2 * top[0] + top[1] + 2) >> 2 : (3 * top[0] + top[1] + 2) >> 2;
            for (int i = 1; i < 15; i++) ft[i] = (top[i - 1] + 2 * top[i] + top[i + 1] + 2) >> 2;
            ft[15] = (top[14] + 3 * top[15] + 2) >> 2;
        }
        if (hl) {
            fl[0] = htl ? (tl + 2 * left[0] + left[1] + 2) >> 2 : (3 * left[0] + left[1] + 2) >> 2;
            for (int i = 1; i < 7; i++) fl[i] = (left[i - 1] + 2 * left[i] + left[i + 1] + 2) >> 2;
            fl[7] = (left[6] + 3 * left[7] + 2) >> 2;
        }
        if (htl) {
            if (ht && hl) ftl = (top[0] + 2 * tl + left[0] + 2) >> 2;
            else if (ht) ftl = (3 * tl + top[0] + 2) >> 2;
            else if (hl) ftl = (3 * tl + left[0] + 2) >> 2;
            else ftl = tl;
            tl = ftl;
        }
        if (ht) memcpy(top, ft, sizeof ft);
        if (hl) memcpy(left, fl, sizeof fl);
    }
    pred_nxn(p, stride, n, mode, top, left, tl, ht, hl);
}

static int mb_intra_ok(h264_dec *d, int dx, int dy)
{
    const h264_mbinfo *m = nb_mb(d, dx, dy);
    return m && (!d->cpps->constrained_intra || (m->flags & MBF_INTRA));
}

/* Does the macroblock have the neighbours an Intra16x16 (chroma = 0: V, H, DC, plane) or intra chroma (chroma = 1: DC,
 * H, V, plane) prediction mode reads? A stream that asks for others is broken and must not be predicted from. */
static int intra_mode_possible(h264_dec *d, int mode, int chroma)
{
    const int top = mb_intra_ok(d, 0, -1), left = mb_intra_ok(d, -1, 0), tl = mb_intra_ok(d, -1, -1);
    const int vertical = chroma ? 2 : 0, horizontal = 1, dc = chroma ? 0 : 2;
    if (mode == dc) return 1;
    if (mode == vertical) return top;
    if (mode == horizontal) return left;
    return top && left && tl;
}

static void intra16(h264_dec *d, int mode)
{
    h264_pic *cur = d->cur;
    const int stride = cur->stride[0];
    uint8_t *p = cur->p[0] + (size_t)(d->mb_y * 16) * stride + d->mb_x * 16;
    const int ht = mb_intra_ok(d, 0, -1), hl = mb_intra_ok(d, -1, 0);
    if (mode == 0) {
        for (int y = 0; y < 16; y++) memcpy(p + y * stride, p - stride, 16);
    } else if (mode == 1) {
        for (int y = 0; y < 16; y++) memset(p + y * stride, p[y * stride - 1], 16);
    } else if (mode == 2) {
        int st = 0, sl = 0;
        for (int i = 0; i < 16; i++) { if (ht) st += p[-stride + i]; if (hl) sl += p[i * stride - 1]; }
        const int v = (ht && hl) ? (st + sl + 16) >> 5 : ht ? (st + 8) >> 4 : hl ? (sl + 8) >> 4 : 128;
        for (int y = 0; y < 16; y++) memset(p + y * stride, v, 16);
    } else {
        int h = 0, v = 0;
        for (int i = 0; i < 8; i++) {
            h += (i + 1) * (p[-stride + 8 + i] - p[-stride + 6 - i]);              /* i = 7 reads p[-1, -1] */
            v += (i + 1) * (p[(8 + i) * stride - 1] - p[(6 - i) * stride - 1]);
        }
        const int a = 16 * (p[15 * stride - 1] + p[-stride + 15]), b = (5 * h + 32) >> 6, c = (5 * v + 32) >> 6;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) p[y * stride + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
    }
}

static void intra_chroma(h264_dec *d, int mode)
{
    h264_pic *cur = d->cur;
    const int ht = mb_intra_ok(d, 0, -1), hl = mb_intra_ok(d, -1, 0);
    for (int c = 1; c <= 2; c++) {
        const int stride = cur->stride[c];
        uint8_t *p = cur->p[c] + (size_t)(d->mb_y * 8) * stride + d->mb_x * 8;
        if (mode == 0) {
            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    int st = 0, sl = 0;
                    for (int i = 0; i < 4; i++) {
                        if (ht) st += p[-stride + bx * 4 + i];
                        if (hl) sl += p[(by * 4 + i) * stride - 1];
                    }
                    int v;
                    if ((bx == 0 && by == 0) || (bx == 1 && by == 1)) v = (ht && hl) ? (st + sl + 4) >> 3 : ht ? (st + 2) >> 2 : hl ? (sl + 2) >> 2 : 128;
                    else if (bx == 1) v = ht ? (st + 2) >> 2 : hl ? (sl + 2) >> 2 : 128;
                    else v = hl ? (sl + 2) >> 2 : ht ? (st + 2) >> 2 : 128;
                    for (int y = 0; y < 4; y++) memset(p + (by * 4 + y) * stride + bx * 4, v, 4);
                }
            }
        } else if (mode == 1) {
            for (int y = 0; y < 8; y++) memset(p + y * stride, p[y * stride - 1], 8);
        } else if (mode == 2) {
            for (int y = 0; y < 8; y++) memcpy(p + y * stride, p - stride, 8);
        } else {
            int h = 0, v = 0;
            for (int i = 0; i < 4; i++) {
                h += (i + 1) * (p[-stride + 4 + i] - (i == 3 ? p[-stride - 1] : p[-stride + 2 - i]));
                v += (i + 1) * (p[(4 + i) * stride - 1] - (i == 3 ? p[-stride - 1] : p[(2 - i) * stride - 1]));
            }
            const int a = 16 * (p[7 * stride - 1] + p[-stride + 7]), b = (34 * h + 32) >> 6, cc = (34 * v + 32) >> 6;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) p[y * stride + x] = clip1((a + b * (x - 3) + cc * (y - 3) + 16) >> 5);
        }
    }
}

/* ---- the macroblock ------------------------------------------------------------------------------------------------ */

typedef struct {
    int flags;           /* MBF_* */
    int i16_mode, cbp;
    int shape;           /* inter: 0 16x16, 1 16x8, 2 8x16, 3 8x8 */
    int pmode[2];        /* inter 16x16/16x8/8x16: prediction mode (1 L0, 2 L1, 3 Bi) of each partition */
    int sub[4];          /* 8x8: sub macroblock types */
    int sub_mode[4], sub_shape[4];   /* prediction mode and shape (0 8x8, 1 8x4, 2 4x8, 3 4x4) of each quadrant */
    int direct_mask;     /* quadrants predicted in direct mode */
    int t8;
    int chroma_mode;
} mbdesc;

static void reset_mb_arrays(h264_dec *d, int flags)
{
    const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4;
    for (int y = 0; y < 4; y++) {
        memset(&d->nnz_l[(gy0 + y) * d->w4 + gx0], 0, 4);
        memset(&d->ipm[(gy0 + y) * d->w4 + gx0], 2, 4);
    }
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < 2; y++) memset(&d->nnz_c[c][(d->mb_y * 2 + y) * d->w8 + d->mb_x * 2], 0, 2);
    for (int l = 0; l < 2; l++) {
        set_ref(d, l, d->mb_x * 2, d->mb_y * 2, 2, 2, -1);
        set_mv(d, l, gx0, gy0, 4, 4, 0, 0);
        set_mvd(d, l, gx0, gy0, 4, 4, 0, 0);
    }
    h264_mbinfo *m = &d->mbi[d->mb_addr];
    m->slice = (int16_t)d->sl.number;
    m->flags = (uint8_t)flags;
    m->cbp = 0; m->chroma_mode = 0; m->cbf_dc = 0; m->direct8 = 0;
    m->qp = (int8_t)d->qp;
    m->dbf_idc = (int8_t)d->sl.dbf_idc; m->dbf_alpha = (int8_t)d->sl.dbf_alpha; m->dbf_beta = (int8_t)d->sl.dbf_beta;
    m->chroma_qp_offset[0] = (int8_t)d->cpps->chroma_qp_offset[0]; m->chroma_qp_offset[1] = (int8_t)d->cpps->chroma_qp_offset[1];
}

/* Parses the residual of the macroblock into d->coef / d->dc and the nnz arrays. Returns <0 on error. */
static void parse_residual(h264_dec *d, const mbdesc *mb)
{
    h264_mbinfo *m = &d->mbi[d->mb_addr];
    const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4;
    const int intra16x16 = mb->flags & MBF_I16;
    memset(d->coef, 0, sizeof d->coef);
    memset(d->dc, 0, sizeof d->dc);
    memset(d->blk_coded, 0, sizeof d->blk_coded);
    if (intra16x16) {
        if (read_cbf(d, 0, dc_cbf_inc(d, 0))) {
            residual_coeffs(d, 0, 16, zz4, d->dc[0]);
            m->cbf_dc |= 1;
            d->blk_coded[16] = 1;
        }
    }
    for (int b8 = 0; b8 < 4; b8++) {
        if (!(mb->cbp & (1 << b8))) continue;
        if (mb->t8) {
            int32_t *c = d->coef + b8 * 64;
            const int n = residual_coeffs(d, 5, 64, zz8, c);
            const int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) d->nnz_l[(gy0 + by + j) * d->w4 + gx0 + bx + i] = (uint8_t)n;
            d->blk_coded[b8 * 4] = 1;
            continue;
        }
        for (int i = 0; i < 4; i++) {
            const int blk = b8 * 4 + i, bx = blk_x(blk), by = blk_y(blk);
            const int cat = intra16x16 ? 1 : 2;
            if (!read_cbf(d, cat, luma_cbf_inc(d, gx0 + bx, gy0 + by))) continue;
            int n;
            if (intra16x16) n = residual_coeffs(d, 1, 15, zz4 + 1, d->coef + blk * 16);
            else n = residual_coeffs(d, 2, 16, zz4, d->coef + blk * 16);
            d->nnz_l[(gy0 + by) * d->w4 + gx0 + bx] = (uint8_t)n;
            d->blk_coded[blk] = 1;
        }
    }
    if (mb->cbp & 0x30) {
        for (int c = 0; c < 2; c++) {
            static const uint8_t raster4[4] = {0, 1, 2, 3};
            if (read_cbf(d, 3, dc_cbf_inc(d, 1 + c))) {
                residual_coeffs(d, 3, 4, raster4, d->dc[1 + c]);
                m->cbf_dc |= 2 << c;
                d->blk_coded[17 + c] = 1;
            }
        }
    }
    if (mb->cbp & 0x20) {
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 4; i++) {
                const int cx = d->mb_x * 2 + (i & 1), cy = d->mb_y * 2 + (i >> 1);
                if (!read_cbf(d, 4, chroma_ac_cbf_inc(d, c, cx, cy))) continue;
                const int n = residual_coeffs(d, 4, 15, zz4 + 1, d->coef + 256 + c * 64 + i * 16);
                d->nnz_c[c][cy * d->w8 + cx] = (uint8_t)n;
                d->blk_coded[19 + c * 4 + i] = 1;
            }
        }
    }
}

/* Dequantises and adds the residual of luma block `blk` (4x4) to the picture. */
static void add_luma_residual(h264_dec *d, const mbdesc *mb, int blk, int qp)
{
    h264_pic *cur = d->cur;
    const int stride = cur->stride[0];
    const int bx = blk_x(blk), by = blk_y(blk);
    uint8_t *dst = cur->p[0] + (size_t)(d->mb_y * 16 + by * 4) * stride + d->mb_x * 16 + bx * 4;
    int32_t *c = d->coef + blk * 16;
    const int i16 = (mb->flags & MBF_I16) != 0;
    if (!d->blk_coded[blk] && !(i16 && c[0])) return;
    if (d->blk_coded[blk]) dequant4(c, qp, i16);
    add_idct4(dst, stride, c);
}

static void add_luma8_residual(h264_dec *d, int b8, int qp)
{
    h264_pic *cur = d->cur;
    const int stride = cur->stride[0];
    uint8_t *dst = cur->p[0] + (size_t)(d->mb_y * 16 + (b8 >> 1) * 8) * stride + d->mb_x * 16 + (b8 & 1) * 8;
    if (!d->blk_coded[b8 * 4]) return;
    int32_t *c = d->coef + b8 * 64;
    dequant8(c, qp);
    add_idct8(dst, stride, c);
}

static void luma_dc_transform(h264_dec *d, int qp)
{
    int32_t *f = d->dc[0], t[16];
    /* inverse 4x4 Hadamard on the 4x4 matrix of DC levels */
    for (int i = 0; i < 4; i++) {
        const int a = f[i * 4], b = f[i * 4 + 1], c = f[i * 4 + 2], e = f[i * 4 + 3];
        t[i * 4] = a + b + c + e; t[i * 4 + 1] = a + b - c - e; t[i * 4 + 2] = a - b - c + e; t[i * 4 + 3] = a - b + c - e;
    }
    const int q6 = qp / 6, ls = 16 * norm4[qp % 6][0];
    for (int j = 0; j < 4; j++) {
        const int a = t[j], b = t[4 + j], c = t[8 + j], e = t[12 + j];
        const int r[4] = {a + b + c + e, a + b - c - e, a - b - c + e, a - b + c - e};
        for (int i = 0; i < 4; i++) {
            const int64_t v = qp >= 36 ? ((int64_t)r[i] * ls) << (q6 - 6) : ((int64_t)r[i] * ls + (1 << (5 - q6))) >> (6 - q6);
            d->coef[zidx(j, i) * 16] = clamp_coef(v);
        }
    }
}

static void reconstruct_chroma_residual(h264_dec *d, const mbdesc *mb)
{
    h264_pic *cur = d->cur;
    if (!(mb->cbp & 0x30)) return;
    for (int c = 0; c < 2; c++) {
        const int qpc = h264_chroma_qp(d->cpps->chroma_qp_offset[c], d->qp);
        const int q6 = qpc / 6, ls = 16 * norm4[qpc % 6][0];
        int32_t *f = d->dc[1 + c];
        int32_t dcv[4];
        const int a = f[0], b = f[1], e = f[2], g = f[3];
        const int r[4] = {a + b + e + g, a - b + e - g, a + b - e - g, a - b - e + g};
        for (int i = 0; i < 4; i++) dcv[i] = clamp_coef((((int64_t)r[i] * ls) << q6) >> 5);
        const int stride = cur->stride[1 + c];
        for (int i = 0; i < 4; i++) {
            int32_t *blk = d->coef + 256 + c * 64 + i * 16;
            if (d->blk_coded[19 + c * 4 + i]) dequant4(blk, qpc, 1);
            blk[0] = dcv[i];
            if (!d->blk_coded[19 + c * 4 + i] && !dcv[i]) continue;
            uint8_t *dst = cur->p[1 + c] + (size_t)(d->mb_y * 8 + (i >> 1) * 4) * stride + d->mb_x * 8 + (i & 1) * 4;
            add_idct4(dst, stride, blk);
        }
    }
}

static void read_intra_modes(h264_dec *d, mbdesc *mb)
{
    const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4;
    const int step = mb->t8 ? 4 : 1;
    for (int blk = 0; blk < 16; blk += step) {
        const int bx = blk_x(blk), by = blk_y(blk), gx = gx0 + bx, gy = gy0 + by;
        const h264_mbinfo *ma = mb_of_blk(d, gx - 1, gy), *mbb = mb_of_blk(d, gx, gy - 1);
        int pred = 2;
        if (ma && mbb && !(d->cpps->constrained_intra && (!(ma->flags & MBF_INTRA) || !(mbb->flags & MBF_INTRA))))
            pred = imin(d->ipm[gy * d->w4 + gx - 1], d->ipm[(gy - 1) * d->w4 + gx]);
        int mode;
        if (DEC(d, 68)) {
            mode = pred;
        } else {
            mode = DEC(d, 69);
            mode += 2 * DEC(d, 69);
            mode += 4 * DEC(d, 69);
            if (mode >= pred) mode++;
        }
        const int w = mb->t8 ? 2 : 1;
        for (int j = 0; j < w; j++)
            for (int i = 0; i < w; i++) d->ipm[(gy + j) * d->w4 + gx + i] = (int8_t)mode;
    }
}

static void commit_motion(h264_dec *d)
{
    h264_pic *cur = d->cur;
    for (int l = 0; l < 2; l++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) {
                const int i = (d->mb_y * 2 + y) * d->w8 + d->mb_x * 2 + x;
                const int r = cur->refi[l][i];
                cur->refid[l][i] = (r >= 0 && r < d->sl.list_n[l]) ? d->sl.list[l][r]->id : -1;
            }
}

/* TODO: untested (none of the sample streams has an I_PCM macroblock): the samples start at the first byte boundary after
 * the bits the arithmetic decoder has consumed. */
static int decode_pcm(h264_dec *d)
{
    h264_cabac *c = &d->cabac;
    size_t byte = (c->pos + 7) >> 3;
    if (byte + 384 > (c->nbits >> 3)) return -1;
    const uint8_t *src = c->buf + byte;
    h264_pic *cur = d->cur;
    for (int y = 0; y < 16; y++) memcpy(cur->p[0] + (size_t)(d->mb_y * 16 + y) * cur->stride[0] + d->mb_x * 16, src + y * 16, 16);
    src += 256;
    for (int p = 1; p <= 2; p++) {
        for (int y = 0; y < 8; y++) memcpy(cur->p[p] + (size_t)(d->mb_y * 8 + y) * cur->stride[p] + d->mb_x * 8, src + y * 8, 8);
        src += 64;
    }
    cabac_start(c, c->buf, byte + 384, c->nbits >> 3);
    return 0;
}

static int decode_macroblock(h264_dec *d)
{
    const int type = d->sl.type;
    mbdesc mb;
    memset(&mb, 0, sizeof mb);
    int t, intra = 0;
    if (type == 2) { t = read_intra_mb_type(d, 3, 1); intra = 1; }
    else if (type == 0) {
        if (!DEC(d, 14)) {
            if (!DEC(d, 15)) t = 3 * DEC(d, 16);
            else t = 2 - DEC(d, 17);
        } else {
            t = 5 + read_intra_mb_type(d, 17, 0);
            intra = 1;
        }
    } else {
        t = read_b_mb_type(d);
        if (t >= 23) intra = 1;
    }
    d->mbi[d->mb_addr].slice = (int16_t)d->sl.number;

    if (intra) {
        const int it = type == 2 ? t : type == 0 ? t - 5 : t - 23;
        if (it == 25) {   /* I_PCM */
            reset_mb_arrays(d, MBF_INTRA | MBF_PCM);
            h264_mbinfo *m = &d->mbi[d->mb_addr];
            m->cbp = 0x2F; m->cbf_dc = 7; m->qp = 0;
            for (int y = 0; y < 4; y++) memset(&d->nnz_l[(d->mb_y * 4 + y) * d->w4 + d->mb_x * 4], 16, 4);
            for (int c = 0; c < 2; c++)
                for (int y = 0; y < 2; y++) memset(&d->nnz_c[c][(d->mb_y * 2 + y) * d->w8 + d->mb_x * 2], 16, 2);
            d->last_dqp = 0;
            d->cur->mbintra[d->mb_addr] = 1;
            commit_motion(d);
            return decode_pcm(d);
        }
        mb.flags = MBF_INTRA;
        if (it == 0) {
            mb.flags |= MBF_INXN;
        } else {
            mb.flags |= MBF_I16;
            mb.i16_mode = (it - 1) & 3;
            mb.cbp = (((it - 1) >> 2) % 3) << 4 | ((it - 1) >= 12 ? 15 : 0);
        }
        reset_mb_arrays(d, mb.flags);
        if ((mb.flags & MBF_I16) && !intra_mode_possible(d, mb.i16_mode, 0)) return -1;
        if (mb.flags & MBF_INXN) {
            const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
            if (d->cpps->transform_8x8 && DEC(d, 399 + ((a && (a->flags & MBF_T8)) ? 1 : 0) + ((b && (b->flags & MBF_T8)) ? 1 : 0))) mb.t8 = 1;
            read_intra_modes(d, &mb);
        }
        {
            const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
            const int ca = a && a->chroma_mode != 0, cb = b && b->chroma_mode != 0;
            int cm = 0;
            if (DEC(d, 64 + ca + cb)) {
                cm = 1;
                if (DEC(d, 64 + 3)) cm = 2 + DEC(d, 64 + 3);
            }
            mb.chroma_mode = cm;
            d->mbi[d->mb_addr].chroma_mode = (uint8_t)cm;
            if (!intra_mode_possible(d, cm, 1)) return -1;
        }
    } else {
        mb.flags = 0;
        if (type == 1 && t == 0) mb.flags |= MBF_DIRECT;
        reset_mb_arrays(d, mb.flags);
        h264_mbinfo *m = &d->mbi[d->mb_addr];
        const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4, gx8 = d->mb_x * 2, gy8 = d->mb_y * 2;
        if (type == 0) {
            mb.shape = t;
            mb.pmode[0] = mb.pmode[1] = 1;
            for (int q = 0; q < 4; q++) mb.sub_mode[q] = 1;
        } else if (t == 0) {
            mb.direct_mask = 15;
        } else if (t <= 3) {
            mb.shape = 0; mb.pmode[0] = t;
        } else if (t < 22) {
            static const uint8_t pm[9][2] = {{1, 1}, {2, 2}, {1, 2}, {2, 1}, {1, 3}, {2, 3}, {3, 1}, {3, 2}, {3, 3}};
            mb.shape = (t & 1) ? 2 : 1;
            mb.pmode[0] = pm[(t - 4) / 2][0]; mb.pmode[1] = pm[(t - 4) / 2][1];
        } else {
            mb.shape = 3;
        }
        if (mb.shape == 3 || (type == 0 && t == 3)) {
            mb.shape = 3;
            for (int q = 0; q < 4; q++) {
                if (type == 0) {
                    const int st = read_p_sub_type(d);
                    mb.sub[q] = st; mb.sub_shape[q] = st; mb.sub_mode[q] = 1;
                } else {
                    const int st = read_b_sub_type(d);
                    mb.sub[q] = st;
                    if (st == 0) { mb.direct_mask |= 1 << q; mb.sub_shape[q] = 0; mb.sub_mode[q] = 0; }
                    else if (st <= 3) { mb.sub_shape[q] = 0; mb.sub_mode[q] = st; }
                    else if (st <= 9) { mb.sub_shape[q] = 1 + ((st - 4) & 1); mb.sub_mode[q] = 1 + (st - 4) / 2; }
                    else { mb.sub_shape[q] = 3; mb.sub_mode[q] = st - 9; }
                }
            }
        }
        m->direct8 = (uint8_t)mb.direct_mask;
        if (mb.direct_mask) direct_motion(d, mb.direct_mask);
        const int nlists = type == 1 ? 2 : 1;
        /* reference indices */
        for (int l = 0; l < nlists; l++) {
            if (mb.shape == 0 && !mb.direct_mask) {
                if (mb.pmode[0] & (1 << l)) {
                    const int r = d->sl.num_ref[l] > 1 ? read_ref_idx(d, l, gx8, gy8) : 0;
                    if (r < 0 || r >= d->sl.num_ref[l]) return -1;
                    set_ref(d, l, gx8, gy8, 2, 2, r);
                }
            } else if (mb.shape == 1) {
                for (int p = 0; p < 2; p++) {
                    if (!(mb.pmode[p] & (1 << l))) continue;
                    const int r = d->sl.num_ref[l] > 1 ? read_ref_idx(d, l, gx8, gy8 + p) : 0;
                    if (r < 0 || r >= d->sl.num_ref[l]) return -1;
                    set_ref(d, l, gx8, gy8 + p, 2, 1, r);
                }
            } else if (mb.shape == 2) {
                for (int p = 0; p < 2; p++) {
                    if (!(mb.pmode[p] & (1 << l))) continue;
                    const int r = d->sl.num_ref[l] > 1 ? read_ref_idx(d, l, gx8 + p, gy8) : 0;
                    if (r < 0 || r >= d->sl.num_ref[l]) return -1;
                    set_ref(d, l, gx8 + p, gy8, 1, 2, r);
                }
            } else if (mb.shape == 3) {
                for (int q = 0; q < 4; q++) {
                    if (mb.direct_mask & (1 << q) || !(mb.sub_mode[q] & (1 << l))) continue;
                    const int r = d->sl.num_ref[l] > 1 ? read_ref_idx(d, l, gx8 + (q & 1), gy8 + (q >> 1)) : 0;
                    if (r < 0 || r >= d->sl.num_ref[l]) return -1;
                    set_ref(d, l, gx8 + (q & 1), gy8 + (q >> 1), 1, 1, r);
                }
            }
        }
        /* motion vectors */
        for (int l = 0; l < nlists; l++) {
            if (mb.shape == 0 && !mb.direct_mask) {
                if (mb.pmode[0] & (1 << l)) parse_partition_mv(d, l, gx0, gy0, 4, 4, 0, 0);
            } else if (mb.shape == 1) {
                for (int p = 0; p < 2; p++)
                    if (mb.pmode[p] & (1 << l)) parse_partition_mv(d, l, gx0, gy0 + 2 * p, 4, 2, 1, p);
            } else if (mb.shape == 2) {
                for (int p = 0; p < 2; p++)
                    if (mb.pmode[p] & (1 << l)) parse_partition_mv(d, l, gx0 + 2 * p, gy0, 2, 4, 2, p);
            } else if (mb.shape == 3) {
                for (int q = 0; q < 4; q++) {
                    if ((mb.direct_mask & (1 << q)) || !(mb.sub_mode[q] & (1 << l))) continue;
                    const int qx = gx0 + (q & 1) * 2, qy = gy0 + (q >> 1) * 2;
                    switch (mb.sub_shape[q]) {
                    case 0: parse_partition_mv(d, l, qx, qy, 2, 2, 0, 0); break;
                    case 1: parse_partition_mv(d, l, qx, qy, 2, 1, 0, 0); parse_partition_mv(d, l, qx, qy + 1, 2, 1, 0, 0); break;
                    case 2: parse_partition_mv(d, l, qx, qy, 1, 2, 0, 0); parse_partition_mv(d, l, qx + 1, qy, 1, 2, 0, 0); break;
                    default:
                        for (int s = 0; s < 4; s++) parse_partition_mv(d, l, qx + (s & 1), qy + (s >> 1), 1, 1, 0, 0);
                        break;
                    }
                }
            }
        }
    }

    if (!(mb.flags & MBF_I16)) {
        mb.cbp = read_cbp(d);
        if (!(mb.flags & MBF_INTRA) && d->cpps->transform_8x8 && (mb.cbp & 15)) {
            int allowed = 1;
            if (mb.direct_mask && !d->csps->direct_8x8_inference) allowed = 0;
            if (mb.shape == 3) {
                for (int q = 0; q < 4; q++) {
                    if (mb.direct_mask & (1 << q)) { if (!d->csps->direct_8x8_inference) allowed = 0; }
                    else if (mb.sub_shape[q] != 0) allowed = 0;
                }
            }
            if (allowed) {
                const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
                mb.t8 = DEC(d, 399 + ((a && (a->flags & MBF_T8)) ? 1 : 0) + ((b && (b->flags & MBF_T8)) ? 1 : 0));
            }
        }
    }
    h264_mbinfo *m = &d->mbi[d->mb_addr];
    m->cbp = (uint8_t)mb.cbp;
    if (mb.t8) { m->flags |= MBF_T8; mb.flags |= MBF_T8; }
    if (mb.cbp || (mb.flags & MBF_I16)) {
        int dqp = 0;
        if (DEC(d, 60 + (d->last_dqp != 0))) {
            int val = 1, ctx = 2;
            while (DEC(d, 60 + ctx)) {
                ctx = 3;
                if (++val > 104) return -1;
            }
            dqp = (val & 1) ? (val + 1) >> 1 : -((val + 1) >> 1);
        }
        d->last_dqp = dqp;
        d->qp = ((d->qp + dqp + 52) % 52);
        m->qp = (int8_t)d->qp;
        parse_residual(d, &mb);
    } else {
        d->last_dqp = 0;
        memset(d->coef, 0, sizeof d->coef);
        memset(d->blk_coded, 0, sizeof d->blk_coded);
        memset(d->dc, 0, sizeof d->dc);
    }
    const int qp = d->qp;

    /* reconstruction */
    if (mb.flags & MBF_INTRA) {
        d->cur->mbintra[d->mb_addr] = 1;
        if (mb.flags & MBF_I16) {
            intra16(d, mb.i16_mode);
            if (d->blk_coded[16] || (mb.cbp & 15)) {
                luma_dc_transform(d, qp);
                for (int b = 0; b < 16; b++) add_luma_residual(d, &mb, b, qp);
            }
        } else if (mb.t8) {
            for (int b8 = 0; b8 < 4; b8++) {
                const int bx = (b8 & 1) * 2, by = (b8 >> 1) * 2;
                intra_nxn(d, bx, by, 8, d->ipm[(d->mb_y * 4 + by) * d->w4 + d->mb_x * 4 + bx]);
                add_luma8_residual(d, b8, qp);
            }
        } else {
            for (int b = 0; b < 16; b++) {
                const int bx = blk_x(b), by = blk_y(b);
                intra_nxn(d, bx, by, 4, d->ipm[(d->mb_y * 4 + by) * d->w4 + d->mb_x * 4 + bx]);
                add_luma_residual(d, &mb, b, qp);
            }
        }
        intra_chroma(d, mb.chroma_mode);
    } else {
        d->cur->mbintra[d->mb_addr] = 0;
        commit_motion(d);
        h264_mc_macroblock(d);
        if (mb.t8) for (int b8 = 0; b8 < 4; b8++) add_luma8_residual(d, b8, qp);
        else for (int b = 0; b < 16; b++) add_luma_residual(d, &mb, b, qp);
    }
    if (mb.flags & MBF_INTRA) commit_motion(d);
    reconstruct_chroma_residual(d, &mb);
    return 0;
}

/* P_Skip and B_Skip. */
static void decode_skip(h264_dec *d)
{
    const int type = d->sl.type;
    reset_mb_arrays(d, MBF_SKIP | (type == 1 ? MBF_DIRECT : 0));
    h264_mbinfo *m = &d->mbi[d->mb_addr];
    d->last_dqp = 0;
    d->cur->mbintra[d->mb_addr] = 0;
    const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4;
    if (type == 0) {
        nbinfo A, B;
        get_nb(d, 0, gx0 - 1, gy0, 0, &A);
        get_nb(d, 0, gx0, gy0 - 1, 0, &B);
        int mv[2] = {0, 0};
        if (A.avail && B.avail && !(A.ref == 0 && A.mv[0] == 0 && A.mv[1] == 0) && !(B.ref == 0 && B.mv[0] == 0 && B.mv[1] == 0)) {
            mv_pred(d, 0, 0, gx0, gy0, 4, 0, 0, mv);
        }
        set_ref(d, 0, d->mb_x * 2, d->mb_y * 2, 2, 2, 0);
        set_mv(d, 0, gx0, gy0, 4, 4, mv[0], mv[1]);
    } else {
        m->direct8 = 15;
        direct_motion(d, 15);
    }
    commit_motion(d);
    h264_mc_macroblock(d);
}

static int read_skip_flag(h264_dec *d)
{
    const h264_mbinfo *a = nb_mb(d, -1, 0), *b = nb_mb(d, 0, -1);
    int inc = (a && !(a->flags & MBF_SKIP)) + (b && !(b->flags & MBF_SKIP));
    if (d->sl.type == 1) inc += 13;
    return DEC(d, 11 + inc);
}

int h264_decode_slice_data(h264_dec *d, const uint8_t *data, size_t size, size_t bit_pos)
{
    init_scans();
    init_contexts(d);
    cabac_start(&d->cabac, data, (bit_pos + 7) >> 3, size);
    d->qp = d->sl.qp;
    d->last_dqp = 0;
    int addr = d->sl.first_mb;
    for (;;) {
        if (addr < 0 || addr >= d->nmbs) return -1;
        d->mb_addr = addr;
        d->mb_x = addr % d->mb_w;
        d->mb_y = addr / d->mb_w;
        int skip = 0;
        if (d->sl.type != 2) skip = read_skip_flag(d);
        d->mbi[addr].slice = (int16_t)d->sl.number;
        if (skip) {
            decode_skip(d);
        } else if (decode_macroblock(d) < 0) {
            return -1;
        }
        addr++;
        if (terminate(&d->cabac)) break;
    }
    return 0;
}
