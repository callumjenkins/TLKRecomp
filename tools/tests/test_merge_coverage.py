import json
from pathlib import Path
import pytest

from tools.merge_coverage import (
    CoverageTarget,
    IndirectTarget,
    merge_coverage,
    parse_coverage_log,
    parse_mx,
    parse_pc,
)


def test_parse_pc():
    assert parse_pc(0xC08000) == 0xC08000
    assert parse_pc("0xC08000") == 0xC08000
    assert parse_pc("C08000") == 0xC08000
    assert parse_pc("0xc08000") == 0xC08000


def test_parse_mx():
    assert parse_mx("M0X0") == (0, 0)
    assert parse_mx("M0X1") == (0, 1)
    assert parse_mx("M1X0") == (1, 0)
    assert parse_mx("M1X1") == (1, 1)
    assert parse_mx("0,1") == (0, 1)
    assert parse_mx((1, 0)) == (1, 0)


def test_parse_json_checkpoint(tmp_path):
    log_file = tmp_path / "capture.json"
    doc = {
        "schema": "snesrecomp tier2 coverage v2",
        "capture_id": "test_session",
        "discoveries": [
            {
                "site_pc24": "0xC08000",
                "target_pc24": "0xC08123",
                "entry_mx": "M0X1",
                "site_kind": "call_gap",
                "observed_hits": 5,
            },
            {
                "site_pc24": "0xC0FEBF",
                "target_pc24": "0xC09999",
                "entry_mx": "M0X0",
                "site_kind": "indirect_dispatch",
                "fallback_reason": "unresolved_indirect",
                "observed_hits": 2,
            },
        ],
    }
    log_file.write_text(json.dumps(doc), encoding="utf-8")

    targets, indirects = parse_coverage_log(log_file)
    assert len(targets) == 1
    assert targets[0].target_pc24 == 0xC08123
    assert targets[0].entry_m == 0
    assert targets[0].entry_x == 1

    assert len(indirects) == 1
    assert indirects[0].site_pc24 == 0xC0FEBF
    assert indirects[0].target_pc24 == 0xC09999
    assert indirects[0].dispatch_type == "ptrcall"


def test_parse_jsonl_journal(tmp_path):
    log_file = tmp_path / "capture.jsonl"
    line1 = {
        "schema": "snesrecomp tier2 discovery v2",
        "row": {
            "site_pc24": "0xC08000",
            "target_pc24": "0xC12345",
            "entry_mx": "M1X1",
            "site_kind": "call_gap",
            "observed_hits": 1,
        },
    }
    line2 = {
        "schema": "snesrecomp tier2 discovery v2",
        "row": {
            "site_pc24": "0xC21000",
            "target_pc24": "0xC22000",
            "site_kind": "indirect_goto",
            "observed_hits": 1,
        },
    }
    log_file.write_text(f"{json.dumps(line1)}\n{json.dumps(line2)}\n", encoding="utf-8")

    targets, indirects = parse_coverage_log(log_file)
    assert len(targets) == 1
    assert targets[0].target_pc24 == 0xC12345
    assert targets[0].entry_m == 1
    assert targets[0].entry_x == 1

    assert len(indirects) == 1
    assert indirects[0].site_pc24 == 0xC21000
    assert indirects[0].target_pc24 == 0xC22000
    assert indirects[0].dispatch_type == "ptrtail"


def test_parse_text_trace(tmp_path):
    log_file = tmp_path / "trace.txt"
    log_file.write_text(
        "FUNC C01111 0 1\n"
        "IND C09EA7 C084C3 0 0\n",
        encoding="utf-8",
    )
    targets, indirects = parse_coverage_log(log_file)
    assert len(targets) == 1
    assert targets[0].target_pc24 == 0xC01111
    assert targets[0].entry_m == 0
    assert targets[0].entry_x == 1

    assert len(indirects) == 1
    assert indirects[0].site_pc24 == 0xC09EA7
    assert indirects[0].target_pc24 == 0xC084C3


