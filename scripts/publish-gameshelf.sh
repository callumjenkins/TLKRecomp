#!/bin/sh
# Builds The Lion King for Linux here and for macOS on a Mac, then publishes both to a gameshelf library, where
# each starts through tlk-play so its saves sync.
#
#   scripts/publish-gameshelf.sh LIBRARY ROM
#
# GAMESHELF_PUBLISH names gameshelf's publish_game.py, GAMESHELF_CHECK_HOST a Linux host to check the libraries on,
# and GAMESHELF_MAC_HOST the ssh host that builds the Mac copy, with Homebrew's cmake and ninja. Without a Mac host
# only Linux is published. The two builds must give the same build id, so a save moves between them.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
library=${1:?the gameshelf library folder}
rom=${2:?the ROM, Lion King, The (USA).sfc}
publish=${GAMESHELF_PUBLISH:-$HOME/gh/gameshelf/gameshelf/scripts/publish_game.py}
mac=${GAMESHELF_MAC_HOST:-}

if [ -n "$(git -C "$here" status --porcelain)" ]; then
    echo "Commit first, because the version is named after the commit." >&2
    exit 1
fi
if [ "$(sha1sum "$rom" | cut -d' ' -f1)" != e5319132495fd34d03f141962d9d6e3d76fe7b02 ]; then
    echo "$rom isn't the USA ROM the code was generated from." >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Stages one platform's build, from the folder holding tlk_sdl and assets/.
stage() {
    built=$1
    dest=$2
    mkdir -p "$dest"
    cp -R "$built/tlk_sdl" "$built/assets" "$dest/"
    cp "$here/sync/tlk-play" "$here/sync/tlk_sync.py" "$here/sync/tlk_saves.py" "$dest/"
    cp "$rom" "$dest/Lion King, The (USA).sfc"
    cat > "$dest/gameshelf.json" << 'JSON'
{"title": "The Lion King",
 "command": ["tlk-play", "--saves", "{data}/saves", "--",
             "--rom", "{game}/Lion King, The (USA).sfc", "--fullscreen"]}
JSON
}

sh "$here/scripts/package.sh" > "$work/linux.log"
tar -C "$work" -xzf "$here/dist/TLKRecomp-linux-x86_64.tar.gz"
linux_id=$("$work/TLKRecomp-linux-x86_64/tlk_sdl" --build-id | cut -d' ' -f1)
stage "$work/TLKRecomp-linux-x86_64" "$work/linux"

if [ -n "$mac" ]; then
    remote=$(ssh "$mac" 'mktemp -d /tmp/tlk-build.XXXXXX')
    # The generated code, the submodules, and the ROM aren't in git, so the tree goes across as it is.
    rsync -a --exclude='/build*' --exclude=/.git --exclude=/android --exclude=/tapes --exclude=/dist \
        --exclude=__pycache__ "$here/" "$mac:$remote/"
    ssh "$mac" "cd '$remote' && PATH=/opt/homebrew/bin:\$PATH sh scripts/package_mac.sh" > "$work/mac.log"
    scp -q "$mac:$remote/dist/TLKRecomp-macos-arm64.zip" "$work/"
    mac_id=$(ssh "$mac" "unzip -q -o '$remote/dist/TLKRecomp-macos-arm64.zip' -d '$remote/check' &&
        '$remote/check/TLKRecomp-macos-arm64/tlk_sdl' --build-id" | cut -d' ' -f1)
    ssh "$mac" "rm -rf '$remote'"
    if [ "$mac_id" != "$linux_id" ]; then
        echo "The Mac build is $mac_id and the Linux build $linux_id, so saves wouldn't move between them." >&2
        exit 1
    fi
    (cd "$work" && unzip -q TLKRecomp-macos-arm64.zip)
    stage "$work/TLKRecomp-macos-arm64" "$work/mac"
fi

label=$(git -C "$here" rev-parse --short HEAD)
set -- "$work/linux" --id the-lion-king --platform linux-x86_64 --to "$library" --label "$label" \
    --icon "$here/packaging/icon/tlkrecomp.png"
if [ -n "${GAMESHELF_CHECK_HOST:-}" ]; then
    set -- "$@" --check-host "$GAMESHELF_CHECK_HOST"
fi
python3 "$publish" "$@"
if [ -n "$mac" ]; then
    python3 "$publish" "$work/mac" --id the-lion-king --platform macos-arm64 --to "$library" --label "$label"
fi
echo "Published build $linux_id"
