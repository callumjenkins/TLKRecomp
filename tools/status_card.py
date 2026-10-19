"""Draw the README's recompilation status card (docs/img/status.png), in the style of the game's HUD.

usage (from TLKRecomp/): python3 tools/status_card.py [--out docs/img/status.png]

The numbers are typed in below, each with the command that measured it. Re-run those, update ROWS and MEASURED,
then run this script. It draws the card only; it measures nothing. Everything is drawn here (palette taken from
the game's sky and HUD meters, font and shapes made by hand); no game graphics are used. Needs Pillow.
"""
import argparse
import math

from PIL import Image, ImageDraw

MEASURED = "2026-10-18"

# (label, done, total, count shown above the meter)
ROWS = [
    # tools/function_count.py --tables build/rommap/*.map: "entries with an AOT body 1305" of 1305 entry pcs
    ("ROUTINES COMPILED", 1305, 1305, "1305/1305"),
    # scripts/interp_share.py --tape allbugsF (as .polled.tape) --frames 60648: interpreted share 5.5%
    ("GAME CODE RUN COMPILED", 94.5, 100, ""),
    # tools/function_count.py: 1,305 in the manifest of ~1,390 known (1,318 walked + ~72 pointer-only)
    ("FUNCTIONS FOUND", 1305, 1390, "1305/~1390"),
    # scripts/check_all_tapes.sh -s: "71 MATCH, 0 MISMATCH"
    ("TAPES MATCH INTERPRETER", 71, 71, "71/71"),
    # tools/ram_sync_compare.py on 6987M, 6781M, 4735M + scripts/check_allbugs.sh: ALLBUGS PASS
    ("SPEEDRUNS IN SYNC", 4, 4, "4/4"),
]

SCALE = 3
W, H = 272, 244
# From the game: sky bands, HUD meter frame / stripe / zigzag, health-meter red.
SKY = ["#de4a00", "#ff3900", "#ff7300", "#ff8c00", "#ffad00"]
PINK, PURPLE, STRIPE, ZIG, ZIG_SHADE = "#ff7bbd", "#390039", "#ff3900", "#ffff00", "#ff8c00"
SUN = "#ffde9c"
CREAM, SHADE = "#fff7de", "#ffde9c"

# 5x7 capitals, digits and the few signs the card needs.
GLYPHS = {
    "A": "01110 10001 10001 11111 10001 10001 10001", "B": "11110 10001 10001 11110 10001 10001 11110",
    "C": "01110 10001 10000 10000 10000 10001 01110", "D": "11110 10001 10001 10001 10001 10001 11110",
    "E": "11111 10000 10000 11110 10000 10000 11111", "F": "11111 10000 10000 11110 10000 10000 10000",
    "G": "01110 10001 10000 10111 10001 10001 01111", "H": "10001 10001 10001 11111 10001 10001 10001",
    "I": "01110 00100 00100 00100 00100 00100 01110", "J": "00111 00010 00010 00010 00010 10010 01100",
    "K": "10001 10010 10100 11000 10100 10010 10001", "L": "10000 10000 10000 10000 10000 10000 11111",
    "M": "10001 11011 10101 10101 10001 10001 10001", "N": "10001 11001 10101 10011 10001 10001 10001",
    "O": "01110 10001 10001 10001 10001 10001 01110", "P": "11110 10001 10001 11110 10000 10000 10000",
    "Q": "01110 10001 10001 10001 10101 10010 01101", "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110", "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110", "V": "10001 10001 10001 10001 10001 01010 00100",
    "W": "10001 10001 10001 10101 10101 10101 01010", "X": "10001 10001 01010 00100 01010 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100", "Z": "11111 00001 00010 00100 01000 10000 11111",
    "0": "01110 10001 10011 10101 11001 10001 01110", "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00010 00100 01000 11111", "3": "11110 00001 00001 01110 00001 00001 11110",
    "4": "00010 00110 01010 10010 11111 00010 00010", "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110", "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110", "9": "01110 10001 10001 01111 00001 00010 01100",
    "%": "11001 11001 00010 00100 01000 10011 10011", "/": "00001 00001 00010 00100 01000 10000 10000",
    "~": "00000 00000 01000 10101 00010 00000 00000", ".": "00000 00000 00000 00000 00000 01100 01100",
    "-": "00000 00000 00000 11111 00000 00000 00000", " ": "00000 00000 00000 00000 00000 00000 00000",
}


