import http.server
import json
import subprocess
import sys
import textwrap
import threading
from pathlib import Path

import pytest

import tlk_saves
import tlk_sync
from conftest import make_save

BUILD = "b1"


@pytest.fixture
def fake_sdl(tmp_path: Path) -> Path:
    """Stands in for tlk_sdl: prints a build id, records its arguments, and writes an autosave as it exits."""
    path = tmp_path / "tlk_sdl"
    path.write_text(textwrap.dedent(f"""\
        #!{sys.executable}
        import hashlib, json, sys
        args = sys.argv[1:]
        if args == ["--build-id"]:
            print("{BUILD} v0"); sys.exit(0)
        open({str(tmp_path / "args.json")!r}, "w").write(json.dumps(args))
        auto = args[args.index("--autosave") + 1]
        state = b"played on " + args[args.index("--device") + 1].encode()
        open(auto, "wb").write(state)
        info = {{"build": "{BUILD}", "saved_unix": 5000, "level_name": "Exile", "lives": 4,
                 "device": args[args.index("--device") + 1], "state_sha256": hashlib.sha256(state).hexdigest()}}
        open(auto[:-len(".state")] + ".json", "w").write(json.dumps(info))
        """))
    path.chmod(0o755)
    return path


def put_local(saves: Path, base: str, saved_unix: int, state: bytes = b"local") -> None:
    saves.mkdir(parents=True, exist_ok=True)
    st, info, png = tlk_saves.unpack(make_save(saved_unix, state))
    (saves / f"{base}.state").write_bytes(st)
    (saves / f"{base}.png").write_bytes(png)
    (saves / f"{base}.json").write_text(json.dumps(info))


def test_tlk_play_loads_newest_autosave_and_pushes_on_exit(tmp_path, fake_sdl):
    store = tlk_saves.DirStore(tmp_path / "store")
    laptop = make_save(300, b"from the laptop", device="cj-cos-laptop")
    store.put("tlk_autosave@cj-cos-laptop", laptop)
    saves = tmp_path / "saves"
    put_local(saves, "tlk_autosave", 100, b"older here")
    cfg = {"store": str(tmp_path / "store"), "device": "cj-desk-pc", "tlk_sdl": str(fake_sdl)}
    out = []
    assert tlk_sync.play(cfg, saves, False, [], echo=out.append) == 0
    args = json.loads((tmp_path / "args.json").read_text())
    assert args[args.index("--load") + 1] == str(saves / "tlk_autosave.state")
    assert (saves / "tlk_autosave.prev.state").read_bytes() == b"older here"
    pushed = tlk_saves.unpack(store.get("tlk_autosave@cj-desk-pc"))[1]
    assert pushed["device"] == "cj-desk-pc" and pushed["saved_unix"] == 5000
    assert store.get("tlk_autosave@cj-cos-laptop") == laptop
    assert "pulled tlk_autosave@cj-cos-laptop" in out and "pushed tlk_autosave@cj-desk-pc" in out


def test_tlk_play_loads_the_laptops_autosave_into_tlk_autosave(tmp_path, fake_sdl, monkeypatch):
    store = tlk_saves.DirStore(tmp_path / "store")
    store.put("tlk_autosave@cj-cos-laptop", make_save(300, b"from the laptop"))
    saves = tmp_path / "saves"
    loaded = []
    real_run = tlk_sync.subprocess.run

    def run(args: list[str], *a: object, **k: object) -> subprocess.CompletedProcess:
        if "--load" in args:
            loaded.append((saves / "tlk_autosave.state").read_bytes())
        return real_run(args, *a, **k)

    monkeypatch.setattr(tlk_sync.subprocess, "run", run)
    tlk_sync.play({"store": str(tmp_path / "store"), "device": "pc", "tlk_sdl": str(fake_sdl)}, saves, False, [],
                  echo=lambda _: None)
    assert loaded == [b"from the laptop"]


def test_tlk_play_new_starts_at_the_title(tmp_path, fake_sdl):
    saves = tmp_path / "saves"
    put_local(saves, "tlk_autosave", 100)
    tlk_sync.play({"tlk_sdl": str(fake_sdl), "device": "pc"}, saves, True, ["--scale", "3"], echo=lambda _: None)
    args = json.loads((tmp_path / "args.json").read_text())
    assert "--load" not in args and args[-2:] == ["--scale", "3"]


