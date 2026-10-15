/*
 * Compact H.264/AVC decoder -- inter prediction: quarter-sample luma interpolation, eighth-sample chroma
 * interpolation and weighted prediction.
 *
 * Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>

#include "h264_int.h"

#define clip255 h264_clip_pel
#define clampi(v, lo, hi) h264_clip3(lo, hi, v)

#define PS 24   /* stride of the fetched patch: at most 16 + 5 */

static void fetch_patch(const uint8_t *plane, int stride, int pw, int ph, int x0, int y0, int w, int h, uint8_t *out)
{
    for (int j = 0; j < h; j++) {
        const uint8_t *row = plane + (size_t)clampi(y0 + j, 0, ph - 1) * stride;
        for (int i = 0; i < w; i++) out[j * PS + i] = row[clampi(x0 + i, 0, pw - 1)];
    }
}

/* Predicts a w x h luma block whose top-left sample is (x, y) in the reference, displaced by the quarter-sample mv. */
static void luma_mc(const h264_pic *ref, int pw, int ph, int x, int y, int w, int h, int mvx, int mvy, uint8_t *dst, int dstride)
{
    const int fx = mvx & 3, fy = mvy & 3;
    const int ix = x + (mvx >> 2), iy = y + (mvy >> 2);
    uint8_t patch[PS * PS];
    fetch_patch(ref->p[0], ref->stride[0], pw, ph, ix - 2, iy - 2, w + 5, h + 5, patch);
#define G(i, j) ((int)patch[((j) + 2) * PS + (i) + 2])
#define TAPH(i, j) (G((i) - 2, j) - 5 * G((i) - 1, j) + 20 * G(i, j) + 20 * G((i) + 1, j) - 5 * G((i) + 2, j) + G((i) + 3, j))
#define TAPV(i, j) (G(i, (j) - 2) - 5 * G(i, (j) - 1) + 20 * G(i, j) + 20 * G(i, (j) + 1) - 5 * G(i, (j) + 2) + G(i, (j) + 3))
    /* The half-sample planes (8.4.2.2.1), each computed once for the block and only if the position reads it: b
     * horizontal (rows 0..h: s is b one row down), h vertical (columns 0..w: m is h one column right), j the centre,
     * from the unclipped horizontal taps of rows -2..h+2. */
    uint8_t sb[(16 + 1) * PS], sh[16 * PS], sj[16 * PS];
    const int need_b = fx && fy != 2, need_h = fy && fx != 2, need_j = (fx == 2 && fy) || (fy == 2 && fx);
    if (need_b)
        for (int j = 0; j <= h; j++)
            for (int i = 0; i < w; i++) sb[j * PS + i] = (uint8_t)clip255((TAPH(i, j) + 16) >> 5);
    if (need_h)
        for (int j = 0; j < h; j++)
            for (int i = 0; i <= w; i++) sh[j * PS + i] = (uint8_t)clip255((TAPV(i, j) + 16) >> 5);
    if (need_j) {
        int t[(16 + 5) * 16];
        for (int j = -2; j < h + 3; j++)
            for (int i = 0; i < w; i++) t[(j + 2) * 16 + i] = TAPH(i, j);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                const int *c = t + (j + 2) * 16 + i;
                sj[j * PS + i] = (uint8_t)clip255((c[-32] - 5 * c[-16] + 20 * c[0] + 20 * c[16] - 5 * c[32] + c[48] + 512) >> 10);
            }
    }
#define HB(i, j) sb[(j) * PS + (i)]
#define HV(i, j) sh[(j) * PS + (i)]
#define HJ(i, j) sj[(j) * PS + (i)]
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int v;
            switch (fy * 4 + fx) {
            case 0: v = G(i, j); break;
            case 1: v = (G(i, j) + HB(i, j) + 1) >> 1; break;
            case 2: v = HB(i, j); break;
            case 3: v = (G(i + 1, j) + HB(i, j) + 1) >> 1; break;
            case 4: v = (G(i, j) + HV(i, j) + 1) >> 1; break;
            case 5: v = (HB(i, j) + HV(i, j) + 1) >> 1; break;
            case 6: v = (HB(i, j) + HJ(i, j) + 1) >> 1; break;
            case 7: v = (HB(i, j) + HV(i + 1, j) + 1) >> 1; break;
            case 8: v = HV(i, j); break;
            case 9: v = (HV(i, j) + HJ(i, j) + 1) >> 1; break;
            case 10: v = HJ(i, j); break;
            case 11: v = (HJ(i, j) + HV(i + 1, j) + 1) >> 1; break;
            case 12: v = (G(i, j + 1) + HV(i, j) + 1) >> 1; break;
            case 13: v = (HV(i, j) + HB(i, j + 1) + 1) >> 1; break;
            case 14: v = (HJ(i, j) + HB(i, j + 1) + 1) >> 1; break;
            default: v = (HV(i + 1, j) + HB(i, j + 1) + 1) >> 1; break;
            }
            dst[j * dstride + i] = (uint8_t)v;
        }
    }