def text_width(s, size=1):
    return (len(s) * 6 - 1) * size


def text(d, x, y, s, size=1, fill=CREAM, shade=SHADE, outline=PURPLE):
    """Pixel text with a 1-pixel outline; the lower rows take the shade colour, like the game's menu."""
    pixels = [(gy, x + (i * 6 + gx) * size, y + gy * size)
              for i, ch in enumerate(s)
              for gy, row in enumerate(GLYPHS[ch].split())
              for gx, bit in enumerate(row) if bit == "1"]
    for _, px, py in pixels:
        d.rectangle([px - 1, py - 1, px + size, py + size], fill=outline)
    for gy, px, py in pixels:
        d.rectangle([px, py, px + size - 1, py + size - 1], fill=shade if gy >= 5 else fill)


def sky(d):
    band = H // len(SKY)
    for i, c in enumerate(SKY):
        d.rectangle([0, i * band, W, (i + 1) * band + 1], fill=c)
        if i:   # dithered seam between bands
            for x in range(0, W, 2):
                d.point((x + (i % 2), i * band), fill=SKY[i - 1])
    for y0, x0, n in ((59, -20, 120), (92, 150, 140), (125, -40, 150), (158, 170, 120)):   # pink streaks, between rows
        for x in range(x0, x0 + n):
            y = y0 + (x // 8) % 4 if (x // 32) % 2 else y0 + 3 - (x // 8) % 4
            d.line([x, y, x, y + 1], fill=PINK)


def savanna(d):
    d.ellipse([168, 206, 208, 246], fill=SUN)   # setting sun, half under the hills
    for x in range(W):
        top = 228 - round(3 * math.sin(x / 23) + 2 * math.sin(x / 9 + 1))
        d.line([x, top, x, H], fill=PURPLE)
    # Acacia: trunk, two branches and a flat crown.
    d.line([229, 230, 231, 214], fill=PURPLE, width=2)
    d.line([231, 216, 222, 210], fill=PURPLE)
    d.line([231, 216, 240, 209], fill=PURPLE)
    d.ellipse([208, 202, 256, 212], fill=PURPLE)


def meter(d, x, y, w, pct):
    """A HUD meter: purple frame, red-orange stripes, yellow zigzag up to pct."""
    d.rectangle([x, y, x + w, y + 11], fill=PURPLE)
    d.line([x + 2, y + 2, x + w - 2, y + 2], fill=STRIPE)
    d.line([x + 2, y + 9, x + w - 2, y + 9], fill=STRIPE)
    end = x + 2 + round((w - 4) * pct / 100)
    for px in range(x + 2, end):
        wave = (px - x) % 8
        py = y + 3 + (wave if wave < 4 else 8 - wave)   # zigzag between rows 3 and 7
        d.rectangle([px, py, px, py + 1], fill=ZIG)
        d.point((px, py + 2), fill=ZIG_SHADE)


def card():
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    sky(d)
    savanna(d)
    title = "RECOMPILATION STATUS"
    text(d, (W - text_width(title, 2)) // 2, 8, title, size=2)
    mx, mw = 10, 206
    for i, (label, done, total, count) in enumerate(ROWS):
        y = 34 + i * 33
        pct = 100.0 * done / total
        shown = f"{pct:.0f}%" if pct == 100 else f"{pct:.1f}%"
        text(d, mx + 1, y, label)
        if count:
            text(d, mx + mw - text_width(count), y, count)
        meter(d, mx, y + 10, mw, pct)
        text(d, W - 8 - text_width(shown), y + 12, shown)
    text(d, 8, H - 11, "MEASURED " + MEASURED, fill=SHADE)
    return im.resize((W * SCALE, H * SCALE), Image.NEAREST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="docs/img/status.png")
    card().save(ap.parse_args().out, optimize=True)


if __name__ == "__main__":
    main()
