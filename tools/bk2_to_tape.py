#!/usr/bin/env python3
"""Convert BizHawk .bk2 TAS movie files into raw SNES input tapes.

The emitted tape contains raw little-endian 16-bit words (u16), one per frame,
matching the standard SNES auto-joypad register ($4218) bit layout:

  Bit 15: B
  Bit 14: Y
  Bit 13: Select
  Bit 12: Start
  Bit 11: Up
  Bit 10: Down
  Bit  9: Left
  Bit  8: Right
  Bit  7: A
  Bit  6: X
  Bit  5: L
  Bit  4: R
  Bits 3..0: 0000

Memory layout for each 16-bit word:
  Low byte  ($4218): A, X, L, R, 0, 0, 0, 0   (bits 7..0)
  High byte ($4219): B, Y, Select, Start, Up, Down, Left, Right (bits 15..8)

Note on lag frames:
  BizHawk BK2 movie logs record inputs per emulated guest frame (every VBlank
  tick). The movie log does NOT filter out or flag lag frames (frames where the
  game engine skipped polling the controller registers). The headless host or
  recomp runner advances one frame per tick from the tape, mirroring BizHawk's
  frame-by-frame feed.

Reset / Power events:
  BizHawk records reset and power events in the first pipe segment (e.g. |r.|).
  Frames containing reset or power transitions are tracked and exported to a
  side file (<tapename>.resets.txt).
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import struct
import sys
from typing import Dict, List, NamedTuple, Optional, Tuple
import zipfile

# Standard SNES $4218 auto-joypad bit masks
BUTTON_MASKS: Dict[str, int] = {
    "B": 1 << 15,       # 0x8000
    "Y": 1 << 14,       # 0x4000
    "SELECT": 1 << 13,  # 0x2000
    "START": 1 << 12,   # 0x1000
    "UP": 1 << 11,      # 0x0800
    "DOWN": 1 << 10,    # 0x0400
    "LEFT": 1 << 9,     # 0x0200
    "RIGHT": 1 << 8,    # 0x0100
    "A": 1 << 7,        # 0x0080
    "X": 1 << 6,        # 0x0040
    "L": 1 << 5,        # 0x0020
    "R": 1 << 4,        # 0x0010
}


class ResetEvent(NamedTuple):
    frame: int          # 0-based frame index
    kind: str           # "RESET" or "POWER"
    raw_char: str       # character from log


class MovieData(NamedTuple):
    header: Dict[str, str]
    p1_words: List[int]
    p2_words: Optional[List[int]]
    reset_events: List[ResetEvent]
    log_key: str


def normalize_button_name(name: str) -> str:
    """Normalize a button name from LogKey (e.g. 'P1 Up', 'Joypad1 B' -> 'UP', 'B')."""
    cleaned = name.strip().upper()
    prefixes = (
        "P1 ", "P2 ", "P3 ", "P4 ",
        "PLAYER 1 ", "PLAYER 2 ", "PLAYER 3 ", "PLAYER 4 ",
        "JOYPAD 1 ", "JOYPAD 2 ", "JOYPAD 3 ", "JOYPAD 4 ",
    )
    for p in prefixes:
        if cleaned.startswith(p):
            cleaned = cleaned[len(p):]
            break
    return cleaned


def parse_log_key(log_key_str: str) -> List[List[str]]:
    """Parse a BizHawk LogKey line into groups of button names.

    Example LogKey:
      LogKey:#Reset|Power|#P1 Up|P1 Down|...|#P2 Up|...|
    Returns:
      [
        ['Reset', 'Power'],
        ['P1 Up', 'P1 Down', ...],
        ['P2 Up', ...]
      ]
    """
    if log_key_str.startswith("LogKey:"):
        log_key_str = log_key_str[len("LogKey:"):]

    raw_groups = [g for g in log_key_str.split("#") if g]
    groups: List[List[str]] = []
    for g in raw_groups:
        btns = [b for b in g.split("|") if b]
        groups.append(btns)
    return groups


def parse_bk2(bk2_path: str | Path) -> MovieData:
    """Read and parse a .bk2 archive into MovieData."""
    bk2_path = Path(bk2_path)
    if not bk2_path.is_file():
        raise FileNotFoundError(f"BK2 movie file not found: {bk2_path}")

    with zipfile.ZipFile(bk2_path, "r") as z:
        if "Header.txt" not in z.namelist():
            raise ValueError(f"Invalid BK2 archive: Header.txt missing in {bk2_path}")
        if "Input Log.txt" not in z.namelist():
            raise ValueError(f"Invalid BK2 archive: Input Log.txt missing in {bk2_path}")

        header_text = z.read("Header.txt").decode("utf-8", errors="replace")
        log_text = z.read("Input Log.txt").decode("utf-8", errors="replace")

    # Parse Header.txt
    header: Dict[str, str] = {}
    for line in header_text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(maxsplit=1)
        if len(parts) == 2:
            header[parts[0]] = parts[1]
        elif len(parts) == 1:
            header[parts[0]] = ""

    # Parse Input Log.txt
    lines = log_text.splitlines()
    log_key_line = ""
    for line in lines:
        if line.startswith("LogKey:"):
            log_key_line = line
            break

    if not log_key_line:
        raise ValueError(f"No LogKey found in Input Log.txt of {bk2_path}")

    groups = parse_log_key(log_key_line)
    if len(groups) < 2:
        raise ValueError(f"Malformed LogKey with fewer than 2 groups in {bk2_path}")

    # Group 0: System controls (Reset, Power)
    sys_group = [b.upper() for b in groups[0]]

    # Group 1: Joypad 1
    p1_group = [normalize_button_name(b) for b in groups[1]]

    # Group 2 (optional): Joypad 2
    has_p2 = len(groups) > 2
    p2_group = [normalize_button_name(b) for b in groups[2]] if has_p2 else []

    p1_words: List[int] = []
    p2_words: List[int] = [] if has_p2 else None
    reset_events: List[ResetEvent] = []

    frame_idx = 0
    in_input_section = False

    for line in lines:
        stripped = line.strip()
        if stripped == "[Input]":
            in_input_section = True
            continue
        if stripped == "[/Input]":
            in_input_section = False
            break

        if not stripped.startswith("|"):
            continue

        # Split segments delimited by '|'
        # e.g. '|..|............|' -> ['', '..', '............', '']
        parts = stripped.split("|")
        segs = parts[1:-1]
        if len(segs) < 2:
            continue

        # Check system controls (segment 0)
        sys_seg = segs[0]
        for idx, ch in enumerate(sys_seg):
            if ch not in (".", " "):
                name = sys_group[idx] if idx < len(sys_group) else f"SYS_{idx}"
                kind = "RESET" if "RESET" in name else ("POWER" if "POWER" in name else name)
                reset_events.append(ResetEvent(frame=frame_idx, kind=kind, raw_char=ch))

        # Joypad 1 (segment 1)
        p1_seg = segs[1]
        p1_word = 0
        for idx, ch in enumerate(p1_seg):
            if ch not in (".", " ") and idx < len(p1_group):
                btn = p1_group[idx]
                p1_word |= BUTTON_MASKS.get(btn, 0)
        p1_words.append(p1_word)

        # Joypad 2 (segment 2 if present)
        if has_p2 and len(segs) > 2 and p2_words is not None:
            p2_seg = segs[2]
            p2_word = 0
            for idx, ch in enumerate(p2_seg):
                if ch not in (".", " ") and idx < len(p2_group):
                    btn = p2_group[idx]
                    p2_word |= BUTTON_MASKS.get(btn, 0)
            p2_words.append(p2_word)

        frame_idx += 1

    return MovieData(
        header=header,
        p1_words=p1_words,
        p2_words=p2_words,
        reset_events=reset_events,
        log_key=log_key_line,
    )


def encode_tape(words: List[int]) -> bytes:
    """Pack a list of 16-bit integer words into little-endian u16 bytes."""
    return struct.pack(f"<{len(words)}H", *words)


def write_tape_file(path: str | Path, words: List[int]) -> int:
    """Write raw tape words to a file, returning total bytes written."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    raw = encode_tape(words)
    target.write_bytes(raw)
    return len(raw)


