"""List known routines (static walk) whose first byte never ran on any TLK_ROMMAP map, and which maps run their callers.

usage (from TLKRecomp/): python3 tools/never_ran.py build/rommap/*.map [more.map ...]
"""
import sys, json
from pathlib import Path
sys.path.insert(0, "tools")
import function_count as fc
ROOT = Path(".").resolve()
rom = Path("../Lion King, The (USA).sfc").read_bytes()
if len(rom) % 0x8000 == 512: rom = rom[512:]
man = json.loads(Path("generated/snesrecomp/program_manifest.json").read_text())["nodes"]
roots = [(v["key"]["pc24"], bool(v["key"]["m"]), bool(v["key"]["x"])) for v in man.values()] + fc.cfg_roots()
for vec in (0xFFFC, 0xFFEA, 0xFFEE): roots.append((0xC00000 | rom[vec] | rom[vec+1] << 8, True, True))
w = fc.Walker(rom, True); w.walk(roots)
acc = 0
for p in sys.argv[1:]:
    acc |= int.from_bytes(Path(p).read_bytes()[:len(rom)].ljust(len(rom), b"\0"), "big")
flags = acc.to_bytes(len(rom), "big")
funcs = set(w.funcs)
never = sorted(f for f in funcs if not flags[f & 0x3FFFFF] & 1)
# callers: walked funcs whose body has a JSR/JSL to f
callers = {}
for g, body in w.owner.items():
    for pc in body:
        o = rom[pc & 0x3FFFFF]
        if o == 0x20: t = (pc & 0xFF0000) | rom[(pc&0x3FFFFF)+1] | rom[(pc&0x3FFFFF)+2] << 8
        elif o == 0x22: t = rom[(pc&0x3FFFFF)+1] | rom[(pc&0x3FFFFF)+2] << 8 | rom[(pc&0x3FFFFF)+3] << 16
        elif o == 0x4C: t = (pc & 0xFF0000) | rom[(pc&0x3FFFFF)+1] | rom[(pc&0x3FFFFF)+2] << 8
        else: continue
        t = fc.norm(t) if hasattr(fc, "norm") else t
        if t in never: callers.setdefault(t, set()).add((g, bool(flags[pc & 0x3FFFFF] & 1)))
for f in never:
    n = sum(w.isize[pc] for pc in w.owner.get(f, ()))
    cs = callers.get(f, set())
    print(f"${f:06X} {n:5d}B callers: " + " ".join(f"${g:06X}{'*' if r else ''}" for g, r in sorted(cs)[:6]))
print(len(never), "never ran")
# neighbourhood: which tapes execute code of the callers (or within 0x200 bytes if no caller)
maps = {Path(p).stem: Path(p).read_bytes() for p in sys.argv[1:]}
print("---")
for f in never:
    if f in (0xC09EC6,0xC09F44,0xC0A033,0xC0A0F7,0xC0A2EC,0xC5383D): continue
    cs = [g for g,_ in callers.get(f, ())]
    probe = set()
    for g in cs: probe |= {pc & 0x3FFFFF for pc in w.owner.get(g, ())}
    if not probe: probe = set(range((f&0x3FFFFF)-0x200, (f&0x3FFFFF)+0x200))
    hits = sorted(n for n, m in maps.items() if any(i < len(m) and m[i] & 1 for i in probe))
    print(f"${f:06X} {('caller' if cs else 'near')}: {len(hits)} tapes: {' '.join(hits[:8])}")
