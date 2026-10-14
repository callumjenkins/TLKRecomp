"""Check the emitter's pacing model (recompiler/v2/aot_pace.py, patch 0003)
against the interpreter core itself.

usage: verify.py SAMPLES [--derive OUT.py] [--model-dir DIR]
SAMPLES comes from interp_samples (see README.md). Every sample is one
interp816_runOpcode() on a random state with a logging bus; the model must
give the same cycle count, the same data/pointer/stack/vector bus addresses
and therefore the same master clocks. --derive writes a fresh BASE table
(cycles with D.l=0, no index page cross, branch not taken) to OUT.py.
"""
import sys, os
here = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:]]
model_dir = os.path.join(here, '..', '..', 'build', 'snesrecomp-codegen', 'recompiler', 'v2')
if '--model-dir' in args:
    model_dir = args[args.index('--model-dir') + 1]
derive_out = args[args.index('--derive') + 1] if '--derive' in args else None
derive = derive_out is not None
sys.path.insert(0, model_dir)
import aot_pace_table
if derive:
    aot_pace_table.BASE = {k: 0 for k in aot_pace_table.BASE}
import aot_pace

M32 = 0xFFFFFFFF
def memf(a, seed):
    h = ((a ^ seed) * 2654435761) & M32
    h ^= h >> 15; h = (h * 2246822519) & M32; h ^= h >> 13
    return h & 0xFF

def spd(a, memsel=1):
    bank = (a >> 16) & 0xFF; lo = a & 0xFFFF
    if 0x40 <= bank <= 0x7F: return 8
    if bank >= 0xC0: return 6 if memsel else 8
    if lo >= 0x8000:
        if bank <= 0x3F: return 8
        return 6 if memsel else 8
    if lo < 0x2000: return 8
    if lo < 0x4000: return 6
    if lo < 0x4200: return 12
    if lo < 0x6000: return 6
    return 8

base = {}
bad = 0; n = 0
for line in open(args[0]):
    f = line.split()
    op = int(f[0], 16); m = int(f[1]); x = int(f[2])
    A, X, Y, S, D = (int(v, 16) for v in f[3:8])
    DB, K = int(f[8], 16), int(f[9], 16); PC = int(f[10], 16); fl = int(f[11], 16)
    ob = bytes.fromhex(f[12]); seed = int(f[13], 16); cyc = int(f[14]); nacc = int(f[15])
    accs = [(t[0], int(t[1:], 16)) for t in f[16:]]
    L = aot_pace.length(op, m, x)
    operand = int.from_bytes(ob[:L - 1], 'little') if L > 1 else 0
    pcbase = (K << 16) | PC
    def peek(a):
        a &= 0xFFFFFF
        if pcbase <= a < pcbase + 4:
            return ([op] + list(ob))[a - pcbase]
        return memf(a, seed)
    env = dict(D=D, X=X, Y=Y, S=S, DB=DB, PB=K, DPL=1 if D & 0xFF else 0,
               PEEK0=lambda a: peek(a & 0xFFFF), PEEK=peek,
               XCROSS=lambda b, i: 1 if (b >> 8) != ((b + i) >> 8) else 0)
    if derive: aot_pace.BASE = aot_pace_table.BASE
    c0, dyn, acc = aot_pace.model(op, m, x, operand)
    dync = sum(eval(e, env) for e in dyn)
    mn = aot_pace.OPS[op][0]
    taken = 0
    if aot_pace.OPS[op][1] == aot_pace.REL and mn != 'BRA':
        c_, z_, v_, n_ = fl & 1, (fl >> 1) & 1, (fl >> 2) & 1, (fl >> 3) & 1
        taken = {'BPL': not n_, 'BMI': n_, 'BVC': not v_, 'BVS': v_, 'BCC': not c_,
                 'BCS': c_, 'BNE': not z_, 'BEQ': z_}[mn]
        taken = 1 if taken else 0
    if derive:
        b = cyc - dync - taken
        base.setdefault((op, m, x), set()).add(b)
        continue
    n += 1
    # interpreter: fetches are the first L accesses at PB:PC..; compare data list
    fetch = [a for t, a in accs[:L]]
    data = [a for t, a in accs[L:]]
    mdata = [eval(e, env) & 0xFFFFFF for e in acc]
    want_cyc = c0 + dync + taken
    def master(cyc_, fetch_, data_):
        nb = len(fetch_) + len(data_)
        return sum(spd(a) for a in fetch_) + sum(spd(a) for a in data_) + 6 * max(0, cyc_ - nb)
    mi = master(cyc, fetch, data)
    mm = sum(spd((pcbase & 0xFF0000) | ((PC + i) & 0xFFFF)) for i in range(L)) \
        + sum(spd(a) for a in mdata) + 6 * max(0, want_cyc - L - len(mdata))
    ok = (cyc == want_cyc and sorted(data) == sorted(mdata) and mi == mm and
          fetch == [(pcbase & 0xFF0000) | ((PC + i) & 0xFFFF) for i in range(L)])
    if not ok:
        bad += 1
        if bad < 25:
            print(f"BAD op={op:02X} {mn} m={m} x={x} cyc={cyc}/{want_cyc} master={mi}/{mm} "
                  f"data={[hex(a) for a in data]} model={[hex(a) for a in mdata]} fetch={[hex(a) for a in fetch]}")
if derive:
    out = {}
    for k, v in sorted(base.items()):
        if len(v) != 1:
            print("inconsistent", k, aot_pace.OPS[k[0]], v)
        out[k] = min(v)
    with open(derive_out, 'w') as fo:
        fo.write('# (opcode, m, x) -> interp816 cycles with D.l=0, no index page cross,\n'
                 '# branch not taken. Derived from the core by tests (aot_pace verify).\n')
        fo.write('BASE = {\n')
        for k, v in sorted(out.items()):
            fo.write(f"    (0x{k[0]:02X}, {k[1]}, {k[2]}): {v},\n")
        fo.write('}\n')
    print("derived", len(out))
else:
    print(f"verified {n} samples, {bad} mismatches")
