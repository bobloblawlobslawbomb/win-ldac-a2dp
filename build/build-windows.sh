#!/usr/bin/env bash
# Build a2dp_source_demo.exe with LDAC support, cross-compiling from Linux/WSL
# (or natively in MSYS2 mingw64 on Windows).
#
#   sudo apt install mingw-w64 git make
#   ./build/build-windows.sh
#
# Output: dist/a2dp_source_demo.exe
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$REPO_ROOT/.build}
DIST="$REPO_ROOT/dist"

BTSTACK_REPO=${BTSTACK_REPO:-https://github.com/bluekitchen/btstack.git}
BTSTACK_REV=${BTSTACK_REV:-9edb5d82ea6ec4466a393415d65030929330acdf}
LIBLDAC_REPO=${LIBLDAC_REPO:-https://github.com/wdv4758h/libldac.git}
LIBLDAC_REV=${LIBLDAC_REV:-8bd45f45c0384e9ee3c06e8d9fac4be0bc355558}

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    CROSS=x86_64-w64-mingw32-gcc
    MINGW_INC=${MINGW_INC:-/usr/x86_64-w64-mingw32/include}
elif command -v gcc >/dev/null 2>&1 && [ -d /mingw64/include ]; then
    CROSS=gcc                       # native MSYS2 mingw64
    MINGW_INC=${MINGW_INC:-/mingw64/include}
else
    echo "error: no mingw-w64 gcc found (apt install mingw-w64)" >&2
    exit 1
fi

echo "==> work dir: $WORK"
mkdir -p "$WORK" "$DIST"

fetch_pinned() {  # url dir rev
    local url=$1 dir=$2 rev=$3
    if [ ! -d "$dir/.git" ]; then
        git clone --quiet "$url" "$dir"
    fi
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo none)" != "$rev" ]; then
        git -C "$dir" fetch --quiet origin "$rev" 2>/dev/null || git -C "$dir" fetch --quiet --tags
        git -C "$dir" checkout --quiet "$rev"
    fi
}

echo "==> fetching BTstack @${BTSTACK_REV:0:10}"
fetch_pinned "$BTSTACK_REPO" "$WORK/btstack" "$BTSTACK_REV"

echo "==> fetching libldac @${LIBLDAC_REV:0:10}"
fetch_pinned "$LIBLDAC_REPO" "$WORK/libldac" "$LIBLDAC_REV"

echo "==> applying patch set"
cd "$WORK/btstack"
if git apply --check "$REPO_ROOT/patches/0001-btstack-windows-ldac.patch" 2>/dev/null; then
    git apply "$REPO_ROOT/patches/0001-btstack-windows-ldac.patch"
    echo "    applied"
elif git apply --check -R "$REPO_ROOT/patches/0001-btstack-windows-ldac.patch" 2>/dev/null; then
    echo "    already applied"
else
    echo "error: patch does not apply cleanly to this revision" >&2
    exit 1
fi

# The example target for a *new* file does not get a link rule in this tree, so the
# LDAC demo is built through the proven a2dp_source_demo target.
cp "$REPO_ROOT/src/wasapi_capture.c" "$REPO_ROOT/src/wasapi_capture.h" example/
cp "$REPO_ROOT/src/a2dp_ldac_demo.c" example/a2dp_ldac_demo.c
cp "$REPO_ROOT/src/a2dp_ldac_demo.c" example/a2dp_source_demo.c

# Windows headers are case-insensitive; mingw on Linux is not.
SHIM="$WORK/winshim"
if [ "$(uname -s)" != "MINGW64_NT" ] && [ "$(uname -s)" != "MSYS_NT" ]; then
    echo "==> generating header case shim"
    mkdir -p "$SHIM"
    # Only Windows/WinUSB headers: shimming libc headers breaks their include guards.
    add_shim() {
        local src="$MINGW_INC/$1"; shift
        [ -f "$src" ] || return 0
        local a; for a in "$@"; do ln -sf "$src" "$SHIM/$a"; done
    }
    add_shim windows.h        Windows.h windows.h
    add_shim winbase.h        WinBase.h winbase.h
    add_shim windef.h         WinDef.h windef.h
    add_shim winnt.h          WinNT.h winnt.h
    add_shim winsock2.h       Winsock2.h winsock2.h
    add_shim winusb.h         Winusb.h WinUSB.h winusb.h
    add_shim setupapi.h       SetupApi.h SetupAPI.h setupapi.h
    add_shim bluetoothapis.h  BluetoothAPIs.h bluetoothapis.h
    add_shim bthsdpdef.h      Bthsdpdef.h bthsdpdef.h
    add_shim ws2bth.h         Ws2bth.h ws2bth.h
    add_shim initguid.h       InitGuid.h initguid.h
    add_shim usb.h            Usb.h usb.h
    EXTRA_CFLAGS="-I$SHIM"
else
    EXTRA_CFLAGS=""
fi

echo "==> building (clean: header deps are not tracked by this Makefile)"
cd "$WORK/btstack/port/windows-winusb"
rm -f *.o *.exe
make a2dp_source_demo \
    CC="$CROSS $EXTRA_CFLAGS" \
    LDFLAGS="-lsetupapi -lwinusb -lwinmm"

cp a2dp_source_demo.exe "$DIST/"
echo
echo "==> done: $DIST/a2dp_source_demo.exe ($(du -h "$DIST/a2dp_source_demo.exe" | cut -f1))"
echo "    next: docs/SETUP.md"
