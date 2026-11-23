# wup-stream

Stream to a Wii U GamePad over Wi-Fi, without a Wii U. The GamePad shows video, plays sound and sends its
controls back; your program makes the pictures and sound with **libwupstream**.

- `pad/` runs on the GamePad: **WUP Stream**, an app for the Homebrew Menu ([wup-homebrew](https://github.com/ravensclawx/wup-homebrew)),
  built with [libwup](https://github.com/ravensclawx/wup-homebrew/tree/main/libwup): the receiver that feeds the GamePad's own H.264
  decoder and speaker and sends the controls back. The GamePad joins Wi-Fi in the Homebrew Menu's settings.
- `host/` is libwupstream for macOS, Linux and Windows: x264, patched for the GamePad's decoder, inside one
  static library.

## Using libwupstream

```c
#include "wupstream.h"

wupstream *pad = wupstream_open(NULL, 30000);       /* the first GamePad waiting on the network */
wupstream_input_t in;
while (wupstream_input(pad, &in) == 0) {            /* buttons, sticks, touch, motion */
    /* draw 854 x 480 pixels */
    wupstream_frame(pad, pixels, 854 * 4, WUPSTREAM_RGBA);   /* returns when it went out: one per refresh, 59.94 a second */
    wupstream_audio(pad, samples, n);                         /* 48 kHz stereo: 800.8 frames per video frame */
}
wupstream_close(pad);
```

Build with `sh host/build.sh` (macOS, Linux, or Windows in an MSYS2 UCRT64 shell). It writes
`build/host/libwupstream.a` and `build/host/sketch`, the example in `host/examples/sketch.c`. The GamePad and the
computer must be on the same network.

The examples in `host/examples`: `sketch.c` (draw on the GamePad with your finger: touch, buttons, sound, rumble),
`pipe.c` (video from ffmpeg).

What a program gets: the picture and sound out; buttons, both sticks (and their clicks), the touch screen, the
accelerometer and gyroscope, the volume slider, the battery (percent, charging, plugged in) and the microphone
(`wupstream_mic`: 16-bit mono, 48 kHz) in; the rumble motor (`wupstream_rumble`) and the screen's brightness
(`wupstream_backlight`, 1..6, or 0 = off) out. Several GamePads: one `wupstream_open` each. Not there: camera, NFC, infrared.

Brightness: six levels, the pad's own table (its EEPROM; the Wii U's five levels are the first five, or with its
brighter mode the top two move one up). A program's level lasts while it streams; `keep` makes it wupstream's own level (kept in wupstream's
config, used at every start and between streams). Nintendo's level (the pad's EEPROM, which the Wii U and Nintendo's
mode use) is only read, as the default before anything was kept; it is never changed.

A program slower than the GamePad's screen needs nothing special: the library sends the last picture again in
between. libwupstream contains x264 (GPL): programs that link it must be GPL-compatible.

## The GamePad side

WUP Stream is an app like any other for the Homebrew Menu. With [libwup](https://github.com/ravensclawx/wup-homebrew/tree/main/libwup) (wup-homebrew) checked out next
to this folder:

```
python3 ../wup-homebrew/libwup/tools/wupbuild.py pad                       # -> build/apps/wup_stream.wup
python3 ../wup-homebrew/libwup/tools/wuplink.py build/apps/wup_stream.wup   # press Y in the Homebrew Menu first: it is kept and started
```

Open it from the Homebrew Menu ("Waiting for a Stream"), then start a program here. Only GamePad firmware 7677
(Nintendo's last) is supported. (The first pad side, a firmware of its own with a text menu and its own network stack,
is in this repository's history.)

While WUP Stream is running, hold **HOME + B for 2 seconds** to quit and return to the Homebrew Menu, even during an active stream. When waiting for a stream, **B** alone still exits.

## Examples

`host/examples`: `sketch.c` (draw on the GamePad with your finger: touch, buttons, sound, rumble) and `pipe.c` (video
from ffmpeg).

## Testing

`tools/stress.py` plays hard video sources on the GamePad (still, moving bars, scene cuts, noise...) and prints the
pad's own counters for each: frames, keyframes, frames lost, packets sent again, decoder stalls. `WUPSTREAM_DROP=100`
makes the library lose 1 packet in 100 on purpose. The pad also broadcasts a log line every 5 seconds while a stream
runs (UDP port 28803).

## How it works

**Install.** The stock firmware in Product1 gets one change: the call that picks the boot app (LVC 0x63a0e) goes to
a 104-byte hook in 160 bytes of unused strings. On every normal boot the hook loads the loader from flash 0x20000;
with A held the loader loads wupstream from 0x30000 (checked by CRC) and runs it, otherwise the stock firmware boots.
The slots are outside both firmware banks. A GamePad firmware update rewrites a bank and drops the hook.

**Storage.** The saved network is one 4 KB flash sector at 0x10000, outside both banks.

**The decoder's rules** (measured on the pad). Its screen refreshes 59.94 times a second, the decoder starts at most
one frame per refresh, and when a refresh comes with nothing queued it often stops until the next keyframe. So the
library sends exactly one frame per refresh, in step with the pad's screen (the pad reports its timing), and keeps two
frames queued on the pad as a cushion (33 ms). Wi-Fi loses packets; the cushion gives the time to ask for a lost one
again, so a loss does not cost a keyframe.

**Protocol.** Everything is UDP; the pad listens on port 50000.

| Direction | Message |
|---|---|
| pad → broadcast:50001 | `B`: announces itself while waiting |
| PC → pad | `H`: hello and keepalive (every 250 ms); it also asks for the microphone and says the brightness |
| PC → pad | `V`: video, a piece of a frame's raw slice data |
| PC → pad | `A`: 48 kHz PCM audio |
| PC → pad | `R`: the rumble motor on or off |
| PC → pad | `Q`: done |
| pad → PC | `I`: the 128-byte input report, time since the screen's refresh, frames waiting, "send a keyframe" |
| pad → PC | `M`: the microphone's samples, when the PC asked for them in its hello |
| pad → PC | `N`: these packets are missing, send them again |

The full details are in `pad/stream/stream.c`.

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)), as [wup-homebrew](https://github.com/ravensclawx/wup-homebrew). libwupstream links x264 (GPL-2.0-or-later).
