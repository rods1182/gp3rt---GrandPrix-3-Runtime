#!/bin/sh
# Build gp3rt ddraw.dll (32-bit Windows) with clang + lld-link; no Windows SDK needed.
set -e
cd "$(dirname "$0")"
OUT=build/win
mkdir -p $OUT/obj $OUT/lib
R=$(clang -print-resource-dir)
CFLAGS="--target=i686-pc-windows-msvc -O2 -msse2 -mfpmath=sse -ffreestanding -nostdinc -isystem $R/include -isystem src/win/crt -Isrc -Isrc/win -D_WIN32 -DGP3_WIN -fno-stack-protector -Wall -Wno-unused-parameter -Wno-unused-function -Wno-pragma-pack -Wno-microsoft-anon-tag -fms-extensions"
SRC="src/ddraw.c src/d3d.c src/rdr_soft.c src/rdr_gl.c src/rdr_capture.c src/rdr_framedump.c src/png.c src/gamepatch.c src/win/win_main.c src/win/win_plat.c src/win/win_crt.c src/win/win_gpu.c"
OBJS=""
for f in $SRC; do o=$OUT/obj/$(basename $f .c).obj; clang $CFLAGS -c $f -o $o; OBJS="$OBJS $o"; done
llvm-rc-18 /l 0x416 /fo $OUT/obj/ddraw.res src/win/ddraw.rc
for d in src/win/defs/*.def; do llvm-dlltool-18 -m i386 -k -d $d -l $OUT/lib/$(basename $d .def).lib; done
lld-link-18 /nologo /dll /machine:x86 /subsystem:windows /safeseh:no /nodefaultlib /entry:DllMain /def:src/win/ddraw.def \
    /errorlimit:0 /map:$OUT/ddraw.map /out:$OUT/ddraw.dll $OBJS $OUT/obj/ddraw.res $OUT/lib/*.lib
echo "built $OUT/ddraw.dll"