def test_merge_coverage_proposes_new_func(tmp_path):
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    bank_c0 = cfg_dir / "bankc0.cfg"
    bank_c0.write_text("bank = 0xC0\nfunc FN_C08007 8007 entry_mx:1,1\n", encoding="utf-8")

    targets = [CoverageTarget(target_pc24=0xC08500, entry_m=0, entry_x=1)]
    indirects = []

    diffs = merge_coverage(cfg_dir, targets, indirects)
    assert bank_c0 in diffs

    orig, mod = diffs[bank_c0]
    mod_str = "".join(mod)
    assert "func FN_C08500 8500 entry_mx:0,1" in mod_str


def test_merge_coverage_updates_existing_indirect_dispatch(tmp_path):
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    bank_c0 = cfg_dir / "bankc0.cfg"
    bank_c0.write_text(
        "bank = 0xC0\n"
        "indirect_dispatch FEBF 2 ptrcall return:FEC2 targets:C084C3,C09151\n",
        encoding="utf-8",
    )

    targets = []
    # Add a 3rd target to FEBF
    indirects = [
        IndirectTarget(site_pc24=0xC0FEBF, target_pc24=0xC09264, dispatch_type="ptrcall")
    ]

    diffs = merge_coverage(cfg_dir, targets, indirects)
    assert bank_c0 in diffs

    orig, mod = diffs[bank_c0]
    mod_str = "".join(mod)
    # Count should update to 3, and C09264 should be added in sorted order
    assert "indirect_dispatch FEBF 3 ptrcall return:FEC2 targets:C084C3,C09151,C09264" in mod_str


def test_merge_coverage_adds_new_indirect_dispatch(tmp_path):
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    bank_c1 = cfg_dir / "bankc1.cfg"
    bank_c1.write_text("bank = 0xC1\n", encoding="utf-8")

    indirects = [
        IndirectTarget(site_pc24=0xC19000, target_pc24=0xC1A000, dispatch_type="ptrcall")
    ]
    diffs = merge_coverage(cfg_dir, [], indirects)
    assert bank_c1 in diffs

    orig, mod = diffs[bank_c1]
    mod_str = "".join(mod)
    assert "indirect_dispatch 9000 1 ptrcall targets:C1A000" in mod_str


def test_merge_coverage_proposes_entry_mx_at(tmp_path):
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    bank_c0 = cfg_dir / "bankc0.cfg"
    bank_c0.write_text("bank = 0xC0\nfunc FN_C070F6 70F6\n", encoding="utf-8")

    targets = [CoverageTarget(target_pc24=0xC070F6, entry_m=0, entry_x=0)]
    diffs = merge_coverage(cfg_dir, targets, [], propose_entry_mx_at=True)
    assert bank_c0 in diffs

    orig, mod = diffs[bank_c0]
    mod_str = "".join(mod)
    assert "entry_mx_at 70F6 0 0" in mod_str


def test_merge_coverage_idempotency(tmp_path):
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    bank_c0 = cfg_dir / "bankc0.cfg"
    bank_c0.write_text("bank = 0xC0\n", encoding="utf-8")

    targets = [CoverageTarget(target_pc24=0xC08500, entry_m=0, entry_x=1)]
    indirects = [IndirectTarget(site_pc24=0xC0FEBF, target_pc24=0xC084C3, dispatch_type="ptrcall")]

    # First pass: generates diff
    diffs = merge_coverage(cfg_dir, targets, indirects)
    assert bank_c0 in diffs

    # Apply to disk
    bank_c0.write_text("".join(diffs[bank_c0][1]), encoding="utf-8")

    # Second pass: should generate no diff
    diffs2 = merge_coverage(cfg_dir, targets, indirects)
    assert len(diffs2) == 0


def _row(**kw):
    base = {"site_pc24": "0xC08000", "entry_mx": "M0X0", "emulation": 0,
            "site_kind": "call_gap", "fallback_reason": "x", "observed_hits": 1,
            "completed_hits": 0, "yielded_hits": 0, "pending_hits": 0,
            "bail_hits": 0, "sequence": 1, "first_frame": 1, "last_frame": 1}
    base.update(kw)
    return base


