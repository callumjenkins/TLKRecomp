import io
import json
import os
import sys
import textwrap
import threading
import zipfile
from pathlib import Path

import pytest

import tlk_saves
from conftest import SYNC, make_save
from tlk_save_server import SaveServer


@pytest.fixture
def cmd_store(tmp_path: Path) -> tlk_saves.SaveStore:
    """A program speaking the store protocol over a DirStore, as a third-party backend would."""
    prog = tmp_path / "store-prog"
    prog.write_text(textwrap.dedent(f"""\
        #!{sys.executable}
        import json, sys
        sys.path.insert(0, {str(SYNC)!r})
        import tlk_saves
        store = tlk_saves.DirStore({str(tmp_path / "data")!r})
        verb = sys.argv[1]
        try:
            if verb == "list":
                print(json.dumps(store.list()))
            elif verb == "get":
                sys.stdout.buffer.write(store.get(sys.argv[2]))
            elif verb == "put":
                store.put(sys.argv[2], sys.stdin.buffer.read())
        except KeyError:
            sys.exit(2)
        except tlk_saves.Conflict as e:
            print(e, file=sys.stderr)
            sys.exit(3)
        """))
    prog.chmod(0o755)
    return tlk_saves.open_store(f"cmd:{prog}")


def test_cmd_store_speaks_the_protocol(cmd_store):
    zip_ = make_save(100)
    cmd_store.put("tlk_slot1", zip_)
    assert cmd_store.get("tlk_slot1") == zip_
    assert cmd_store.list()["tlk_slot1"]["saved_unix"] == 100
    with pytest.raises(KeyError):
        cmd_store.get("tlk_slot2")
    with pytest.raises(tlk_saves.Conflict):
        cmd_store.put("tlk_slot1", make_save(50, b"older"))


def test_cmd_store_that_cannot_run_is_unreachable(tmp_path):
    with pytest.raises(tlk_saves.Unreachable):
        tlk_saves.open_store(f"cmd:{tmp_path / 'missing'}").list()


@pytest.mark.parametrize("spec, kind", [
    pytest.param("https://cj-omv.example.ts.net:8450", tlk_saves.HttpStore, id="https"),
    pytest.param("http://127.0.0.1:4790", tlk_saves.HttpStore, id="http"),
    pytest.param("cmd:/usr/bin/true", tlk_saves.CmdStore, id="cmd"),
    pytest.param("~/Sync/tlk", tlk_saves.DirStore, id="folder"),
])
def test_open_store_picks_the_backend_by_its_spec(spec, kind):
    assert isinstance(tlk_saves.open_store(spec), kind)


def test_dir_store_skips_files_that_are_not_saves(tmp_path):
    store = tlk_saves.DirStore(tmp_path)
    store.put("tlk_slot1", make_save(100))
    (tmp_path / "notes.zip").write_bytes(b"x")
    (tmp_path / "tlk_slot2.zip").write_bytes(b"not a zip")
    assert list(store.list()) == ["tlk_slot1"]


def test_unpack_refuses_a_zip_without_its_state():
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("save.json", json.dumps({"saved_unix": 1}))
    with pytest.raises(tlk_saves.BadSave):
        tlk_saves.unpack(out.getvalue())


def test_cmd_store_failure_exit_is_a_store_error(tmp_path):
    prog = tmp_path / "failing"
    prog.write_text("#!/bin/sh\necho broken >&2\nexit 1\n")
    prog.chmod(0o755)
    with pytest.raises(tlk_saves.StoreError, match="broken"):
        tlk_saves.CmdStore(str(prog)).list()


def test_a_stray_file_in_history_does_not_lose_the_live_save(tmp_path):
    store = tlk_saves.DirStore(tmp_path)
    store.put("tlk_slot1", make_save(100, b"one"))
    hist = tmp_path / "history" / "tlk_slot1"
    hist.mkdir(parents=True)
    (hist / "100.sync-conflict-20261029-ABC.zip").write_bytes(b"x")
    store.put("tlk_slot1", make_save(200, b"two"))
    assert tlk_saves.unpack(store.get("tlk_slot1"))[0] == b"two"
    assert not list(tmp_path.glob(".put-*"))


def test_http_store_maps_the_servers_answers(tmp_path):
    srv = SaveServer(0, tmp_path, None)
    threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
    try:
        store = tlk_saves.HttpStore(f"http://127.0.0.1:{srv.server_address[1]}")
        with pytest.raises(KeyError):
            store.get("tlk_slot1")
        store.put("tlk_slot1", make_save(200))
        with pytest.raises(tlk_saves.Conflict):
            store.put("tlk_slot1", make_save(100, b"older"))
        with pytest.raises(tlk_saves.BadSave):
            store.put("tlk_slot1", b"not a zip")
        assert store.list()["tlk_slot1"]["saved_unix"] == 200
    finally:
        srv.shutdown()
        srv.server_close()
    with pytest.raises(tlk_saves.Unreachable):
        tlk_saves.HttpStore("http://127.0.0.1:9", timeout=0.5).list()


@pytest.fixture
def rclone_store(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tlk_saves.SaveStore:
    """The rclone example over a stub rclone that serves a local folder: lsf, cat, and copyto."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    stub = bin_dir / "rclone"
    stub.write_text(textwrap.dedent(f"""\
        #!{sys.executable}
        import fnmatch, os, shutil, sys
        args = [a for a in sys.argv[1:]]
        verb = args.pop(0)
        if verb == "lsf":
            pattern = args[args.index("--include") + 1] if "--include" in args else None
            path = args[-1]
            if os.path.isfile(path):
                print(os.path.basename(path)); sys.exit(0)
            if not os.path.isdir(path):
                sys.exit(3)
            for f in sorted(os.listdir(path)):
                if pattern is None or fnmatch.fnmatch(f, pattern):
                    print(f)
        elif verb == "cat":
            try:
                sys.stdout.buffer.write(open(args[0], "rb").read())
            except FileNotFoundError:
                sys.exit(3)
        elif verb == "copyto":
            os.makedirs(os.path.dirname(args[1]), exist_ok=True)
            shutil.copyfile(args[0], args[1])
        """))
    stub.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bin_dir}:{os.environ['PATH']}")
    monkeypatch.setenv("TLK_RCLONE_REMOTE", str(tmp_path / "remote"))
    return tlk_saves.open_store(f"cmd:{SYNC / 'examples' / 'rclone-store.sh'}")


def test_rclone_example_speaks_the_protocol(rclone_store):
    zip_ = make_save(200)
    rclone_store.put("tlk_slot1", zip_)
    assert rclone_store.get("tlk_slot1") == zip_
    assert rclone_store.list() == {"tlk_slot1": tlk_saves.unpack(zip_)[1]}
    rclone_store.put("tlk_slot1", make_save(100))   # the same state again: accepted, nothing changes
    with pytest.raises(tlk_saves.Conflict):
        rclone_store.put("tlk_slot1", make_save(150, b"older"))
    with pytest.raises(KeyError):
        rclone_store.get("tlk_slot2")
    bad = tlk_saves.pack(b"s", {"saved_unix": 300, "state_sha256": "0" * 64})
    with pytest.raises(tlk_saves.StoreError):
        rclone_store.put("tlk_slot1", bad)
    assert rclone_store.get("tlk_slot1") == zip_
