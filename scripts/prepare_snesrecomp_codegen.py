#!/usr/bin/env python3
"""Build a patched copy of the snesrecomp submodule for code generation.

The pinned submodule is never edited. This copies it (without .git) to
``build/snesrecomp-codegen``, applies every ``patches/snesrecomp/*.patch`` in
name order with ``patch -p1`` (fails closed if a hunk does not apply), and
builds the native analyzer there. The copy is reused while the submodule
commit and the patch contents are unchanged (stamp file).

usage: prepare_snesrecomp_codegen.py [--out DIR] [--force]
Prints the patched root on the last line.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUBMODULE = REPO / "snesrecomp"
PATCHES = REPO / "patches" / "snesrecomp"


def stamp_text() -> str:
    head = subprocess.run(["git", "-C", str(SUBMODULE), "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip() or "unknown"
    h = hashlib.sha256()
    for p in sorted(PATCHES.glob("*.patch")):
        h.update(p.name.encode()); h.update(p.read_bytes())
    return f"{head} {h.hexdigest()}\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=REPO / "build" / "snesrecomp-codegen")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    out = a.out.resolve()
    stamp = out / ".tlk-codegen-stamp"
    want = stamp_text()
    if a.force or not stamp.is_file() or stamp.read_text() != want:
        target = out / "recompiler-rs" / "target"
        keep = None
        if target.is_dir():  # keep cargo's cache across re-copies
            keep = out.parent / (out.name + ".target-keep")
            shutil.rmtree(keep, ignore_errors=True)
            target.rename(keep)
        shutil.rmtree(out, ignore_errors=True)
        shutil.copytree(SUBMODULE, out, symlinks=True,
                        ignore=shutil.ignore_patterns(".git", "target", "__pycache__"))
        if keep:
            keep.rename(target)
        for p in sorted(PATCHES.glob("*.patch")):
            print(f"applying {p.name}", file=sys.stderr)
            r = subprocess.run(["patch", "-p1", "--forward", "--batch", "-d", str(out),
                                "-i", str(p)], capture_output=True, text=True)
            if r.returncode:
                sys.stderr.write(r.stdout + r.stderr)
                raise SystemExit(f"error: {p.name} does not apply to the pinned snesrecomp")
        stamp.write_text(want)
    r = subprocess.run(["cargo", "build", "--release", "--manifest-path",
                        str(out / "recompiler-rs" / "Cargo.toml")])
    if r.returncode:
        raise SystemExit("error: patched native analyzer build failed")
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
