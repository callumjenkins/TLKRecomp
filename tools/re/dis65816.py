"""Minimal linear 65816 disassembler for HiROM dumps.

Usage: python3 -I dis65816.py ROM START_ADDR [COUNT] [--m8] [--x8]

Tracks REP/SEP to size immediates. Linear sweep only: data and computed
jumps are not detected.
"""
import sys

# mode -> (operand bytes or None for flag-dependent immediates, format)
MODES = {
    "imp": (0, "{op}"),
    "acc": (0, "{op} A"),
    "immm": (None, "{op} #${v}"),   # size depends on M
    "immx": (None, "{op} #${v}"),   # size depends on X
    "imm8": (1, "{op} #${v}"),
    "dp": (1, "{op} ${v}"),
    "dpx": (1, "{op} ${v},X"),
    "dpy": (1, "{op} ${v},Y"),
    "dpi": (1, "{op} (${v})"),
    "dpil": (1, "{op} [${v}]"),
    "dpix": (1, "{op} (${v},X)"),
    "dpiy": (1, "{op} (${v}),Y"),
    "dpily": (1, "{op} [${v}],Y"),
    "sr": (1, "{op} ${v},S"),
    "sriy": (1, "{op} (${v},S),Y"),
    "abs": (2, "{op} ${v}"),
    "absx": (2, "{op} ${v},X"),
    "absy": (2, "{op} ${v},Y"),
    "absi": (2, "{op} (${v})"),
    "absil": (2, "{op} [${v}]"),
    "absix": (2, "{op} (${v},X)"),
    "long": (3, "{op} ${v}"),
    "longx": (3, "{op} ${v},X"),
    "rel": (1, "{op} ${t}"),
    "rell": (2, "{op} ${t}"),
    "bm": (2, "{op} ${v}"),
}

OPS = {}
def g(group, op, modes):
    for code, mode in zip(group, modes):
        if code is not None:
            OPS[code] = (op, mode)

# Standard ALU groups: ORA AND EOR ADC STA LDA CMP SBC
alu_modes = ["dpix", "sr", "dp", "dpil", "immm", "abs", "long", "dpiy",
             "dpi", "sriy", "dpx", "dpily", "absy", "absx", "longx"]
alu_offsets = [0x01, 0x03, 0x05, 0x07, 0x09, 0x0D, 0x0F, 0x11,
               0x12, 0x13, 0x15, 0x17, 0x19, 0x1D, 0x1F]
for base, op in zip(range(0x00, 0x100, 0x20), ["ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC"]):
    for off, mode in zip(alu_offsets, alu_modes):
        if op == "STA" and mode == "immm":
            continue
        OPS[base + off] = (op, mode)

