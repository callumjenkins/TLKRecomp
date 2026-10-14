#!/usr/bin/env python3
"""Check declared callee exit widths against the ROM bytes.

usage: python3 -I scripts/check_exit_mx.py [--rom ROM] [--manifest program_manifest.json]
                                           [--cfg-dir recomp] [PC24[:M,X] ...]

For every `exit_mx_at` / `exit_mx_variant` in the cfgs (or for the PCs given),
walk the routine from each entry width with a small path-sensitive 65816
abstract interpreter and print the set of (M,X) widths at its RTS/RTL exits.

Model:
- REP/SEP/PHP/PLP/XCE tracked exactly; the stack is modelled byte by byte
  (PHP pushes a P token, PER pushes a return token, other pushes push opaque
  bytes). PLP restores widths only if it pops a P token pushed in this routine.
- `PER *+2 / BRL helper` local calls (the $C21924 library) are followed: the
  helper's RTS that pops the PER token continues after the BRL.
- JSR/JSL are summarised by walking the callee recursively (depth-limited).
  A callee that cannot be walked (indirect dispatch, RAM code, frame
  rewriting) is assumed to return balanced with unknown widths; such
  results are tagged "[assumes balanced $X]" and are only proofs if a later
  REP/SEP/PLP re-establishes the widths (which is what the walk checks).
- Indirect jumps, RTI, BRK/COP/STP/WAI, recursion past the limit, a PLP of an
  unknown byte, or a return with a non-empty local stack give UNPROVEN.

Verdicts per declaration:
  ok        every entry variant the analyzer requested returns exactly the
            declared width
  WRONG     some entry variant provably returns another width
  UNPROVEN  the walk could not decide
Entry variants come from the manifest's call demands (default:
generated/snesrecomp/program_manifest.json) plus the func's entry_mx.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# operand size by opcode; -1 = M-dependent immediate, -2 = X-dependent
SIZE = [0] * 256
_ALU = {0x01: 1, 0x03: 1, 0x05: 1, 0x07: 1, 0x09: -1, 0x0D: 2, 0x0F: 3, 0x11: 1,
        0x12: 1, 0x13: 1, 0x15: 1, 0x17: 1, 0x19: 2, 0x1D: 2, 0x1F: 3}
for base in range(0, 0x100, 0x20):
    for off, sz in _ALU.items():
        SIZE[base + off] = sz
for op, sz in {
    0x00: 1, 0x02: 1, 0x04: 1, 0x06: 1, 0x0C: 2, 0x0E: 2, 0x10: 1, 0x14: 1, 0x16: 1,
    0x1C: 2, 0x1E: 2, 0x20: 2, 0x22: 3, 0x24: 1, 0x26: 1, 0x2C: 2, 0x2E: 2, 0x30: 1,
    0x34: 1, 0x36: 1, 0x3C: 2, 0x3E: 2, 0x42: 1, 0x44: 2, 0x46: 1, 0x4C: 2, 0x4E: 2,
    0x50: 1, 0x54: 2, 0x56: 1, 0x5C: 3, 0x5E: 2, 0x62: 2, 0x64: 1, 0x66: 1, 0x6C: 2,
    0x6E: 2, 0x70: 1, 0x74: 1, 0x76: 1, 0x7C: 2, 0x7E: 2, 0x80: 1, 0x82: 2, 0x84: 1,
    0x86: 1, 0x89: -1, 0x8C: 2, 0x8E: 2, 0x90: 1, 0x94: 1, 0x96: 1, 0x9C: 2, 0x9E: 2,
    0xA0: -2, 0xA2: -2, 0xA4: 1, 0xA6: 1, 0xAC: 2, 0xAE: 2, 0xB0: 1, 0xB4: 1, 0xB6: 1,
    0xBC: 2, 0xBE: 2, 0xC0: -2, 0xC2: 1, 0xC4: 1, 0xC6: 1, 0xCC: 2, 0xCE: 2, 0xD0: 1,
    0xD4: 1, 0xD6: 1, 0xDC: 2, 0xDE: 2, 0xE0: -2, 0xE2: 1, 0xE4: 1, 0xE6: 1, 0xEC: 2,
    0xEE: 2, 0xF0: 1, 0xF4: 2, 0xF6: 1, 0xFC: 2, 0xFE: 2,
}.items():
    SIZE[op] = sz
BRANCH = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0}
UNKNOWN = None


class Unproven(Exception):
    pass


# RAM routines whose bytes are known and leave P untouched (exit = entry).
# $7E2006: `MVN dd,ss / RTL`, written by $C1CCFE ($C1CD35: LDA #$54 / STA $7E2006,
# LDA #$6B / STA $7E2009; operands at $7E2007/8). MVN and RTL do not touch P.
WIDTH_PRESERVING_RAM = {0x7E2006}


# Traced axioms (task AA). The game's callbacks are its own compiled-C routines
# (PHP / REP #$30 ... PLP / RTL), reached through function pointers the walker cannot
# enumerate: the trampolines $C07770 (`JML [$001F]`, pointer pushed by the caller) and
# $C0CD0E (`JML [$001F]`, set by $C0CC61), and `PHK / PEA r / JML [dp]` or
# `PHK / PEA r / PHA / RTS` frames. They are assumed to return with the widths of the
# call; every return site that this assumption feeds is collected in TRACED_RETURNS and
# checked against a TLK_MX_HIST capture (--mx-hist, --lle on all play tapes + TASes).
TRACED_AXIOMS = {(0xC07770, 0, 0): (0, 0), (0xC0CD0E, 0, 0): (0, 0),
                 # $C09BFD/$C09C64: PHP / PHB / REP #$30 / PHX ... PLX / PLB / PLP / RTS around a
                 # loop that pops a PEA $0000-terminated list (`PLX / BEQ`): stack depth depends
                 # on data, so the walk cannot pair the pulls. Every list value pushed is non-zero
                 # ($0001-$0003, $0020, A+4 with A < $1D), so the loop ends on the sentinel.
                 (0xC09BFD, 0, 0): (0, 0), (0xC09C64, 0, 0): (0, 0),
                 # task CC: $C0A0F0 (task U manual proof, cfg bankc0): PEA / PHP into $C0A0F7's
                 # body, whose PLP / RTS returns through the PEA to the RTL: exit = entry. Needed
                 # by $C07624 (PHK / PEA $76AB / JML $C0A0F0).
                 (0xC0A0F0, 0, 0): (0, 0), (0xC0A0F0, 0, 1): (0, 1)}
TRACED_RETURNS: set = set()
# cfg `data_region` ranges (pc24, end exclusive), filled by parse_cfgs.
DATA_REGIONS: list = []


class Walker:
    def __init__(self, rom: bytes, max_depth: int = 12, max_states: int = 40000,
                 assume=None):
        self.rom = rom
        # Induction hypotheses (task AA): {(pc24, m, x): (m, x)}. A call to one
        # of these is summarised by the hypothesis instead of being walked.
        self.assume = assume or {}
        self.max_depth = max_depth
        self.max_states = max_states
        self.memo: dict = {}

    def byte(self, pc24: int) -> int:
        bank, addr = pc24 >> 16, pc24 & 0xFFFF
        if bank >= 0xC0 or (0x40 <= bank < 0x7E):
            off = pc24 & 0x3FFFFF
        elif (bank & 0x7F) < 0x40 and addr >= 0x8000:
            off = ((bank & 0x3F) << 16) | addr
        else:
            raise Unproven(f"code outside ROM at ${pc24:06X}")
        if off >= len(self.rom):
            raise Unproven(f"code outside ROM at ${pc24:06X}")
        return self.rom[off]

    @staticmethod
    def _pushed_frame(op, stack):
        """Depth of the `PHK / PEA r` frame (counted from the top) a JML [dp] / RTS
        dispatches through, or None."""
        if op == 0x60:
            if len(stack) >= 5 and [t[0] for t in stack[-5:-2]] == ["K", "?", "A"] \
                    and all(t[0] == "?" for t in stack[-2:]):
                return 5
            return None
        for k in range(3, len(stack) + 1):
            if [t[0] for t in stack[-k:][:3]] == ["K", "?", "A"]:
                return k if all(t[0] == "?" for t in stack[-k + 3:]) or k == 3 else None
        return None

    def summary(self, entry: int, m, x, depth: int = 0):
        """-> (frozenset of (m, x) at returns, frozenset of opaque callees
        assumed to return balanced), or raises Unproven."""
        key = (entry, m, x)
        if depth > 0 and key in self.assume:
            return frozenset({self.assume[key]}), frozenset()
        if depth > 0 and key in TRACED_AXIOMS:
            return frozenset({TRACED_AXIOMS[key]}), frozenset({entry})
        if key in self.memo:
            r = self.memo[key]
            if r == "busy":
                raise Unproven(f"recursion at ${entry:06X}")
            if isinstance(r, Unproven):
                raise r
            return r
        if depth > self.max_depth:
            raise Unproven(f"depth limit at ${entry:06X}")
        self.memo[key] = "busy"
        try:
            cond = set()
            r = (frozenset(self._walk(entry, m, x, depth, cond)), frozenset(cond))
            # task CC: no reachable return = the routine never returns (every path ends
            # in an S reset, see TXS below, or a data region). Callers' paths end there;
            # any declared exit width is vacuously true.
        except Unproven as e:
            self.memo[key] = e
            raise
        self.memo[key] = r
        return r

    def _walk(self, entry, m, x, depth, cond):
        exits = set()
        seen = set()
        work = [(entry, m, x, ())]
        while work:
            st = work.pop()
            if st in seen:
                continue
            seen.add(st)
            if len(seen) > self.max_states:
                raise Unproven(f"state limit in ${entry:06X}")
            pc, m, x, stack = st
            if any(lo <= pc < hi for lo, hi in DATA_REGIONS):
                continue    # cfg data_region (assertion BRK): never executed, no exit
            op = self.byte(pc)
            sz = SIZE[op]
            if sz == -1:
                if m is UNKNOWN:
                    raise Unproven(f"M unknown at immediate ${pc:06X}")
                sz = 1 if m else 2
            elif sz == -2:
                if x is UNKNOWN:
                    raise Unproven(f"X unknown at immediate ${pc:06X}")
                sz = 1 if x else 2
            bank = pc & 0xFF0000
            nxt = bank | ((pc + 1 + sz) & 0xFFFF)
            opnd = int.from_bytes(bytes(self.byte(bank | ((pc + 1 + i) & 0xFFFF))
                                        for i in range(sz)), "little")

            def go(p, m_=m, x_=x, s=stack):
                work.append((p, m_, x_, s))

            def push(n, s=stack):
                return s + (("?",),) * n

            def pop(n, s=stack):
                if len(s) < n:
                    raise Unproven(f"pops caller frame at ${pc:06X}")
                return s[:-n] if n else s

            def reg_m():
                if m is UNKNOWN:
                    raise Unproven(f"M unknown at ${pc:06X}")
                return 1 if m else 2

            def reg_x():
                if x is UNKNOWN:
                    raise Unproven(f"X unknown at ${pc:06X}")
                return 1 if x else 2

            if op == 0xC2:      # REP
                go(nxt, 0 if opnd & 0x20 else m, 0 if opnd & 0x10 else x)
            elif op == 0xE2:    # SEP
                go(nxt, 1 if opnd & 0x20 else m, 1 if opnd & 0x10 else x)
            elif op == 0x08:    # PHP
                go(nxt, s=stack + (("P", m, x),))
            elif op == 0x28:    # PLP
                if not stack:
                    raise Unproven(f"PLP of caller byte at ${pc:06X}")
                top = stack[-1]
                if top[0] == "P":
                    go(nxt, top[1], top[2], stack[:-1])
                else:
                    go(nxt, UNKNOWN, UNKNOWN, stack[:-1])
            elif op in (0x48,):            go(nxt, s=push(reg_m()))       # PHA
            elif op in (0x5A, 0xDA):       go(nxt, s=push(reg_x()))       # PHY PHX
            elif op == 0x8B:               go(nxt, s=push(1))             # PHB
            elif op == 0x4B:               go(nxt, s=stack + (("K",),))   # PHK
            elif op == 0xF4:               go(nxt, s=stack + (("?",), ("A", opnd)))  # PEA
            elif op in (0x0B, 0xD4):       go(nxt, s=push(2))             # PHD PEI
            elif op == 0x68:               go(nxt, s=pop(reg_m()))        # PLA
            elif op in (0x7A, 0xFA):       go(nxt, s=pop(reg_x()))        # PLY PLX
            elif op == 0xAB:               go(nxt, s=pop(1))              # PLB
            elif op == 0x2B:               go(nxt, s=pop(2))              # PLD
            elif op == 0x62:    # PER: push a return token for PER *+2 / BRL
                ret = bank | ((pc + 3 + opnd) & 0xFFFF)
                go(nxt, s=stack + (("R", ret),))
            elif (op == 0x3B and m == 0
                  and [self.byte(bank | ((pc + i) & 0xFFFF)) for i in (1, 2, 5)] == [0x18, 0x69, 0x1B]):
                # TSC / CLC / ADC #n / TCS (task AA): caller-side argument cleanup of
                # the compiled-C calling convention. Drops n bytes this routine pushed;
                # only opaque bytes may be dropped (never a P or return token).
                n = self.byte(bank | ((pc + 3) & 0xFFFF)) | (self.byte(bank | ((pc + 4) & 0xFFFF)) << 8)
                if len(stack) < n or any(t[0] not in ("?", "A", "K") for t in stack[len(stack) - n:]):
                    raise Unproven(f"TSC/ADC #{n}/TCS drops non-local bytes at ${pc:06X}")
                go(bank | ((pc + 6) & 0xFFFF), s=stack[:len(stack) - n] if n else stack)
            elif op in BRANCH:
                d = opnd - 256 if opnd > 127 else opnd
                go(nxt)
                go(bank | ((nxt + d) & 0xFFFF))
            elif op == 0x80:
                d = opnd - 256 if opnd > 127 else opnd
                go(bank | ((nxt + d) & 0xFFFF))
            elif op == 0x82:
                d = opnd - 65536 if opnd > 32767 else opnd
                go(bank | ((nxt + d) & 0xFFFF))
            elif op == 0x4C:
                go(bank | opnd)
            elif op == 0x5C and len(stack) >= 3 and [t[0] for t in stack[-3:]] == ["K", "?", "A"]:
                # task CC: `PHK / PEA r / JML abs` is a long call returning to r+1
                # ($C076A4 PHK / PEA $76AB / JML $C0A0F0 in $C07624).
                if m is UNKNOWN or x is UNKNOWN:
                    raise Unproven(f"call ${opnd:06X} at ${pc:06X} with unknown width")
                ret = bank | ((stack[-1][1] + 1) & 0xFFFF)
                if (opnd, m, x) in TRACED_AXIOMS:
                    TRACED_RETURNS.add((ret, m, x))
                try:
                    sub, subcond = self.summary(opnd, m, x, depth + 1)
                except Unproven:
                    cond.add(opnd)
                    go(ret, UNKNOWN, UNKNOWN, stack[:-3])
                    continue
                cond.update(subcond)
                for em, ex in sub:
                    go(ret, em, ex, stack[:-3])
            elif op == 0x5C:
                go(opnd)
            elif op == 0xFB and self.byte(bank | ((pc - 1) & 0xFFFF)) == 0x18:
                # task CC: `CLC / XCE` (soft reset at $C08000) stays in / enters native mode;
                # widths are conservatively forgotten (1/1 from emulation, unchanged if native).
                go(nxt, UNKNOWN, UNKNOWN)
            elif (op == 0x9A and x == 0 and self.byte(bank | ((pc - 3) & 0xFFFF)) == 0xA2):
                # task CC: `LDX #imm16 / TXS` resets S to a constant ($C08140 in $C08122:
                # LDX #$02FF / TXS). The caller's return frame is gone, so this path never
                # returns to the caller: it contributes no exit width (no-return tail,
                # e.g. $C50119 JML $C08122, $C00201 JML $C089B7 ... $C08122).
                continue
            elif op in (0x20, 0x22):       # JSR JSL
                tgt = (bank | opnd) if op == 0x20 else opnd
                if m is UNKNOWN or x is UNKNOWN:
                    raise Unproven(f"call ${tgt:06X} at ${pc:06X} with unknown width")
                if tgt in WIDTH_PRESERVING_RAM:
                    go(nxt)
                    continue
                if (tgt, m, x) in TRACED_AXIOMS:
                    TRACED_RETURNS.add((nxt, m, x))
                try:
                    sub, subcond = self.summary(tgt, m, x, depth + 1)
                except Unproven:
                    # Opaque callee (dynamic dispatch, RAM code, frame
                    # rewriting): assume the ordinary balanced call ABI the
                    # recompiler itself assumes, but forget both widths.
                    cond.add(tgt)
                    go(nxt, UNKNOWN, UNKNOWN)
                    continue
                cond.update(subcond)
                for em, ex in sub:
                    go(nxt, em, ex)
            elif (op in (0x60, 0xDC) and self._pushed_frame(op, stack) is not None):
                # task AA: call through a pushed frame. `PHK / PEA r / JML [dp]` or
                # `PHK / PEA r / (push target-1) / RTS`: the target RTLs to r+1. With
                # JML, opaque argument bytes pushed after the frame are the callee's to
                # pull ($C0B97B passes object parameters that way).
                k = self._pushed_frame(op, stack)
                ret = bank | ((stack[-k + 2][1] + 1) & 0xFFFF)
                if m is UNKNOWN or x is UNKNOWN:
                    raise Unproven(f"callback at ${pc:06X} with unknown width")
                # Axiom (TRACED): a callback returns with the widths of its call;
                # every such return site is checked against TLK_MX_HIST (--mx-hist).
                TRACED_RETURNS.add((ret, m, x))
                go(ret, m, x, stack[:-k])
            elif op == 0x60:    # RTS
                if stack and stack[-1][0] == "R":
                    go(bank | ((stack[-1][1] + 1) & 0xFFFF), s=stack[:-1])
                elif stack:
                    raise Unproven(f"RTS with {len(stack)} local bytes at ${pc:06X}")
                else:
                    if m is UNKNOWN or x is UNKNOWN:
                        raise Unproven(f"return with unknown width at ${pc:06X}")
                    exits.add((m, x))
            elif op == 0x6B:    # RTL
                if stack:
                    raise Unproven(f"RTL with {len(stack)} local bytes at ${pc:06X}")
                if m is UNKNOWN or x is UNKNOWN:
                    raise Unproven(f"return with unknown width at ${pc:06X}")
                exits.add((m, x))
            elif op in (0x6C, 0x7C, 0xDC, 0xFC, 0x40, 0x00, 0x02, 0xDB, 0xCB, 0xFB,
                        0x1B, 0x9A):
                raise Unproven(f"op ${op:02X} at ${pc:06X}")
            else:
                go(nxt)
        return exits


def parse_cfgs(cfg_dir: Path):
    decls = []   # (pc24, kind, data, file, line)
    entry = {}
    for f in sorted(cfg_dir.glob("bank*.cfg")):
        bank = None
        for n, line in enumerate(f.read_text().splitlines(), 1):
            t = line.split("#", 1)[0].split()
            if not t:
                continue
            if t[0] == "bank" and len(t) >= 3:
                bank = int(t[2], 16)
            elif t[0] == "func" and len(t) >= 3 and bank is not None:
                pc = (bank << 16) | int(t[2], 16)
                for tok in t[3:]:
                    mm = re.match(r"entry_mx:(\d),(\d)", tok)
                    if mm:
                        entry[pc] = (int(mm.group(1)), int(mm.group(2)))
            elif t[0] == "data_region" and len(t) >= 4:
                b = int(t[1], 16) << 16
                DATA_REGIONS.append((b | int(t[2], 16), b | int(t[3], 16)))
            elif t[0] == "exit_mx_at":
                decls.append((int(t[1], 16), "at", (int(t[2]), int(t[3])), f.name, n))
            elif t[0] == "exit_mx_variant":
                decls.append((int(t[1], 16), "variant",
                              tuple(int(v) for v in t[2:6]), f.name, n))
    return decls, entry


def manifest_entries(path: Path):
    out = {}
    if not path.is_file():
        return out
    d = json.loads(path.read_text())
    for node in d.get("nodes", {}).values():
        k = node["key"]
        out.setdefault(k["pc24"], set()).add((k["m"], k["x"]))
        for dem in node.get("demands", []):
            t = dem.get("target")
            if t and "pc24" in t:
                out.setdefault(t["pc24"], set()).add((t["m"], t["x"]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", type=Path, default=ROOT.parent / "Lion King, The (USA).sfc")
    ap.add_argument("--cfg-dir", type=Path, default=ROOT / "recomp")
    ap.add_argument("--manifest", type=Path,
                    default=ROOT / "generated" / "snesrecomp" / "program_manifest.json")
    ap.add_argument("--induct", action="store_true",
                    help="with PCs: prove exit = entry for all of them together (induction)")
    ap.add_argument("--mx-hist", nargs="+", type=Path, default=[],
                    help="TLK_MX_HIST captures: check every traced-axiom return site")
    ap.add_argument("--call-pairs", nargs="+", type=Path, default=[],
                    help="TLK_CALL_PAIR captures: check every traced-axiom return site per call width")
    ap.add_argument("pcs", nargs="*")
    a = ap.parse_args()
    rom = a.rom.read_bytes()
    decls, entry = parse_cfgs(a.cfg_dir)
    ent = manifest_entries(a.manifest)
    if a.pcs:
        decls = []
        for p in a.pcs:
            pc, _, mx = p.partition(":")
            pc = int(pc, 16)
            if mx:
                ent[pc] = {tuple(int(v) for v in mx.split(","))}
            decls.append((pc, "query", None, "-", 0))

    def variants_of(pc, kind, data):
        if kind == "variant":
            return {(data[0], data[1])}
        v = set(ent.get(pc, set()))
        if pc in entry:
            v.add(entry[pc])
        return v

    def wanted(kind, data, m, x):
        if kind == "variant":
            return (data[2], data[3])
        if kind == "at":
            return tuple(data)
        return (m, x) if a.induct else None

    # Induction (task AA): mutually recursive routines (the compiled-C game logic
    # in $C0/$C1/$C3/$C5/$C6) can only be proven together. Hypotheses = every
    # declared exit width; drop each one whose body does not return exactly that
    # width when calls to the remaining hypotheses are summarised by them, until
    # nothing changes (greatest fixpoint). What is left holds for every
    # terminating call, by induction on call depth.
    hyp = {}
    for pc, kind, data, _f, _l in decls:
        for m, x in variants_of(pc, kind, data):
            want = wanted(kind, data, m, x)
            if want is not None:
                if hyp.get((pc, m, x), want) != want:
                    hyp[(pc, m, x)] = None      # conflicting declarations
                else:
                    hyp[(pc, m, x)] = want
    hyp = {k: v for k, v in hyp.items() if v is not None}
    while True:
        wk = Walker(rom, assume=hyp)
        drop = []
        for (pc, m, x), want in hyp.items():
            try:
                exs, _ = wk.summary(pc, m, x)
                if exs and sorted(exs) != [want]:
                    drop.append((pc, m, x))
            except Unproven:
                drop.append((pc, m, x))
        if not drop:
            break
        for k in drop:
            del hyp[k]
    w = Walker(rom, assume=hyp)
    bad = 0
    for pc, kind, data, fname, line in decls:
        variants = set(ent.get(pc, set()))
        if pc in entry:
            variants.add(entry[pc])
        if kind == "variant":
            variants = {(data[0], data[1])}
        res = []
        verdict = "ok"
        for m, x in sorted(variants):
            try:
                exs, cond = w.summary(pc, m, x)
                ex = sorted(exs)
                if not ex:
                    res.append(f"M{m}X{x}->never returns")
                    continue
                res.append(f"M{m}X{x}->" + ",".join(f"M{a_}X{b}" for a_, b in ex)
                           + (" [assumes balanced " + ",".join(f"${c:06X}" for c in sorted(cond)) + "]"
                              if cond else ""))
                want = (data[2], data[3]) if kind == "variant" else data
                if kind != "query" and ex != [tuple(want)]:
                    verdict = "WRONG" if verdict != "UNPROVEN" else verdict
            except Unproven as e:
                res.append(f"M{m}X{x}->? ({e})")
                if verdict == "ok":
                    verdict = "UNPROVEN"
        if not variants:
            verdict, res = "UNPROVEN", ["no known entry width"]
        if kind == "query":
            verdict = "-"
        if verdict != "ok":
            bad += 1
        decl = "" if data is None else " ".join(map(str, data))
        print(f"{verdict:9s} ${pc:06X} {kind:7s} {decl:8s} {fname}:{line}  " + "; ".join(res))
    if a.mx_hist:
        seen = {}
        for f in a.mx_hist:
            for ln in f.read_text().split("\n"):
                if ln.strip():
                    pc_, bits = ln.split()
                    seen[int(pc_, 16)] = seen.get(int(pc_, 16), 0) | int(bits)
        n_ok = n_unseen = 0
        for ret, m, x in sorted(TRACED_RETURNS):
            bits = seen.get(ret, 0)
            if not bits:
                n_unseen += 1
            elif bits == 1 << (2 * m + x):
                n_ok += 1
            else:
                bad += 1
                got = ",".join(f"M{i >> 1}X{i & 1}" for i in range(4) if bits >> i & 1)
                print(f"TRACED-WRONG return ${ret:06X} expects M{m}X{x}, seen {got}")
        print(f"traced axiom return sites: {n_ok} ok, {n_unseen} never ran, "
              f"{len(TRACED_RETURNS) - n_ok - n_unseen} wrong ({len(a.mx_hist)} captures)")
    if a.call_pairs:
        pairs = {}
        for f in a.call_pairs:
            for ln in f.read_text().split("\n"):
                if ln.strip():
                    pc_, mask = ln.split()
                    pairs[int(pc_, 16)] = pairs.get(int(pc_, 16), 0) | int(mask)
        n_ok = n_unseen = n_wrong = 0
        for ret, m, x in sorted(TRACED_RETURNS):
            cw = 2 * m + x
            got = (pairs.get(ret, 0) >> (4 * cw)) & 0xF
            if not got:
                n_unseen += 1
            elif got == 1 << cw:
                n_ok += 1
            else:
                n_wrong += 1
                bad += 1
                print(f"TRACED-WRONG call returning to ${ret:06X} at M{m}X{x} came back at "
                      + ",".join(f"M{i >> 1}X{i & 1}" for i in range(4) if got >> i & 1))
        print(f"traced axiom calls (call width -> return width): {n_ok} ok, {n_unseen} never ran, "
              f"{n_wrong} wrong ({len(a.call_pairs)} captures)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
