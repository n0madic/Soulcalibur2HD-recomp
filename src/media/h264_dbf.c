/*
 * Compact H.264/AVC decoder -- the in-loop deblocking filter (frame macroblocks, 4:2:0).
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "h264_int.h"
#include "h264_tables.h"

#define clip3 h264_clip3
#define clip1 h264_clip_pel

/* Filters `len` sample lines across an edge. pix points at q0 of the first line; xs steps across the edge, ys along it. */
static void filter_luma(uint8_t *pix, int xs, int ys, int len, int bs, int alpha, int beta, int index_a)
{
    for (int k = 0; k < len; k++, pix += ys) {
        const int p0 = pix[-xs], p1 = pix[-2 * xs], q0 = pix[0], q1 = pix[xs];
        if (abs(p0 - q0) >= alpha || abs(p1 - p0) >= beta || abs(q1 - q0) >= beta) continue;
        const int p2 = pix[-3 * xs], q2 = pix[2 * xs];
        const int ap = abs(p2 - p0), aq = abs(q2 - q0);
        if (bs < 4) {
            const int tc0 = h264_tc0[index_a * 3 + bs - 1];
            const int tc = tc0 + (ap < beta) + (aq < beta);
            const int delta = clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
            pix[-xs] = clip1(p0 + delta);
            pix[0] = clip1(q0 - delta);
            if (ap < beta) pix[-2 * xs] = (uint8_t)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 * 2)) >> 1));
            if (aq < beta) pix[xs] = (uint8_t)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 * 2)) >> 1));
        } else {
            const int small = abs(p0 - q0) < ((alpha >> 2) + 2);
            if (ap < beta && small) {
                const int p3 = pix[-4 * xs];
                pix[-xs] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                pix[-2 * xs] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                pix[-3 * xs] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else {
                pix[-xs] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            }
            if (aq < beta && small) {
                const int q3 = pix[3 * xs];
                pix[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                pix[xs] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                pix[2 * xs] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else {
                pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }
}

static void filter_chroma(uint8_t *pix, int xs, int ys, int len, int bs, int alpha, int beta, int index_a)
{
    for (int k = 0; k < len; k++, pix += ys) {
        const int p0 = pix[-xs], p1 = pix[-2 * xs], q0 = pix[0], q1 = pix[xs];
        if (abs(p0 - q0) >= alpha || abs(p1 - p0) >= beta || abs(q1 - q0) >= beta) continue;
        if (bs < 4) {
            const int tc = h264_tc0[index_a * 3 + bs - 1] + 1;
            const int delta = clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
            pix[-xs] = clip1(p0 + delta);
            pix[0] = clip1(q0 - delta);
        } else {
            pix[-xs] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static int mv_far(const int16_t *a, const int16_t *b) { return abs(a[0] - b[0]) >= 4 || abs(a[1] - b[1]) >= 4; }

/* 1 if the motion of the two 4x4 blocks differs enough to filter (bS = 1). */
static int motion_differs(const h264_dec *d, int gxp, int gyp, int gxq, int gyq)
{
    const h264_pic *c = d->cur;
    const int bp = (gyp >> 1) * d->w8 + (gxp >> 1), bq = (gyq >> 1) * d->w8 + (gxq >> 1);
    const int rp[2] = {c->refid[0][bp], c->refid[1][bp]}, rq[2] = {c->refid[0][bq], c->refid[1][bq]};
    const int16_t *mp[2] = {&c->mv[0][(gyp * d->w4 + gxp) * 2], &c->mv[1][(gyp * d->w4 + gxp) * 2]};
    const int16_t *mq[2] = {&c->mv[0][(gyq * d->w4 + gxq) * 2], &c->mv[1][(gyq * d->w4 + gxq) * 2]};
    const int np = (rp[0] >= 0) + (rp[1] >= 0), nq = (rq[0] >= 0) + (rq[1] >= 0);
    if (np != nq) return 1;
    if (np == 1) {
        const int lp = rp[0] >= 0 ? 0 : 1, lq = rq[0] >= 0 ? 0 : 1;
        if (rp[lp] != rq[lq]) return 1;
        return mv_far(mp[lp], mq[lq]);
    }
    if (np == 0) return 0;
    if (!((rp[0] == rq[0] && rp[1] == rq[1]) || (rp[0] == rq[1] && rp[1] == rq[0]))) return 1;
    if (rp[0] != rp[1]) {
        if (rp[0] == rq[0]) return mv_far(mp[0], mq[0]) || mv_far(mp[1], mq[1]);
        return mv_far(mp[0], mq[1]) || mv_far(mp[1], mq[0]);
    }
    return (mv_far(mp[0], mq[0]) || mv_far(mp[1], mq[1])) && (mv_far(mp[0], mq[1]) || mv_far(mp[1], mq[0]));
}

static int boundary_strength(const h264_dec *d, int gxp, int gyp, int gxq, int gyq, int mb_edge)
{
    const h264_mbinfo *mp = &d->mbi[(gyp >> 2) * d->mb_w + (gxp >> 2)], *mq = &d->mbi[(gyq >> 2) * d->mb_w + (gxq >> 2)];
    if ((mp->flags | mq->flags) & MBF_INTRA) return mb_edge ? 4 : 3;
    if (d->nnz_l[gyp * d->w4 + gxp] || d->nnz_l[gyq * d->w4 + gxq]) return 2;
    return motion_differs(d, gxp, gyp, gxq, gyq);
}

static void deblock_mb(h264_dec *d, int mx, int my)
{
    const int addr = my * d->mb_w + mx;
    const h264_mbinfo *m = &d->mbi[addr];
    if (m->dbf_idc == 1) return;
    const h264_mbinfo *left = mx > 0 ? &d->mbi[addr - 1] : NULL, *top = my > 0 ? &d->mbi[addr - d->mb_w] : NULL;
    if (left && m->dbf_idc == 2 && left->slice != m->slice) left = NULL;
    if (top && m->dbf_idc == 2 && top->slice != m->slice) top = NULL;
    const int t8 = m->flags & MBF_T8;
    const int gx0 = mx * 4, gy0 = my * 4;
    int bs_v[4][4], bs_h[4][4];
    for (int e = 0; e < 4; e++) {
        for (int s = 0; s < 4; s++) {
            bs_v[e][s] = 0; bs_h[e][s] = 0;
            if (e == 0 ? left != NULL : !(t8 && (e & 1))) bs_v[e][s] = boundary_strength(d, gx0 + e - 1, gy0 + s, gx0 + e, gy0 + s, e == 0);
            if (e == 0 ? top != NULL : !(t8 && (e & 1))) bs_h[e][s] = boundary_strength(d, gx0 + s, gy0 + e - 1, gx0 + s, gy0 + e, e == 0);
        }
    }
    const int qp_q = m->qp;
    const int off_a = m->dbf_alpha, off_b = m->dbf_beta;
    const int stride = d->cur->stride[0];
    uint8_t *y = d->cur->p[0] + (size_t)(my * 16) * stride + mx * 16;
    for (int dir = 0; dir < 2; dir++) {
        for (int e = 0; e < 4; e++) {
            const h264_mbinfo *nb = e == 0 ? (dir == 0 ? left : top) : m;
            if (!nb) continue;
            const int qpav = (nb->qp + qp_q + 1) >> 1;
            const int index_a = clip3(0, 51, qpav + off_a), index_b = clip3(0, 51, qpav + off_b);
            const int alpha = h264_alpha[index_a], beta = h264_beta[index_b];
            if (!alpha || !beta) continue;
            for (int s = 0; s < 4; s++) {
                const int bs = dir == 0 ? bs_v[e][s] : bs_h[e][s];
                if (!bs) continue;
                if (dir == 0) filter_luma(y + s * 4 * stride + e * 4, 1, stride, 4, bs, alpha, beta, index_a);
                else filter_luma(y + e * 4 * stride + s * 4, stride, 1, 4, bs, alpha, beta, index_a);
            }
        }
    }
    for (int c = 0; c < 2; c++) {
        const int cs = d->cur->stride[1 + c];
        uint8_t *p = d->cur->p[1 + c] + (size_t)(my * 8) * cs + mx * 8;
        const int cq = h264_chroma_qp(m->chroma_qp_offset[c], qp_q);
        for (int dir = 0; dir < 2; dir++) {
            for (int e = 0; e < 2; e++) {
                const h264_mbinfo *nb = e == 0 ? (dir == 0 ? left : top) : m;
                if (!nb) continue;
                const int qpav = (h264_chroma_qp(nb->chroma_qp_offset[c], nb->qp) + cq + 1) >> 1;
                const int index_a = clip3(0, 51, qpav + off_a), index_b = clip3(0, 51, qpav + off_b);
                const int alpha = h264_alpha[index_a], beta = h264_beta[index_b];
                if (!alpha || !beta) continue;
                for (int s = 0; s < 4; s++) {
                    const int bs = dir == 0 ? bs_v[e * 2][s] : bs_h[e * 2][s];
                    if (!bs) continue;
                    if (dir == 0) filter_chroma(p + s * 2 * cs + e * 4, 1, cs, 2, bs, alpha, beta, index_a);
                    else filter_chroma(p + e * 4 * cs + s * 2, cs, 1, 2, bs, alpha, beta, index_a);
                }
            }
        }
    }
}

void h264_deblock_picture(h264_dec *d)
{
    for (int my = 0; my < d->mb_h; my++)
        for (int mx = 0; mx < d->mb_w; mx++)
            if (d->mbi[my * d->mb_w + mx].slice >= 0) deblock_mb(d, mx, my);
}
