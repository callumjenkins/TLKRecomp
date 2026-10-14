"""Dump The Lion King (SNES, USA) level header table.

Usage: python3 -I dump_levels.py "Lion King, The (USA).sfc"

Field meanings are from NOTES.md; anything marked ? is a guess.
"""
import sys

LEVEL_POINTER_TABLE = 0xC00357
LEVEL_COUNT = 16
RECORD_SIZE = 0x32

NAMES = {
    0x00: "Pride Lands",
    0x01: "Can't Wait to Be King",
    0x02: "Elephant Graveyard",
    0x03: "The Stampede",
    0x04: "Simba's Exile",
    0x05: "Hakuna Matata",
    0x06: "Simba's Destiny",
    0x07: "Be Prepared",
    0x08: "Simba's Return",
    0x09: "Pride Rock",
    0x0A: "bonus?",
    0x0B: "bonus?",
    0x0C: "bonus?",
    0x0D: "bonus?",
    0x0E: "special? (no gfx)",
    0x0F: "Simba's Exile (alt start)?",
}

# (offset, size, name)
FIELDS = [
    (0x00, 3, "gfx_a (compressed)"),
    (0x03, 3, "common_a"),
    (0x06, 3, "gfx_b (compressed)"),
    (0x09, 3, "gfx_c (compressed)"),
    (0x0C, 3, "palette"),
    (0x0F, 3, "data_d"),
    (0x12, 2, "width?"),
    (0x14, 2, "height?"),
    (0x16, 2, "checkpoint_x"),
    (0x18, 2, "checkpoint_y"),
    (0x1A, 3, "gfx_e (compressed)"),
    (0x1D, 1, "flags?"),
    (0x26, 3, "level_routine"),
    (0x29, 1, "music?"),
    (0x2B, 3, "ptr_list_entry"),
    (0x2E, 2, "next_level"),
]


def main(path):
    rom = open(path, "rb").read()

    def rd(off, size):
        return int.from_bytes(rom[off:off + size], "little")

    for i in range(LEVEL_COUNT):
        rec = rd((LEVEL_POINTER_TABLE & 0x3FFFFF) + i * 3, 3) & 0x3FFFFF
        print(f"== level ${i:02X} {NAMES[i]}  (record ${0xC00000 | rec:06X})")
        for off, size, name in FIELDS:
            v = rd(rec + off, size)
            print(f"   +${off:02X} {name:20s} ${v:0{size * 2}X}")


if __name__ == "__main__":
    main(sys.argv[1])
