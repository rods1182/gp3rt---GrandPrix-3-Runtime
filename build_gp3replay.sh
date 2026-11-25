#!/bin/sh
# Build GP3Replay.dll (32-bit Windows; also loaded by GP3RT on Linux) with clang + lld-link.
set -e
cd "$(dirname "$0")"
OUT=build/win
mkdir -p $OUT/obj $OUT/lib
R=$(clang -print-resource-dir)
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -nostdinc -isystem $R/include -Isrc -fno-stack-protector -Wall -c src/gp3replay.c -o $OUT/obj/gp3replay.obj
llvm-dlltool-18 -m i386 -k -d src/win/defs/kernel32.def -l $OUT/lib/kernel32.lib
lld-link-18 /nologo /dll /machine:x86 /subsystem:windows /safeseh:no /nodefaultlib /entry:DllMain \
    /export:GP3Replay_Status=_GP3Replay_Status@8 /out:$OUT/GP3Replay.dll $OUT/obj/gp3replay.obj $OUT/lib/kernel32.lib
echo "built $OUT/GP3Replay.dll"
