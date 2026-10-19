#!/usr/bin/env sh
# Build the Windows player zip: dist/TLKRecomp-windows-x86_64.zip, and the
# same tlk_sdl.exe alone: dist/TLKRecomp-windows-x86_64.exe (its shader
# presets and font are compiled in and unpacked to %APPDATA%\TLKRecomp).
#
# usage: scripts/package_win.sh [-b builddir]
# Cross-compiles with MinGW-w64 (cmake/mingw-w64-x86_64.cmake) into builddir
# (default build-win/) with -DTLK_STATIC_SDL=ON: SDL3 and the MinGW runtime
# are linked in, so tlk_sdl.exe needs only system DLLs (checked below). Packs
# tlk_sdl.exe + README.txt + licence notes, as scripts/package.sh does.
# The ROM, extracted assets and generated game C are never packed.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-win"
while getopts "b:" opt; do
    case $opt in
        b) BUILD=$OPTARG ;;
        *) sed -n '2,10p' "$0"; exit 2 ;;
    esac
done
NAME=TLKRecomp-windows-x86_64
DIST="$ROOT/dist"
STAGE="$DIST/stage-win"
PKG="$DIST/$NAME"
OBJDUMP=x86_64-w64-mingw32-objdump

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake"
cmake --build "$BUILD" --target tlk_sdl -j"$(nproc 2>/dev/null || echo 4)"
rm -rf "$STAGE" "$PKG"
cmake --install "$BUILD" --prefix "$STAGE"

mkdir -p "$PKG"
cp "$STAGE/bin/tlk_sdl.exe" "$PKG/"
cp -r "$STAGE/bin/assets" "$PKG/"
x86_64-w64-mingw32-strip "$PKG/tlk_sdl.exe" 2>/dev/null || true
cp "$STAGE"/share/doc/*/README.txt "$STAGE"/share/doc/*/LICENSE-NOTES.txt \
   "$STAGE"/share/doc/*/LICENSE-*.txt "$PKG/"
rm -rf "$STAGE"
# Notepad-friendly line endings for the text files.
for f in "$PKG"/*.txt; do sed -i 's/\r*$/\r/' "$f"; done
# Only system DLLs may be imported (no SDL3.dll, libgcc, libwinpthread...).
dlls=$($OBJDUMP -p "$PKG/tlk_sdl.exe" | sed -n 's/^[[:space:]]*DLL Name: //p')
bad=$(printf '%s\n' "$dlls" | grep -viE '^(kernel32|user32|gdi32|winmm|imm32|ole32|oleaut32|version|advapi32|setupapi|shell32|msvcrt|ucrtbase|api-ms-win-[a-z0-9-]+|ws2_32|dbghelp|cfgmgr32|hid|dinput8|dxgi|d3d9|d3d11|d3d12|xinput1_4|comdlg32|shlwapi|uuid|bcrypt|rpcrt4|dwmapi|uxtheme|dxguid|opengl32|d2d1|dwrite|windowscodecs)\.dll$' || true)
[ -z "$bad" ] || { echo "tlk_sdl.exe imports non-system DLLs: $bad" >&2; exit 1; }

# Nothing but the binary, the text files, the shader presets and the menu
# font may go in.
bad=$(find "$PKG" -type f ! -name tlk_sdl.exe ! -name '*.txt' \
      ! -path "$PKG/assets/shaders/*" ! -path "$PKG/assets/fonts/*.ttf" -print)
[ -z "$bad" ] || { echo "unexpected files in package: $bad" >&2; exit 1; }
if find "$PKG" -iname '*.sfc' -o -iname '*.smc' -o -name '*.c' -o -name '*.h' | grep -q .; then
    echo "ROM or source file in package" >&2; exit 1
fi

rm -f "$DIST/$NAME.zip"
(cd "$DIST" && zip -qr "$NAME.zip" "$NAME")
cp "$PKG/tlk_sdl.exe" "$DIST/$NAME.exe"
rm -rf "$PKG"
echo "$DIST/$NAME.zip"
unzip -l "$DIST/$NAME.zip"
echo "$DIST/$NAME.exe"
