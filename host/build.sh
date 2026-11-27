#!/bin/sh
# Build libwupstream (wupstream.c and x264 with the GamePad patch, in one build/host/libwupstream.a) and the examples.
# macOS, Linux, or Windows in an MSYS2 UCRT64 shell. Needs a C compiler, make and git (nasm on x86 for x264's
# assembly; without it x264 is built without, and slower).
set -e
cd "$(dirname "$0")"
B=../build/host; X=$B/x264; mkdir -p $B
if [ ! -f $X/libx264.a ]; then
  if [ ! -d $X/.git ]; then
    git init -q $X
    git -C $X fetch -q --depth 1 https://code.videolan.org/videolan/x264.git b35605ace3ddf7c1a5d67a2eb553f034aef41d55
    git -C $X checkout -q FETCH_HEAD
    git -C $X apply "$PWD/x264-drh-mode.patch"
  fi
  ASM=; case "$(uname -m)" in x86*|i?86|amd64) command -v nasm >/dev/null || ASM=--disable-asm;; esac
  (cd $X && ./configure --enable-static --disable-cli --disable-opencl --bit-depth=8 $ASM >/dev/null && make -j8 libx264.a >/dev/null 2>&1)
fi
CC=${CC:-cc}
case "$(uname -s)" in MINGW*|MSYS*) LIBS="-lws2_32 -lwinmm -lpthread";; *) LIBS="-lpthread -ldl";; esac
$CC -O2 -Wall -I$X -c wupstream.c -o $B/wupstream.o
cp $X/libx264.a $B/libwupstream.a && ar rs $B/libwupstream.a $B/wupstream.o 2>/dev/null
for e in sketch camera padtest; do $CC -O2 -Wall -I. examples/$e.c $B/libwupstream.a -lm $LIBS -o $B/$e; done
case "$(uname -s)" in MINGW*|MSYS*) ;; *) for e in mirror play; do $CC -O2 -Wall -I. examples/$e.c $B/libwupstream.a -lm $LIBS -o $B/$e; done;; esac   # (fork and pipes: not on Windows)
echo "built $B/libwupstream.a and the examples (sketch, padtest, camera, mirror, play)"
