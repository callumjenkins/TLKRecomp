# Save sync

A savestate moves between machines as it is. The same code on a Linux PC (GCC, x86_64) and on an Android phone
(clang, arm64) writes states the other loads and plays on from identically, frame and WRAM hashes included
(`scripts/check_cross_device_state.sh`). The files differ only in `CpuState.ram`, a host pointer that a load replaces
with its own. So a state's build id names the code and the data layout (`57c0c56d30d6-lp64le`), not the platform.
Linux, Android, and macOS on Apple Silicon share `lp64le`. The MinGW Windows build is `llp64le` and keeps its own
states.

Sync keeps slots 1 to 4 and each device's autosave in a store, and brings the newest across:

- **Slots** are shared by name. The newer copy, by the sidecar's `saved_unix`, replaces the older, and the one it
  replaces is kept beside it as `tlk_slotN.prev.*`.
- **Autosaves** are stored per device, as `tlk_autosave@DEVICE`, so two devices never overwrite each other's. Before
  play, the newest autosave this build can load, from any device, becomes the local `tlk_autosave` that Continue
  resumes.
- **Deleting** a save stays on that device.
- A save travels as one zip of `save.state`, `save.png`, and `save.json`. The sidecar names the device and the
  state's SHA-256, and a store refuses a zip whose state doesn't match it, so no one ever pairs a state with
  another state's sidecar.

The merge rule is `decide` and `pick_continue` in `sync/tlk_saves.py`, with a Kotlin copy in the Android launcher.
`sync/merge_cases.json` holds the cases both test suites run.

## Stores

A store is anything with three operations on whole zips: `list` (every save's sidecar, by name), `get NAME`, and
`put NAME` (refused when the stored copy is as new or newer). Three come with the port:

| Store | Spec | For |
|---|---|---|
| The save server | `https://HOST:PORT` | `sync/tlk_save_server.py`, behind Tailscale Serve. Keeps the last 10 replaced copies of each save under `history/` |
| A folder | a path | a directory that Syncthing, rclone mount, or NFS keeps in step. The server keeps its saves in the same layout |
| A program | `cmd:PROGRAM` | anything else, desktop only. `sync/examples/rclone-store.sh` reaches S3, R2, Google Cloud Storage and every other rclone remote |

A `cmd:` program is called as `PROGRAM list`, `PROGRAM get NAME`, or `PROGRAM put NAME` with the zip on stdin.
`list` prints a JSON object of sidecars by name, and `get` writes the zip to stdout. Exit 0 is success, 2 means no
such save, 3 means the stored copy is as new or newer, and anything else is a failure. Names are `tlk_slot1` to `tlk_slot4`
and `tlk_autosave@DEVICE`, where DEVICE is letters, digits, `.`, `_`, and `-`. The rclone example has not been run
against a real remote.

On Android a new backend is a class implementing `SaveStore` in `SaveSync.kt`. The launcher builds an
`HttpSaveStore` from the server setting.

## The save server

```sh
sync/install_server.sh HOST ROOT LOGIN      # a systemd user service on HOST, on 127.0.0.1:4790
sudo tailscale serve --bg --https=8450 http://127.0.0.1:4790     # on HOST, as the script prints
```

It answers only requests that Tailscale Serve forwards with `Tailscale-User-Login: LOGIN`. `--no-auth` runs a test
server that takes any request, such as one the phone reaches through `adb reverse tcp:4790 tcp:4790` at
`http://127.0.0.1:4790`, the one plain-HTTP address the app accepts.

## The desktop

`tlk-play` wraps `tlk_sdl`. It pulls, resumes the newest autosave (`tlk-play --new` starts at the title), and
pushes once the game exits. Saves and `tlk_sdl.ini` live in `~/.local/share/tlk-recomp/saves/`, and the first run
sets `autosave_seconds = 60` there. Settings are in `~/.config/tlk-recomp/sync.json`:

```json
{"store": "https://cj-omv.example.ts.net:8450", "device": "my-laptop", "tlk_sdl": "/path/to/tlk_sdl"}
```

All three are optional. With no store it plays without sync. The device defaults to the hostname, and `tlk_sdl` to
the one beside `tlk-play`. `sync/tlk_sync.py status` lists what the store holds.

`scripts/install_linux_host.sh [-s STORE] HOST [ROM]` builds the Linux tarball from this commit, refuses when its
build id differs from the Android build's, and installs the player, `tlk-play`, and a menu entry into
`~/.local/share/tlk-recomp/app/` on HOST over ssh. `localhost` installs on this machine.

## Android

Settings has "Save sync server" (empty for no sync) and "This device". The launcher syncs each time it comes to
the front and says what it did on its last line. It pulls only once the game process has ended. When the game goes
to the background, the game pushes its autosave too, since its process can stay alive long after. Two pushes at once are
harmless: a store accepts the same state again without change and refuses an older one. With the server out
of reach, the launcher says "Not synced: HOST not reachable. Is Tailscale on?", and Play and Continue still work.