#undef G
#undef TAPH
#undef TAPV
#undef HB
#undef HV
#undef HJ
}

static void chroma_mc(const h264_pic *ref, int comp, int pw, int ph, int x, int y, int w, int h, int mvx, int mvy, uint8_t *dst, int dstride)
{
    const int fx = mvx & 7, fy = mvy & 7;
    const int ix = x + (mvx >> 3), iy = y + (mvy >> 3);
    uint8_t patch[PS * PS];
    fetch_patch(ref->p[comp], ref->stride[comp], pw, ph, ix, iy, w + 1, h + 1, patch);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            const int a = patch[j * PS + i], b = patch[j * PS + i + 1], c = patch[(j + 1) * PS + i], d = patch[(j + 1) * PS + i + 1];
            dst[j * dstride + i] = (uint8_t)(((8 - fx) * (8 - fy) * a + fx * (8 - fy) * b + (8 - fx) * fy * c + fx * fy * d + 32) >> 6);
        }
    }
}

/* Predicts one block of the current macroblock (global 4x4 position gx, gy; size w4 x h4 blocks) and stores it. */
static void mc_block(h264_dec *d, int gx, int gy, int w4, int h4)
{
    h264_pic *cur = d->cur;
    const h264_slice *sl = &d->sl;
    const h264_pps *pps = d->cpps;
    const int b8 = (gy >> 1) * d->w8 + (gx >> 1);
    const int ref[2] = {cur->refi[0][b8], cur->refi[1][b8]};
    const int x = gx * 4, y = gy * 4, w = w4 * 4, h = h4 * 4;
    const int pw = d->mb_w * 16, ph = d->mb_h * 16;
    uint8_t pl[2][256], pc[2][2][64];
    for (int l = 0; l < 2; l++) {
        if (ref[l] < 0) continue;
        const h264_pic *rp = sl->list[l][ref[l]];
        const int mvx = cur->mv[l][(gy * d->w4 + gx) * 2], mvy = cur->mv[l][(gy * d->w4 + gx) * 2 + 1];
        luma_mc(rp, pw, ph, x, y, w, h, mvx, mvy, pl[l], 16);
        for (int c = 0; c < 2; c++) chroma_mc(rp, 1 + c, pw / 2, ph / 2, x / 2, y / 2, w / 2, h / 2, mvx, mvy, pc[l][c], 8);
    }
    const int bi = ref[0] >= 0 && ref[1] >= 0;
    const int explicit_w = (sl->type == 0 && pps->weighted_pred) || (sl->type == 1 && pps->weighted_bipred == 1);
    const int implicit_w = sl->type == 1 && pps->weighted_bipred == 2 && bi;
    for (int comp = 0; comp < 3; comp++) {
        const int cw = comp ? w / 2 : w, ch = comp ? h / 2 : h;
        const int stride = cur->stride[comp];
        uint8_t *dst = cur->p[comp] + (size_t)(comp ? y / 2 : y) * stride + (comp ? x / 2 : x);
        const uint8_t *s0 = comp ? pc[0][comp - 1] : pl[0], *s1 = comp ? pc[1][comp - 1] : pl[1];
        const int ss = comp ? 8 : 16;
        int w0 = 0, w1 = 0, o0 = 0, o1 = 0, logwd = 0, weighted = 0;
        if (explicit_w) {
            weighted = 1;
            logwd = comp ? sl->chroma_denom : sl->luma_denom;
            if (ref[0] >= 0) { w0 = comp ? sl->cw[0][ref[0]][comp - 1] : sl->lw[0][ref[0]]; o0 = comp ? sl->co[0][ref[0]][comp - 1] : sl->lo[0][ref[0]]; }
            if (ref[1] >= 0) { w1 = comp ? sl->cw[1][ref[1]][comp - 1] : sl->lw[1][ref[1]]; o1 = comp ? sl->co[1][ref[1]][comp - 1] : sl->lo[1][ref[1]]; }
        } else if (implicit_w) {
            weighted = 1;
            logwd = 5;
            w1 = sl->implicit_w1[ref[0]][ref[1]];
            w0 = 64 - w1;
        }
        for (int j = 0; j < ch; j++) {
            for (int i = 0; i < cw; i++) {
                int v;
                if (bi) {
                    const int a = s0[j * ss + i], b = s1[j * ss + i];
                    v = weighted ? clip255(((a * w0 + b * w1 + (1 << logwd)) >> (logwd + 1)) + ((o0 + o1 + 1) >> 1)) : (a + b + 1) >> 1;
                } else {
                    const int a = (ref[0] >= 0 ? s0 : s1)[j * ss + i];
                    if (explicit_w) {
                        const int ww = ref[0] >= 0 ? w0 : w1, oo = ref[0] >= 0 ? o0 : o1;
                        v = logwd >= 1 ? clip255(((a * ww + (1 << (logwd - 1))) >> logwd) + oo) : clip255(a * ww + oo);
                    } else {
                        v = a;
                    }
                }
                dst[j * stride + i] = (uint8_t)v;
            }
        }
    }
}

