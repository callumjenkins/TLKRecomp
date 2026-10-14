#!/usr/bin/env sh
# Build the player tarball: dist/TLKRecomp-linux-x86_64.tar.gz
#
# usage: scripts/package.sh [-b builddir]
# Configures builddir (default build-pkg/) with -DTLK_STATIC_SDL=ON, so SDL3
# (pinned release, built from source) is linked into tlk_sdl and players need
# no SDL3 install; builds tlk_sdl, installs it with `cmake --install` into a
# staging dir and packs tlk_sdl + README.txt + licence notes (incl. SDL3's).
# The ROM, extracted assets and generated game C are never packed (checked below).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build-pkg"
while getopts "b:" opt; do
    case $opt in
        b) BUILD=$OPTARG ;;
        *) sed -n '2,7p' "$0"; exit 2 ;;
    esac
done
NAME=TLKRecomp-linux-x86_64
DIST="$ROOT/dist"
STAGE="$DIST/stage"
PKG="$DIST/$NAME"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON
cmake --build "$BUILD" --target tlk_sdl -j"$(nproc 2>/dev/null || echo 4)"
rm -rf "$STAGE" "$PKG"
cmake --install "$BUILD" --prefix "$STAGE"

mkdir -p "$PKG"
cp "$STAGE/bin/tlk_sdl" "$PKG/"
strip "$PKG/tlk_sdl" 2>/dev/null || true
cp "$STAGE"/share/doc/*/README.txt "$STAGE"/share/doc/*/LICENSE-NOTES.txt \
   "$STAGE"/share/doc/*/LICENSE-snesrecomp.txt \
   "$STAGE"/share/doc/*/LICENSE-SDL3.txt "$PKG/"
rm -rf "$STAGE"
# The packaged binary must not need a system SDL.
if ldd "$PKG/tlk_sdl" | grep -qi sdl; then
    echo "tlk_sdl still links a shared SDL:" >&2; ldd "$PKG/tlk_sdl" >&2; exit 1
fi

# Nothing but the binary and the text files may go in.
bad=$(find "$PKG" -type f ! -name tlk_sdl ! -name '*.txt' -print)
[ -z "$bad" ] || { echo "unexpected files in package: $bad" >&2; exit 1; }
if find "$PKG" -iname '*.sfc' -o -iname '*.smc' -o -name '*.c' -o -name '*.h' | grep -q .; then
    echo "ROM or source file in package" >&2; exit 1
fi

tar -C "$DIST" -czf "$DIST/$NAME.tar.gz" "$NAME"
rm -rf "$PKG"
echo "$DIST/$NAME.tar.gz"
tar -tzvf "$DIST/$NAME.tar.gz"
