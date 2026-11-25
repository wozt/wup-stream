/* libwupstream: draw to a Wii U GamePad running wupstream, play sound on it and read its controls, over Wi-Fi.
 *
 *   wupstream *pad = wupstream_open(NULL, 30000);         // the first GamePad waiting on the network (or its IP address)
 *   while (wupstream_input(pad, &in) == 0) {        // the controls; nonzero once the pad is gone
 *       ... draw 854 x 480 pixels ...
 *       wupstream_frame(pad, pixels, 854 * 4, WUPSTREAM_RGBA);   // returns when it has gone out: one per refresh
 *       wupstream_audio(pad, samples, n);                         // 48 kHz stereo: 800.8 frames per video frame
 *   }
 *   wupstream_close(pad);
 *
 * The GamePad's screen refreshes 59.94 times a second, and the library sends one frame for each refresh, in step with
 * it: wupstream_frame() paces the program to that. A program that is slower (30 frames a second, a loading pause) needs
 * nothing special: the last picture is sent again in between. Two frames wait on the GamePad as a cushion against late
 * ones (33 ms).
 * Frames are encoded with x264 the way the GamePad's hardware decoder needs them (built into libwupstream.a). The
 * GamePad and this computer must be on the same network.
 * Environment: WUPSTREAM_CRF (quality, lower is better, default 18), WUPSTREAM_KBPS (the most it may use, default
 * 10000), WUPSTREAM_DEBUG=1 (statistics), WUPSTREAM_DROP=n (lose 1 packet in n: a test), WUPSTREAM_X264 (more x264 settings,
 * "key=value:..."), WUPSTREAM_DUMP=file (the encoded stream, Annex B). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WUPSTREAM_W 854
#define WUPSTREAM_H 480
typedef struct wupstream wupstream;
enum { WUPSTREAM_RGBA, WUPSTREAM_BGRA };                     /* 4 bytes per pixel, in this byte order (the 4th is ignored) */
enum { WUPSTREAM_A = 0x8000, WUPSTREAM_B = 0x4000, WUPSTREAM_X = 0x2000, WUPSTREAM_Y = 0x1000, WUPSTREAM_LEFT = 0x0800, WUPSTREAM_RIGHT = 0x0400,
       WUPSTREAM_UP = 0x0200, WUPSTREAM_DOWN = 0x0100, WUPSTREAM_ZL = 0x0080, WUPSTREAM_ZR = 0x0040, WUPSTREAM_L = 0x0020, WUPSTREAM_R = 0x0010,
       WUPSTREAM_PLUS = 0x0008, WUPSTREAM_MINUS = 0x0004, WUPSTREAM_HOME = 0x0002, WUPSTREAM_L3 = 0x10000, WUPSTREAM_R3 = 0x20000, WUPSTREAM_TV = 0x40000 };
typedef struct {
    uint32_t buttons;                            /* WUPSTREAM_* held */
    int lx, ly, rx, ry;                          /* sticks: 0 at rest, about +-1100 at full tilt (measured), right and up positive */
    int touched, tx, ty;                         /* touch screen: 0..853, 0..479 */
    int accel[3], gyro[3];                       /* raw motion sensors; accel: about 8200 per g (lying flat: z near -8200) */
    int volume;                                  /* the volume slider, raw (0..255) */
    int backlight;                               /* the screen's brightness now, 1..6; 0: off */
    int battery, charging, plugged;              /* about 0..100 (-1: not known); charging; the charger is plugged in.
                                                    Less exact while charging: the pad's reading rises then */
    uint8_t report[128];                         /* the whole input report, as the GamePad has it */
} wupstream_input_t;

wupstream *wupstream_open(const char *ip, int timeout_ms);   /* NULL ip: the first GamePad waiting; NULL if none answered.
                                                                Several GamePads: one wupstream each. */
int wupstream_frame(wupstream *w, const void *pixels, int stride, int format);   /* 0, or -1: the GamePad is gone; paced to its refresh */
int wupstream_audio(wupstream *w, const int16_t *lr, int frames);                /* 16-bit L,R pairs at 48 kHz */
int wupstream_mic(wupstream *w, int16_t *samples, int max);       /* the microphone: 16-bit mono, 48 kHz; -> how many
                                                                      (it is switched on by the first call) */
int wupstream_camera(wupstream *w, uint8_t *h264, int max);       /* the camera (640 x 480, ~59 frames a second): the next
                                                                      frame as H.264 (Annex B, with its parameter sets), ->
                                                                      its length, 0: none waiting. 100 KB hold any. Call it
                                                                      often; switched on by the first call. The first is
                                                                      preceded by a grey keyframe; then none: a lost frame
                                                                      heals by itself (intra refresh). */
void wupstream_rumble(wupstream *w, int on);                       /* the motor on or off (it stops by itself if the program does) */
void wupstream_backlight(wupstream *w, int level, int keep);       /* the screen's brightness, 1..6, or 0: the backlight off,
                                                                      while this program runs; keep: also make it wupstream's
                                                                      own level (Nintendo's is never changed) */
int wupstream_input(wupstream *w, wupstream_input_t *in);          /* the latest controls; 0, or -1: the GamePad is gone */
void wupstream_close(wupstream *w);

#ifdef __cplusplus
}
#endif
