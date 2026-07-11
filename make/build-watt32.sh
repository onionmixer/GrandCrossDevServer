#!/bin/bash
# build-watt32.sh - reproduce the 16-bit large-model Watt-32 library
# our DOS TCP daemon links against (doc/dos.md).
#
#   ./make/build-watt32.sh <watt32-src-dir> <open-watcom-dir>
#
# Needs: Linux gcc + libslang (for util/mkmake), Open Watcom v2
# (Linux-hosted wcc/wmake/wlib), and DOSBox-X (to run wcerr, which
# must be a DOS binary to emit Watcom's errno values). Set DOSBOXX
# to the dosbox-x binary if not on PATH.
set -e

WATT="${1:?usage: build-watt32.sh <watt32-src> <ow-dir>}"
export WATCOM="${2:?usage: build-watt32.sh <watt32-src> <ow-dir>}"
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h"
DOSBOXX="${DOSBOXX:-dosbox-x}"

cd "$WATT/src"

echo "== 1. mkmake (Linux) =="
gcc -O2 -o ../util/linux/mkmake ../util/mkmake.c -lslang

echo "== 2. generate large-model makefile =="
mkdir -p build/watcom/large
../util/linux/mkmake -w -o watcom_l.mak -d build/watcom/large \
    makefile.all WATCOM LARGE

echo "== 3. force USE_BSD_API in the __LARGE__ config block =="
# stock Watt-32 excludes the BSD socket API from large model; our
# net.c needs it. Idempotent.
if ! grep -q "GCDS: BSD sockets in large model" config.h; then
  perl -0pi -e 's/(#if !defined\(OPT_DEFINED\) && defined\(__LARGE__\)\n  #define USE_DEBUG\n)(  #define OPT_DEFINED)/$1  #define USE_BSD_API      \/* GCDS: BSD sockets in large model *\/\n$2/' config.h
fi

echo "== 4. errno files (wcerr must run under DOS) =="
wcl -zq -bcl=dos -ml -wx -I../inc -I"$WATCOM/h" -fe=wcerr.exe errnos.c \
    ../util/errnos.c 2>/dev/null || \
  wcl -zq -bcl=dos -ml -wx -I../inc -I"$WATCOM/h" -fe=wcerr.exe \
    ../util/errnos.c
WD="$(mktemp -d)"
cp wcerr.exe "$WD/wcerr.exe"
cat > "$WD/gen.bat" <<'BAT'
@echo off
c:
wcerr -e > watcom.err
wcerr -s > syserr.c
BAT
cat > "$WD/dbx.conf" <<CFG
[autoexec]
mount c $WD
c:
call gen.bat
CFG
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$DOSBOXX" \
    -conf "$WD/dbx.conf" -nolog >/dev/null 2>&1 &
DP=$!; sleep 6; kill $DP 2>/dev/null || true
cp "$WD"/WATCOM.ERR ../inc/sys/watcom.err
cp "$WD"/SYSERR.C build/watcom/syserr.c
mkdir -p build/watcom/large
cp "$WD"/SYSERR.C build/watcom/large/syserr.c
rm -rf "$WD"

echo "== 5. wmake the library =="
wmake -h -f watcom_l.mak

echo "== done: $WATT/lib/wattcpwl.lib =="
ls -la "$WATT/lib/wattcpwl.lib"
