"""Save sync for tlk_sdl: the save zip, the merge rule, and the stores a save can be kept in.

A save travels as one zip of `save.state`, `save.png` (optional), and `save.json`, the sidecar tlk_sdl writes last.
A store keeps zips by save name and offers three operations, which is all a new backend has to provide:

    list()          {name: sidecar dict} for every save it holds
    get(name)       the zip's bytes, or KeyError
    put(name, zip)  stores it, or raises Conflict when the stored copy is as new or newer

A zip or name the store won't take is BadSave, and any other failure is StoreError. `docs/SAVE_SYNC.md` describes the stores and the protocol a `cmd:` store's program
speaks.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import re
import subprocess
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Protocol

AUTOSAVE = "tlk_autosave"
HISTORY_KEPT = 10
PARTS = ("state", "png", "json")
SLOTS = tuple(f"tlk_slot{n}" for n in range(1, 5))
_NAME = re.compile(r"^(tlk_slot[1-4]|tlk_autosave@[A-Za-z0-9._-]{1,63})$")


class BadSave(ValueError):
    """A zip or name the store refuses: unreadable, missing a part, or a state that doesn't match its sidecar."""


class CmdStore:
    """A program that speaks the store protocol: `PROGRAM list`, `PROGRAM get NAME`, and `PROGRAM put NAME`.

    `list` prints a JSON object of sidecars by name, `get` writes the zip to stdout, and `put` reads it from stdin.
    Exit 0 is success, 2 means no such save, 3 means the stored copy is as new or newer, and anything else is a failure.
    """

    NOT_FOUND, CONFLICT = 2, 3

    def __init__(self, program: str, timeout: float = 30.0) -> None:
        self.program = program
        self.timeout = timeout

    def get(self, name: str) -> bytes:
        return self._run("get", name)

    def list(self) -> dict[str, dict]:
        try:
            saves = json.loads(self._run("list") or b"{}")
        except json.JSONDecodeError as e:
            raise StoreError(f"{self.program} list printed something other than JSON: {e}") from e
        if not isinstance(saves, dict):
            raise StoreError(f"{self.program} list printed {type(saves).__name__}, not an object")
        return saves

    def put(self, name: str, data: bytes) -> None:
        self._run("put", name, data=data)

    def _run(self, *args: str, data: bytes | None = None) -> bytes:
        try:
            r = subprocess.run([self.program, *args], input=data, capture_output=True, timeout=self.timeout)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise Unreachable(f"{self.program}: {e}") from e
        err = r.stderr.decode(errors="replace").strip()
        if r.returncode == self.NOT_FOUND:
            raise KeyError(args[-1])
        if r.returncode == self.CONFLICT:
            raise Conflict(err)
        if r.returncode != 0:
            raise StoreError(f"{self.program} {args[0]} exited {r.returncode}: {err}")
        return r.stdout


class Conflict(Exception):
    """The store already holds a copy of the save at least as new, with another state."""


class DirStore:
    """A folder of NAME.zip files, with the copies each put replaced under history/NAME/.

    The server keeps its saves this way, and pointed at a folder that Syncthing, rclone, or NFS keeps in step, it is a
    store on its own.
    """

    def __init__(self, root: str | os.PathLike) -> None:
        self.root = Path(root)

    def get(self, name: str) -> bytes:
        if not valid_name(name):
            raise BadSave(f"not a save name: {name!r}")
        try:
            return (self.root / f"{name}.zip").read_bytes()
        except FileNotFoundError:
            raise KeyError(name) from None

    def list(self) -> dict[str, dict]:
        out = {}
        for p in sorted(self.root.glob("*.zip")):
            if not valid_name(p.stem):
                continue
            try:
                _, info, _ = unpack(p.read_bytes())
            except BadSave:
                continue
            out[p.stem] = info
        return out

    def put(self, name: str, data: bytes) -> None:
        if not valid_name(name):
            raise BadSave(f"not a save name: {name!r}")
        _, info, _ = unpack(data)
        self.root.mkdir(parents=True, exist_ok=True)
        path = self.root / f"{name}.zip"
        old = None
        if path.exists():
            try:
                _, old, _ = unpack(path.read_bytes())
            except BadSave:
                pass
        if old is not None:
            if old["state_sha256"] == info["state_sha256"]:
                return
            if old["saved_unix"] >= info["saved_unix"]:
                raise Conflict(f"{name} is stored at saved_unix {old['saved_unix']}, not older than {info['saved_unix']}")
        tmp = _write_temp(self.root, data)
        if old is not None:
            self._keep(name, path, old["saved_unix"])
        os.replace(tmp, path)

    def _keep(self, name: str, path: Path, saved_unix: int) -> None:
        hist = self.root / "history" / name
        hist.mkdir(parents=True, exist_ok=True)
        os.replace(path, hist / f"{saved_unix}.zip")
        # Only the files this wrote: a synced folder can gain others, such as Syncthing's conflict copies.
        kept = sorted((p for p in hist.glob("*.zip") if p.stem.isdigit()), key=lambda p: int(p.stem))
        for stale in kept[:-HISTORY_KEPT]:
            stale.unlink()


