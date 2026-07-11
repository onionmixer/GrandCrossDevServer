#!/bin/bash
# toolchain/setup.sh - populate the win32/dos cross toolchains under
# toolchain/ from the cached archives (toolchain/archives/), downloading
# them first if they are missing. Idempotent: already-present tools are
# left as-is. After it runs, `source toolchain/env.sh` and build.
#
#   ./toolchain/setup.sh          # set up all three
#   ./toolchain/setup.sh mingw    # just llvm-mingw (win32)
#   ./toolchain/setup.sh ow watt  # Open Watcom + Watt-32 (dos)
#
# The archives are the canonical, version-pinned inputs and are kept in
# toolchain/archives/ so a fresh checkout on this machine needs no
# network. If an archive is absent, the documented upstream URL is
# fetched (best effort - upstream snapshot tags rotate; see doc/toolchain.md).
set -u

TC="$(cd "$(dirname "$0")" && pwd)"
ARC="$TC/archives"
mkdir -p "$ARC"

# archive filename  |  upstream URL (fallback if archive missing)
MINGW_A="llvm-mingw-20260616-msvcrt-i686.tar.xz"
MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-msvcrt-ubuntu-22.04-x86_64.tar.xz"
OW_A="open-watcom-v2-snapshot.tar.xz"
OW_URL="https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz"
WATT_A="watt32.zip"
WATT_URL="https://github.com/gvanem/Watt-32/archive/refs/heads/master.zip"

say() { echo "[setup] $*"; }

_fetch() {  # <archive> <url>
    local a="$ARC/$1" url="$2"
    [ -f "$a" ] && { say "archive present: $1"; return 0; }
    say "archive missing, downloading: $1"
    if command -v curl >/dev/null 2>&1; then curl -fL -o "$a" "$url"
    elif command -v wget >/dev/null 2>&1; then wget -O "$a" "$url"
    else say "no curl/wget - fetch $url into $ARC/$1 manually"; return 1
    fi
}

setup_mingw() {
    if [ -x "$TC/llvm-mingw/bin/i686-w64-mingw32-gcc" ]; then say "mingw: ready"; return; fi
    _fetch "$MINGW_A" "$MINGW_URL" || return
    say "extracting llvm-mingw ..."
    rm -rf "$TC/.mgwtmp"; mkdir -p "$TC/.mgwtmp"
    tar xf "$ARC/$MINGW_A" -C "$TC/.mgwtmp"
    mv "$TC"/.mgwtmp/llvm-mingw-* "$TC/llvm-mingw"; rmdir "$TC/.mgwtmp" 2>/dev/null
    [ -x "$TC/llvm-mingw/bin/i686-w64-mingw32-gcc" ] && say "mingw: OK" || say "mingw: FAILED"
}

setup_ow() {
    if [ -x "$TC/ow/binl64/wcl" ]; then say "Open Watcom: ready"; return; fi
    _fetch "$OW_A" "$OW_URL" || return
    say "extracting Open Watcom ..."
    rm -rf "$TC/.owtmp"; mkdir -p "$TC/.owtmp"
    tar xf "$ARC/$OW_A" -C "$TC/.owtmp"
    # snapshot extracts to a top dir (usually ow2/); take whatever holds binl64
    local d; d="$(dirname "$(find "$TC/.owtmp" -maxdepth 3 -type d -name binl64 | head -1)")"
    [ -n "$d" ] && mv "$d" "$TC/ow"; rm -rf "$TC/.owtmp"
    [ -x "$TC/ow/binl64/wcl" ] && say "Open Watcom: OK" || say "Open Watcom: FAILED"
}

setup_watt() {
    if [ -f "$TC/watt32/lib/wattcpwl.lib" ]; then say "Watt-32: ready (prebuilt lib)"; return; fi
    _fetch "$WATT_A" "$WATT_URL" || return
    say "extracting Watt-32 ..."
    rm -rf "$TC/.wtmp"; mkdir -p "$TC/.wtmp"
    unzip -q "$ARC/$WATT_A" -d "$TC/.wtmp"
    local d; d="$(find "$TC/.wtmp" -maxdepth 2 -type d -name src | head -1)"
    [ -n "$d" ] && mv "$(dirname "$d")" "$TC/watt32"; rm -rf "$TC/.wtmp"
    if [ ! -f "$TC/watt32/lib/wattcpwl.lib" ]; then
        say "Watt-32 lib not prebuilt - build the 16bit large-model lib:"
        say "  ./make/build-watt32.sh $TC/watt32 $TC/ow   (needs Open Watcom + DOSBox-X)"
    fi
}

TARGETS=("$@"); [ ${#TARGETS[@]} -eq 0 ] && TARGETS=(mingw ow watt)
for t in "${TARGETS[@]}"; do
    case "$t" in
        mingw|win32) setup_mingw ;;
        ow|watcom|dos) setup_ow ;;
        watt|watt32) setup_watt ;;
        *) echo "[setup] unknown: $t" ;;
    esac
done
say "done. now:  source toolchain/env.sh"
