/* wupstream: a computer's picture, sound and controls on the GamePad. The other side is libwupstream
 * (this repository's host/): a program there draws, the library encodes it for the pad's hardware
 * H.264 decoder and sends it here. This is wupstream's receiver (its pad/stream/stream.c) as a libwup app.
 *
 * Protocol (UDP, little endian). The pad listens on port 50000.
 *   pad -> broadcast:50001  'B' version 0 0                      every second while no computer is connected
 *   PC  -> pad   'H' version flags(1 = send the microphone) light  hello / keepalive (every 250 ms); light: the
 *                                                                 backlight 1..6 (0 = leave it), bit 6 = off, bit 7 = keep it
 *                'V' flags(1 = keyframe) 0 0, u32 frame, u32 offset, u32 total, then <= 1400 bytes
 *                'A' 0 0 0, u32 seq, then 48 kHz 16-bit stereo PCM (a multiple of 16 bytes)
 *                'R' on(1)/off(0)                                 the rumble motor (said again with every frame while on)
 *                'Q'                                             the computer is done
 *   pad -> PC    'M' 0 0 0, u32 sample number, then the microphone: 16-bit mono 48 kHz
 *   pad -> PC    'N' count 0 0, then count x {u32 frame, u16 packet (0xffff: all of it), u16 0}: send these again
 *   pad -> PC    'I' flags(1 = send a keyframe; bits 1-3: frames waiting for the decoder), u16 clock ticks (~1 us)
 *                since the screen's last refresh, u8 backlight (1..6, 0 = off), 0 0 0, the 128-byte input report
 * Video is each frame's raw CABAC slice data, put together here from its packets and handed whole to the hardware
 * decoder (libwup's wup_video_*: the kernel's driver). While video plays the screen's memory is the decoder's: the
 * waiting screen is drawn again from scratch when a stream ends. */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "wup.h"
#include "wup_hw.h"                                   /* (hw_backlight_percent: the table's levels) */

WUP_APP("wupstream");
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define CLOCK   (*(volatile u32 *)0xf0000408u)                /* free-running, ~997.3 kHz */

static u32 ms_, last_clk, frac;
static u32 clock_ms(void) { u32 c = CLOCK; frac += c - last_clk; last_clk = c; ms_ += frac / 997; frac %= 997; return ms_; }

/* ---- the computer */
static int sock = -1;
static struct sockaddr_in peer_sa;
static u32 peer_on, last_ms, quiet_until, beacon_ms, in_ms, stat_ms;
static u16 in_seq;
static void send_to(const struct sockaddr_in *to, const void *d, u32 n) { sendto(sock, d, n, 0, (const struct sockaddr *)to, sizeof *to); }
static void send_peer(const void *d, u32 n) { if (peer_on) send_to(&peer_sa, d, n); }

/* ---- video: whole frames to the hardware decoder (wup_video_frame). Every keyframe restarts it; after a lost or late
   frame only a keyframe goes on (wup_video_need_key); the kernel watches it for stalls. The screen stays dark until
   the decoder has taken 3 frames (no stale picture shown), then lights up. */
static u32 von, need_idr = 1, frames, shown, lit_at, lit_ms;
static u32 keys, lost, stalls, gap_max, poll_ms, full, t_start;   /* (counters, logged every 5 s) */
static int frame_done(const u8 *data, u32 len, int idr, u32 now) {   /* 0, or < 0: dropped */
    if (!idr && (need_idr || wup_video_need_key())) { need_idr = 1; return -1; }
    if (!shown) { gfx_brightness(0); if (wup_video_open()) return -1; shown = 1; lit_at = 0; }
    if (wup_video_frame(data, len, idr)) { need_idr = 1; if (von) stalls++; von = 0; return -1; }
    von = 1; need_idr = 0; frames++; if (idr) keys++;
    if (shown == 1 && ++lit_at == 3) { shown = 2; lit_ms = now; }
    return 0;
}
static u32 waiting(void) { return von ? (u32)wup_video_waiting() : 0; }

/* Frames come in packets of 1400 bytes, and Wi-Fi loses some. The frames waiting for the decoder (the cushion) give the
   time to get a lost packet again: up to 4 frames are put together at once, each at its place in the ring, and go to
   the decoder in order as they become whole. What is missing is asked for again ('N'): when later packets show the
   gap, or nothing came for 6 ms. A frame still not whole after 45 ms is given up: a keyframe then. */
