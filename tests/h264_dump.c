/* Decodes the video track of an MP4 file with the built-in H.264 decoder and writes the pictures, in display order,
 * as raw planar YUV 4:2:0 -- for comparing against another decoder (see tests/check_media.py).
 *
 *   h264_dump <file.mp4> <out.yuv> [max frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/media/h264.h"
#include "../src/media/mp4.h"

typedef struct { uint8_t *data; int64_t cts; int used; } slot;

static void write_frame(FILE *out, const slot *s, int w, int h)
{
    fwrite(s->data, 1, (size_t)w * h * 3 / 2, out);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: h264_dump file.mp4 out.yuv [max frames]\n"); return 2; }
    mp4 *m = mp4_open(argv[1]);
    const mp4_track *t = m ? mp4_find(m, MP4_VIDEO) : NULL;
    if (!t || !t->extra) { fprintf(stderr, "no video track\n"); return 1; }
    h264_dec *d = h264_open(t->extra, t->extra_size);
    if (!d) { fprintf(stderr, "h264_open failed\n"); return 1; }
    const int w = h264_width(d), h = h264_height(d);
    const int depth = mp4_reorder_depth(t);
    const uint32_t limit = argc > 3 ? (uint32_t)atoi(argv[3]) : t->nsamples;
    FILE *out = fopen(argv[2], "wb");
    slot *slots = (slot *)calloc((size_t)depth + 2, sizeof *slots);
    for (int i = 0; i < depth + 2; i++) slots[i].data = (uint8_t *)malloc((size_t)w * h * 3 / 2);
    uint8_t *pkt = (uint8_t *)malloc(1 << 22);
    int errors = 0, written = 0;
    fprintf(stderr, "%dx%d, %u samples, reorder depth %d\n", w, h, t->nsamples, depth);
    for (uint32_t i = 0; i < t->nsamples && i < limit; i++) {
        const int n = mp4_read(m, t, i, pkt, 1 << 22);
        h264_frame fr;
        const int r = n < 0 ? -1 : h264_decode(d, pkt, (size_t)n, &fr);
        if (r < 0) { errors++; fprintf(stderr, "sample %u: decode error\n", i); }
        if (r != 1) continue;
        slot *s = NULL;
        for (int k = 0; k < depth + 2 && !s; k++) if (!slots[k].used) s = &slots[k];
        uint8_t *p = s->data;
        for (int y = 0; y < h; y++) { memcpy(p, fr.plane[0] + (size_t)y * fr.stride[0], w); p += w; }
        for (int c = 1; c < 3; c++)
            for (int y = 0; y < h / 2; y++) { memcpy(p, fr.plane[c] + (size_t)y * fr.stride[c], w / 2); p += w / 2; }
        s->cts = t->cts[i];
        s->used = 1;
        int pending = 0;
        for (int k = 0; k < depth + 2; k++) pending += slots[k].used;
        while (pending > depth) {
            slot *best = NULL;
            for (int k = 0; k < depth + 2; k++) if (slots[k].used && (!best || slots[k].cts < best->cts)) best = &slots[k];
            write_frame(out, best, w, h);
            best->used = 0;
            written++;
            pending--;
        }
    }
    for (;;) {
        slot *best = NULL;
        for (int k = 0; k < depth + 2; k++) if (slots[k].used && (!best || slots[k].cts < best->cts)) best = &slots[k];
        if (!best) break;
        write_frame(out, best, w, h);
        best->used = 0;
        written++;
    }
    fclose(out);
    fprintf(stderr, "wrote %d frames, %d errors\n", written, errors);
    return errors != 0;
}
