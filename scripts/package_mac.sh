#!/usr/bin/env sh
# Build the macOS player zip: dist/TLKRecomp-macos-arm64.zip
#
# usage: scripts/package_mac.sh [-b builddir]
# Run on a Mac. Configures builddir (default build-mac/) with
# -DTLK_STATIC_SDL=ON, so SDL3 is linked into tlk_sdl and players need no
# SDL3 install (checked below: only system libraries and frameworks). Packs
# tlk_sdl + README.txt + licence notes, as scripts/package.sh does. The
# binary is ad-hoc signed, not notarized, so Gatekeeper asks on first start.
# The ROM, extracted assets and generated game C are never packed.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-mac"
while getopts "b:" opt; do
    case $opt in
        b) BUILD=$OPTARG ;;
        *) sed -n '2,10p' "$0"; exit 2 ;;
    esac
done
NAME=TLKRecomp-macos-$(uname -m)
DIST="$ROOT/dist"
STAGE="$DIST/stage-mac"
PKG="$DIST/$NAME"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON
cmake --build "$BUILD" --target tlk_sdl -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
rm -rf "$STAGE" "$PKG"
cmake --install "$BUILD" --prefix "$STAGE"

mkdir -p "$PKG"
cp "$STAGE/bin/tlk_sdl" "$PKG/"
cp -r "$STAGE/bin/assets" "$PKG/"
/usr/bin/strip "$PKG/tlk_sdl"
# Stripping drops the linker's ad-hoc signature; arm64 macOS won't run
# an unsigned binary.
/usr/bin/codesign --force --sign - "$PKG/tlk_sdl"
cp "$STAGE"/share/doc/*/README.txt "$STAGE"/share/doc/*/LICENSE-NOTES.txt \
   "$STAGE"/share/doc/*/LICENSE-*.txt "$PKG/"
rm -rf "$STAGE"
# Only system libraries and frameworks may be linked (no SDL3 dylib).
bad=$(otool -L "$PKG/tlk_sdl" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)
[ -z "$bad" ] || { echo "tlk_sdl links non-system libraries: $bad" >&2; exit 1; }

# Nothing but the binary, the text files, the shader presets and the menu
# font may go in.
bad=$(find "$PKG" -type f ! -name tlk_sdl ! -name '*.txt' \
      ! -path "$PKG/assets/shaders/*" ! -path "$PKG/assets/fonts/*.ttf" -print)
[ -z "$bad" ] || { echo "unexpected files in package: $bad" >&2; exit 1; }
if find "$PKG" -iname '*.sfc' -o -iname '*.smc' -o -name '*.c' -o -name '*.h' | grep -q .; then
    echo "ROM or source file in package" >&2; exit 1
fi

rm -f "$DIST/$NAME.zip"
(cd "$DIST" && zip -qry "$NAME.zip" "$NAME")
rm -rf "$PKG"
echo "$DIST/$NAME.zip"
unzip -l "$DIST/$NAME.zip"