#define NASM 4
#define FRAME_MAX (128 * 1400u)                               /* (a frame of up to 128 packets) */
static struct { u32 len, npk, have, got[4], t_rx, t_nack, t_head; u8 known, idr; u8 *data; } as[NASM];
static u32 next_f, seen_f, sync, resent;
static void nack(u32 now);
static void as_clear(int i) { u8 *d = as[i].data; memset(&as[i], 0, sizeof as[i]); as[i].data = d; }
static void as_reset(u32 f) { for (int i = 0; i < NASM; i++) as_clear(i); next_f = seen_f = f; sync = 1; }
static void video(const u8 *d16, u32 len, const u8 *h, u32 now) {      /* d16: the data after the 16-byte header */
    u32 fno = *(u32 *)(h + 4), off = *(u32 *)(h + 8), total = *(u32 *)(h + 12), n = len - 16u, idr = h[1] & 1;
    if (idr && (!sync || fno != next_f || !as[0].idr)) {       /* a keyframe: the decoder starts again with it, at the ring's start */
        if (sync && (int)(fno - next_f) < 0) return;           /* (an old one, again) */
        as_reset(fno); as[0].idr = 1; as[0].t_head = now;
    }
    if (!sync || (need_idr && !as[0].idr)) return;             /* waiting for a keyframe */
    int d = (int)(fno - next_f);
    if (d < 0) return;                                         /* (already whole: a packet sent twice) */
    if (d >= NASM) { lost++; need_idr = 1; sync = 0; return; } /* too far ahead: we lost the thread */
    if ((int)(fno - seen_f) > 0) seen_f = fno;
    if (!as[d].known) {
        if (!total || total > FRAME_MAX) { full++; need_idr = 1; sync = 0; return; }
        as[d].known = 1; as[d].len = total; as[d].npk = (total + 1399) / 1400; as[d].have = 0; as[d].idr = (u8)idr;
        memset(as[d].got, 0, sizeof as[d].got);
    }
    u32 k = off / 1400;
    if (off % 1400 || k >= as[d].npk || off + n > as[d].len || (n != 1400 && off + n != as[d].len)) return;
    if (!(as[d].got[k / 32] >> (k % 32) & 1)) {
        memcpy(as[d].data + off, d16, n);
        as[d].got[k / 32] |= 1u << (k % 32); as[d].have++;
    }
    as[d].t_rx = now;
    if (d > 0 && as[d - 1].have != as[d - 1].npk) nack(now);   /* a later frame has begun: what the earlier lacks is lost */
    while (as[0].known && as[0].have == as[0].npk) {           /* whole frames, in order, to the decoder */
        if (frame_done(as[0].data, as[0].len, as[0].idr, now) && !as[0].idr) { sync = 0; return; }
        u8 *d0 = as[0].data;
        for (int i = 0; i < NASM - 1; i++) as[i] = as[i + 1];
        as[NASM - 1].data = d0; as_clear(NASM - 1);
        next_f++; as[0].t_head = now;
    }
}
/* the screen lit once the decoder has had its first 3 frames a moment (its stalls: the kernel's) */
static void watchdog(u32 now) {
    if (shown == 2 && now - lit_ms > 50) { gfx_brightness(256); shown = 3; }
}
/* ask for what is missing of the frames being put together: 'N' count 0 0, then per packet u32 frame, u16 index
   (0xffff: the whole frame: none of it came), u16 0. A frame at most every 8 ms. */
static void nack(u32 now) {
    u8 m[4 + 8 * 40] = { 'N' }; u32 c = 0;
    for (int i = 0; i < NASM && (int)(next_f + i - seen_f) <= 0 && c < 40; i++) {
        int newest = next_f + i == seen_f;
        if (as[i].known && as[i].have == as[i].npk) continue;
        if (now - as[i].t_nack < 8 && as[i].t_nack) continue;
        if (newest && as[i].known && now - as[i].t_rx < 6) continue;   /* (still coming) */
        u32 f = next_f + i;
        if (!as[i].known) { memcpy(m + 4 + 8 * c, &f, 4); m[8 + 8 * c] = m[9 + 8 * c] = 0xff; m[10 + 8 * c] = m[11 + 8 * c] = 0; c++; }
        else for (u32 k = 0; k < as[i].npk && c < 40; k++)
            if (!(as[i].got[k / 32] >> (k % 32) & 1)) { memcpy(m + 4 + 8 * c, &f, 4); m[8 + 8 * c] = (u8)k; m[9 + 8 * c] = (u8)(k >> 8); m[10 + 8 * c] = m[11 + 8 * c] = 0; c++; }
        as[i].t_nack = now ? now : 1;
    }
    if (!c || !peer_on) return;
    m[1] = (u8)c; resent += c;
    send_peer(m, 4 + 8 * c);
}