def test_tlk_play_writes_a_60_second_autosave_ini_once(tmp_path, fake_sdl):
    saves = tmp_path / "saves"
    tlk_sync.play({"tlk_sdl": str(fake_sdl), "device": "pc"}, saves, True, [], echo=lambda _: None)
    assert (saves / "tlk_sdl.ini").read_text() == "autosave_seconds = 60\n"
    (saves / "tlk_sdl.ini").write_text("autosave_seconds = 0\n")
    tlk_sync.play({"tlk_sdl": str(fake_sdl), "device": "pc"}, saves, True, [], echo=lambda _: None)
    assert (saves / "tlk_sdl.ini").read_text() == "autosave_seconds = 0\n"


def test_offline_store_reports_on_pull_and_push_and_still_plays(tmp_path, fake_sdl):
    out = []
    cfg = {"store": "http://127.0.0.1:9", "device": "pc", "tlk_sdl": str(fake_sdl)}
    assert tlk_sync.play(cfg, tmp_path / "saves", False, [], echo=out.append) == 0
    assert (tmp_path / "args.json").exists()
    assert out == ["Not synced: 127.0.0.1 not reachable. Is Tailscale on?"] * 2


def test_pull_keeps_the_replaced_slot_as_prev(tmp_path):
    store = tlk_saves.DirStore(tmp_path / "store")
    store.put("tlk_slot2", make_save(300, b"remote"))
    saves = tmp_path / "saves"
    put_local(saves, "tlk_slot2", 100, b"old bytes")
    report = tlk_sync.pull(store, saves, "pc", BUILD)
    assert report.pulled == ["tlk_slot2"]
    assert (saves / "tlk_slot2.state").read_bytes() == b"remote"
    assert (saves / "tlk_slot2.prev.state").read_bytes() == b"old bytes"
    assert tlk_sync.local_info(saves, "tlk_slot2")["sha_ok"]


def test_push_sends_newer_slots_and_leaves_older_ones(tmp_path):
    store = tlk_saves.DirStore(tmp_path / "store")
    store.put("tlk_slot1", make_save(500, b"newer remote"))
    saves = tmp_path / "saves"
    put_local(saves, "tlk_slot1", 100, b"older local")
    put_local(saves, "tlk_slot3", 100, b"only here")
    report = tlk_sync.push(store, saves, "pc")
    assert report.pushed == ["tlk_slot3"]
    assert tlk_saves.unpack(store.get("tlk_slot1"))[0] == b"newer remote"


def test_a_state_changed_after_its_json_is_not_pushed(tmp_path):
    store = tlk_saves.DirStore(tmp_path / "store")
    saves = tmp_path / "saves"
    put_local(saves, "tlk_slot1", 100)
    (saves / "tlk_slot1.state").write_bytes(b"rewritten")
    assert tlk_sync.push(store, saves, "pc").pushed == []


class _Answer(http.server.BaseHTTPRequestHandler):
    code = 502

    def do_GET(self) -> None:
        self.send_response(self.code)
        self.send_header("Content-Length", "3")
        self.end_headers()
        self.wfile.write(b"bad")

    def log_message(self, *args: object) -> None:
        pass


@pytest.mark.parametrize("code", [403, 502])
def test_tlk_play_with_a_store_answering_403_or_502_reports_and_still_plays(tmp_path, fake_sdl, code):
    handler = type("H", (_Answer,), {"code": code})
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
    try:
        out = []
        cfg = {"store": f"http://127.0.0.1:{srv.server_address[1]}", "device": "pc", "tlk_sdl": str(fake_sdl)}
        assert tlk_sync.play(cfg, tmp_path / "saves", False, [], echo=out.append) == 0
    finally:
        srv.shutdown()
        srv.server_close()
    assert (tmp_path / "args.json").exists()
    assert out and all(line.startswith("Not synced:") and str(code) in line for line in out)