def test_gating_and_hirom_mirror_fold(tmp_path):
    from tools import merge_coverage as mc
    mc.SKIPPED.clear()
    log = tmp_path / "c.json"
    log.write_text(json.dumps({"discoveries": [
        _row(target_pc24="0x401234"),                      # mirror -> C01234
        _row(target_pc24="0x008800"),                      # mirror -> C08800
        _row(target_pc24="0xC05555", bail_hits=1),
        _row(target_pc24="0xC06666", emulation=1),
        _row(target_pc24="0xC07777", site_kind="goto_gap"),
        _row(target_pc24="0x7E2000"),
        _row(target_pc24="0xC08888", entry_mx="unknown"),
    ]}))
    targets, indirects = mc.parse_coverage_log(log)
    assert sorted(t.target_pc24 for t in targets) == [0xC01234, 0xC08800]
    reasons = sorted(r for _pc, r in mc.SKIPPED)
    assert len(reasons) == 5
    assert "bailed_requires_investigation" in reasons
    assert "landing_requires_function_boundary" in reasons
    assert "missing_entry_width" in reasons


def test_main_uses_upstream_loader_for_real_v2_capture(tmp_path, capsys):
    from tools import merge_coverage as mc
    mc.SKIPPED.clear()
    cfg = tmp_path / "recomp"
    cfg.mkdir()
    (cfg / "bankc0.cfg").write_text("bank = 0xC0\nfunc FN_C08000 8000\n")
    ident = {"rom_sha256": "a" * 64, "module_id": "tlk", "mapper": "hirom"}
    cp = tmp_path / "s.json"
    cp.write_text(json.dumps({
        "schema": "snesrecomp tier2 coverage v2", "capture_id": "s1", "identity": ident,
        "sequence": 2, "checkpoint_complete": True,
        "discoveries": [_row(target_pc24="0xC0A000", sequence=2, observed_hits=4)]}))
    jl = tmp_path / "s.jsonl"
    jl.write_text(json.dumps({"schema": "snesrecomp tier2 discovery v2", "capture_id": "s1",
                              "identity": ident, "sequence": 1,
                              "row": _row(target_pc24="0xC0A000", sequence=1)}) + "\n")
    assert mc.main([str(cp), str(jl), "--cfg-dir", str(cfg)]) == 0
    out = capsys.readouterr().out
    assert "via snesrecomp coverage_profile.load_profiles" in out
    assert "+func FN_C0A000 A000 entry_mx:0,0" in out
    assert "A000" not in (cfg / "bankc0.cfg").read_text()  # dry run
    assert mc.main([str(cp), str(jl), "--cfg-dir", str(cfg), "--apply"]) == 0
    assert "func FN_C0A000 A000 entry_mx:0,0" in (cfg / "bankc0.cfg").read_text()


def test_text_trace_folds_mirrors(tmp_path):
    log = tmp_path / "t.txt"
    log.write_text("FUNC 41ABCD 1 1\nIND 82FEBF 40FFF0 0 0\nFUNC 7E1000 0 0\n")
    targets, indirects = parse_coverage_log(log)
    assert [t.target_pc24 for t in targets] == [0xC1ABCD]
    assert (indirects[0].site_pc24, indirects[0].target_pc24) == (0xC2FEBF, 0xC0FFF0)


def test_parse_headless_log_nested_tier2(tmp_path):
    # tlk_headless --log writes the tier-2 table under "tier2_coverage", not at top level.
    log_file = tmp_path / "run.json"
    doc = {
        "title": "tlk",
        "frames": 10,
        "interp_pc_runs": ["C00096-C000B3"],
        "tier2_coverage": {
            "distinct_sites": 1,
            "discoveries": [
                {
                    "site_pc24": "0xC0828D",
                    "target_pc24": "0xC070F6",
                    "entry_mx": "M0X0",
                    "emulation": 0,
                    "site_kind": "call_gap",
                    "fallback_reason": "missing_exact_variant",
                    "observed_hits": 10,
                }
            ],
        },
        "_end": True,
    }
    log_file.write_text(json.dumps(doc), encoding="utf-8")
    targets, indirects = parse_coverage_log(log_file)
    assert [(t.target_pc24, t.entry_m, t.entry_x) for t in targets] == [(0xC070F6, 0, 0)]
    assert indirects == []
