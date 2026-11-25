/* mirror: the GamePad shows its own camera, by way of this computer: the pad encodes its camera, this program decodes
 * it (ffmpeg must be installed) and streams the picture back to the pad's screen, mirrored. Both of the pad's video
 * engines at work at once: its encoder and its decoder. With "mic" a bar along the bottom follows the pad's microphone
 * too; with "echo" what it hears also comes back from the pad's speakers 1.5 seconds later: everything the pad can
 * send, while it shows video and plays sound.
 *   build/host/mirror [pad IP] [mic | echo]
 * Stream (wup-os) must be open on the GamePad; HOME ends it.
 * (macOS and Linux) */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wupstream.h"

#define FRAME (WUPSTREAM_W * WUPSTREAM_H * 4)
static unsigned char latest[FRAME], shown[FRAME], out_px[FRAME];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int fresh, from_ff;

static void *reader(void *arg) {                       /* whole decoded pictures, as ffmpeg makes them */
    static unsigned char f[FRAME];
    (void)arg;
    for (;;) {
        size_t have = 0;
        while (have < FRAME) { ssize_t n = read(from_ff, f + have, FRAME - have); if (n <= 0) return NULL; have += (size_t)n; }
        pthread_mutex_lock(&mu); memcpy(latest, f, FRAME); fresh = 1; pthread_mutex_unlock(&mu);
    }
}

int main(int argc, char **argv) {
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return 1;
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid == 0) {                                    /* ffmpeg: H.264 in, 854 x 480 RGBA out (the picture centred) */
        dup2(in[0], 0); dup2(out[1], 1); close(in[1]); close(out[0]);
        execlp("ffmpeg", "ffmpeg", "-loglevel", "error", "-probesize", "32", "-analyzeduration", "0", "-flags", "low_delay", "-f", "h264", "-i", "pipe:0", "-vf", "hflip,pad=854:480:107:0:black",
               "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]); from_ff = out[0];
    pthread_t th; pthread_create(&th, NULL, reader, NULL);

    setenv("WUPSTREAM_KBPS", "4000", 0);                /* (the pad's radio carries the camera the other way at once) */
    int echo = argc > 1 && !strcmp(argv[argc - 1], "echo"), mic = echo || (argc > 1 && !strcmp(argv[argc - 1], "mic"));
    const char *ip = argc > 1 && strcmp(argv[1], "mic") && strcmp(argv[1], "echo") ? argv[1] : NULL;
    wupstream *pad = wupstream_open(ip, 30000);
    if (!pad) { fprintf(stderr, "no GamePad found\n"); kill(pid, SIGTERM); return 1; }
    static unsigned char h264[100000];
    static int16_t silence[801 * 2];
    wupstream_input_t st;
    unsigned t = 0, frames = 0, samples = 0;
    static int16_t heard[9600], line[1 << 18], sound[801 * 2];   /* (line: the echo's delay, 5 s of room) */
    unsigned lw = 0, lr = 0;
    int level = 0, playing = 0, loud = 0;
    for (int i = 0; i < FRAME; i += 4) shown[i + 3] = 255;
    while (!wupstream_input(pad, &st) && !(st.buttons & WUPSTREAM_HOME)) {
        for (int n; (n = wupstream_camera(pad, h264, sizeof h264)) > 0; frames++)
            if (write(in[1], h264, (size_t)n) != n) break;
        pthread_mutex_lock(&mu); if (fresh) { memcpy(shown, latest, FRAME); fresh = 0; } pthread_mutex_unlock(&mu);
        const unsigned char *px = shown;
        if (mic) {                                         /* the loudest sample since the last frame, falling slowly */
            int n = wupstream_mic(pad, heard, 9600), peak = 0;
            for (int i = 0; i < n; i++) { int v = heard[i] < 0 ? -heard[i] : heard[i]; if (v > peak) peak = v; }
            samples += (unsigned)n;
            /* (while the echo sounds, what the microphone hears is the echo: silence goes into the line instead) */
            for (int i = 0; i < n; i++) line[lw++ & ((1 << 18) - 1)] = loud ? 0 : heard[i];
            level = peak > level ? peak : level * 7 / 8;
            int w = level * WUPSTREAM_W / 12000; if (w > WUPSTREAM_W) w = WUPSTREAM_W;
            memcpy(out_px, shown, FRAME); px = out_px;
            for (int y = WUPSTREAM_H - 28; y < WUPSTREAM_H - 12; y++)
                for (int x = 0; x < w; x++) { unsigned char *p = out_px + 4 * (y * WUPSTREAM_W + x); p[0] = 52; p[1] = 199; p[2] = 89; }
        }
        int na = 801 - (int)(++t & 1);
        const int16_t *au = silence;
        if (echo) {                                        /* 1.5 s behind the microphone; begun again when it runs dry */
            if (!playing && lw - lr >= 72000) playing = 1;
            if (playing && lw - lr < (unsigned)na) playing = 0;
            int peak = 0;
            for (int i = 0; i < na; i++) {
                int v = playing ? line[lr++ & ((1 << 18) - 1)] * 3 : 0;      /* (the microphone is quiet: 3 times louder) */
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                sound[2 * i] = sound[2 * i + 1] = (int16_t)v;
                if (v > peak) peak = v; else if (-v > peak) peak = -v;
            }
            loud = peak > 1500 ? 30 : loud > 0 ? loud - 1 : 0;              /* (and half a second after) */
            au = sound;
        }
        if (wupstream_frame(pad, px, WUPSTREAM_W * 4, WUPSTREAM_RGBA) || wupstream_audio(pad, au, na)) break;
        if (t % 300 == 0) { fprintf(stderr, "mirror: %u camera frames, %u microphone samples\n", frames, samples); }
    }
    fprintf(stderr, "mirror: ended: %s\n", wupstream_input(pad, &st) ? "the pad stopped answering" : st.buttons & WUPSTREAM_HOME ? "HOME" : "sending failed");
    wupstream_close(pad);
    kill(pid, SIGTERM);
    return 0;
}
