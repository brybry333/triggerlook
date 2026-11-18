#!/usr/bin/env bash
# build.sh: triggerlook.dll for 64-bit Windows CS:S, cross-compiled on Linux into win/build/out/ (clang-cl + xwin's
# MSVC CRT/SDK: MSVC ABI like the game, so the SDK headers give the Windows interface layouts). Same sources as the
# Linux build (src/, tl_platform_win.cpp instead of tl_platform_linux.cpp). Loaded by triggerlook.exe (MinGW-w64,
# win/launcher.c) or by anything that LoadLibrary's it (the bhoptools fork).
# Needs clang, lld, mingw-w64-gcc, an xwin splat in ~/.local/share/xwin (or XWIN=) and the SDK in third_party/source-sdk-2013 (see README).
# win/sdk-compat = CS:TV's two SDK headers patched for clang-cl. Run it through a space-free path:
#   ~/.local/lib/css/triggerlook/win/build.sh
set -euo pipefail
W=$(dirname "$0")
C=$(cd "$W/.." && pwd -L)
W=$C/win
X=${XWIN:-$HOME/.local/share/xwin}
B=$W/build
OUT=$B/out
mkdir -p "$OUT" "$B/obj"
SDK=$C/third_party/source-sdk-2013/src
MSVC="--target=x86_64-pc-windows-msvc -nologo -imsvc $X/crt/include -imsvc $X/sdk/include/ucrt -imsvc $X/sdk/include/um -imsvc $X/sdk/include/shared"
LIBS="-libpath:$X/crt/lib/x86_64 -libpath:$X/sdk/lib/um/x86_64 -libpath:$X/sdk/lib/ucrt/x86_64 -libpath:$SDK/lib/public/x64"
CF="$MSVC /clang:-ffile-prefix-map=$C=. -O2 -MT -EHs-c- -GR- -Wno-everything -D_WIN32 -DWIN32 -D_WINDOWS -D_WIN64 \
    -DCOMPILER_MSVC -DCOMPILER_MSVC64 -DCLIENT_DLL -D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DNDEBUG -I$W/sdk-compat \
    -I$SDK/public -I$SDK/public/tier0 -I$SDK/public/tier1 -I$SDK/common -I$C/src \
    -FI$W/sdk-compat/tier1/bitbuf.h -FI$W/sdk-compat/mathlib/ssemath.h"

python3 "$C/tools/embed.py" "$C/game" "$C/src/tl_files.h"
echo "== triggerlook.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -municode -mwindows -o "$OUT/triggerlook.exe" "$W/launcher.c" -static -ladvapi32 -lshell32 -luser32

echo "== triggerlook.dll"
objs=()
for f in "$C"/src/*.cpp; do
    n=$(basename "$f" .cpp)
    clang-cl $CF -c "$f" -Fo"$B/obj/$n.obj"
    objs+=("$B/obj/$n.obj")
done
clang-cl $MSVC -O2 -MT -Wno-everything -TC -c "$SDK/utils/lzma/C/LzmaDec.c" -Fo"$B/obj/LzmaDec.obj"
objs+=("$B/obj/LzmaDec.obj")
lld-link -nologo -dll -out:"$OUT/triggerlook.dll" $LIBS "${objs[@]}" \
    tier0.lib libcmt.lib libvcruntime.lib libucrt.lib kernel32.lib user32.lib
ls -la "$OUT"