def test_cmd_store_with_bad_list_output_reports_and_still_plays(tmp_path, fake_sdl):
    prog = tmp_path / "bad-store"
    prog.write_text("#!/bin/sh\necho not json\n")
    prog.chmod(0o755)
    out = []
    cfg = {"store": f"cmd:{prog}", "device": "pc", "tlk_sdl": str(fake_sdl)}
    assert tlk_sync.play(cfg, tmp_path / "saves", False, [], echo=out.append) == 0
    assert (tmp_path / "args.json").exists() and out[0].startswith("Not synced:")


def test_remote_autosave_without_a_hash_is_skipped(tmp_path):
    saves = tmp_path / "saves"
    put_local(saves, "tlk_autosave", 100)

    class Store(tlk_saves.DirStore):
        def list(self) -> dict[str, dict]:
            return {"tlk_autosave@x": {"build": BUILD, "saved_unix": 900}}

    report = tlk_sync.pull(Store(tmp_path / "store"), saves, "pc", BUILD)
    assert report.continue_from["where"] == "local" and report.pulled == []


def test_device_name_from_config_is_made_store_safe(tmp_path, fake_sdl):
    store = tlk_saves.DirStore(tmp_path / "store")
    cfg = {"store": str(tmp_path / "store"), "device": "My Laptop", "tlk_sdl": str(fake_sdl)}
    tlk_sync.play(cfg, tmp_path / "saves", True, [], echo=lambda _: None)
    assert list(store.list()) == ["tlk_autosave@My-Laptop"]
    args = json.loads((tmp_path / "args.json").read_text())
    assert args[args.index("--device") + 1] == "My-Laptop"


def test_local_info_flags_a_state_rewritten_after_its_json(tmp_path):
    put_local(tmp_path, "tlk_slot1", 100)
    assert tlk_sync.local_info(tmp_path, "tlk_slot1")["sha_ok"]
    (tmp_path / "tlk_slot1.state").write_bytes(b"rewritten")
    assert not tlk_sync.local_info(tmp_path, "tlk_slot1")["sha_ok"]
    assert tlk_sync.local_info(tmp_path, "tlk_slot4") is None


def test_continue_skips_a_local_autosave_whose_state_does_not_match(tmp_path):
    saves = tmp_path / "saves"
    put_local(saves, "tlk_autosave", 900)
    (saves / "tlk_autosave.state").write_bytes(b"cut short")
    store = tlk_saves.DirStore(tmp_path / "store")
    store.put("tlk_autosave@laptop", make_save(100, b"laptop"))
    report = tlk_sync.pull(store, saves, "pc", BUILD)
    assert report.continue_from["where"] == "tlk_autosave@laptop"
    assert (saves / "tlk_autosave.state").read_bytes() == b"laptop"


def test_pull_does_not_refetch_an_autosave_it_already_has(tmp_path):
    saves = tmp_path / "saves"
    store = tlk_saves.DirStore(tmp_path / "store")
    store.put("tlk_autosave@laptop", make_save(500, b"laptop"))
    tlk_sync.pull(store, saves, "pc", BUILD)
    (saves / "tlk_autosave.prev.state").write_bytes(b"keep me")
    report = tlk_sync.pull(store, saves, "pc", BUILD)
    assert report.pulled == []
    assert (saves / "tlk_autosave.prev.state").read_bytes() == b"keep me"


def test_push_conflict_is_reported_and_the_rest_still_go(tmp_path):
    saves = tmp_path / "saves"
    put_local(saves, "tlk_slot1", 200, b"mine")
    put_local(saves, "tlk_slot2", 200, b"also mine")

    class Store(tlk_saves.DirStore):
        def put(self, name: str, data: bytes) -> None:
            if name == "tlk_slot1":
                raise tlk_saves.Conflict("newer")
            super().put(name, data)

    report = tlk_sync.push(Store(tmp_path / "store"), saves, "pc")
    assert report.pushed == ["tlk_slot2"]
    assert any("tlk_slot1" in p for p in report.problems)


@pytest.mark.parametrize("url, want", [
    pytest.param("https://cj-omv.bandicoot-exponential.ts.net:8450", "cj-omv", id="tailnet name"),
    pytest.param("http://127.0.0.1:4790", "127.0.0.1", id="address"),
])
def test_offline_message_names_the_host(url, want):
    assert tlk_sync._store_label(tlk_saves.HttpStore(url)) == want
