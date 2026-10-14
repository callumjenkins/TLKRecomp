"""Render a Lion King (SNES) level map to PNG.

Usage: python3 -I render_level.py ROM LEVEL OUT.png

Format (see NOTES.md):
  +06 = BG tiles, 4bpp
  +09 = map, one 16-bit word per 16x16 cell; +12 = row size in bytes, +14 = rows
  +0C = palette, BGR555
  +0F = block table: cell word & $FFF8 is a byte offset to 4 tilemap words
        (top-left, top-right, bottom-left, bottom-right)
Only the foreground layer is drawn; color 0 is black.
"""
import os
import sys
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decompress import decompress


def rd(rom, off, n):
    return int.from_bytes(rom[off:off + n], "little")


def decode_tile(gfx, index):
    """Return 8x8 list of 4bpp color indices, or None past the end."""
    base = index * 32
    if base + 32 > len(gfx):
        return None
    rows = []
    for y in range(8):
        p0, p1 = gfx[base + y * 2], gfx[base + y * 2 + 1]
        p2, p3 = gfx[base + 16 + y * 2], gfx[base + 16 + y * 2 + 1]
        rows.append([((p0 >> (7 - x)) & 1) | ((p1 >> (7 - x)) & 1) << 1 |
                     ((p2 >> (7 - x)) & 1) << 2 | ((p3 >> (7 - x)) & 1) << 3
                     for x in range(8)])
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rom = open(args[0], "rb").read()
    level = int(args[1], 16)
    out = args[2]
    slots = [(0, 0), (8, 0), (0, 8), (8, 8)]

    rec = rd(rom, 0x357 + level * 3, 3) & 0x3FFFFF
    width, height = rd(rom, rec + 0x12, 2) // 2, rd(rom, rec + 0x14, 2)
    tiles, _ = decompress(rom, rd(rom, rec + 0x06, 3))
    cells, _ = decompress(rom, rd(rom, rec + 0x09, 3))
    pal_off = rd(rom, rec + 0x0C, 3) & 0x3FFFFF
    blocks_off = rd(rom, rec + 0x0F, 3) & 0x3FFFFF

    palette = []
    for i in range(256):
        c = rd(rom, pal_off + i * 2, 2)
        palette.append(((c & 31) << 3, (c >> 5 & 31) << 3, (c >> 10 & 31) << 3))

    cache = {}
    img = Image.new("RGB", (width * 16, height * 16), (255, 0, 255))
    px = img.load()
    for cy in range(height):
        for cx in range(width):
            cell = rd(cells, (cy * width + cx) * 2, 2) & 0xFFF8
            for slot, (ox, oy) in enumerate(slots):
                entry = rd(rom, blocks_off + cell + slot * 2, 2)
                tile_no = entry & 0x3FF
                pal = (entry >> 10) & 7
                hflip, vflip = entry & 0x4000, entry & 0x8000
                if tile_no not in cache:
                    cache[tile_no] = decode_tile(tiles, tile_no)
                tile = cache[tile_no]
                if tile is None:
                    continue
                for y in range(8):
                    sy = 7 - y if vflip else y
                    for x in range(8):
                        sx = 7 - x if hflip else x
                        c = tile[sy][sx]
                        px[cx * 16 + ox + x, cy * 16 + oy + y] = (
                            palette[pal * 16 + c] if c else (0, 0, 0))
    img.save(out)
    print(f"level ${level:02X}: {width}x{height} cells -> {img.size[0]}x{img.size[1]} px, {out}")


if __name__ == "__main__":
    main()
