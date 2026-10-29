#!/bin/sh
# Installs the save server on HOST over ssh as a systemd user service, tlk-saves.service, keeping saves in ROOT and
# letting in only LOGIN's Tailscale devices. It listens on 127.0.0.1:PORT; publishing it on the tailnet needs sudo,
# so the last step prints that command to run on HOST.
#
# usage: sync/install_server.sh HOST ROOT LOGIN [PORT] [HTTPS_PORT]    (default 4790 8450)
set -eu
[ $# -ge 3 ] || { sed -n '2,6p' "$0"; exit 2; }
host=$1 root=$2 login=$3 port=${4:-4790} https=${5:-8450}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app='$HOME/.local/share/tlk-recomp/server'
ssh "$host" "mkdir -p $app ~/.config/systemd/user '$root'"
scp -q "$here/tlk_saves.py" "$here/tlk_save_server.py" "$host:.local/share/tlk-recomp/server/"
ssh "$host" "cat > ~/.config/systemd/user/tlk-saves.service" <<UNIT
# Installed by TLKRecomp's sync/install_server.sh. Runs without a login only with lingering on (loginctl enable-linger).
[Unit]
Description=TLKRecomp save sync server (Tailscale/localhost only)
After=network-online.target

[Service]
Environment=TLK_SAVES_LOGIN=$login
ExecStart=/usr/bin/python3 %h/.local/share/tlk-recomp/server/tlk_save_server.py --root "$root" --port $port
Restart=on-failure
RestartSec=15

[Install]
WantedBy=default.target
UNIT
ssh "$host" "systemctl --user daemon-reload && systemctl --user enable tlk-saves.service >/dev/null 2>&1 && systemctl --user restart tlk-saves.service && sleep 1 && systemctl --user is-active tlk-saves.service && curl -s -o /dev/null -w 'local check: %{http_code} (403 is right: no Tailscale login)\n' http://127.0.0.1:$port/v1/saves"
echo "Now run this on $host to publish it on the tailnet:"
echo "  sudo tailscale serve --bg --https=$https http://127.0.0.1:$port"