def write_resets_file(path: str | Path, events: List[ResetEvent], total_frames: int) -> None:
    """Write reset events side file."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    lines: List[str] = [
        f"# Reset/power events (total frames: {total_frames:,})",
        f"# Total events: {len(events)}",
        "# Format: <frame_0_based> <kind> <raw_char>",
    ]
    if not events:
        lines.append("# No reset or power events occurred in this movie.")
    else:
        for ev in events:
            lines.append(f"{ev.frame} {ev.kind} {ev.raw_char}")
    lines.append("")
    target.write_text("\n".join(lines), encoding="utf-8")


def read_lag_file(path: str | Path) -> set:
    """Lag frames from tools/bizhawk/lag_log.lua or ram_log.lua: lines "lag N", where N is
    emu.framecount() after the lagged frame ran (movie input row N-1)."""
    lag = set()
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "lag":
            lag.add(int(parts[1]))
    return lag


def read_values(path: str | Path) -> List[int]:
    """Pad words the game actually READ, from a tools/bizhawk/ram_log.lua RAM_OUT log: the $005A/$005B
    word (copied from $4218/$4219 by $C0A07C) on every non-lag frame. Unlike the movie rows this
    includes stale reads (a $4218 read in a vblank with no auto-joypad latch returns the previous
    latch, e.g. 6987M frame 942, the first poll after the level-1 load)."""
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        p = line.split()
        if len(p) == 9 and p[1] == "0":
            out.append(int(p[8], 16))
    return out


def polled_words(words: List[int], lag: set) -> List[int]:
    """Keep only rows of frames where the game read the pad (BizHawk non-lag frames), in order."""
    return [w for i, w in enumerate(words) if (i + 1) not in lag]


def convert_bk2(
    bk2_path: str | Path,
    out_p1_path: Optional[str | Path] = None,
    out_p2_path: Optional[str | Path] = None,
    out_resets_path: Optional[str | Path] = None,
    out_dir: Optional[str | Path] = None,
    lag_path: Optional[str | Path] = None,
    use_read_values: bool = False,
) -> MovieData:
    """Convert a .bk2 file and write the tape files and reset side file."""
    bk2_path = Path(bk2_path)
    movie = parse_bk2(bk2_path)
    stem = bk2_path.stem
    if lag_path is not None:
        lag = read_lag_file(lag_path) if not use_read_values else set()
        movie = movie._replace(
            p1_words=read_values(lag_path) if use_read_values else polled_words(movie.p1_words, lag),
            p2_words=None if movie.p2_words is None else polled_words(movie.p2_words, lag),
            reset_events=[],  # reset rows are frame-indexed; meaningless on a polled tape
        )
        stem = f"{stem}.polled"

    if out_dir is not None:
        dest_dir = Path(out_dir)
        p1_path = dest_dir / f"{stem}.tape"
        p2_path = dest_dir / f"{stem}_p2.tape" if movie.p2_words is not None else None
        resets_path = dest_dir / f"{stem}.resets.txt"
    else:
        p1_path = Path(out_p1_path) if out_p1_path else bk2_path.with_name(f"{stem}.tape")
        p2_path = Path(out_p2_path) if out_p2_path else (
            bk2_path.parent / f"{stem}_p2.tape" if movie.p2_words is not None else None
        )
        resets_path = Path(out_resets_path) if out_resets_path else bk2_path.with_name(f"{stem}.resets.txt")

    write_tape_file(p1_path, movie.p1_words)
    if movie.p2_words is not None and p2_path is not None:
        write_tape_file(p2_path, movie.p2_words)
    write_resets_file(resets_path, movie.reset_events, len(movie.p1_words))

    return movie


def format_duration(frames: int, fps: float = 60.0988) -> str:
    """Format frame count into MM:SS.ff at SNES NTSC rate."""
    total_seconds = frames / fps
    mins = int(total_seconds // 60)
    secs = total_seconds % 60
    return f"{mins:02d}:{secs:05.2f}"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert BizHawk .bk2 TAS movies to SNES raw input tapes."
    )
    parser.add_argument("inputs", nargs="+", help="Path(s) to .bk2 movie file(s)")
    parser.add_argument("--out-dir", "-d", help="Output directory for generated tapes")
    parser.add_argument("--output", "-o", help="Explicit path for Joypad 1 output tape")
    parser.add_argument("--p2-output", help="Explicit path for Joypad 2 output tape")
    parser.add_argument("--resets-output", help="Explicit path for resets side file")
    parser.add_argument("--polled", action="store_true",
                        help="Emit a polled tape (one word per game pad read); needs --lag")
    parser.add_argument("--lag", help="BizHawk lag log (tools/bizhawk/ram_log.lua LAG_OUT) for --polled; "
                        "with --read-values, the RAM_OUT log instead")
    parser.add_argument("--read-values", action="store_true",
                        help="--polled: take each poll's word from what the game read in BizHawk "
                             "($005A per non-lag frame of the --lag RAM log) instead of the movie row")
    parser.add_argument("--quiet", "-q", action="store_true", help="Suppress progress output")
    args = parser.parse_args(argv)

    if args.polled != bool(args.lag):
        parser.error("--polled and --lag go together")
    if args.polled and len(args.inputs) > 1:
        parser.error("--polled takes one movie (its own lag log)")
    if len(args.inputs) > 1 and args.output:
        parser.error("--output cannot be specified when converting multiple files; use --out-dir")

    for input_file in args.inputs:
        bk2_path = Path(input_file)
        movie = convert_bk2(
            bk2_path,
            out_p1_path=args.output,
            out_p2_path=args.p2_output,
            out_resets_path=args.resets_output,
            out_dir=args.out_dir,
            lag_path=args.lag,
            use_read_values=args.read_values,
        )

        if not args.quiet:
            stem = bk2_path.stem + (".polled" if args.polled else "")
            frames = len(movie.p1_words)
            tape_size = frames * 2
            core = movie.header.get("Core", "unknown")
            emu = movie.header.get("emuVersion", "unknown")
            sha1 = movie.header.get("SHA1", "none")
            rerecords = movie.header.get("rerecordCount", "unknown")

            out_dir_str = str(args.out_dir) if args.out_dir else str(bk2_path.parent)
            print(f"=== {stem}.bk2 ===")
            print(f"  Frames:     {frames:,} ({format_duration(frames)})")
            print(f"  Core / Emu: {core} ({emu})")
            print(f"  ROM SHA-1:  {sha1}")
            print(f"  Rerecords:  {rerecords}")
            print(f"  P1 Tape:    {out_dir_str}/{stem}.tape ({tape_size:,} bytes)")
            if movie.p2_words is not None:
                p2_size = len(movie.p2_words) * 2
                print(f"  P2 Tape:    {out_dir_str}/{stem}_p2.tape ({p2_size:,} bytes, joypad 2 in log)")
            print(f"  Resets:     {len(movie.reset_events)} events -> {out_dir_str}/{stem}.resets.txt")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
