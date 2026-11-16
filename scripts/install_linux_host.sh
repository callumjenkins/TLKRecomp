#!/usr/bin/env bash
# Installs the Linux player on HOST with save sync: builds the tarball from this commit (scripts/package.sh), refuses
# when its build id differs from the Android build's, then copies it, the sync scripts, and the ROM (when HOST has
# none) into ~/.local/share/tlk-recomp/app/ there, with a menu entry that runs tlk-play. HOST=localhost installs here.
#
# usage: scripts/install_linux_host.sh [-s STORE] [-p TARBALL] [-a ANDROID_BUILD_DIR] HOST [ROM]
#   -s STORE  written to ~/.config/tlk-recomp/sync.json on HOST when it has none, such as https://HOST:8450
#   -p        use this tarball instead of building one
#   -a        the Android native build whose id must match (default build-android/native-arm64-v8a)
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
store="" tarball="" android="$ROOT/build-android/native-arm64-v8a"
while getopts s:p:a: o; do
    case $o in s) store=$OPTARG ;; p) tarball=$OPTARG ;; a) android=$OPTARG ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { sed -n '2,9p' "$0"; exit 2; }
host=$1 rom=${2:-}

[ -n "$tarball" ] || tarball=$("$ROOT/scripts/package.sh" -b "$ROOT/build-pkg" | grep '\.tar\.gz$' | head -n1)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
tar -C "$work" -xzf "$tarball"
pkg_id=$("$work"/*/tlk_sdl --build-id | cut -d' ' -f1)
apk_id=$(sed -n 's/^#define TLK_BUILD_ID "\(.*\)"$/\1/p' "$android/tlk_build_id/tlk_build_id.h" 2>/dev/null || true)
if [ "$pkg_id" != "$apk_id" ]; then
    echo "build ids differ: Linux $pkg_id, Android ${apk_id:-none in $android}" >&2
    echo "Saves would not move between them. Build both from the same commit." >&2
    exit 1
fi
echo "build $pkg_id"

if [ "$host" = localhost ]; then
    on() { sh -c "$1"; }
    put() { cp "$1" "$HOME/$2"; }
else
    on() { ssh "$host" "$1"; }
    put() { scp -q "$1" "$host:$2"; }
fi
app=.local/share/tlk-recomp/app
on "mkdir -p ~/$app ~/.local/share/applications ~/.config/tlk-recomp"
put "$tarball" "$app/player.tar.gz"
on "tar -C ~/$app --strip-components=1 -xzf ~/$app/player.tar.gz && rm ~/$app/player.tar.gz"
for f in tlk_saves.py tlk_sync.py tlk-play; do put "$ROOT/sync/$f" "$app/$f"; done
put "$ROOT/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" "$app/icon.png"
if [ -n "$rom" ] && ! on "ls ~/$app/*.sfc ~/$app/*.smc >/dev/null 2>&1"; then
    put "$rom" "$app/rom.sfc"
fi
if [ -n "$store" ]; then
    on "[ -f ~/.config/tlk-recomp/sync.json ] || printf '{\"store\": \"%s\"}\n' '$store' > ~/.config/tlk-recomp/sync.json"
fi
home=$(on 'echo $HOME')
on "cat > ~/.local/share/applications/tlk-recomp.desktop" <<EOF2
[Desktop Entry]
Type=Application
Name=The Lion King
Comment=TLKRecomp, with saves synced between devices
Exec=$home/$app/tlk-play
Icon=$home/$app/icon.png
Categories=Game;
EOF2
on "chmod +x ~/$app/tlk-play && ~/$app/tlk_sdl --build-id"
echo "installed on $host: ~/$app/tlk-play, and The Lion King in the menu"
