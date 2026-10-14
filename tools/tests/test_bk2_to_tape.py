import io
import os
from pathlib import Path
import struct
import zipfile
import pytest

from tools.bk2_to_tape import (
    BUTTON_MASKS,
    MovieData,
    ResetEvent,
    convert_bk2,
    encode_tape,
    normalize_button_name,
    parse_bk2,
    parse_log_key,
)

TAS_DIR = Path(os.environ.get("TLK_TAS_DIR", Path(__file__).resolve().parents[3] / "tas"))


def test_button_masks():
    assert BUTTON_MASKS["B"] == 0x8000
    assert BUTTON_MASKS["Y"] == 0x4000
    assert BUTTON_MASKS["SELECT"] == 0x2000
    assert BUTTON_MASKS["START"] == 0x1000
    assert BUTTON_MASKS["UP"] == 0x0800
    assert BUTTON_MASKS["DOWN"] == 0x0400
    assert BUTTON_MASKS["LEFT"] == 0x0200
    assert BUTTON_MASKS["RIGHT"] == 0x0100
    assert BUTTON_MASKS["A"] == 0x0080
    assert BUTTON_MASKS["X"] == 0x0040
    assert BUTTON_MASKS["L"] == 0x0020
    assert BUTTON_MASKS["R"] == 0x0010


def test_encode_tape():
    # Bit 15 (B) -> 0x8000. In little-endian u16: low byte 0x00 ($4218), high byte 0x80 ($4219)
    raw = encode_tape([0x8000])
    assert raw == b"\x00\x80"

    # Bit 9 (Left) -> 0x0200. Little-endian: \x00\x02
    raw = encode_tape([0x0200])
    assert raw == b"\x00\x02"

    # B + Right + R: 0x8000 | 0x0100 | 0x0010 = 0x8110. Little-endian: \x10\x81
    raw = encode_tape([0x8110])
    assert raw == b"\x10\x81"


def test_normalize_button_name():
    assert normalize_button_name("P1 Up") == "UP"
    assert normalize_button_name("P2 Select") == "SELECT"
    assert normalize_button_name("Joypad 1 B") == "B"
    assert normalize_button_name("Player 2 X") == "X"
    assert normalize_button_name("Start") == "START"


def test_parse_log_key():
    key = "LogKey:#Reset|Power|#P1 Up|P1 Down|P1 Left|P1 Right|#P2 Up|P2 Down|"
    groups = parse_log_key(key)
    assert len(groups) == 3
    assert groups[0] == ["Reset", "Power"]
    assert groups[1] == ["P1 Up", "P1 Down", "P1 Left", "P1 Right"]
    assert groups[2] == ["P2 Up", "P2 Down"]


def create_mock_bk2(tmp_path, log_content, header_content="Platform SNES\nCore BSNES\n"):
    zip_path = tmp_path / "mock.bk2"
    with zipfile.ZipFile(zip_path, "w") as z:
        z.writestr("Header.txt", header_content)
        z.writestr("Input Log.txt", log_content)
    return zip_path


def test_parse_bk2_bizhawk2_format(tmp_path):
    log = """[Input]
LogKey:#Reset|Power|#P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Y|P1 B|P1 X|P1 A|P1 L|P1 R|
|..|............|
|..|U...........|
|..|....S.......|
|..|.......B....|
|..|.......BA...|
|r.|............|
[/Input]
"""
    bk2 = create_mock_bk2(tmp_path, log)
    movie = parse_bk2(bk2)

    assert len(movie.p1_words) == 6
    assert movie.p2_words is None
    assert movie.p1_words[0] == 0
    assert movie.p1_words[1] == BUTTON_MASKS["UP"]
    assert movie.p1_words[2] == BUTTON_MASKS["SELECT"]
    assert movie.p1_words[3] == BUTTON_MASKS["B"]
    assert movie.p1_words[4] == BUTTON_MASKS["B"] | BUTTON_MASKS["X"]
    assert movie.p1_words[5] == 0

    assert len(movie.reset_events) == 1
    assert movie.reset_events[0].frame == 5
    assert movie.reset_events[0].kind == "RESET"
    assert movie.reset_events[0].raw_char == "r"


