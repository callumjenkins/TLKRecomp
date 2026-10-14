#!/usr/bin/env python3
"""List compiled functions that can reach given guest routines.

usage: python3 -I scripts/lle_closure.py [--gen generated/snesrecomp]
           [--exclude PC24,...] [--chain PC24] [--cfg] TARGET_PC24...

Reads the generated C (direct compiled calls `FN_xxxxxx_MxXy(cpu)` and exact
LLE edges `interp_tier_run_call_frame(cpu, 0xPC, ...)`) and prints every
compiled function whose call tree reaches one of TARGET_PC24, transitively.

Why: a compiled body runs an interpreted callee in a nested interpreter frame
(interp_tier_run_call_frame). That frame never yields to the frame driver, so
a callee that spins until an interrupt handler changes RAM cannot finish and
hits the 2M step cap. Such waits must run in the top-level interpreter frame,
so every compiled caller above them is kept on the interpreter (force_lle).

--cfg     print `force_lle` lines instead of a plain list
--chain   print one caller chain from PC24 down to a target
--exclude host-invoked roots (interrupt handlers) to leave compiled
"""
from __future__ import annotations

import argparse
import collections
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FN = re.compile(r"RecompReturn (?:FN_|bank_)([0-9A-F_]+?)_M\dX\d\(CpuState \*cpu\) \{")
LLE = re.compile(r"interp_tier_run_call_frame\(cpu, 0x([0-9a-f]+)u")
CALL = re.compile(r"(?:FN_|bank_)([0-9A-F_]{6,8})_M\dX\d\(cpu\)")


def pc(s: str) -> int:
    return int(s.replace("_", ""), 16)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", type=Path, default=ROOT / "generated" / "snesrecomp")
    ap.add_argument("--exclude", default="")
    ap.add_argument("--chain")
    ap.add_argument("--cfg", action="store_true")
    ap.add_argument("targets", nargs="+")
    a = ap.parse_args()

    callers = collections.defaultdict(set)
    for f in sorted(a.gen.glob("*_v2.c")):
        fn = None
        for line in f.open():
            m = FN.match(line)
            if m:
                fn = pc(m.group(1))
                continue
            if fn is None:
                continue
            for m in LLE.finditer(line):
                callers[int(m.group(1), 16)].add(fn)
            for m in CALL.finditer(line):
                c = pc(m.group(1))
                if c != fn:
                    callers[c].add(fn)

    targets = {int(t, 16) for t in a.targets}
    exclude = {int(t, 16) for t in a.exclude.split(",") if t}
    parent, seen, work = {}, set(), list(targets)
    while work:
        t = work.pop()
        for c in callers.get(t, ()):
            if c not in seen and c not in targets:
                seen.add(c)
                parent[c] = t
                work.append(c)
    out = sorted(seen - exclude)
    if a.chain:
        c = int(a.chain, 16)
        chain = [c]
        while c in parent:
            c = parent[c]
            chain.append(c)
        print(" -> ".join(f"${x:06X}" for x in chain))
    elif a.cfg:
        for x in out:
            print(f"force_lle {x:06X}")
    else:
        print(" ".join(f"{x:06X}" for x in out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
