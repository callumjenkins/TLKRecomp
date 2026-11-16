"""Desktop save sync, and `tlk-play`, which wraps tlk_sdl in it.

    tlk_sync.py play [--new] [-- tlk_sdl options]    pull, play the newest autosave, push on exit
    tlk_sync.py pull | push | status

Settings come from ~/.config/tlk-recomp/sync.json: {"store": SPEC, "device": NAME, "tlk_sdl": PATH}. SPEC is anything
tlk_saves.open_store takes; with no store, play runs without sync. The saves and tlk_sdl.ini live in
~/.local/share/tlk-recomp/saves/.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import urllib.parse
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tlk_saves  # noqa: E402
from tlk_saves import AUTOSAVE, PARTS, SLOTS, BadSave, Conflict, SaveStore, StoreError, Unreachable  # noqa: E402

CONFIG = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "tlk-recomp" / "sync.json"
DATA = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")) / "tlk-recomp"
OFFLINE = "Not synced: {store} not reachable. Is Tailscale on?"


@dataclass
class Report:
    pulled: list[str] = field(default_factory=list)
    pushed: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)
    continue_from: dict | None = None

    def lines(self) -> list[str]:
        out = list(self.problems)
        if self.pulled:
            out.append("pulled " + ", ".join(self.pulled))
        if self.pushed:
            out.append("pushed " + ", ".join(self.pushed))
        return out


def build_id(tlk_sdl: str) -> str:
    return subprocess.run([tlk_sdl, "--build-id"], capture_output=True, text=True, check=True,
                          timeout=10).stdout.split()[0]


def install(saves: Path, base: str, data: bytes) -> None:
    """Writes a downloaded zip as BASE, keeping the copy it replaces as BASE.prev.*. The json goes last."""
    state, info, png = tlk_saves.unpack(data)
    saves.mkdir(parents=True, exist_ok=True)
    for part in PARTS:
        prev = saves / f"{base}.prev.{part}"
        prev.unlink(missing_ok=True)
        cur = saves / f"{base}.{part}"
        if cur.exists():
            os.replace(cur, prev)
    _write(saves / f"{base}.state", state)
    if png is not None:
        _write(saves / f"{base}.png", png)
    _write(saves / f"{base}.json", json.dumps(info).encode())


def load_config() -> dict:
    try:
        return json.loads(CONFIG.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}


def local_info(saves: Path, base: str) -> dict | None:
    """The sidecar of BASE in `saves`, with `sha_ok` set from the state file, or None when there is no state."""
    state = saves / f"{base}.state"
    try:
        data = state.read_bytes()
    except FileNotFoundError:
        return None
    try:
        info = json.loads((saves / f"{base}.json").read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        info = {}
    info["sha_ok"] = info.get("state_sha256") == tlk_saves.sha256(data)
    return info


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["play", "pull", "push", "status"])
    ap.add_argument("--new", action="store_true", help="play: start at the title, not the newest autosave")
    ap.add_argument("--saves", type=Path, default=DATA / "saves")
    ap.add_argument("extra", nargs="*", help="play: options passed on to tlk_sdl, after --")
    args = ap.parse_args(argv)
    cfg = load_config()
    if args.command == "play":
        return play(cfg, args.saves, args.new, args.extra, echo=_tell)
    if not cfg.get("store"):
        print(f"no store in {CONFIG}", file=sys.stderr)
        return 2
    store = tlk_saves.open_store(cfg["store"])
    device = _device(cfg)
    if args.command == "status":
        try:
            remote = store.list()
        except StoreError as e:
            print(_not_synced(store, e))
            return 1
        for name, info in sorted(remote.items()):
            print(f"{name:32} {info.get('level_name', '?'):12} lives {info.get('lives', '?')}  {info.get('saved_unix')}")
        return 0
    if args.command == "pull":
        report = pull(store, args.saves, device, build_id(_tlk_sdl(cfg)))
    else:
        report = push(store, args.saves, device)
    for line in report.lines():
        print(line)
    return 1 if report.problems else 0


def pack_local(saves: Path, base: str) -> bytes:
    info = json.loads((saves / f"{base}.json").read_text(encoding="utf-8"))
    png = saves / f"{base}.png"
    return tlk_saves.pack((saves / f"{base}.state").read_bytes(), info, png.read_bytes() if png.exists() else None)


def play(cfg: dict, saves: Path, new: bool, extra: list[str], echo: Callable[[str], None] = print) -> int:
    tlk_sdl = _tlk_sdl(cfg)
    device = _device(cfg)
    store = tlk_saves.open_store(cfg["store"]) if cfg.get("store") else None
    saves.mkdir(parents=True, exist_ok=True)
    ini = saves / "tlk_sdl.ini"
    if not ini.exists():
        # tlk_sdl's desktop default is no timed autosave; a device someone may leave mid-level wants one.
        ini.write_text("autosave_seconds = 60\n", encoding="utf-8")
    if store is not None:
        for line in pull(store, saves, device, build_id(tlk_sdl)).lines():
            echo(line)
    autosave = saves / f"{AUTOSAVE}.state"
    args = [tlk_sdl, "--config", str(ini), "--device", device, "--autosave", str(autosave)]
    if not new and autosave.exists():
        args += ["--load", str(autosave)]
    code = subprocess.run(args + extra).returncode
    if store is not None:
        for line in push(store, saves, device).lines():
            echo(line)
    return code


def pull(store: SaveStore, saves: Path, device: str, build: str) -> Report:
    """Brings newer slots down, and the newest loadable autosave from any device into tlk_autosave."""
    report = Report()
    try:
        remote = store.list()
        for slot in SLOTS:
            if tlk_saves.decide(local_info(saves, slot), remote.get(slot)) == "pull":
                _fetch(store, saves, slot, slot, report)
        local = local_info(saves, AUTOSAVE)
        candidates = [{**info, "where": name} for name, info in remote.items()
                      if name.startswith(AUTOSAVE + "@") and isinstance(info, dict)]
        if local is not None and local["sha_ok"]:
            candidates.append({**local, "where": "local"})
        best = tlk_saves.pick_continue(candidates, build)
        if best is not None and best["where"] != "local":
            if local is None or best["state_sha256"] != local.get("state_sha256"):
                _fetch(store, saves, best["where"], AUTOSAVE, report)
        report.continue_from = best
    except StoreError as e:
        report.problems.append(_not_synced(store, e))
    return report


def push(store: SaveStore, saves: Path, device: str) -> Report:
    """Sends each slot that is newer here, and this device's autosave as tlk_autosave@DEVICE."""
    report = Report()
    try:
        remote = store.list()
        pairs = [(slot, slot) for slot in SLOTS] + [(AUTOSAVE, tlk_saves.autosave_name(device))]
        for base, name in pairs:
            if tlk_saves.decide(local_info(saves, base), remote.get(name)) != "push":
                continue
            try:
                store.put(name, pack_local(saves, base))
                report.pushed.append(name)
            except Conflict:
                report.problems.append(f"{name}: the stored copy is as new or newer, kept it")
            except BadSave as e:
                report.problems.append(f"{name}: {e}")
    except StoreError as e:
        report.problems.append(_not_synced(store, e))
    return report


