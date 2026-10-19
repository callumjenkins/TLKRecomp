#!/usr/bin/env sh
# Build, check and pack the release packages into dist/:
# TLKRecomp-linux-x86_64.tar.gz, TLKRecomp-windows-x86_64.zip and SHA256SUMS.
#
# usage: scripts/release.sh -r ROM
# Generates the C from ROM, builds tlk_headless and tlk_sdl for Linux
# (build-pkg/) and Windows (build-win/) with the settings scripts/package.sh
# and scripts/package_win.sh use, and compares compiled mode with --lle on
# both (Windows under Wine): attract mode at frames 600 and 3000, and the
# 6987M speedrun tape at frame 6000. Any MISMATCH, WRAM difference or missing
# hash stops it before anything is packed. .gitea/workflows/release.yml runs
# this on a v* tag.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROM=""
while getopts "r:" opt; do
    case $opt in
        r) ROM=$OPTARG ;;
        *) sed -n '2,12p' "$0"; exit 2 ;;
    esac
done
[ -f "$ROM" ] || { sed -n '2,12p' "$0"; echo "ROM not found: '$ROM'" >&2; exit 2; }
JOBS=$(nproc 2>/dev/null || echo 4)
GATE="$ROOT/build-pkg/release-gate"
DIST="$ROOT/dist"

python3 "$ROOT/scripts/generate_snesrecomp.py" --rom "$ROM"

cmake -S "$ROOT" -B "$ROOT/build-pkg" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON
cmake --build "$ROOT/build-pkg" --target tlk_headless tlk_sdl -j"$JOBS"
cmake -S "$ROOT" -B "$ROOT/build-win" -G Ninja -DCMAKE_BUILD_TYPE=Release -DTLK_STATIC_SDL=ON \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake"
cmake --build "$ROOT/build-win" --target tlk_headless tlk_sdl -j"$JOBS"

rm -rf "$GATE"
mkdir -p "$GATE"
cat >"$GATE/wine_headless" <<EOF
#!/bin/sh
WINEDEBUG=-all exec wine "$ROOT/build-win/tlk_headless.exe" "\$@"
EOF
chmod +x "$GATE/wine_headless"

gate() { # name binary [compare_modes options] frame...
    name=$1 bin=$2; shift 2
    echo "== $name"
    "$ROOT/scripts/compare_modes.sh" -r "$ROM" -b "$bin" -o "$GATE/$name" "$@" | tee "$GATE/$name.txt" || true
    # Each frame needs MATCH, real hashes (not "none") and the same WRAM.
    want=$(($(grep -c . "$GATE/$name.txt") - 1))
    good=$(awk '$2 == "MATCH" && $3 != "none" && $5 == "same"' "$GATE/$name.txt" | wc -l)
    [ "$want" -gt 0 ] && [ "$good" -eq "$want" ] || {
        echo "$name: compiled mode differs from --lle ($good of $want frames good)" >&2; exit 1; }
}
gate linux "$ROOT/build-pkg/tlk_headless" 600 3000
gate linux-6987M "$ROOT/build-pkg/tlk_headless" -p "$ROOT/tapes/6987M.read.tape" 6000
gate windows "$GATE/wine_headless" 600 3000
gate windows-6987M "$GATE/wine_headless" -p "$ROOT/tapes/6987M.read.tape" 6000
# And both builds must agree with each other.
for run in "" -6987M; do
    awk '{print $1, $3, $4}' "$GATE/linux$run.txt" >"$GATE/linux$run.hashes"
    awk '{print $1, $3, $4}' "$GATE/windows$run.txt" >"$GATE/windows$run.hashes"
    cmp "$GATE/linux$run.hashes" "$GATE/windows$run.hashes" ||
        { echo "linux$run and windows$run hashes differ" >&2; exit 1; }
done

"$ROOT/scripts/package.sh" -b "$ROOT/build-pkg"
"$ROOT/scripts/package_win.sh" -b "$ROOT/build-win"
(cd "$DIST" && sha256sum TLKRecomp-linux-x86_64.tar.gz TLKRecomp-windows-x86_64.zip >SHA256SUMS)
cat "$DIST/SHA256SUMS"