void h264_mc_macroblock(h264_dec *d)
{
    const h264_pic *cur = d->cur;
    const int gx0 = d->mb_x * 4, gy0 = d->mb_y * 4;
    int same_mb = 1;
    for (int q = 0; q < 4; q++) {
        const int gx = gx0 + (q & 1) * 2, gy = gy0 + (q >> 1) * 2;
        int same = 1;
        for (int l = 0; l < 2 && same; l++) {
            const int16_t *m = cur->mv[l];
            const int i0 = (gy * d->w4 + gx) * 2;
            if (cur->refi[l][(gy >> 1) * d->w8 + (gx >> 1)] < 0) continue;
            for (int s = 1; s < 4; s++) {
                const int i = ((gy + (s >> 1)) * d->w4 + gx + (s & 1)) * 2;
                if (m[i] != m[i0] || m[i + 1] != m[i0 + 1]) { same = 0; break; }
            }
        }
        if (!same) same_mb = 0;
        else {
            /* quadrant uniform: compare with quadrant 0 for a whole-macroblock prediction */
            for (int l = 0; l < 2 && same_mb; l++) {
                const int b0 = (gy0 >> 1) * d->w8 + (gx0 >> 1), bq = (gy >> 1) * d->w8 + (gx >> 1);
                if (cur->refi[l][b0] != cur->refi[l][bq]) same_mb = 0;
                else if (cur->refi[l][b0] >= 0) {
                    const int i0 = (gy0 * d->w4 + gx0) * 2, iq = (gy * d->w4 + gx) * 2;
                    if (cur->mv[l][i0] != cur->mv[l][iq] || cur->mv[l][i0 + 1] != cur->mv[l][iq + 1]) same_mb = 0;
                }
            }
        }
    }
    if (same_mb) { mc_block(d, gx0, gy0, 4, 4); return; }
    for (int q = 0; q < 4; q++) {
        const int gx = gx0 + (q & 1) * 2, gy = gy0 + (q >> 1) * 2;
        int same = 1;
        for (int l = 0; l < 2 && same; l++) {
            if (cur->refi[l][(gy >> 1) * d->w8 + (gx >> 1)] < 0) continue;
            const int i0 = (gy * d->w4 + gx) * 2;
            for (int s = 1; s < 4; s++) {
                const int i = ((gy + (s >> 1)) * d->w4 + gx + (s & 1)) * 2;
                if (cur->mv[l][i] != cur->mv[l][i0] || cur->mv[l][i + 1] != cur->mv[l][i0 + 1]) { same = 0; break; }
            }
        }
        if (same) mc_block(d, gx, gy, 2, 2);
        else for (int s = 0; s < 4; s++) mc_block(d, gx + (s & 1), gy + (s >> 1), 1, 1);
    }
}
