#!/usr/bin/env python3
"""Estimate how many game functions Lion King has vs how many the recomp compiles (task Z).

1. compiled: generated/snesrecomp/program_manifest.json nodes (entry pc x M/X variant), by disposition.
2. static walk: recursive descent over ROM bytes from every known root (manifest nodes, recomp/*.cfg `func`
   lines, `indirect_dispatch` targets, the reset/NMI/IRQ vectors). Follows branches, JMP/BRL/JML and the
   fall-through after calls; REP/SEP update M/X; PLP/RTI keep the current widths (approximation). A JSR/JSL
   target is a function entry. Stops a path at RTS/RTL/RTI, indirect jumps, BRK/COP/STP/WDM (counted as
   "bad": usually a width guess gone wrong or data). With --tables, unresolved `JMP/JSR (abs,X)` sites in the
   walked code read a same-bank word table: entries are taken while the target lies in $C0-$C6 code banks,
   is not inside the table, decodes to a plausible first opcode, max 64.
3. execution: with rommap files (tlk_headless --lle, TLK_ROMMAP), entries whose first byte ran as an opcode.
4. what is left: bytes in $C0-$C6 that neither the walk nor execution covered, nor any data class of
   tools/rom_accounting.py (code banks only), divided by the median walked function size.

usage: function_count.py [--rom R] [--tables] [MAP...]
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT.parent / "tlk-map"))
import dis65816 as d  # noqa: E402

CODE_BANKS = range(0xC0, 0xC7)
BAD = {"BRK", "COP", "STP", "WDM"}
BRANCH = {"BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ"}


def size_of(op, mode, m8, x8):
    s = d.MODES[mode][0]
    if s is None:
        s = 1 if (m8 if mode == "immm" else x8) else 2
    return 1 + s


def norm(pc):
    """Fold HiROM mirrors onto $C0-$FF."""
    b, a = pc >> 16, pc & 0xFFFF
    if b < 0x40 or 0x80 <= b < 0xC0:
        if a < 0x8000:
            return None
        return ((b & 0x3F) | 0xC0) << 16 | a
    if 0x40 <= b < 0x7E:
        return (b | 0xC0) << 16 | a
    if b >= 0xC0:
        return pc
    return None


class Walker:
    def __init__(self, rom, tables):
        self.rom, self.tables = rom, tables
        self.funcs = {}        # entry pc24 -> set of (m8, x8)
        self.owner = {}        # entry -> set of instruction pcs
        self.seen = set()      # (pc, m8, x8) instruction states
        self.bad = 0
        self.table_targets = set()
        self.isize = {}        # instruction pc -> byte length

    def byte(self, pc):
        return self.rom[pc & 0x3FFFFF]

    def add(self, pc, m8, x8, work):
        pc = norm(pc)
        if pc is None or (pc >> 16) not in CODE_BANKS:
            return
        if (m8, x8) not in self.funcs.setdefault(pc, set()):
            self.funcs[pc].add((m8, x8))
            work.append((pc, m8, x8))

    def walk(self, roots):
        work = []
        for pc, m8, x8 in roots:
            self.add(pc, m8, x8, work)
        while work:
            entry, m8, x8 = work.pop()
            body = self.owner.setdefault(entry, set())
            stack = [(entry, m8, x8)]
            while stack:
                pc, m8, x8 = stack.pop()
                while True:
                    if (pc >> 16) not in CODE_BANKS or (pc, m8, x8) in self.seen:
                        break
                    self.seen.add((pc, m8, x8))
                    body.add(pc)
                    op, mode = d.OPS[self.byte(pc)]
                    n = size_of(op, mode, m8, x8)
                    self.isize[pc] = n
                    v = int.from_bytes(self.rom[(pc & 0x3FFFFF) + 1:(pc & 0x3FFFFF) + n], "little")
                    bank = pc & 0xFF0000
                    nxt = bank | ((pc + n) & 0xFFFF)
                    if op in BAD:
                        self.bad += 1
                        break
                    if op == "REP":
                        m8 = m8 and not (v & 0x20); x8 = x8 and not (v & 0x10)
                    elif op == "SEP":
                        m8 = m8 or bool(v & 0x20); x8 = x8 or bool(v & 0x10)
                    if op in ("RTS", "RTL", "RTI"):
                        break
                    if mode == "rel":
                        t = bank | ((pc + 2 + (v - 256 if v > 127 else v)) & 0xFFFF)
                        if op == "BRA":
                            pc = t; continue
                        stack.append((t, m8, x8))
                    elif op == "BRL":
                        pc = bank | ((pc + 3 + (v - 65536 if v > 32767 else v)) & 0xFFFF); continue
                    elif op == "PER":
                        pass
                    elif op == "JSR" and mode == "abs":
                        self.add(bank | v, m8, x8, work)
                    elif op == "JSL":
                        self.add(v, m8, x8, work)
                    elif op == "JMP" and mode == "abs":
                        pc = bank | v; continue
                    elif op == "JML" and mode == "long":
                        self.add(v, m8, x8, work); break
                    elif mode == "absix":     # JMP (abs,X) / JSR (abs,X): jump table in this bank
                        if self.tables:
                            for t in self.read_table(bank | v):
                                self.table_targets.add(t)
                                self.add(t, m8, x8, work)
                        if op == "JMP":
                            break
                    elif op in ("JMP", "JML"):  # (abs) / [abs]: RAM pointer
                        break
                    pc = nxt

    def read_table(self, tbl):
        out = []
        bank = tbl & 0xFF0000
        lo = tbl & 0xFFFF
        for i in range(64):
            a = lo + 2 * i
            if a + 1 > 0xFFFF:
                break
            w = self.rom[(bank | a) & 0x3FFFFF] | self.rom[((bank | a) + 1) & 0x3FFFFF] << 8
            t = bank | w
            if tbl <= t < bank | (a + 2):
                break
            if d.OPS[self.byte(t)][0] in BAD or w < 0x0100:
                break
            out.append(t)
        return out


def cfg_roots():
    roots = []
    for p in sorted((ROOT / "recomp").glob("bank*.cfg")):
        bank = None
        for line in p.read_text().splitlines():
            mb = re.match(r"bank\s*=\s*0x([0-9A-Fa-f]+)", line)
            if mb:
                bank = int(mb.group(1), 16)
            mf = re.match(r"func\s+\S+\s+([0-9A-Fa-f]+)(.*)", line)
            if mf and bank is not None:
                mx = re.search(r"entry_mx:(\d),(\d)", mf.group(2))
                m, x = (int(mx.group(1)), int(mx.group(2))) if mx else (1, 1)
                roots.append((bank << 16 | int(mf.group(1), 16), bool(m), bool(x)))
            mt = re.search(r"targets:([0-9A-Fa-f,]+)", line)
            if line.startswith("indirect_dispatch") and mt:
                roots += [(int(t, 16), True, True) for t in mt.group(1).split(",") if t]
    return roots


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rom", default=str(ROOT.parent / "Lion King, The (USA).sfc"))
    ap.add_argument("--manifest", default=str(ROOT / "generated/snesrecomp/program_manifest.json"))
    ap.add_argument("--tables", action="store_true")
    ap.add_argument("maps", nargs="*")
    a = ap.parse_args(argv)
    rom = Path(a.rom).read_bytes()
    if len(rom) % 0x8000 == 512:
        rom = rom[512:]

    man = json.loads(Path(a.manifest).read_text())["nodes"]
    disp = {}
    for v in man.values():
        disp.setdefault(v["disposition"], 0)
        disp[v["disposition"]] += 1
    comp_pcs = {norm(v["key"]["pc24"]) for v in man.values()}
    aot_pcs = {norm(v["key"]["pc24"]) for v in man.values() if v["disposition"] == "aot_eligible"}
    print(f"compiled manifest: {len(man)} variants, {len(comp_pcs)} entry pcs; dispositions {disp}; "
          f"entries with an AOT body {len(aot_pcs)}")

    roots = [(v["key"]["pc24"], bool(v["key"]["m"]), bool(v["key"]["x"])) for v in man.values()]
    roots += cfg_roots()
    for vec in (0xFFFC, 0xFFEA, 0xFFEE):
        roots.append((0xC00000 | rom[vec] | rom[vec + 1] << 8, True, True))
    w = Walker(rom, a.tables)
    w.walk(roots)
    funcs = set(w.funcs)
    sizes = sorted(len(w.owner.get(f, ())) for f in funcs)
    covered = set()
    for b in w.owner.values():
        covered |= b
    print(f"static walk{' + tables' if a.tables else ''}: {len(funcs)} entry pcs "
          f"({sum(len(s) for s in w.funcs.values())} entry x width), {len(covered)} instruction starts, "
          f"bad stops {w.bad}, table targets {len(w.table_targets)}, median instrs/func {statistics.median(sizes)}")
    print(f"  walked entries not in the manifest: {len(funcs - comp_pcs)}; manifest entries not reached: "
          f"{len(comp_pcs - funcs)}")
    by_bank = {}
    for f in funcs:
        by_bank.setdefault(f >> 16, [0, 0])
        by_bank[f >> 16][0] += 1
        by_bank[f >> 16][1] += f in comp_pcs
    print("  by bank (walked / of which compiled): " +
          ", ".join(f"${b:02X} {n}/{c}" for b, (n, c) in sorted(by_bank.items())))

    if a.maps:
        acc = 0
        for p in a.maps:
            acc |= int.from_bytes(Path(p).read_bytes()[:len(rom)].ljust(len(rom), b"\0"), "big")
        flags = acc.to_bytes(len(rom), "big")
        ex = {f for f in funcs if flags[f & 0x3FFFFF] & 1}
        print(f"executed (rommap, {len(a.maps)} maps): {len(ex)} walked entries ran; "
              f"{len(ex & comp_pcs)} of them in the manifest; not executed {len(funcs - ex)}")
        # bytes in code banks covered by nothing: no flag, not in a walked instruction
        cov = bytearray(len(rom))
        for pc, n in w.isize.items():
            cov[pc & 0x3FFFFF:(pc & 0x3FFFFF) + n] = b"\x01" * n
        fb = [sum(w.isize[pc] for pc in w.owner[f]) for f in funcs if w.owner.get(f)]
        med = statistics.median(fb)
        left = 0
        for bank in CODE_BANKS:
            base = (bank & 0x3F) << 16
            for i in range(base, base + 0x10000):
                if not flags[i] and not cov[i] and rom[i] not in (0x00, 0xFF):
                    left += 1
        # executed opcodes the walk never reached: code entered through RAM pointers / data tables
        runs, last = [], -100
        for i in range(0, 7 << 16):
            if flags[i] & 1 and not cov[i]:
                if i - last > 16:
                    runs.append(i)
                last = i
        nexec = sum(1 for i in range(0, 7 << 16) if flags[i] & 1)
        nexec_out = sum(1 for i in range(0, 7 << 16) if flags[i] & 1 and not cov[i])
        print(f"executed opcode starts in $C0-$C6: {nexec}; outside the walk: {nexec_out}, in {len(runs)} clusters "
              f"(gap > 16 bytes splits), i.e. roughly that many functions reached only through pointers")
        print(f"code-bank bytes untouched by walk/execution/data reads (non-fill): {left}; median walked function "
              f"~{med:.0f} bytes -> at most ~{left / med:.0f} more functions if all of it were code")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
