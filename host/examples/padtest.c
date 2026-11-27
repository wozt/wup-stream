/* padtest: everything the GamePad sends and takes, at once, to check a pad (and libwupstream) end to end.
 *   On the pad: a dot for each stick (clicked: filled), the touch point, the pad's tilt (accelerometer) as a level
 *   bubble, the gyroscope as three bars, a lamp per button, the volume slider and the battery as bars.
 *   A: the rumble motor while held.  B: a tone while held.  L / R: the screen dimmer / brighter (1..6).
 *   Here: the same values, ten times a second. HOME ends it.
 *   build/host/padtest [pad IP] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wupstream.h"

#define W WUPSTREAM_W
#define H WUPSTREAM_H
static uint32_t px[W * H];
static uint32_t rgb(int r, int g, int b) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xff000000u; }
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) px[j * W + i] = c;
}
static void disc(int cx, int cy, int r, uint32_t c, int filled) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            int d = i * i + j * j;
            if (d <= r * r && (filled || d >= (r - 3) * (r - 3))) { int x = cx + i, y = cy + j; if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = c; }
        }
}
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int main(int argc, char **argv) {
    wupstream *pad = wupstream_open(argc > 1 ? argv[1] : NULL, 30000);
    if (!pad) { fprintf(stderr, "no GamePad found\n"); return 1; }
    static const struct { uint32_t bit; const char *name; } btn[] = {
        { WUPSTREAM_A, "A" }, { WUPSTREAM_B, "B" }, { WUPSTREAM_X, "X" }, { WUPSTREAM_Y, "Y" }, { WUPSTREAM_UP, "Up" },
        { WUPSTREAM_DOWN, "Down" }, { WUPSTREAM_LEFT, "Left" }, { WUPSTREAM_RIGHT, "Right" }, { WUPSTREAM_L, "L" },
        { WUPSTREAM_R, "R" }, { WUPSTREAM_ZL, "ZL" }, { WUPSTREAM_ZR, "ZR" }, { WUPSTREAM_PLUS, "+" }, { WUPSTREAM_MINUS, "-" },
        { WUPSTREAM_L3, "L3" }, { WUPSTREAM_R3, "R3" }, { WUPSTREAM_TV, "TV" } };
    const int nb = (int)(sizeof btn / sizeof btn[0]);
    wupstream_input_t in;
    uint32_t was = 0;
    int level = 4, frame = 0;
    double phase = 0;
    static int16_t tone[2 * 801];
    wupstream_backlight(pad, level, 0);
    while (wupstream_input(pad, &in) == 0) {
        if (in.buttons & WUPSTREAM_HOME) break;
        uint32_t pressed = in.buttons & ~was; was = in.buttons;
        if (pressed & WUPSTREAM_L && level > 1) wupstream_backlight(pad, --level, 0);
        if (pressed & WUPSTREAM_R && level < 6) wupstream_backlight(pad, ++level, 0);
        wupstream_rumble(pad, (in.buttons & WUPSTREAM_A) != 0);

        rect(0, 0, W, H, rgb(16, 20, 32));
        disc(130, 150, 100, rgb(70, 80, 110), 0);                    /* the sticks */
        disc(130 + in.lx * 90 / 1100, 150 - in.ly * 90 / 1100, 18, rgb(90, 200, 250), (in.buttons & WUPSTREAM_L3) != 0);
        disc(370, 150, 100, rgb(70, 80, 110), 0);
        disc(370 + in.rx * 90 / 1100, 150 - in.ry * 90 / 1100, 18, rgb(90, 200, 250), (in.buttons & WUPSTREAM_R3) != 0);
        disc(640, 150, 100, rgb(70, 80, 110), 0);                    /* the tilt: a bubble level */
        disc(640 + clamp(in.accel[0] * 90 / 8200, -90, 90), 150 + clamp(in.accel[1] * 90 / 8200, -90, 90), 14, rgb(250, 210, 80), 1);
        for (int k = 0; k < 3; k++) {                                 /* the gyroscope: three bars from the middle */
            int v = clamp(in.gyro[k] / 64, -100, 100);
            rect(560, 280 + 22 * k, 160, 14, rgb(40, 46, 64));
            rect(v < 0 ? 640 + v : 640, 280 + 22 * k, abs(v), 14, rgb(250, 120, 90));
        }
        for (int i = 0; i < nb; i++)                                  /* a lamp per button */
            rect(30 + 30 * i, 300, 24, 24, (in.buttons & btn[i].bit) ? rgb(120, 230, 120) : rgb(50, 56, 74));
        rect(30, 350, 255 * 2, 14, rgb(40, 46, 64)); rect(30, 350, in.volume * 2, 14, rgb(200, 200, 240));          /* volume */
        rect(30, 380, 200, 14, rgb(40, 46, 64));                                                                   /* battery */
        if (in.battery >= 0) rect(30, 380, in.battery * 2, 14, in.charging ? rgb(120, 230, 120) : rgb(230, 230, 230));
        for (int l = 1; l <= 6; l++) rect(30 + 26 * (l - 1), 410, 20, 20, l <= level ? rgb(250, 220, 120) : rgb(50, 56, 74));   /* brightness */
        if (in.touched) disc(in.tx, in.ty, 22, rgb(255, 255, 255), 1);
        wupstream_frame(pad, px, W * 4, WUPSTREAM_RGBA);

        int n = 0;                                                    /* B: a 440 Hz tone */
        for (; n < 800; n++) {
            int16_t v = (in.buttons & WUPSTREAM_B) ? (int16_t)(6000 * sin(phase)) : 0;
            phase += 2 * M_PI * 440 / 48000; tone[2 * n] = tone[2 * n + 1] = v;
        }
        wupstream_audio(pad, tone, n);

        if (++frame % 6 == 0) {
            printf("\rL %5d %5d  R %5d %5d  touch %d %3d %3d  accel %6d %6d %6d  gyro %6d %6d %6d  vol %3d  bat %3d%s  light %d  btn",
                   in.lx, in.ly, in.rx, in.ry, in.touched, in.tx, in.ty, in.accel[0], in.accel[1], in.accel[2],
                   in.gyro[0], in.gyro[1], in.gyro[2], in.volume, in.battery, in.charging ? "+" : " ", in.backlight);
            for (int i = 0; i < nb; i++) if (in.buttons & btn[i].bit) printf(" %s", btn[i].name);
            printf("        ");
            fflush(stdout);
        }
    }
    printf("\n");
    wupstream_close(pad);
    return 0;
}
