#!/bin/sh
# Build the Linux release: gp3 (64-bit host: SDL2 + OpenGL) and gp3-game
# (32-bit static game process). SDL2 headers/lib: $SDL2_PREFIX (default deps64/sdl2-inst).
set -e
cd "$(dirname "$0")"
OUT=build/linux
mkdir -p $OUT/obj
SDL2_PREFIX=${SDL2_PREFIX:-/usr}
CFLAGS="-O2 -g -Wall -Wno-unused-parameter -mstackrealign -fno-strict-aliasing -fno-omit-frame-pointer -D_GNU_SOURCE"
SRC="src/core.c src/pe.c src/thread.c src/kernel32.c src/user32.c src/gdi32.c src/misc.c src/ddraw.c src/d3d.c src/rdr_soft.c src/rdr_capture.c src/rdr_framedump.c src/harness_win.c src/dinput.c src/dsound.c src/plat_bridge.c src/png.c src/gamepatch.c src/replayctl.c src/gpx_api.c src/ttf.c"
OBJS=""
for f in $SRC; do o=$OUT/obj/$(basename $f .c).o; ./cc32 $CFLAGS -DGP3_BRIDGE -c $f -o $o; OBJS="$OBJS $o"; done
./cc32 $OBJS -o $OUT/gp3-game -Wl,-Ttext-segment=0x20000000
echo "built $OUT/gp3-game"
# GPxPatch configuration page, compiled into gp3 (host/launcher.c host/icons.c)
python3 -c "import sys; d=open('host/launcher_ui.html','rb').read(); sys.stdout.write('static const char g_launcher_html[] = {' + ','.join(str(b) for b in d) + ',0};\n')" > host/launcher_ui.h
# GP3RT icon (art/gp3rt-*.png) compiled into gp3
python3 -I -c "
import sys
from PIL import Image
sizes=[16,24,32,48,64,128,256,512]
o=[]
for z in sizes:
    d=open('art/gp3rt-%d.png'%z,'rb').read()
    o.append('static const unsigned char g_icon_%d[] = {%s};' % (z, ','.join(str(b) for b in d)))
o.append('static const struct { int size; const unsigned char *png; unsigned len; } g_gp3rt_icons[] = {' + ','.join('{%d,g_icon_%d,sizeof g_icon_%d}'%(z,z,z) for z in sizes) + '};')
o.append('#define GP3RT_NICONS %d' % len(sizes))
for z in sizes:
    d=open('art/gpxpatch-%d.png'%z,'rb').read()
    o.append('static const unsigned char g_xicon_%d[] = {%s};' % (z, ','.join(str(b) for b in d)))
o.append('static const struct { int size; const unsigned char *png; unsigned len; } g_gpxp_icons[] = {' + ','.join('{%d,g_xicon_%d,sizeof g_xicon_%d}'%(z,z,z) for z in sizes) + '};')
for z in (64,128):
    rgba=Image.open('art/gpxpatch-%d.png'%z).convert('RGBA').tobytes()
    o.append('static const unsigned char g_gpxp_icon%d_rgba[] = {%s};' % (z, ','.join(str(b) for b in rgba)))
rgba=Image.open('art/gp3rt-64.png').convert('RGBA').tobytes()
o.append('static const unsigned char g_gp3rt_icon64_rgba[] = {%s};' % ','.join(str(b) for b in rgba))
open('host/icons_data.h','w').write('\n'.join(o)+'\n')
"
# native GPxPatch screen: translations (from the EN table of launcher_ui.html) and the Inter font
python3 -I - <<'PY'
import re, json
h = open('host/launcher_ui.html', encoding='utf-8').read()
js = h.split('<script>')[1]
en = js[js.index('const EN = {') + len('const EN = {'):]
pairs, i = [], 0
str_re = re.compile(r'"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s*,?')
for m in str_re.finditer(en):
    if en[:m.start()].count('};'): break
    pairs.append((m.group(1), m.group(2)))
def c(s): return json.dumps(json.loads('"' + s + '"'), ensure_ascii=False)
seen = set(); out = []
for a, b in pairs:
    if a in seen: continue
    seen.add(a); out.append('{%s,%s}' % (c(a), c(b)))
open('host/ui_i18n.h', 'w', encoding='utf-8').write('static const char *const g_i18n[][2] = {\n' + ',\n'.join(out) + '\n};\n')
f = lambda p: ','.join(str(x) for x in open(p, 'rb').read())
from fontTools.ttLib import TTFont as _TT
_cj = sorted(c for c in _TT('art/fonts/NotoSansSC-sub.otf').getBestCmap() if c >= 0x2e80)
open('host/classic_font.h', 'w').write('static const unsigned char g_classic_font[] = {%s};\nstatic const unsigned char g_classic_cjk[] = {%s};\nstatic const nk_rune g_cjk_ranges[] = {%s,0};\n' % (f('art/fonts/DejaVuSansCondensed-sub.ttf'), f('art/fonts/NotoSansSC-sub.otf'), ','.join('%d,%d' % (c, c) for c in _cj)))
open('host/ui_fonts.h', 'w').write('static const unsigned char g_inter_regular[] = {%s};\nstatic const unsigned char g_inter_semibold[] = {%s};\n' % (f('art/fonts/Inter-Regular-sub.otf'), f('art/fonts/Inter-SemiBold-sub.otf')))
PY
gcc -O2 -g -w -fno-strict-aliasing -std=gnu11 -D_DEFAULT_SOURCE -fPIE -I$SDL2_PREFIX/include/SDL2 -c host/nk_sdl.c -o $OUT/obj/nk_sdl.o
# host: -D_DEFAULT_SOURCE (not _GNU_SOURCE) keeps glibc from binding the C23 strtol/sscanf
# (GLIBC_2.38); start.S binds __libc_start_main to GLIBC_2.2.5 -> runs on glibc >= 2.17
HCC="gcc -O2 -g -Wall -Wno-unused-parameter -Wno-misleading-indentation -fno-strict-aliasing -std=gnu11 -D_DEFAULT_SOURCE -fPIE -pie -nostartfiles host/start.S -I$SDL2_PREFIX/include/SDL2"
HLD="-L$SDL2_PREFIX/lib -lSDL2 -lm -Wl,-rpath,\$ORIGIN/lib -Wl,--as-needed"
# gp3: GP3RT for Linux (the game). The GPxPatch screen is the separate program below.
$HCC -DGP3_NO_GPXUI host/gp3.c host/joy.c host/icon.c host/icons.c src/rdr_gl.c src/png.c -o $OUT/gp3 $HLD
# GPxPatch: GPxPatch for Linux (the configuration screen; it starts the game through gp3)
$HCC -DGPXP_MAIN host/gp3.c host/joy.c host/icon.c host/launcher.c host/icons.c host/gpxui.c $OUT/obj/nk_sdl.o src/rdr_gl.c src/png.c -o $OUT/GPxPatch $HLD
echo "built $OUT/GPxPatch"
echo "glibc symbol versions needed by gp3:"; objdump -T $OUT/gp3 | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
echo "built $OUT/gp3"
