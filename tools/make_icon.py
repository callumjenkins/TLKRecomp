#!/usr/bin/env python3
"""Draw the app icon: a paw print in front of a sun on a sunset.

Original artwork (no game graphics). Writes packaging/icon/tlkrecomp.png
(1024x1024, for the AppImage and the macOS .icns) and tlkrecomp.ico (16-256,
for the Windows .exe). Rerun after changing the drawing; the outputs are
committed so builds need no Pillow.

usage: python3 tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
SS = 4  # supersampling for smooth edges
OUT = Path(__file__).resolve().parent.parent / "packaging" / "icon"

# Top to bottom: gold sky, orange, red, dusk purple.
SKY = [(0.0, (255, 207, 92)), (0.45, (242, 118, 46)), (0.75, (184, 50, 58)), (1.0, (80, 36, 98))]
SUN = (255, 236, 170)
PAW = (36, 20, 22)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def sky_color(t):
    for (t0, c0), (t1, c1) in zip(SKY, SKY[1:]):
        if t <= t1:
            return lerp(c0, c1, (t - t0) / (t1 - t0))
    return SKY[-1][1]


def draw(size):
    n = size * SS
    sky = Image.new("RGB", (n, n))
    d = ImageDraw.Draw(sky)
    for y in range(n):
        d.line([(0, y), (n, y)], fill=sky_color(y / (n - 1)))

    art = ImageDraw.Draw(sky)
    u = n / 1024
    # Sun, low over the horizon, with a soft glow.
    glow = Image.new("L", (n, n), 0)
    ImageDraw.Draw(glow).ellipse([212 * u, 150 * u, 812 * u, 750 * u], fill=110)
    glow = glow.filter(ImageFilter.GaussianBlur(60 * u))
    sky.paste(Image.new("RGB", (n, n), SUN), (0, 0), glow)
    art.ellipse([292 * u, 230 * u, 732 * u, 670 * u], fill=SUN)

    # Paw: a three-lobed main pad and four toes.
    for cx, cy, rx, ry in [(512, 640, 150, 118), (398, 735, 122, 112), (626, 735, 122, 112), (512, 770, 120, 92)]:
        art.ellipse([(cx - rx) * u, (cy - ry) * u, (cx + rx) * u, (cy + ry) * u], fill=PAW)
    for cx, cy, rx, ry in [(262, 455, 74, 96), (414, 345, 82, 108), (610, 345, 82, 108), (762, 455, 74, 96)]:
        art.ellipse([(cx - rx) * u, (cy - ry) * u, (cx + rx) * u, (cy + ry) * u], fill=PAW)

    # Rounded-square mask (macOS-style margin is left to the platform).
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, n - 1, n - 1], radius=int(200 * u), fill=255)
    icon = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    icon.paste(sky, (0, 0), mask)
    return icon.resize((size, size), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    icon = draw(SIZE)
    icon.save(OUT / "tlkrecomp.png", optimize=True)
    icon.save(OUT / "tlkrecomp.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    print(OUT / "tlkrecomp.png")
    print(OUT / "tlkrecomp.ico")


if __name__ == "__main__":
    main()
