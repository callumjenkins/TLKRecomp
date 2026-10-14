#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -d "$ROOT/generated/snesrecomp" ] || [ -z "$(ls -A "$ROOT/generated/snesrecomp" 2>/dev/null)" ]; then
    printf '%s\n' "Generating snesrecomp sources..."
    python3 "$ROOT/scripts/generate_snesrecomp.py"
fi

cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-D_GNU_SOURCE"
cmake --build "$ROOT/build" --target snesrecomp_game tlk_headless tlk_sdl --config Release --parallel

printf '\n%s\n' "Built snesrecomp_game, build/tlk_headless and build/tlk_sdl."
