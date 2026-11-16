#!/bin/sh
# install_linux_host.sh refuses a tarball whose build id differs from the Android build's, naming both, before it
# touches the host; with equal ids it installs, and a second install keeps the host's sync.json.
#   tests/install_linux_host_test.sh SCRIPT WORKDIR
set -eu
script=$1 work=$2
rm -rf "$work"
mkdir -p "$work/pkg/TLKRecomp-linux-x86_64" "$work/android/tlk_build_id"
printf '#!/bin/sh\necho aaaaaaaaaaaa-lp64le v0\n' > "$work/pkg/TLKRecomp-linux-x86_64/tlk_sdl"
chmod +x "$work/pkg/TLKRecomp-linux-x86_64/tlk_sdl"
tar -C "$work/pkg" -czf "$work/p.tar.gz" TLKRecomp-linux-x86_64
printf '#define TLK_BUILD_ID "bbbbbbbbbbbb-lp64le"\n' > "$work/android/tlk_build_id/tlk_build_id.h"
rc=0
"$script" -p "$work/p.tar.gz" -a "$work/android" no-such-host.invalid > "$work/out" 2>&1 || rc=$?
[ "$rc" = 1 ] || { echo "FAIL: exit $rc"; cat "$work/out"; exit 1; }
grep -q aaaaaaaaaaaa-lp64le "$work/out" && grep -q bbbbbbbbbbbb-lp64le "$work/out" || { echo "FAIL: ids not named"; cat "$work/out"; exit 1; }
! grep -qi "ssh\|no-such-host" "$work/out" || { echo "FAIL: reached the host"; cat "$work/out"; exit 1; }
# The control: with the ids equal it goes on to the host, which this one isn't.
printf '#define TLK_BUILD_ID "aaaaaaaaaaaa-lp64le"\n' > "$work/android/tlk_build_id/tlk_build_id.h"
"$script" -p "$work/p.tar.gz" -a "$work/android" no-such-host.invalid > "$work/out" 2>&1 && { echo "FAIL: installed"; exit 1; }
grep -q "^build aaaaaaaaaaaa-lp64le" "$work/out" || { echo "FAIL: equal ids refused"; cat "$work/out"; exit 1; }
# And to localhost, with HOME in the work folder.
home="$work/home"
mkdir -p "$home"
printf 'rom' > "$work/rom.sfc"
HOME="$home" "$script" -s https://store.example:8450 -p "$work/p.tar.gz" -a "$work/android" localhost "$work/rom.sfc" \
    > "$work/out" 2>&1 || { echo "FAIL: localhost install"; cat "$work/out"; exit 1; }
app="$home/.local/share/tlk-recomp/app"
for f in tlk_sdl tlk-play tlk_sync.py tlk_saves.py rom.sfc icon.png; do
    [ -f "$app/$f" ] || { echo "FAIL: no $f"; exit 1; }
done
grep -q "^Exec=$app/tlk-play$" "$home/.local/share/applications/tlk-recomp.desktop" || { echo "FAIL: menu entry"; exit 1; }
grep -q '"store": "https://store.example:8450"' "$home/.config/tlk-recomp/sync.json" || { echo "FAIL: sync.json"; exit 1; }
printf '{"store": "mine"}\n' > "$home/.config/tlk-recomp/sync.json"
HOME="$home" "$script" -s https://store.example:8450 -p "$work/p.tar.gz" -a "$work/android" localhost > "$work/out" 2>&1
grep -q '"store": "mine"' "$home/.config/tlk-recomp/sync.json" || { echo "FAIL: a second install replaced sync.json"; exit 1; }
echo PASS
