# WUP Stream

Use the Wii U GamePad as a wireless screen and controller for your computer, without a Wii U. Your program draws
and plays sound with **libwupstream**; the GamePad shows it, plays it, and sends back everything it senses: buttons,
sticks, touch, motion, the microphone and the camera.

- **`pad/`** is **WUP Stream**, the app on the GamePad, for [wup-homebrew](https://github.com/ravensclawx/wup-homebrew)
  2. Get it from the Homebrew Menu's App Store. It feeds the GamePad's hardware video decoder and speakers, and sends
  the controls, microphone and camera back.
- **`host/`** is **libwupstream** for macOS, Linux and Windows: one static library with x264 inside (patched for the
  GamePad's decoder), and the examples.

## Try it

1. On the GamePad: the Homebrew Menu, join Wi-Fi (Settings), get **WUP Stream** from the App Store (+) and open it:
   "Waiting for a Stream".
2. On a computer on the same network: `sh host/build.sh`, then run an example:

```
build/host/sketch                    # draw on the GamePad with your finger
build/host/play movie.mp4            # a video file with sound (needs ffmpeg)
build/host/padtest                   # every input, the rumble, the speakers, the brightness: check a pad
build/host/mirror echo               # the GamePad's camera, back on its own screen; its microphone as a bar and an echo
build/host/camera | ffplay -f h264 -flags low_delay -i -     # the GamePad's camera on this computer
```

Each finds the first GamePad waiting on the network (or give its IP address). **Home** ends it.

## Using libwupstream

```c
#include "wupstream.h"

wupstream *pad = wupstream_open(NULL, 30000);                 /* the first GamePad waiting on the network */
wupstream_input_t in;
while (wupstream_input(pad, &in) == 0) {                      /* buttons, sticks, touch, motion, volume, battery */
    /* draw 854 x 480 pixels */
    wupstream_frame(pad, pixels, 854 * 4, WUPSTREAM_RGBA);   /* returns when it went out: one per refresh, 59.94 a second */
    wupstream_audio(pad, samples, n);                         /* 48 kHz stereo: 800.8 frames per video frame */
}
wupstream_close(pad);
```

| In (from the GamePad) | Out (to the GamePad) |
|---|---|
| buttons (Home, TV and the stick clicks too), both sticks, the touch screen | the picture (`wupstream_frame`, 854 x 480) |
| accelerometer and gyroscope, the volume slider, the battery | sound (`wupstream_audio`, 48 kHz stereo) |
| the microphone (`wupstream_mic`: 16-bit mono, 48 kHz) | the rumble motor (`wupstream_rumble`) |
| the camera (`wupstream_camera`: 640 x 480 H.264, about 59 frames a second) | the screen's brightness (`wupstream_backlight`, 1..6 or off) |

Not there: NFC, infrared. Several GamePads: one `wupstream_open` each.

A program slower than the GamePad's screen needs nothing special: the library sends the last picture again in
between. Brightness: six levels, the GamePad's own table. A program's level lasts while it streams; `keep` makes it
wupstream's own level (kept in wupstream's config). Nintendo's level is only read, never changed.

`sh host/build.sh` (macOS, Linux, or Windows in an MSYS2 UCRT64 shell) writes `build/host/libwupstream.a` and the
examples; `mirror` and `play` need fork and pipes (macOS, Linux). libwupstream contains x264 (GPL): programs that link
it must be GPL-compatible.

## The GamePad side

WUP Stream is an app like any other for the Homebrew Menu. With [wup-homebrew](https://github.com/ravensclawx/wup-homebrew)
checked out next to this folder:

```
python3 ../wup-homebrew/libwup/tools/wupbuild.py pad                       # -> build/apps/wup_stream.wup
python3 ../wup-homebrew/libwup/tools/wuplink.py build/apps/wup_stream.wup   # press Y in the Homebrew Menu first: kept and started
```

Open it from the Homebrew Menu ("Waiting for a Stream"), then start a program here. Only GamePad firmware 7677
(Nintendo's last) is supported. (The first pad side, a firmware of its own with a text menu and its own network stack,
is in this repository's history.)

While WUP Stream is running, hold **HOME + B for 2 seconds** to quit and return to the Homebrew Menu, even during an active stream. When waiting for a stream, **B** alone still exits.
## Testing

`tools/stress.py` plays hard video sources on the GamePad (still, moving bars, scene cuts, noise...) and prints the
pad's own counters for each. `WUPSTREAM_DROP=100` makes the library lose 1 packet in 100 on purpose;
`WUPSTREAM_DEBUG=1` prints its statistics at the end.

## How it works

**The decoder's rules** (measured on the pad). Its screen refreshes 59.94 times a second, the decoder starts at most
one frame per refresh, and when a refresh comes with nothing queued it often stops until the next keyframe. So the
library sends exactly one frame per refresh, in step with the pad's screen (the pad reports its timing), and keeps two
frames queued on the pad as a cushion (33 ms). Wi-Fi loses packets; the cushion gives the time to ask for a lost one
again, so a loss does not cost a keyframe.

**The camera** goes through the GamePad's hardware H.264 encoder, running beside the decoder. Its frames go out in
1 KB parts, paced so the controls and the requests for lost video always find room on the radio. A lost camera frame
starts the encoder again (a fresh keyframe); the library hands on only frames in step.

**Protocol.** Everything is UDP; the pad listens on port 50000.

| Direction | Message |
|---|---|
| pad → broadcast:50001 | `B`: announces itself while waiting |
| PC → pad | `H`: hello and keepalive (every 250 ms); it also asks for the microphone and the camera, and says the brightness |
| PC → pad | `V`: video, a piece of a frame's raw slice data |
| PC → pad | `A`: 48 kHz PCM audio |
| PC → pad | `R`: the rumble motor on or off |
| PC → pad | `K`: start the camera's encoder again (a frame was lost) |
| PC → pad | `Q`: done |
| pad → PC | `I`: the 128-byte input report, time since the screen's refresh, frames waiting, "send a keyframe" |
| pad → PC | `M`: the microphone's samples |
| pad → PC | `C`: the camera, a part of an encoded frame |
| pad → PC | `N`: these packets are missing, send them again |

The full details are in `pad/main.c`.

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)), as [wup-homebrew](https://github.com/ravensclawx/wup-homebrew). libwupstream
links x264 (GPL-2.0-or-later).
