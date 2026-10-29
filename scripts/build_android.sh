#!/usr/bin/env bash
# Builds the Android app (arm64) from your own ROM into build-android/, and with --install puts it on the
# phone adb sees. Needs the Android SDK with NDK $NDK_VERSION, Python 3, cmake and ninja; Gradle runs from
# android/gradlew on a JDK 17 or newer with javac (JAVA_HOME).
#
# usage: scripts/build_android.sh [-r ROM] [--native-only] [--release] [--install]
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/build-android"
export ANDROID_HOME=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK_VERSION=${NDK_VERSION:-30.0.16248370}
NDK="$ANDROID_HOME/ndk/$NDK_VERSION"
ABI=arm64-v8a
API=28   # recomp-ui calls posix_spawn, which bionic has from Android 9

rom="" native_only=0 release=0 install=0
while [ $# -gt 0 ]; do
    case $1 in
        -r) rom=$2; shift ;;
        --native-only) native_only=1 ;;
        --release) release=1 ;;
        --install) install=1 ;;
        *) echo "usage: $0 [-r ROM] [--native-only] [--release] [--install]" >&2; exit 2 ;;
    esac
    shift
done
[ -d "$NDK" ] || { echo "no NDK at $NDK (set ANDROID_HOME or NDK_VERSION)" >&2; exit 1; }

if [ -z "$(ls -A "$ROOT/generated/snesrecomp" 2>/dev/null)" ]; then
    python3 "$ROOT/scripts/generate_snesrecomp.py" ${rom:+--rom "$rom"}
fi

# SDL3 as a shared library: SDLActivity loads libSDL3.so by name. Same release and hash as the desktop
# builds' static SDL (snesrecomp's runner.cmake), and its Java sources go into the app.
runner_cmake="$ROOT/snesrecomp/runner/runner.cmake"
sdl_version=$(sed -n 's/^set(SNESRECOMP_SDL3_FETCH_VERSION "\([0-9.]*\)".*/\1/p' "$runner_cmake")
sdl_sha=$(grep -A1 '^set(SNESRECOMP_SDL3_FETCH_SHA256' "$runner_cmake" | sed -n 's/^ *"\([0-9a-f]\{64\}\)".*/\1/p')
[ -n "$sdl_version" ] && [ -n "$sdl_sha" ] || { echo "could not read the SDL3 pin from $runner_cmake" >&2; exit 1; }
sdl_dir="$OUT/sdl"
sdl_src="$sdl_dir/SDL3-$sdl_version"
sdl_prefix="$sdl_dir/install-$ABI"
toolchain=(-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
           -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API)
if [ ! -f "$sdl_prefix/lib/cmake/SDL3/SDL3Config.cmake" ]; then
    mkdir -p "$sdl_dir"
    tarball="$sdl_dir/SDL3-$sdl_version.tar.gz"
    [ -f "$tarball" ] || curl -fL --http1.1 -o "$tarball" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$sdl_version/SDL3-$sdl_version.tar.gz"
    echo "$sdl_sha  $tarball" | sha256sum -c --quiet
    tar -xzf "$tarball" -C "$sdl_dir"
    cmake -S "$sdl_src" -B "$sdl_dir/build-$ABI" -G Ninja "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX="$sdl_prefix"
    cmake --build "$sdl_dir/build-$ABI" --target install
fi

cmake -S "$ROOT" -B "$OUT/native-$ABI" -G Ninja "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-D_GNU_SOURCE" -DSDL3_DIR="$sdl_prefix/lib/cmake/SDL3" -DSNESRECOMP_SDL3_FETCH=OFF
cmake --build "$OUT/native-$ABI" --target tlk_sdl tlk_headless

libs="$OUT/jniLibs/$ABI"
mkdir -p "$libs"
strip="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
"$strip" -o "$libs/libmain.so" "$OUT/native-$ABI/libmain.so"
"$strip" -o "$libs/libSDL3.so" "$sdl_prefix/lib/libSDL3.so"
echo "native libraries in $libs"
[ $native_only = 1 ] && exit 0

variant=$([ $release = 1 ] && echo Release || echo Debug)
"$ROOT/android/gradlew" -p "$ROOT/android" --no-daemon "assemble$variant" \
    -Ptlk.native="$OUT/jniLibs" \
    -Psdl.java="$sdl_src/android-project/app/src/main/java"
apk=$(ls "$ROOT"/android/app/build/outputs/apk/"${variant,,}"/*.apk | head -n1)
cp "$apk" "$OUT/TLKRecomp-android-$ABI.apk"
echo "apk: $OUT/TLKRecomp-android-$ABI.apk"

if [ $install = 1 ]; then
    "$ANDROID_HOME/platform-tools/adb" install -r "$OUT/TLKRecomp-android-$ABI.apk"
fi
