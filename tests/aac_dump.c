/* Decodes the audio track of an MP4 file with the built-in AAC decoder and writes interleaved float32 (native
 * endian) -- for comparing against another decoder (see tests/check_media.py).
 *
 *   aac_dump <file.mp4> <out.f32>
 */
#include <stdio.h>
#include <stdlib.h>

#include "../src/media/aac.h"
#include "../src/media/mp4.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: aac_dump file.mp4 out.f32\n"); return 2; }
    mp4 *m = mp4_open(argv[1]);
    const mp4_track *t = m ? mp4_find(m, MP4_AUDIO) : NULL;
    if (!t || !t->extra) { fprintf(stderr, "no audio track\n"); return 1; }
    aac_dec *d = aac_open(t->extra, t->extra_size);
    if (!d) { fprintf(stderr, "aac_open failed\n"); return 1; }
    const int ch = aac_channels(d);
    FILE *out = fopen(argv[2], "wb");
    float *pcm = (float *)malloc(sizeof(float) * AAC_FRAME_SAMPLES * ch);
    uint8_t *pkt = (uint8_t *)malloc(1 << 16);
    int errors = 0;
    for (uint32_t i = 0; i < t->nsamples; i++) {
        const int n = mp4_read(m, t, i, pkt, 1 << 16);
        const int r = n < 0 ? -1 : aac_decode(d, pkt, (size_t)n, pcm);
        if (r < 0) { errors++; if (errors < 10) fprintf(stderr, "frame %u: error %d\n", i, r); continue; }
        /* the edit list may start the presentation inside the first frame (encoder delay) */
        int skip = 0;
        if (t->cts[i] < 0) { skip = (int)(-t->cts[i] * aac_rate(d) / t->timescale); if (skip > r) skip = r; }
        fwrite(pcm + (size_t)skip * ch, sizeof(float), (size_t)(r - skip) * ch, out);
    }
    fclose(out);
    fprintf(stderr, "%u frames, %d channels, %d Hz, %d errors\n", t->nsamples, ch, aac_rate(d), errors);
    return errors != 0;
}
