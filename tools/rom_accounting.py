#!/usr/bin/env python3
"""Account for every ROM byte by bank: executed code, analysed-but-unexecuted code, level blobs,
compressed graphics, VRAM DMA sources, audio, other data, fill, unknown.

Inputs:
  maps      one or more TLK_ROMMAP files (tlk_headless --lle with TLK_ROMMAP=<path>); OR-ed together.
            Flags per byte (runner/tlk_rommap.c): 01 opcode, 02 operand, 04 CPU data read, 08 VRAM DMA
            source, 10 other DMA source, 20 read by the decompressor $C1CCFE, 40 read by the sound
            library $C21924-$C223FF.
  --manifest generated/snesrecomp/program_manifest.json: [min_pc24, max_pc24] of every analysed body
            (static code; spans can include inline data, so data flags win over it).
  level records ($C00357, tlk-map/NOTES.md): every compressed field of the 16 records is decompressed
            with tlk-map/decompress.py to get its exact compressed extent (counted even if never loaded).

Class of a byte = first match in this order:
  code_exec (01|02), audio (40), level (level-record blob extent), compressed (20, other streams:
  sprites, title, cut scenes), gfx_dma (08), dma_other (10: CGRAM / OAM / WRAM), cpu_data (04: tables,
  pointers, scripts), code_static (in an analysed body, never executed), fill (run of >= 32 equal
  $00/$FF bytes), unknown.

usage: rom_accounting.py [--rom R] [--manifest M] [--md out.md] MAP...
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "tlk-map"))
import decompress as dz  # noqa: E402

CLASSES = ["code_exec", "audio", "level", "compressed", "gfx_dma", "dma_other", "cpu_data",
           "code_static", "fill", "unknown"]
LEVEL_TABLE = 0xC00357
COMPRESSED_FIELDS = (0x00, 0x06, 0x09, 0x0F, 0x1A)  # tried; a field that does not decode is skipped


def level_extents(rom):
    out = []
    for i in range(16):
        p = LEVEL_TABLE & 0x3FFFFF
        rec = int.from_bytes(rom[p + 3 * i:p + 3 * i + 3], "little") & 0x3FFFFF
        out.append((rec, rec + 0x32))
        for f in COMPRESSED_FIELDS:
            a = int.from_bytes(rom[rec + f:rec + f + 3], "little")
            if a < 0xC00000:
                continue
            try:
                _, n = dz.decompress(rom, a)
            except Exception:
                continue
            out.append((a & 0x3FFFFF, (a & 0x3FFFFF) + n))
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("maps", nargs="+")
    ap.add_argument("--rom", default=str(HERE.parent.parent / "Lion King, The (USA).sfc"))
    ap.add_argument("--manifest", default=str(HERE.parent / "generated/snesrecomp/program_manifest.json"))
    ap.add_argument("--md", help="also write the table as Markdown")
    a = ap.parse_args(argv)
    rom = Path(a.rom).read_bytes()
    n = len(rom)
    flags = bytearray(n)
    for m in a.maps:
        b = Path(m).read_bytes()
        for i in range(0, n, 1 << 16):  # OR in chunks
            chunk = int.from_bytes(flags[i:i + 65536], "little") | int.from_bytes(b[i:i + 65536], "little")
            flags[i:i + 65536] = chunk.to_bytes(len(flags[i:i + 65536]), "little")
    level = bytearray(n)
    for s, e in level_extents(rom):
        level[s:e] = b"\1" * (e - s)
    static = bytearray(n)
    if Path(a.manifest).exists():
        for node in json.load(open(a.manifest))["nodes"].values():
            lo, hi = node.get("min_pc24"), node.get("max_pc24")
            if lo is None or hi is None or (lo >> 16) < 0xC0:
                continue
            lo &= 0x3FFFFF; hi &= 0x3FFFFF
            if 0 <= hi - lo < 0x4000:
                static[lo:hi + 4] = b"\1" * (hi + 4 - lo)
    fill = bytearray(n)
    i = 0
    while i < n:
        j = i
        while j < n and rom[j] == rom[i]:
            j += 1
        if rom[i] in (0x00, 0xFF) and j - i >= 32:
            fill[i:j] = b"\1" * (j - i)
        i = j

    def cls(k):
        f = flags[k]
        if f & 3: return 0
        if f & 0x40: return 1
        if level[k]: return 2
        if f & 0x20: return 3
        if f & 0x08: return 4
        if f & 0x10: return 5
        if f & 0x04: return 6
        if static[k]: return 7
        if fill[k]: return 8
        return 9

    banks = []
    tot = [0] * len(CLASSES)
    for b in range(n >> 16):
        c = [0] * len(CLASSES)
        for k in range(b << 16, (b + 1) << 16):
            c[cls(k)] += 1
        banks.append(c)
        tot = [x + y for x, y in zip(tot, c)]

    hdr = "| bank | " + " | ".join(CLASSES) + " |"
    lines = [hdr, "|" + "---|" * (len(CLASSES) + 1)]
    for b, c in enumerate(banks):
        lines.append(f"| ${0xC0 + b:02X} | " + " | ".join(str(x) for x in c) + " |")
    lines.append("| total | " + " | ".join(str(x) for x in tot) + " |")
    lines.append("| % | " + " | ".join(f"{100 * x / n:.1f}" for x in tot) + " |")
    ex, st = tot[0], tot[7]
    lines.append("")
    lines.append(f"ROM {n} bytes. Code bytes executed at least once: {ex}. Analysed code never executed: {st}.")
    lines.append(f"Executed share of known code (exec / (exec + static)): {100 * ex / max(1, ex + st):.1f}%.")
    cb = [c for c in banks if c[0]]
    cunk = sum(c[9] for c in cb)
    lines.append(f"Lower bound, counting every unknown byte in the {len(cb)} banks that hold executed code "
                 f"(${0xC0:02X}-${0xC0 + len(cb) - 1:02X}) as code: {100 * ex / max(1, ex + st + cunk):.1f}% "
                 f"({cunk} unknown bytes there).")
    unk = tot[9]
    lines.append(f"Unknown: {unk} bytes ({100 * unk / n:.1f}%): never read by the CPU, the decompressor, the "
                 f"sound library or DMA on any tape, not in an analysed body, not fill.")
    text = "\n".join(lines)
    print(text)
    if a.md:
        Path(a.md).write_text(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