/* ---- sound: libwup's speaker ring, at most 128 ms queued */
static void audio(const u8 *d8, u32 len) {
    u32 n = (len - 8u) & ~15u;
    if (n && wup_audio_queued() < 48000 * 128 / 1000) wup_audio_queue((const int16_t *)d8, (int)(n / 4));
}
/* ---- the motor: on while the computer keeps saying so (with every frame), off 100 ms after the last "on" */
static u32 motor_ms, motor_on;
static void motor(int on, u32 now) {
    if (on) { if (!motor_on) wup_rumble(1); motor_on = 1; motor_ms = now; } else if (motor_on) { wup_rumble(0); motor_on = 0; }
}
/* ---- the microphone */
static u32 mic_on, mic_seq;
static void mic(int on) { if (on == (int)mic_on) return; if (on) wup_mic_open(); else wup_mic_close(); mic_on = (u32)on; }
static void mic_poll(void) {
    static u8 m[8 + 1024] __attribute__((aligned(4))) = { 'M' };
    for (;;) {
        int n = wup_mic_read((int16_t *)(m + 8), 512);
        if (n <= 0) return;
        memcpy(m + 4, &mic_seq, 4); mic_seq += (u32)n;
        send_peer(m, 8 + 2 * (u32)n);
    }
}
/* ---- the backlight: the computer's level while it streams (1..6: the pad's own table, as wupstream counts them; libwup
   takes a percentage), off and on. Told back to the computer as a level (1..6), the menu's own taken to the nearest. */
static u32 light_now, light_off;
static int level_now(void) {
    if (light_now & 7) return (int)(light_now & 7) > 6 ? 6 : (int)(light_now & 7);
    int p = wup_backlight_level(), best = 1;
    for (int l = 2; l <= 6; l++) if (abs(hw_backlight_percent(l) - p) < abs(hw_backlight_percent(best) - p)) best = l;
    return best;
}
static void light(u32 v) {                                /* (from every hello) */
    if (v == light_now) return;
    light_now = v;
    if (v & 0x40) { if (!light_off) wup_backlight(0); light_off = 1; return; }
    light_off = 0; wup_backlight(light_now & 7 ? hw_backlight_percent(level_now()) : wup_backlight_level());
}

