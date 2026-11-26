#!/usr/bin/env sh
# Build the macOS player: dist/TLKRecomp-macos-arm64.dmg (TLKRecomp.app) and
# dist/TLKRecomp-macos-arm64.zip (the bare tlk_sdl, for Terminal use).
#
# usage: scripts/package_mac.sh [-b builddir]   (TLK_VERSION=v1.2.3 sets the app version)
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
        *) sed -n '2,11p' "$0"; exit 2 ;;
    esac
done
NAME=TLKRecomp-macos-$(uname -m)
DIST="$ROOT/dist"
STAGE="$DIST/stage-mac"
PKG="$DIST/$NAME"

# SDL gets packaging/sdl3-macos-gip.patch, so a third-party Xbox One controller, which macOS has no driver for,
# works through libusb. libusb links in statically, so players still need nothing installed, and its entitlement
# check needs the Security framework.
LIBUSB=$(brew --prefix libusb 2>/dev/null || true)
[ -f "$LIBUSB/lib/libusb-1.0.a" ] || { echo "Install libusb first: brew install libusb" >&2; exit 1; }
RUNNER="$ROOT/snesrecomp/runner/runner.cmake"
SDL_VERSION=$(sed -n 's/^set(SNESRECOMP_SDL3_FETCH_VERSION "\([^"]*\)".*/\1/p' "$RUNNER")
SDL_SHA256=$(sed -n '/^set(SNESRECOMP_SDL3_FETCH_SHA256/{n;s/^ *"\([0-9a-f]*\)".*/\1/p;}' "$RUNNER")
[ -n "$SDL_VERSION" ] && [ -n "$SDL_SHA256" ] || { echo "No SDL version or sha256 in $RUNNER" >&2; exit 1; }
SDL_SRC="$(mkdir -p "$BUILD" && cd "$BUILD" && pwd)/sdl3-gip"
SDL_STAMP="$SDL_VERSION $(shasum -a 256 "$ROOT/packaging/sdl3-macos-gip.patch" | cut -d' ' -f1)"
if [ "$(cat "$SDL_SRC/.stamp" 2>/dev/null)" != "$SDL_STAMP" ]; then
    rm -rf "$SDL_SRC"
    mkdir -p "$SDL_SRC"
    curl -sSfL --http1.1 -o "$BUILD/SDL3.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $BUILD/SDL3.tar.gz" | shasum -a 256 -c - >/dev/null
    tar -xzf "$BUILD/SDL3.tar.gz" -C "$SDL_SRC" --strip-components 1
    patch -s -p1 -d "$SDL_SRC" < "$ROOT/packaging/sdl3-macos-gip.patch"
    echo "$SDL_STAMP" > "$SDL_SRC/.stamp"
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON \
    -DSNESRECOMP_SDL3_SOURCE_DIR="$SDL_SRC" -DSDL_HIDAPI_LIBUSB_SHARED=OFF \
    -DLibUSB_LIBRARY="$LIBUSB/lib/libusb-1.0.a" -DLibUSB_INCLUDE_PATH="$LIBUSB/include/libusb-1.0" \
    -DCMAKE_EXE_LINKER_FLAGS="-framework Security"
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
echo "$DIST/$NAME.zip"
unzip -l "$DIST/$NAME.zip"

# The .app: tlk_sdl in Contents/MacOS, assets/ in Contents/Resources (where
# SDL_GetBasePath points inside a bundle). Started from Finder it runs in /,
# so tlk_sdl keeps its ini and saves in ~/Library/Application Support/TLKRecomp.
VERSION=${TLK_VERSION:-$(git -C "$ROOT" describe --tags --abbrev=0 2>/dev/null || echo 0.0.0)}
VERSION=${VERSION#v}
case $VERSION in *[!0-9.]*|'') VERSION=0.0.0 ;; esac
DMG="$DIST/stage-dmg"
APP="$DMG/TLKRecomp.app"
ICONSET="$DIST/tlkrecomp.iconset"
rm -rf "${DMG:?}" "${ICONSET:?}"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$ICONSET"
cp "$PKG/tlk_sdl" "$APP/Contents/MacOS/"
cp -r "$PKG/assets" "$APP/Contents/Resources/"
for s in 16 32 128 256 512; do
    sips -z $s $s "$ROOT/packaging/icon/tlkrecomp.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$ROOT/packaging/icon/tlkrecomp.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/tlkrecomp.icns"
rm -rf "${ICONSET:?}"
cat >"$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>TLKRecomp</string>
  <key>CFBundleDisplayName</key><string>TLKRecomp</string>
  <key>CFBundleIdentifier</key><string>io.github.kinduff.tlkrecomp</string>
  <key>CFBundleExecutable</key><string>tlk_sdl</string>
  <key>CFBundleIconFile</key><string>tlkrecomp</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
/usr/bin/codesign --force --deep --sign - "$APP"
cp "$PKG"/*.txt "$DMG/"
ln -s /Applications "$DMG/Applications"
rm -f "${DIST:?}/${NAME:?}.dmg"
hdiutil create -quiet -volname TLKRecomp -srcfolder "$DMG" -format UDZO "$DIST/$NAME.dmg"
rm -rf "${DMG:?}" "${PKG:?}"
echo "$DIST/$NAME.dmg"