class HttpStore:
    """The save server (sync/tlk_save_server.py): GET /v1/saves, GET and PUT /v1/saves/NAME."""

    def __init__(self, url: str, timeout: float = 3.0) -> None:
        self.url = url.rstrip("/")
        self.timeout = timeout

    def get(self, name: str) -> bytes:
        return self._call("GET", f"/v1/saves/{name}")

    def list(self) -> dict[str, dict]:
        try:
            saves = json.loads(self._call("GET", "/v1/saves"))["saves"]
        except (json.JSONDecodeError, KeyError, TypeError) as e:
            raise StoreError(f"{self.url} answered something other than a save list") from e
        return saves

    def put(self, name: str, data: bytes) -> None:
        self._call("PUT", f"/v1/saves/{name}", data)

    def _call(self, method: str, path: str, body: bytes | None = None) -> bytes:
        req = urllib.request.Request(self.url + path, data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/zip")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            why = e.read().decode(errors="replace").strip()
            if e.code == 404:
                raise KeyError(path) from None
            if e.code == 409:
                raise Conflict(why) from None
            if e.code == 400:
                raise BadSave(why) from None
            raise StoreError(f"{method} {path}: {e.code} {why}") from None
        except (urllib.error.URLError, OSError) as e:
            raise Unreachable(str(e)) from e


class SaveStore(Protocol):
    """What a store provides. A new backend needs these three and nothing else."""

    def get(self, name: str) -> bytes: ...

    def list(self) -> dict[str, dict]: ...

    def put(self, name: str, data: bytes) -> None: ...


class StoreError(Exception):
    """The store failed in a way that isn't about one save."""


class Unreachable(StoreError):
    """The store could not be reached at all."""


def autosave_name(device: str) -> str:
    return f"{AUTOSAVE}@{device}"


def decide(local: dict | None, remote: dict | None) -> str:
    """What to do with one save name: "pull", "push", or "keep".

    `local` is the sidecar on this device with `sha_ok`, whether the state file still hashes to its
    `state_sha256`. A local save without a good hash is never pushed, and a remote copy replaces it.
    """
    usable = local is not None and bool(local.get("state_sha256")) and bool(local.get("sha_ok"))
    if remote is None:
        return "push" if usable else "keep"
    if not usable:
        return "pull"
    if local["state_sha256"] == remote.get("state_sha256"):
        return "keep"
    if remote.get("saved_unix", 0) > local.get("saved_unix", 0):
        return "pull"
    if local.get("saved_unix", 0) > remote.get("saved_unix", 0):
        return "push"
    return "keep"


def device_name(raw: str) -> str:
    """A device name a store accepts: letters, digits, '.', '_', and '-', at most 63 of them."""
    name = re.sub(r"[^A-Za-z0-9._-]+", "-", raw.strip()).strip("-")[:63]
    return name or "device"


def open_store(spec: str) -> SaveStore:
    """`https://...` or `http://...` for the server, `cmd:PROGRAM` for a program, anything else a folder."""
    if spec.startswith(("http://", "https://")):
        return HttpStore(spec)
    if spec.startswith("cmd:"):
        return CmdStore(spec[4:])
    return DirStore(os.path.expanduser(spec))


def pack(state: bytes, info: dict, png: bytes | None = None) -> bytes:
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("save.state", state)
        if png is not None:
            z.writestr("save.png", png)
        z.writestr("save.json", json.dumps(info))
    return out.getvalue()


def pick_continue(candidates: list[dict], build: str) -> dict | None:
    """The newest autosave this build can load. Each candidate is a sidecar plus `where`, "local" or a save name."""
    loadable = [c for c in candidates if c.get("build") == build and c.get("state_sha256")]
    # Ties go to the local copy, which needs no download.
    return max(loadable, key=lambda c: (c.get("saved_unix", 0), c.get("where") == "local"), default=None)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unpack(data: bytes) -> tuple[bytes, dict, bytes | None]:
    """The state, sidecar, and picture of a zip whose state matches its sidecar's `state_sha256`."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = set(z.namelist())
            if not {"save.state", "save.json"} <= names:
                raise BadSave("the zip needs save.state and save.json")
            state = z.read("save.state")
            info = json.loads(z.read("save.json"))
            png = z.read("save.png") if "save.png" in names else None
    except (zipfile.BadZipFile, json.JSONDecodeError, UnicodeDecodeError, EOFError) as e:
        raise BadSave(f"unreadable zip: {e}") from e
    if not isinstance(info, dict) or not isinstance(info.get("saved_unix"), int):
        raise BadSave("save.json needs a whole-number saved_unix")
    if info.get("state_sha256") != sha256(state):
        raise BadSave("save.state doesn't match the state_sha256 in save.json")
    return state, info, png


def valid_name(name: str) -> bool:
    return bool(_NAME.match(name))


def _write_temp(directory: Path, data: bytes) -> str:
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".put-", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except BaseException:
        os.unlink(tmp)
        raise
    return tmp