table = {
    0x00: ("BRK", "imm8"), 0x02: ("COP", "imm8"), 0x04: ("TSB", "dp"), 0x06: ("ASL", "dp"),
    0x08: ("PHP", "imp"), 0x0A: ("ASL", "acc"), 0x0B: ("PHD", "imp"), 0x0C: ("TSB", "abs"),
    0x0E: ("ASL", "abs"), 0x10: ("BPL", "rel"), 0x14: ("TRB", "dp"), 0x16: ("ASL", "dpx"),
    0x18: ("CLC", "imp"), 0x1A: ("INC", "acc"), 0x1B: ("TCS", "imp"), 0x1C: ("TRB", "abs"),
    0x1E: ("ASL", "absx"), 0x20: ("JSR", "abs"), 0x22: ("JSL", "long"), 0x24: ("BIT", "dp"),
    0x26: ("ROL", "dp"), 0x28: ("PLP", "imp"), 0x2A: ("ROL", "acc"), 0x2B: ("PLD", "imp"),
    0x2C: ("BIT", "abs"), 0x2E: ("ROL", "abs"), 0x30: ("BMI", "rel"), 0x34: ("BIT", "dpx"),
    0x36: ("ROL", "dpx"), 0x38: ("SEC", "imp"), 0x3A: ("DEC", "acc"), 0x3B: ("TSC", "imp"),
    0x3C: ("BIT", "absx"), 0x3E: ("ROL", "absx"), 0x40: ("RTI", "imp"), 0x42: ("WDM", "imm8"),
    0x44: ("MVP", "bm"), 0x46: ("LSR", "dp"), 0x48: ("PHA", "imp"), 0x4A: ("LSR", "acc"),
    0x4B: ("PHK", "imp"), 0x4C: ("JMP", "abs"), 0x4E: ("LSR", "abs"), 0x50: ("BVC", "rel"),
    0x54: ("MVN", "bm"), 0x56: ("LSR", "dpx"), 0x58: ("CLI", "imp"), 0x5A: ("PHY", "imp"),
    0x5B: ("TCD", "imp"), 0x5C: ("JML", "long"), 0x5E: ("LSR", "absx"), 0x60: ("RTS", "imp"),
    0x62: ("PER", "rell"), 0x64: ("STZ", "dp"), 0x66: ("ROR", "dp"), 0x68: ("PLA", "imp"),
    0x6A: ("ROR", "acc"), 0x6B: ("RTL", "imp"), 0x6C: ("JMP", "absi"), 0x6E: ("ROR", "abs"),
    0x70: ("BVS", "rel"), 0x74: ("STZ", "dpx"), 0x76: ("ROR", "dpx"), 0x78: ("SEI", "imp"),
    0x7A: ("PLY", "imp"), 0x7B: ("TDC", "imp"), 0x7C: ("JMP", "absix"), 0x7E: ("ROR", "absx"),
    0x80: ("BRA", "rel"), 0x82: ("BRL", "rell"), 0x84: ("STY", "dp"), 0x86: ("STX", "dp"),
    0x88: ("DEY", "imp"), 0x89: ("BIT", "immm"), 0x8A: ("TXA", "imp"), 0x8B: ("PHB", "imp"),
    0x8C: ("STY", "abs"), 0x8E: ("STX", "abs"), 0x90: ("BCC", "rel"), 0x94: ("STY", "dpx"),
    0x96: ("STX", "dpy"), 0x98: ("TYA", "imp"), 0x9A: ("TXS", "imp"), 0x9B: ("TXY", "imp"),
    0x9C: ("STZ", "abs"), 0x9E: ("STZ", "absx"), 0xA0: ("LDY", "immx"), 0xA2: ("LDX", "immx"),
    0xA4: ("LDY", "dp"), 0xA6: ("LDX", "dp"), 0xA8: ("TAY", "imp"), 0xAA: ("TAX", "imp"),
    0xAB: ("PLB", "imp"), 0xAC: ("LDY", "abs"), 0xAE: ("LDX", "abs"), 0xB0: ("BCS", "rel"),
    0xB4: ("LDY", "dpx"), 0xB6: ("LDX", "dpy"), 0xB8: ("CLV", "imp"), 0xBA: ("TSX", "imp"),
    0xBB: ("TYX", "imp"), 0xBC: ("LDY", "absx"), 0xBE: ("LDX", "absy"), 0xC0: ("CPY", "immx"),
    0xC2: ("REP", "imm8"), 0xC4: ("CPY", "dp"), 0xC6: ("DEC", "dp"), 0xC8: ("INY", "imp"),
    0xCA: ("DEX", "imp"), 0xCB: ("WAI", "imp"), 0xCC: ("CPY", "abs"), 0xCE: ("DEC", "abs"),
    0xD0: ("BNE", "rel"), 0xD4: ("PEI", "dp"), 0xD6: ("DEC", "dpx"), 0xD8: ("CLD", "imp"),
    0xDA: ("PHX", "imp"), 0xDB: ("STP", "imp"), 0xDC: ("JML", "absil"), 0xDE: ("DEC", "absx"),
    0xE0: ("CPX", "immx"), 0xE2: ("SEP", "imm8"), 0xE4: ("CPX", "dp"), 0xE6: ("INC", "dp"),
    0xE8: ("INX", "imp"), 0xEA: ("NOP", "imp"), 0xEB: ("XBA", "imp"), 0xEC: ("CPX", "abs"),
    0xEE: ("INC", "abs"), 0xF0: ("BEQ", "rel"), 0xF4: ("PEA", "abs"), 0xF6: ("INC", "dpx"),
    0xF8: ("SED", "imp"), 0xFA: ("PLX", "imp"), 0xFB: ("XCE", "imp"), 0xFC: ("JSR", "absix"),
    0xFE: ("INC", "absx"),
}
OPS.update(table)
assert len(OPS) == 256, len(OPS)


def disasm(rom, start, count, m8=False, x8=False):
    addr = start
    out = []
    for _ in range(count):
        off = addr & 0x3FFFFF
        code = rom[off]
        op, mode = OPS[code]
        size = MODES[mode][0]
        if size is None:
            size = 1 if (m8 if mode == "immm" else x8) else 2
        raw = rom[off + 1:off + 1 + size]
        v = int.from_bytes(raw, "little")
        bank = addr & 0xFF0000
        t = ""
        if mode == "rel":
            t = f"{bank | ((addr + 2 + (v - 256 if v > 127 else v)) & 0xFFFF):06X}"
        elif mode == "rell":
            t = f"{bank | ((addr + 3 + (v - 65536 if v > 32767 else v)) & 0xFFFF):06X}"
        if mode == "bm":
            text = f"{op} ${raw[1]:02X},${raw[0]:02X}"
        else:
            text = MODES[mode][1].format(op=op, v=f"{v:0{size * 2}X}", t=t)
        if op == "REP":
            m8 = m8 and not (v & 0x20); x8 = x8 and not (v & 0x10)
        elif op == "SEP":
            m8 = m8 or bool(v & 0x20); x8 = x8 or bool(v & 0x10)
        hexb = " ".join(f"{b:02X}" for b in rom[off:off + 1 + size])
        out.append(f"{addr:06X}  {hexb:12s}  {text}")
        addr += 1 + size
    return out


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rom = open(args[0], "rb").read()
    start = int(args[1].lstrip("$"), 16)
    count = int(args[2]) if len(args) > 2 else 40
    print("\n".join(disasm(rom, start, count, "--m8" in sys.argv, "--x8" in sys.argv)))
