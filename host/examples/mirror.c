/* mirror: the GamePad shows its own camera, by way of this computer: the pad encodes its camera, this program decodes
 * it (ffmpeg must be installed) and streams the picture back to the pad's screen, mirrored. Both of the pad's video
 * engines at work at once: its encoder and its decoder.
 *   build/host/mirror [pad IP]        Stream (wup-os) must be open on the GamePad; HOME ends it
 * (macOS and Linux) */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wupstream.h"

#define FRAME (WUPSTREAM_W * WUPSTREAM_H * 4)
static unsigned char latest[FRAME], shown[FRAME];
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
    wupstream *pad = wupstream_open(argc > 1 ? argv[1] : NULL, 30000);
    if (!pad) { fprintf(stderr, "no GamePad found\n"); kill(pid, SIGTERM); return 1; }
    static unsigned char h264[100000];
    static int16_t silence[801 * 2];
    wupstream_input_t st;
    unsigned t = 0, frames = 0;
    for (int i = 0; i < FRAME; i += 4) shown[i + 3] = 255;
    while (!wupstream_input(pad, &st) && !(st.buttons & WUPSTREAM_HOME)) {
        for (int n; (n = wupstream_camera(pad, h264, sizeof h264)) > 0; frames++)
            if (write(in[1], h264, (size_t)n) != n) break;
        pthread_mutex_lock(&mu); if (fresh) { memcpy(shown, latest, FRAME); fresh = 0; } pthread_mutex_unlock(&mu);
        if (wupstream_frame(pad, shown, WUPSTREAM_W * 4, WUPSTREAM_RGBA) || wupstream_audio(pad, silence, 801 - (++t & 1))) break;
        if (t % 300 == 0) fprintf(stderr, "mirror: %u camera frames\n", frames);
    }
    wupstream_close(pad);
    kill(pid, SIGTERM);
    return 0;
}
