"""The Lion King (SNES) decompressor, reimplemented from $C1CCFE.

Command bytes (b):
  $00-$7F  short back-reference: len = (b >> 4) + 3,
           dist = ((b & $0F) << 8) | next byte; copy from out[-dist].
  $80      end of stream.
  $81-$BF  literal: copy (b & $3F) bytes from the stream.
  $C0-$FD  long back-reference: len = (b & $3F) + 3, then a 16-bit
           absolute offset into the output buffer.
  $FF      as $C0-$FD, but len is a 16-bit word before the offset.
  $FE      fill: 16-bit count, then one byte value.

Usage: python3 -I decompress.py ROM ADDR [OUT_FILE]
"""
import sys


def decompress(rom, addr):
    """Return (output bytes, compressed size) for the stream at SNES address."""
    src = addr & 0x3FFFFF
    start = src
    out = bytearray()
    while True:
        b = rom[src]
        if b < 0x80:
            length = (b >> 4) + 3
            dist = ((b & 0x0F) << 8) | rom[src + 1]
            src += 2
            pos = len(out) - dist
            if pos < 0:
                raise ValueError(f"back-reference before start at ${addr:06X}+{src - start:#x}")
            for i in range(length):  # byte-wise: overlapping copies repeat
                out.append(out[pos + i])
            continue
        src += 1
        if b == 0x80:
            break
        n = b & 0x3F
        if b < 0xC0:
            out += rom[src:src + n]
            src += n
        elif n == 0x3E:
            count = rom[src] | rom[src + 1] << 8
            out += bytes([rom[src + 2]]) * count
            src += 3
        else:
            length = n + 3
            if length == 0x42:
                length = rom[src] | rom[src + 1] << 8
                src += 2
            pos = rom[src] | rom[src + 1] << 8
            src += 2
            if pos >= len(out):
                raise ValueError(f"long back-reference past end at ${addr:06X}+{src - start:#x}")
            for i in range(length):  # MVN copies forward, byte by byte
                out.append(out[pos + i])
    return bytes(out), src - start


if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read()
    addr = int(sys.argv[2].lstrip("$"), 16)
    data, used = decompress(rom, addr)
    print(f"${addr:06X}: {used} bytes -> {len(data)} bytes")
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(data)