def _device(cfg: dict) -> str:
    return tlk_saves.device_name(cfg.get("device") or socket.gethostname())


def _fetch(store: SaveStore, saves: Path, name: str, base: str, report: Report) -> None:
    try:
        install(saves, base, store.get(name))
        report.pulled.append(name)
    except (KeyError, BadSave) as e:
        report.problems.append(f"{name}: {e}")


def _not_synced(store: SaveStore, error: StoreError) -> str:
    if isinstance(error, Unreachable):
        return OFFLINE.format(store=_store_label(store))
    return f"Not synced: {error}"


def _store_label(store: SaveStore) -> str:
    if isinstance(store, tlk_saves.HttpStore):
        host = urllib.parse.urlparse(store.url).hostname or store.url
        return host if host.replace(".", "").isdigit() else host.split(".", 1)[0]
    return "the save store"


def _tell(line: str) -> None:
    """Prints, and from a menu entry, where nothing shows stdout, puts it in a desktop notification too."""
    print(line, flush=True)
    if not sys.stdout.isatty() and shutil.which("notify-send"):
        subprocess.run(["notify-send", "The Lion King", line], check=False, timeout=5)


def _tlk_sdl(cfg: dict) -> str:
    return cfg.get("tlk_sdl") or str(Path(__file__).resolve().parent / "tlk_sdl")


def _write(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


if __name__ == "__main__":
    sys.exit(main())
