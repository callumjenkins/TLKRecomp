import http.client
import json
import socket
import threading
from collections.abc import Iterator
from pathlib import Path

import pytest

import tlk_saves
from conftest import make_save
from tlk_save_server import SaveServer

LOGIN = "callum@example.com"


@pytest.fixture
def server(tmp_path: Path) -> Iterator[SaveServer]:
    srv = SaveServer(0, tmp_path / "saves", LOGIN)
    t = threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    t.start()
    yield srv
    srv.shutdown()
    srv.server_close()


def call(srv: SaveServer, method: str, path: str, body: bytes | None = None,
         login: str | None = LOGIN) -> tuple[int, bytes]:
    c = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
    headers = {"Tailscale-User-Login": login} if login else {}
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    return r.status, r.read()


def test_put_then_get_returns_the_same_zip(server):
    zip_ = make_save(100)
    assert call(server, "PUT", "/v1/saves/tlk_slot1", zip_)[0] == 204
    assert call(server, "GET", "/v1/saves/tlk_slot1") == (200, zip_)
    status, body = call(server, "GET", "/v1/saves")
    assert status == 200
    assert json.loads(body)["saves"]["tlk_slot1"]["saved_unix"] == 100


def test_get_of_a_missing_save_is_404(server):
    assert call(server, "GET", "/v1/saves/tlk_slot2")[0] == 404


def test_older_put_is_refused_with_409(server):
    newer = make_save(200, b"new")
    call(server, "PUT", "/v1/saves/tlk_slot1", newer)
    assert call(server, "PUT", "/v1/saves/tlk_slot1", make_save(100, b"old"))[0] == 409
    assert call(server, "PUT", "/v1/saves/tlk_slot1", make_save(200, b"same time"))[0] == 409
    assert call(server, "GET", "/v1/saves/tlk_slot1")[1] == newer


def test_putting_the_stored_state_again_is_accepted(server):
    call(server, "PUT", "/v1/saves/tlk_slot1", make_save(200))
    assert call(server, "PUT", "/v1/saves/tlk_slot1", make_save(100))[0] == 204


def test_put_with_wrong_state_sha_is_refused(server):
    info = {"build": "b1", "saved_unix": 100, "state_sha256": tlk_saves.sha256(b"other")}
    assert call(server, "PUT", "/v1/saves/tlk_slot1", tlk_saves.pack(b"state", info))[0] == 400
    assert call(server, "PUT", "/v1/saves/tlk_slot1", b"not a zip")[0] == 400
    assert call(server, "GET", "/v1/saves/tlk_slot1")[0] == 404


def test_replaced_save_goes_to_history_capped_at_10(server, tmp_path):
    for t in range(1, 12):
        assert call(server, "PUT", "/v1/saves/tlk_slot3", make_save(t, b"s%d" % t))[0] == 204
    hist = sorted(int(p.stem) for p in (tmp_path / "saves" / "history" / "tlk_slot3").glob("*.zip"))
    assert hist == list(range(1, 11))
    call(server, "PUT", "/v1/saves/tlk_slot3", make_save(12, b"s12"))
    hist = sorted(int(p.stem) for p in (tmp_path / "saves" / "history" / "tlk_slot3").glob("*.zip"))
    assert hist == list(range(2, 12))


@pytest.mark.parametrize("name", ["..%2Fx", "../x", "tlk_slot9", "tlk_slot0", "tlk_autosave@", "tlk_autosave",
                                  "tlk_autosave@a/b", "tlk_autosave@" + "x" * 64])
def test_names_outside_the_list_are_refused(server, name):
    assert call(server, "PUT", f"/v1/saves/{name}", make_save(100))[0] == 400
    assert call(server, "GET", f"/v1/saves/{name}")[0] == 400


def test_autosave_with_a_device_name_is_accepted(server):
    assert call(server, "PUT", "/v1/saves/tlk_autosave@cj-cos-laptop", make_save(100))[0] == 204


@pytest.mark.parametrize("login", [None, "rhea@example.com"], ids=["no header", "another login"])
def test_request_without_the_configured_login_is_refused(server, login):
    assert call(server, "GET", "/v1/saves", login=login)[0] == 403
    assert call(server, "PUT", "/v1/saves/tlk_slot1", make_save(100), login=login)[0] == 403
    assert call(server, "GET", "/v1/saves/tlk_slot1")[0] == 404


def test_half_written_upload_leaves_the_old_save(server, tmp_path):
    old = make_save(100, b"old")
    call(server, "PUT", "/v1/saves/tlk_slot1", old)
    new = make_save(200, b"new")
    s = socket.create_connection(("127.0.0.1", server.server_address[1]), timeout=5)
    s.sendall(b"PUT /v1/saves/tlk_slot1 HTTP/1.1\r\nHost: x\r\nTailscale-User-Login: " + LOGIN.encode()
              + b"\r\nContent-Length: %d\r\n\r\n" % len(new) + new[: len(new) // 2])
    s.shutdown(socket.SHUT_WR)
    s.recv(100)
    s.close()
    assert call(server, "GET", "/v1/saves/tlk_slot1")[1] == old
    assert not list((tmp_path / "saves").glob(".put-*"))


def test_put_without_length_or_over_the_cap_is_refused(server):
    port = server.server_address[1]
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(b"PUT /v1/saves/tlk_slot1 HTTP/1.1\r\nHost: x\r\nTailscale-User-Login: " + LOGIN.encode() + b"\r\n\r\n")
    assert s.recv(100).startswith(b"HTTP/1.0 411")
    s.close()
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    c.putrequest("PUT", "/v1/saves/tlk_slot1")
    c.putheader("Tailscale-User-Login", LOGIN)
    c.putheader("Content-Length", str(5 << 20))
    c.endheaders()
    assert c.getresponse().status == 413


def test_put_with_a_non_integer_saved_unix_is_400(server):
    info = {"saved_unix": "100", "state_sha256": tlk_saves.sha256(b"s")}
    assert call(server, "PUT", "/v1/saves/tlk_slot1", tlk_saves.pack(b"s", info))[0] == 400
