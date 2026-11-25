#!/bin/sh
# Build the native Linux runner (static i386, musl) used for testing.
set -e
cd "$(dirname "$0")"
mkdir -p build
CFLAGS="-O2 -g -Wall -Wno-unused-parameter -mstackrealign -fno-strict-aliasing -fno-omit-frame-pointer -D_GNU_SOURCE"
SRC="src/core.c src/pe.c src/thread.c src/kernel32.c src/user32.c src/gdi32.c src/misc.c src/ddraw.c src/d3d.c src/rdr_soft.c src/rdr_capture.c src/rdr_framedump.c src/harness_win.c src/dinput.c src/dsound.c src/plat_headless.c src/png.c src/gamepatch.c src/replayctl.c src/gpx_api.c src/ttf.c src/rptrace.c"
OBJS=""
for f in $SRC; do o=build/$(basename $f .c).o; ./cc32 $CFLAGS -c $f -o $o; OBJS="$OBJS $o"; done
./cc32 $OBJS -o build/gp3run -Wl,-Ttext-segment=0x20000000
echo "built build/gp3run"
