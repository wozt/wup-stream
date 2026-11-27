/* camera: the GamePad's camera on this computer, as an H.264 stream on stdout; the GamePad shows colour bars meanwhile.
 *   build/host/camera [pad IP] | ffplay -f h264 -flags low_delay -i -
 * WUP Stream must be open on the GamePad; HOME ends it. */
#include <stdio.h>
#include "wupstream.h"

int main(int argc, char **argv) {
    static unsigned char px[WUPSTREAM_W * WUPSTREAM_H * 4], h264[100000];
    static int16_t silence[801 * 2];
    static const unsigned char bar[8][3] = { {235,235,235}, {235,235,16}, {16,235,235}, {16,235,16}, {235,16,235}, {235,16,16}, {16,16,235}, {16,16,16} };
    wupstream *pad = wupstream_open(argc > 1 ? argv[1] : NULL, 30000);
    if (!pad) { fprintf(stderr, "no GamePad found\n"); return 1; }
    wupstream_input_t in;
    unsigned frames = 0, bytes = 0, t = 0;
    while (!wupstream_input(pad, &in) && !(in.buttons & WUPSTREAM_HOME)) {
        for (int y = 0; y < WUPSTREAM_H; y++)                /* the bars, and a block that moves: the pad's decoder at work */
            for (int x = 0; x < WUPSTREAM_W; x++) {
                const unsigned char *c = bar[x * 8 / WUPSTREAM_W]; unsigned char *p = px + 4 * (y * WUPSTREAM_W + x);
                int in_block = y >= 200 && y < 280 && (unsigned)(x - (int)(t * 4 % (WUPSTREAM_W - 80))) < 80;
                p[0] = in_block ? 255 : c[0]; p[1] = in_block ? 255 : c[1]; p[2] = in_block ? 255 : c[2];
            }
        t++;
        if (wupstream_frame(pad, px, WUPSTREAM_W * 4, WUPSTREAM_RGBA) || wupstream_audio(pad, silence, 801 - (t & 1))) break;
        for (int n; (n = wupstream_camera(pad, h264, sizeof h264)) > 0; ) { fwrite(h264, 1, (size_t)n, stdout); frames++; bytes += (unsigned)n; }
        fflush(stdout);
        if (t % 300 == 0) fprintf(stderr, "camera: %u frames, %u KB\n", frames, bytes / 1024);
    }
    wupstream_close(pad);
    return 0;
}
