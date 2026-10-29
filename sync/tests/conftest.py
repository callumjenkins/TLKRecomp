import sys
from pathlib import Path

SYNC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SYNC))

import tlk_saves  # noqa: E402


def make_save(saved_unix: int, state: bytes = b"state", **extra: object) -> bytes:
    info = {"build": "b1", "saved_unix": saved_unix, "state_sha256": tlk_saves.sha256(state), **extra}
    return tlk_saves.pack(state, info, b"png")