/* ---- the screen while no computer streams */
static int net_ok;
static void draw_waiting(void) {
    wup_screen();
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(24, 44, 96), C24(6, 10, 30));
    if (!net_ok) {
        gfx_text_c(&font_big, GFX_W / 2, 220, "No Wi-Fi", RGB(255, 255, 255));
        gfx_text_c(&font_body, GFX_W / 2, 270, "Connect to Wi-Fi in the Homebrew Menu's Settings", RGB(190, 200, 225));
    } else {
        gfx_text_c(&font_big, GFX_W / 2, 200, "Waiting for a Stream", RGB(255, 255, 255));
        gfx_text_c(&font_body, GFX_W / 2, 250, "Start a wupstream program on a computer on this network", RGB(190, 200, 225));
        gfx_text_c(&font_body, GFX_W / 2, 290, wup_net_ip_str(), RGB(150, 165, 200));
    }
    gfx_text_c(&font_body, GFX_W / 2, 440, "Press B to return to the menu", RGB(150, 165, 200));
    gfx_present_all();
}
static void end_session(void) {
    if (shown) { gfx_brightness(0); wup_video_close(); draw_waiting(); }   /* (the screen was the decoder's: drawn again, dark) */
    motor(0, 0); mic(0);
    if (light_off) light_off = 0;
    if (light_now) { light_now = 0; wup_backlight(wup_backlight_level()); }
    peer_on = von = sync = 0; need_idr = 1;
    if (shown) gfx_fade(0, 256);
    shown = 0;
    wup_log("wupstream: stream ended");
}
static void packet(const u8 *d, u32 len, const struct sockaddr_in *from, u32 now) {
    u8 h[16] __attribute__((aligned(4)));
    u32 n = len < 16 ? len : 16;
    memcpy(h, d, n);
    if (n < 4 || (int)(now - quiet_until) < 0) return;
    if (!peer_on && h[0] == 'H') {
        peer_sa = *from; peer_on = 1; need_idr = 1; frames = keys = lost = resent = stalls = full = gap_max = 0; stat_ms = t_start = now;
        wup_log("wupstream: stream started");
    }
    if (!peer_on || from->sin_addr.s_addr != peer_sa.sin_addr.s_addr || from->sin_port != peer_sa.sin_port) return;
    last_ms = now;
    if (h[0] == 'H') { mic(h[2] & 1); light(h[3]); }      /* (hello: byte 2 bit 0 = the microphone, byte 3 = the backlight) */
    if (h[0] == 'V' && n == 16) video(d + 16, len, h, now);
    else if (h[0] == 'A' && len > 8) audio(d + 8, len);
    else if (h[0] == 'R') motor(h[1] & 1, now);
    else if (h[0] == 'Q') { end_session(); quiet_until = now + 1000; }
}
static void tick(u32 now) {                               /* every 1-2 ms */
    if (now - poll_ms > gap_max && poll_ms) gap_max = now - poll_ms;
    poll_ms = now;
    if (peer_on && now - last_ms > 1500) end_session();  /* the computer is gone */
    if (!peer_on) {
        if (now - beacon_ms >= 1000) {
            static const u8 b[4] = { 'B', 1 };
            struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(50001), .sin_addr.s_addr = 0xffffffffu };
            send_to(&to, b, 4); beacon_ms = now;
        }
        return;
    }
    if (sync && (int)(seen_f - next_f) >= 0 && as[0].t_head) {
        if (now - as[0].t_head > 45 && (as[0].known ? as[0].have != as[0].npk : seen_f != next_f)) { lost++; need_idr = 1; sync = 0; }   /* given up */
        else nack(now);
    }
    watchdog(now);
    if (motor_on && now - motor_ms > 100) motor(0, now);
    if (mic_on) mic_poll();
    if (now - stat_ms >= 5000) {                             /* every 5 s: what the stream did (tools/padlog.py) */
        static const char *const name[] = { "frames ", " keyframes ", " lost ", " resent ", " stalls ", " full ", " gap-ms " };
        u32 v[] = { frames, keys, lost, resent, stalls, full, gap_max };
        char l[200], *e = l;
        for (int i = 0; i < 7; i++) {
            for (const char *q = name[i]; *q; q++) *e++ = *q;
            char t[12]; int k = 0; u32 x = v[i];
            do t[k++] = (char)('0' + x % 10); while (x /= 10);
            while (k) *e++ = t[--k];
        }
        *e = 0; wup_log(l); stat_ms = now; gap_max = 0;
    }
    const volatile u8 *report = wup_input_report();
    u16 seq = (u16)(report[0] | report[1] << 8);
    if (seq != in_seq || now - in_ms > 50) {
        u8 m[8 + 128] = { 'I', (u8)(need_idr | waiting() << 1) };
        u32 since = wup_us_since_vblank();                   /* since the screen's last refresh (the computer times frames to it) */
        m[2] = (u8)since; m[3] = (u8)(since >> 8); m[4] = (u8)(light_off ? 0 : level_now());
        for (int i = 0; i < 128; i++) m[8 + i] = report[i];
        send_peer(m, sizeof m); in_seq = seq; in_ms = now;
    }
}

int main(void) {
    last_clk = CLOCK;
    for (int i = 0; i < NASM; i++) if (!(as[i].data = malloc(FRAME_MAX))) return 1;   /* (frames put together: 700 KB) */
    net_ok = wup_net_init() == 0;
    if (net_ok) {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(50000), .sin_addr.s_addr = INADDR_ANY };
        if (sock < 0 || bind(sock, (struct sockaddr *)&me, sizeof me)) net_ok = 0;
    }
    wup_audio_open();
    draw_waiting();
    unsigned held = wup_buttons();
    static u8 buf[1536] __attribute__((aligned(4)));
    for (;;) {
        wup_sleep(peer_on ? 1 : 16);                         /* (the network runs in its own threads) */
        u32 now = clock_ms();
        if (net_ok) {
            for (;;) {
                struct sockaddr_in from; socklen_t fl = sizeof from;
                int n = (int)recvfrom(sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
                if (n <= 0) break;
                packet(buf, (u32)n, &from, clock_ms());
            }
            tick(clock_ms());
        }
        unsigned b = wup_buttons(), pressed = b & ~held; held = b;
        if (!peer_on && (pressed & WUP_B)) break;            /* (while a stream runs, the buttons are the computer's) */
        (void)now;
    }
    if (peer_on) end_session();
    return 0;
}
