/* play: a video file on the GamePad, picture and sound (ffmpeg must be installed: it reads any file it knows).
 *   build/host/play movie.mp4 [pad IP]
 * The picture is fitted to the pad's 854 x 480 (black bars where the shapes differ) at the pad's own 59.94 frames a
 * second, the sound at 48 kHz stereo. A: pause and go on. HOME: stop. WUP Stream must be open on the GamePad.
 * ffmpeg writes the picture to one pipe and the sound to another; each picture takes the sound that plays with it
 * (800.8 samples: 801 and 800 in turn, near enough). (macOS and Linux) */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "wupstream.h"

#define FRAME (WUPSTREAM_W * WUPSTREAM_H * 4)

static int read_all(int fd, void *p, size_t n) {           /* 0 once n bytes are in, -1 at the end */
    for (size_t have = 0; have < n; ) { ssize_t k = read(fd, (char *)p + have, n - have); if (k <= 0) return -1; have += (size_t)k; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s video-file [pad IP]\n", argv[0]); return 1; }
    int vp[2], ap[2];
    if (pipe(vp) || pipe(ap)) return 1;
    pid_t pid = fork();
    if (pid == 0) {                                        /* ffmpeg: the picture to fd 3, the sound to fd 4 */
        int v = fcntl(vp[1], F_DUPFD, 10), a = fcntl(ap[1], F_DUPFD, 10);   /* (out of the way first: the pipes may be 3 and 4) */
        close(vp[0]); close(ap[0]); close(vp[1]); close(ap[1]);
        dup2(v, 3); dup2(a, 4); close(v); close(a);
        execlp("ffmpeg", "ffmpeg", "-loglevel", "error", "-nostdin", "-i", argv[1],
               "-map", "0:v:0", "-vf", "fps=60000/1001,scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:(ow-iw)/2:(oh-ih)/2",
               "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:3",
               "-map", "0:a:0?", "-f", "s16le", "-ar", "48000", "-ac", "2", "pipe:4", (char *)NULL);
        perror("ffmpeg"); _exit(127);
    }
    close(vp[1]); close(ap[1]);
    signal(SIGPIPE, SIG_IGN);

    wupstream *pad = wupstream_open(argc > 2 ? argv[2] : NULL, 30000);
    if (!pad) { fprintf(stderr, "no GamePad found\n"); kill(pid, SIGTERM); return 1; }
    static unsigned char px[FRAME];
    static int16_t sound[801 * 2], quiet[801 * 2];
    int has_sound = 1, paused = 0;
    unsigned frame = 0, was = 0;
    wupstream_input_t in;
    while (!wupstream_input(pad, &in) && !(in.buttons & WUPSTREAM_HOME)) {
        unsigned pressed = in.buttons & ~was; was = in.buttons;
        if (pressed & WUPSTREAM_A) paused = !paused;
        int n = 801 - (int)(frame & 1);
        if (!paused) {
            if (read_all(vp[0], px, FRAME)) break;         /* (the end of the video) */
            if (has_sound && read_all(ap[0], sound, (size_t)n * 4)) has_sound = 0;   /* (a file without sound) */
            frame++;
        }
        if (wupstream_frame(pad, px, WUPSTREAM_W * 4, WUPSTREAM_RGBA)) break;
        wupstream_audio(pad, paused || !has_sound ? quiet : sound, n);
    }
    wupstream_close(pad);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    return 0;
}
