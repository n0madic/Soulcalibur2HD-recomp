/* Internals shared by the H.264 decoder's files. Copyright (C) the sc2hd authors. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SC2HD_H264_INT_H
#define SC2HD_H264_INT_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "h264.h"

#define H264_MAX_PICS 20
#define H264_MAX_REFS 32     /* entries of a reference list */
#define H264_MAX_MBS 139264  /* the largest picture of level 6.2, in macroblocks */

static inline int h264_clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static inline int h264_clip_pel(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline int h264_min(int a, int b) { return a < b ? a : b; }
static inline int h264_max(int a, int b) { return a > b ? a : b; }

/* QPC of a macroblock whose luma QP is `qp`, for the chroma_qp_index_offset `offset` (8.5.8). */
static inline int h264_chroma_qp(int offset, int qp)
{
    static const uint8_t tab[22] = {29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};
    const int q = h264_clip3(0, 51, qp + offset);
    return q < 30 ? q : tab[q - 30];
}

typedef struct { const uint8_t *buf; size_t pos, nbits; } bitreader;

typedef struct {
    int valid;
    int profile, level;
    int log2_max_frame_num, poc_type, log2_max_poc_lsb;
    int max_num_ref_frames;
    int mb_w, mb_h;
    int frame_mbs_only, direct_8x8_inference;
    int crop_l, crop_r, crop_t, crop_b;       /* luma samples */
    int full_range, matrix;
} h264_sps;

typedef struct {
    int valid;
    int sps_id;
    int bottom_poc_present;
    int num_ref_default[2];
    int weighted_pred, weighted_bipred;
    int init_qp, init_qs;
    int chroma_qp_offset[2];
    int deblocking_control, constrained_intra, redundant_pic_cnt;
    int transform_8x8;
} h264_pps;

/* Per macroblock state kept for the whole picture: neighbour contexts and the deblocking filter. */
enum {
    MBF_INTRA = 1, MBF_I16 = 2, MBF_PCM = 4, MBF_SKIP = 8, MBF_DIRECT = 16, MBF_T8 = 32, MBF_INXN = 64,
};
typedef struct {
    int16_t slice;                /* number of the slice that decoded it in this picture; -1 = not yet */
    uint8_t flags;
    uint8_t cbp;                  /* luma bits 0..3, chroma value << 4 */
    uint8_t chroma_mode;          /* intra_chroma_pred_mode, 0 for everything else */
    uint8_t cbf_dc;               /* coded_block_flag of the luma (bit 0) and chroma (1, 2) DC blocks */
    uint8_t direct8;              /* bit per 8x8 quadrant predicted in direct mode */
    int8_t qp;
    int8_t dbf_idc, dbf_alpha, dbf_beta;      /* deblocking parameters of its slice */
    int8_t chroma_qp_offset[2];               /* chroma_qp_index_offset of its slice's PPS (Cb, Cr) */
} h264_mbinfo;

typedef struct h264_pic {
    uint8_t *buf;
    uint8_t *p[3];
    int stride[3];
    int id;                       /* unique per decoded picture; identifies a reference picture */
    int poc, frame_num, frame_num_wrap;
    int ref;                      /* 0 unused, 1 short term, 2 long term */
    int long_idx;
    int busy;                     /* holds a picture (decoding it, a reference, or just output) */
    int16_t *mv[2];               /* per 4x4 block: x, y */
    int8_t *refi[2];              /* per 8x8 block: reference index in its slice's list, -1 unused */
    int32_t *refid[2];            /* per 8x8 block: id of the picture referenced, -1 unused */
    uint8_t *mbintra;             /* per macroblock */
} h264_pic;

typedef struct {
    int first_mb, type;           /* type: 0 P, 1 B, 2 I */
    int pps_id, frame_num, nal_ref_idc, idr, idr_pic_id;
    int poc_lsb, delta_poc_bottom;
    int direct_spatial, num_ref[2];
    int qp, dbf_idc, dbf_alpha, dbf_beta, cabac_init_idc;
    int mod_n[2];
    struct { int op, val; } mod[2][H264_MAX_REFS + 1];
    int luma_denom, chroma_denom;
    int lw[2][H264_MAX_REFS], lo[2][H264_MAX_REFS];
    int cw[2][H264_MAX_REFS][2], co[2][H264_MAX_REFS][2];
    int mmco_n, long_term_reference, adaptive_marking;
    struct { int op, a, b; } mmco[34];
    h264_pic *list[2][H264_MAX_REFS];
    int list_n[2];
    int number;                   /* index of the slice in its picture */
    int implicit_w1[H264_MAX_REFS][H264_MAX_REFS];   /* implicit bi-prediction weight of list 1 (list 0 gets 64 - w1) */
    int dist_scale[H264_MAX_REFS];                   /* temporal direct DistScaleFactor by list 0 index */
} h264_slice;

typedef struct {
    const uint8_t *buf;
    size_t pos, nbits;
    uint32_t range, offset;
} h264_cabac;

struct h264_dec {
    h264_sps sps[32];
    h264_pps pps[256];
    int nal_len;
    h264_sps *csps;
    h264_pps *cpps;
    int mb_w, mb_h, w4, h4, w8;
    int nmbs;
    h264_pic pics[H264_MAX_PICS];
    int npics;                    /* pictures in use: max_num_ref_frames + 2 */
    h264_sps psps;                /* the SPS of the picture being decoded (csps follows the newest slice) */
    h264_pic *cur;
    int next_id;

    /* per picture, sized for the current SPS */
    h264_mbinfo *mbi;
    uint8_t *nnz_l;               /* per 4x4 luma block: number of coefficients */
    uint8_t *nnz_c[2];            /* per 4x4 chroma block (2 per macroblock side) */
    int8_t *ipm;                  /* per 4x4 luma block: intra prediction mode (2 for everything but I_NxN) */
    uint8_t *mvd[2];              /* per 4x4 block: |mvd| x, y, capped */

    /* picture order count / frame number state */
    int prev_poc_msb, prev_poc_lsb, prev_frame_num, prev_frame_num_offset;
    int prev_mmco5;
    int max_frame_num;
    int have_picture;             /* a picture is being decoded */

    /* slice and macroblock state */
    h264_slice sl;
    int slice_count;
    h264_cabac cabac;
    uint8_t ctx[1024];
    int mb_x, mb_y, mb_addr;
    int qp;                       /* QPY of the previous macroblock */
    int last_dqp;                 /* mb_qp_delta of the previous macroblock != 0 */
    int err;

    uint8_t *rbsp;                /* unescaped NAL payload */
    size_t rbsp_cap;
    int32_t coef[256 + 128];      /* luma levels / dequantised coefficients, then Cb and Cr */
    int32_t dc[3][16];            /* DC blocks: luma (Intra16x16), Cb, Cr */
    uint8_t blk_coded[32];        /* a 4x4 (or 8x8) block of the macroblock has coefficients; 16: luma DC, 17-18: chroma DC, 19-26: chroma AC */
};

/* h264.c */

/* h264_mb.c */
int h264_decode_slice_data(h264_dec *d, const uint8_t *data, size_t size, size_t bit_pos);

/* h264_mc.c: inter-predict the current macroblock from the motion in d->cur and the slice's reference lists */
void h264_mc_macroblock(h264_dec *d);

/* h264_dbf.c */
void h264_deblock_picture(h264_dec *d);

#endif