def test_parse_bk2_bizhawk1_format_with_p2(tmp_path):
    # Old format from BizHawk 1.7.4: no [Input], ends with blank line, P2 included
    log = """LogKey:#Reset|Power|#P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Y|P1 B|P1 X|P1 A|P1 L|P1 R|#P2 Up|P2 Down|P2 Left|P2 Right|P2 Select|P2 Start|P2 Y|P2 B|P2 X|P2 A|P2 L|P2 R|
|..|............|............|
|..|U...........|...R........|
|..|.......B....|.......B....|

"""
    bk2 = create_mock_bk2(tmp_path, log, header_content="emuVersion Version 1.7.4\nCore BSNES\n")
    movie = parse_bk2(bk2)

    assert len(movie.p1_words) == 3
    assert movie.p2_words is not None
    assert len(movie.p2_words) == 3

    assert movie.p1_words[0] == 0
    assert movie.p1_words[1] == BUTTON_MASKS["UP"]
    assert movie.p1_words[2] == BUTTON_MASKS["B"]

    assert movie.p2_words[0] == 0
    assert movie.p2_words[1] == BUTTON_MASKS["RIGHT"]
    assert movie.p2_words[2] == BUTTON_MASKS["B"]


def test_convert_bk2_writes_files(tmp_path):
    log = """[Input]
LogKey:#Reset|Power|#P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Y|P1 B|P1 X|P1 A|P1 L|P1 R|
|..|U...........|
|..|............|
[/Input]
"""
    bk2 = create_mock_bk2(tmp_path, log)
    out_dir = tmp_path / "tapes"
    movie = convert_bk2(bk2, out_dir=out_dir)

    p1_tape = out_dir / "mock.tape"
    resets_txt = out_dir / "mock.resets.txt"

    assert p1_tape.is_file()
    assert resets_txt.is_file()
    assert p1_tape.stat().st_size == 4  # 2 frames * 2 bytes = 4 bytes


@pytest.mark.skipif(not TAS_DIR.exists(), reason="TAS directory not available")
@pytest.mark.parametrize(
    "filename,expected_frames,has_p2",
    [
        ("6987M.bk2", 39088, False),
        ("6781M.bk2", 39270, False),
        ("4735M.bk2", 41047, False),
        ("2716M.bk2", 46348, True),
    ],
)
def test_real_tas_movies(filename, expected_frames, has_p2):
    bk2_path = TAS_DIR / filename
    movie = parse_bk2(bk2_path)
    assert len(movie.p1_words) == expected_frames
    if has_p2:
        assert movie.p2_words is not None
        assert len(movie.p2_words) == expected_frames
    else:
        assert movie.p2_words is None
    assert len(movie.reset_events) == 0


def test_polled_words_drops_lag_rows(tmp_path):
    from tools.bk2_to_tape import polled_words, read_lag_file
    lagf = tmp_path / "lag.txt"
    lagf.write_text("lag 1\nlag 3\nlagcount 2\n")
    lag = read_lag_file(lagf)
    assert lag == {1, 3}
    # rows 0 and 2 are frames 1 and 3 (lagged)
    assert polled_words([10, 11, 12, 13], lag) == [11, 13]


def test_read_values_takes_pad_of_nonlag_frames(tmp_path):
    from tools.bk2_to_tape import read_values
    log = tmp_path / "ram.txt"
    log.write_text("1 1 000000 0000 0000 0000 0000 0000 0000\n"
                   "2 0 000000 0000 0000 0000 0000 0000 0100\n"
                   "3 0 000000 0000 0000 0000 0000 0000 8100\n")
    assert read_values(log) == [0x0100, 0x8100]
