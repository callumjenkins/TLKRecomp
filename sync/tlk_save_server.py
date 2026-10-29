"""The save server: a DirStore over HTTP, for devices that reach it through Tailscale Serve.

    python3 tlk_save_server.py --root DIR [--port 4790] --login EMAIL
    python3 tlk_save_server.py --root DIR --no-auth      # a local test server

GET /v1/saves lists every save's sidecar, GET /v1/saves/NAME returns its zip, and PUT /v1/saves/NAME stores one:
400 for a bad zip or name, 409 when the stored copy is as new or newer. It binds to 127.0.0.1 only. Tailscale Serve sets
Tailscale-User-Login on what it forwards, so a request without the configured login gets 403.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tlk_saves import BadSave, Conflict, DirStore  # noqa: E402

MAX_ZIP = 4 << 20


class SaveHandler(BaseHTTPRequestHandler):
    server: SaveServer

    def do_GET(self) -> None:
        if not self._allowed():
            return
        if self.path == "/v1/saves":
            with self.server.lock:
                saves = self.server.store.list()
            self._send(200, json.dumps({"saves": saves}).encode(), "application/json")
            return
        name = self._name()
        if name is None:
            return
        try:
            with self.server.lock:
                data = self.server.store.get(name)
        except KeyError:
            self._send(404, b"no such save\n")
        except BadSave as e:
            self._send(400, f"{e}\n".encode())
        else:
            self._send(200, data, "application/zip")

    def do_PUT(self) -> None:
        if not self._allowed():
            return
        name = self._name()
        if name is None:
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self._send(411, b"Content-Length needed\n")
            return
        if length > MAX_ZIP:
            self._send(413, b"too large\n")
            return
        if length <= 0:
            self._send(400, b"empty\n")
            return
        data = self.rfile.read(length)
        if len(data) != length:
            self.close_connection = True
            return   # cut off mid-body: nothing stored
        try:
            with self.server.lock:
                self.server.store.put(name, data)
        except BadSave as e:
            self._send(400, f"{e}\n".encode())
        except Conflict as e:
            self._send(409, f"{e}\n".encode())
        else:
            self._send(204, b"")

    def log_message(self, format: str, *args: object) -> None:
        sys.stderr.write(f"{self.address_string()} {format % args}\n")

    def _allowed(self) -> bool:
        login = self.server.login
        if login is None or self.headers.get("Tailscale-User-Login") == login:
            return True
        self._send(403, b"forbidden\n")
        return False

    def _name(self) -> str | None:
        prefix = "/v1/saves/"
        if not self.path.startswith(prefix):
            self._send(404, b"not found\n")
            return None
        return self.path[len(prefix):]

    def _send(self, code: int, body: bytes, kind: str = "text/plain") -> None:
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class SaveServer(ThreadingHTTPServer):
    def __init__(self, port: int, root: str | os.PathLike, login: str | None) -> None:
        super().__init__(("127.0.0.1", port), SaveHandler)
        self.store = DirStore(root)
        self.login = login
        self.lock = threading.Lock()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True, help="the folder saves are kept in")
    ap.add_argument("--port", type=int, default=4790)
    ap.add_argument("--login", default=os.environ.get("TLK_SAVES_LOGIN"),
                    help="the Tailscale login allowed in (default: $TLK_SAVES_LOGIN)")
    ap.add_argument("--no-auth", action="store_true", help="let any request in, for a test server")
    args = ap.parse_args(argv)
    if not args.login and not args.no_auth:
        ap.error("give --login (or TLK_SAVES_LOGIN), or --no-auth for a test server")
    server = SaveServer(args.port, args.root, None if args.no_auth else args.login)
    print(f"serving {args.root} on 127.0.0.1:{server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
