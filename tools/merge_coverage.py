#!/usr/bin/env python3
"""Merge runtime coverage and LLE fallback logs into recomp bank configurations.

Reads snesrecomp coverage profiles (v2 checkpoints, v2 JSONL journals, v1 JSON),
or raw trace/fallback logs, and proposes:
  1. New `func <name> <pc16> entry_mx:M,X` lines for unmapped call targets.
  2. `entry_mx_at <pc16> <m> <x>` overrides for functions needing entry width annotations.
  3. `indirect_dispatch <pc16> <count> ptrcall|ptrtail targets:<pc24>,...`
     updates or new directives for resolved indirect jump/call targets.

Syntax adheres to HANDOFF.md and snesrecomp/recompiler/v2/cfg_loader.py:
  bank = 0xC0
  func <name> <hex_pc16> [end:<hex>] [entry_mx:M,X] [exit_mx:M,X] [tail_call:<hex>]
  entry_mx_at <pc16> <m> <x>
  exit_mx_at <pc24> <m> <x>
  indirect_dispatch <pc16> <count> ptrcall|ptrtail targets:<pc24>,<pc24>,...
  data_region <bank> <start16> <end16>

Behavior:
  - By default, prints a unified diff of proposed changes and exits (dry-run).
  - Only writes changes to bank*.cfg files when `--apply` is passed.
"""

from __future__ import annotations

import argparse
import difflib
import json
from pathlib import Path
import re
import sys
from typing import Any, Dict, List, NamedTuple, Optional, Set, Tuple


class CoverageTarget(NamedTuple):
    target_pc24: int
    entry_m: int
    entry_x: int
    site_pc24: Optional[int] = None
    site_kind: str = "call_gap"
    fallback_reason: str = "unknown"
    hits: int = 1


class IndirectTarget(NamedTuple):
    site_pc24: int
    target_pc24: int
    dispatch_type: str  # 'ptrcall' or 'ptrtail'
    hits: int = 1


def parse_pc(val: Any) -> int:
    """Parse a PC value from int or hex string."""
    if isinstance(val, int):
        return val & 0xFFFFFF
    s = str(val).strip()
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16) & 0xFFFFFF
    try:
        return int(s, 16) & 0xFFFFFF
    except ValueError:
        return int(s) & 0xFFFFFF


def parse_mx(val: Any) -> Tuple[int, int]:
    """Parse entry_mx into (m, x) ints, e.g. 'M0X1' -> (0, 1) or '0,1' -> (0, 1)."""
    if isinstance(val, (tuple, list)) and len(val) == 2:
        return int(val[0]) & 1, int(val[1]) & 1
    s = str(val).strip().upper()
    m_match = re.search(r"M([01])", s)
    x_match = re.search(r"X([01])", s)
    if m_match and x_match:
        return int(m_match.group(1)), int(x_match.group(1))
    parts = s.split(",")
    if len(parts) == 2:
        try:
            return int(parts[0].strip()) & 1, int(parts[1].strip()) & 1
        except ValueError:
            pass
    # Default to 0, 0 if unparseable
    return 0, 0


def load_with_upstream(paths: List[str]) -> Optional[Tuple[List[CoverageTarget], List[IndirectTarget], List[str]]]:
    """Load real v1/v2 captures through snesrecomp's own coverage_profile loader.

    This gives sequence de-duplication across checkpoint+journal, identity
    checks and counter merging exactly as tier2_ingest.py/v2_emit.py see them.
    Returns None when upstream rejects the input (e.g. synthetic logs without
    identity), so the caller can fall back to the lenient local parser.
    """
    root = Path(__file__).resolve().parent.parent / "snesrecomp" / "recompiler"
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    try:
        from v2.coverage_profile import load_profiles  # type: ignore
    except ImportError:
        return None
    try:
        profile = load_profiles(paths)
    except (ValueError, KeyError):
        return None
    targets: List[CoverageTarget] = []
    indirects: List[IndirectTarget] = []
    for row in profile.discoveries:
        _extract_row(row, targets, indirects)
    return targets, indirects, list(profile.warnings)


def parse_coverage_log(log_path: str | Path) -> Tuple[List[CoverageTarget], List[IndirectTarget]]:
    """Parse a coverage log file (JSON, JSONL, or text trace)."""
    log_path = Path(log_path)
    if not log_path.is_file():
        raise FileNotFoundError(f"Coverage log not found: {log_path}")

    targets: List[CoverageTarget] = []
    indirects: List[IndirectTarget] = []

    # Try reading as text
    text = log_path.read_text(encoding="utf-8", errors="replace").strip()
    if not text:
        return targets, indirects

    # 1. Try parsing as JSON checkpoint
    if text.startswith("{") or text.startswith("["):
        try:
            data = json.loads(text)
            records = data if isinstance(data, list) else [data]
            for rec in records:
                if not isinstance(rec, dict):
                    continue
                # Handle snesrecomp bundle or checkpoint
                discoveries = list(rec.get("discoveries", []))
                # tlk_headless --log JSON nests the tier-2 table under "tier2_coverage"
                nested = rec.get("tier2_coverage")
                if isinstance(nested, dict):
                    discoveries.extend(nested.get("discoveries", []))
                if "row" in rec and isinstance(rec["row"], dict):
                    discoveries.append(rec["row"])
                for row in discoveries:
                    _extract_row(row, targets, indirects)
            return targets, indirects
        except json.JSONDecodeError:
            pass

    # 2. Try parsing line-by-line (JSONL or text trace)
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        # Try JSONL line
        if line.startswith("{"):
            try:
                rec = json.loads(line)
                row = rec.get("row", rec)
                _extract_row(row, targets, indirects)
                continue
            except json.JSONDecodeError:
                pass

        # Text trace format:
        # FUNC <pc24> <m> <x>
        # IND <caller_pc24> <target_pc24> <m> <x>
        parts = line.split()
        tag = parts[0].upper()
        if tag == "FUNC" and len(parts) >= 4:
            tgt = canonical_hirom(parse_pc(parts[1]))
            if tgt is None:
                SKIPPED.append((parse_pc(parts[1]), "not_hirom_rom (RAM/IO needs snapshot+guard)"))
                continue
            m, x = int(parts[2]) & 1, int(parts[3]) & 1
            targets.append(CoverageTarget(target_pc24=tgt, entry_m=m, entry_x=x))
        elif tag == "IND" and len(parts) >= 3:
            caller = canonical_hirom(parse_pc(parts[1]))
            tgt = canonical_hirom(parse_pc(parts[2]))
            if caller is None or tgt is None:
                SKIPPED.append((parse_pc(parts[2]), "not_hirom_rom (RAM/IO needs snapshot+guard)"))
                continue
            indirects.append(IndirectTarget(site_pc24=caller, target_pc24=tgt, dispatch_type="ptrcall"))
        elif "FALLBACK" in tag or "UNRESOLVED" in tag:
            # Pattern: fallback at 0xC08000 -> 0xC08100
            pcs = re.findall(r"(?:0x|0X)?([0-9a-fA-F]{6})", line)
            if len(pcs) >= 2:
                caller = int(pcs[0], 16)
                tgt = int(pcs[1], 16)
                indirects.append(IndirectTarget(site_pc24=caller, target_pc24=tgt, dispatch_type="ptrcall"))

    return targets, indirects


SKIPPED: List[Tuple[int, str]] = []  # (target_pc24, reason) for rows not promoted


def canonical_hirom(pc24: int) -> Optional[int]:
    """Fold HiROM mirrors onto the $C0-$FF bank the cfg files use.

    $40-$7D and $00-$3F/$80-$BF:8000+ mirror $C0-$FF. Returns None for WRAM
    ($7E/$7F), low-bank non-ROM addresses and anything else that is not ROM.
    """
    bank, addr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
    if bank >= 0xC0:
        return pc24
    if 0x40 <= bank <= 0x7D:
        return pc24 | 0x800000
    if (bank <= 0x3F or 0x80 <= bank <= 0xBF) and addr >= 0x8000:
        return ((bank & 0x3F) | 0xC0) << 16 | addr
    return None


def _gate(row: Dict[str, Any], site_kind: str) -> Optional[str]:
    """Mirror snesrecomp coverage_profile.promotion_reason; None means promotable."""
    if int(row.get("bail_hits", 0) or 0):
        return "bailed_requires_investigation"
    emu = row.get("emulation")
    if emu not in (0, None):
        return "emulation_mode_requires_analysis_support"
    if not (int(row.get("observed_hits", 0) or 0) or int(row.get("completed_hits", 0) or 0)
            or row.get("_synthetic")):
        return "no_execution_evidence"
    if site_kind in ("goto_gap", "bank_miss"):
        return "landing_requires_function_boundary"
    return None


def _extract_row(row: Dict[str, Any], targets: List[CoverageTarget], indirects: List[IndirectTarget]) -> None:
    """Helper to extract CoverageTarget or IndirectTarget from a dictionary."""
    if not isinstance(row, dict):
        return
    if row.get("record_kind") == "instruction":
        return

    tgt_raw = row.get("target_pc24")
    if tgt_raw is None:
        return
    tgt = canonical_hirom(parse_pc(tgt_raw))
    if tgt is None:
        SKIPPED.append((parse_pc(tgt_raw), "not_hirom_rom (RAM/IO needs snapshot+guard)"))
        return
    site_raw = row.get("site_pc24")
    site = canonical_hirom(parse_pc(site_raw)) if site_raw is not None else None

    site_kind = str(row.get("site_kind", "unknown")).lower()
    fallback = str(row.get("fallback_reason", "unknown")).lower()
    hits = int(row.get("observed_hits", 0)) + int(row.get("completed_hits", 0)) or 1
    reason = _gate(row, site_kind)
    if reason is not None:
        SKIPPED.append((tgt, reason))
        return

    # Check if this is an indirect dispatch / jump fallback
    if (
        "indirect" in site_kind
        or "indirect" in fallback
        or site_kind in ("indirect_dispatch", "indirect_goto", "ptrcall", "ptrtail")
    ):
        if site is not None:
            dtype = "ptrtail" if "goto" in site_kind or "tail" in site_kind else "ptrcall"
            indirects.append(IndirectTarget(site_pc24=site, target_pc24=tgt, dispatch_type=dtype, hits=hits))
            return

    # Regular function entry (call_gap): needs an exact entry width
    if not re.fullmatch(r"M[01]X[01]", str(row.get("entry_mx", "")).upper()):
        SKIPPED.append((tgt, "missing_entry_width"))
        return
    m, x = parse_mx(row["entry_mx"])
    targets.append(
        CoverageTarget(
            target_pc24=tgt,
            entry_m=m,
            entry_x=x,
            site_pc24=site,
            site_kind=site_kind,
            fallback_reason=fallback,
            hits=hits,
        )
    )


class BankConfig:
    """Model of a single bank*.cfg file."""

    def __init__(self, path: Path, bank: int):
        self.path = path
        self.bank = bank
        self.lines: List[str] = []
        self.existing_funcs: Dict[int, str] = {}  # pc16 -> full line
        self.existing_entry_mx: Dict[int, Tuple[int, int]] = {}  # pc16 -> (m, x)
        self.existing_indirect: Dict[int, Tuple[int, str]] = {}  # site_pc16 -> (line_idx, line)
        self.load()

    def load(self) -> None:
        if not self.path.exists():
            self.lines = [f"bank = 0x{self.bank:02X}\n"]
            return

        self.lines = self.path.read_text(encoding="utf-8").splitlines(keepends=True)
        func_re = re.compile(r"^\s*func\s+(\S+)\s+([0-9a-fA-F]{4})\b")
        entry_mx_re = re.compile(r"\bentry_mx:([01]),([01])\b")
        entry_at_re = re.compile(r"^\s*entry_mx_at\s+([0-9a-fA-F]{4})\s+([01])\s+([01])\b")
        ind_re = re.compile(r"^\s*indirect_dispatch\s+([0-9a-fA-F]{4})\b")

        for idx, line in enumerate(self.lines):
            fm = func_re.match(line)
            if fm:
                pc16 = int(fm.group(2), 16)
                self.existing_funcs[pc16] = line.strip()
                mx_match = entry_mx_re.search(line)
                if mx_match:
                    self.existing_entry_mx[pc16] = (int(mx_match.group(1)), int(mx_match.group(2)))

            em = entry_at_re.match(line)
            if em:
                pc16 = int(em.group(1), 16)
                self.existing_entry_mx[pc16] = (int(em.group(2)), int(em.group(3)))

            im = ind_re.match(line)
            if im:
                site_pc16 = int(im.group(1), 16)
                self.existing_indirect[site_pc16] = (idx, line)


def merge_coverage(
    cfg_dir: str | Path,
    targets: List[CoverageTarget],
    indirects: List[IndirectTarget],
    propose_entry_mx_at: bool = False,
) -> Dict[Path, Tuple[List[str], List[str]]]:
    """Calculate proposed changes across bank*.cfg files.

    Returns dict mapping Path -> (original_lines, new_lines).
    """
    cfg_dir = Path(cfg_dir)
    cfg_dir.mkdir(parents=True, exist_ok=True)

    # Group discoveries by bank
    bank_funcs: Dict[int, Dict[int, Tuple[int, int]]] = {}
    for t in targets:
        b = (t.target_pc24 >> 16) & 0xFF
        pc16 = t.target_pc24 & 0xFFFF
        bank_funcs.setdefault(b, {})[pc16] = (t.entry_m, t.entry_x)

    bank_indirects: Dict[int, Dict[int, Tuple[str, Set[int]]]] = {}
    for ind in indirects:
        b = (ind.site_pc24 >> 16) & 0xFF
        site16 = ind.site_pc24 & 0xFFFF
        if b not in bank_indirects:
            bank_indirects[b] = {}
        if site16 not in bank_indirects[b]:
            bank_indirects[b][site16] = (ind.dispatch_type, set())
        bank_indirects[b][site16][1].add(ind.target_pc24)

    all_banks = sorted(set(bank_funcs.keys()) | set(bank_indirects.keys()))
    diff_results: Dict[Path, Tuple[List[str], List[str]]] = {}

    for b in all_banks:
        cfg_path = cfg_dir / f"bank{b:02x}.cfg"
        bcfg = BankConfig(cfg_path, b)
        orig_lines = list(bcfg.lines)
        modified_lines = list(bcfg.lines)

        new_funcs: List[str] = []
        new_entry_at: List[str] = []

        # 1. Process function entries
        for pc16, (m, x) in sorted(bank_funcs.get(b, {}).items()):
            if pc16 not in bcfg.existing_funcs:
                func_name = f"FN_{b:02X}{pc16:04X}"
                new_funcs.append(f"func {func_name} {pc16:04X} entry_mx:{m},{x}\n")
                bcfg.existing_funcs[pc16] = f"func {func_name} {pc16:04X}"
                bcfg.existing_entry_mx[pc16] = (m, x)
            else:
                # Function exists. Check if entry_mx is missing
                if pc16 not in bcfg.existing_entry_mx:
                    if propose_entry_mx_at:
                        new_entry_at.append(f"entry_mx_at {pc16:04X} {m} {x}\n")
                        bcfg.existing_entry_mx[pc16] = (m, x)
                    else:
                        # Append entry_mx:M,X to existing func line if possible
                        for i, line in enumerate(modified_lines):
                            m_match = re.match(rf"^(\s*func\s+\S+\s+{pc16:04X}\b)(.*)$", line, re.IGNORECASE)
                            if m_match and "entry_mx:" not in line:
                                modified_lines[i] = f"{m_match.group(1)} entry_mx:{m},{x}{m_match.group(2)}\n"
                                bcfg.existing_entry_mx[pc16] = (m, x)
                                break

        # 2. Process indirect dispatches
        new_indirects: List[str] = []
        for site16, (dtype, new_tgts) in sorted(bank_indirects.get(b, {}).items()):
            if site16 in bcfg.existing_indirect:
                line_idx, existing_line = bcfg.existing_indirect[site16]
                # Parse existing targets
                targets_match = re.search(r"\btargets:([0-9a-fA-F,]+)", existing_line)
                if targets_match:
                    raw_tgts = targets_match.group(1).split(",")
                    parsed_tgts = {parse_pc(t) for t in raw_tgts if t}
                    combined = parsed_tgts | new_tgts
                    if combined != parsed_tgts:
                        sorted_tgts = sorted(combined)
                        tgts_str = ",".join(f"{t:06X}" for t in sorted_tgts)
                        # Replace count and targets in existing line
                        updated = re.sub(
                            r"(\bindirect_dispatch\s+[0-9a-fA-F]{4}\s+)\d+",
                            rf"\g<1>{len(sorted_tgts)}",
                            existing_line,
                        )
                        updated = re.sub(r"\btargets:[0-9a-fA-F,]+", f"targets:{tgts_str}", updated)
                        modified_lines[line_idx] = updated
                        bcfg.existing_indirect[site16] = (line_idx, updated)
            else:
                sorted_tgts = sorted(new_tgts)
                tgts_str = ",".join(f"{t:06X}" for t in sorted_tgts)
                new_indirects.append(
                    f"indirect_dispatch {site16:04X} {len(sorted_tgts)} {dtype} targets:{tgts_str}\n"
                )

        # Append new additions to modified_lines
        append_block: List[str] = []
        if new_funcs:
            append_block.extend(new_funcs)
        if new_entry_at:
            append_block.extend(new_entry_at)
        if new_indirects:
            append_block.extend(new_indirects)

        if append_block:
            if modified_lines and not modified_lines[-1].endswith("\n"):
                modified_lines[-1] += "\n"
            modified_lines.extend(append_block)

        if modified_lines != orig_lines:
            diff_results[cfg_path] = (orig_lines, modified_lines)

    return diff_results


def print_diff(diff_results: Dict[Path, Tuple[List[str], List[str]]]) -> int:
    """Print unified diffs for modified configs. Returns total added lines."""
    total_added = 0
    for path, (orig, mod) in sorted(diff_results.items()):
        diff = list(
            difflib.unified_diff(
                orig,
                mod,
                fromfile=f"a/{path.name}",
                tofile=f"b/{path.name}",
                lineterm="",
            )
        )
        if diff:
            print(f"\n--- {path} ---")
            for line in diff:
                print(line.rstrip("\n"))
                if line.startswith("+") and not line.startswith("+++"):
                    total_added += 1
    return total_added


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Merge coverage/LLE fallback logs into recomp/*.cfg bank files."
    )
    parser.add_argument("logs", nargs="+", help="Path(s) to coverage logs (.json, .jsonl, .txt)")
    parser.add_argument(
        "--cfg-dir",
        default="recomp",
        help="Path to directory containing bank*.cfg files (default: recomp/)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Apply proposed changes to disk (default is dry-run diff)",
    )
    parser.add_argument(
        "--entry-mx-at",
        action="store_true",
        help="Propose standalone `entry_mx_at` directives rather than in-place `entry_mx:` updates",
    )
    parser.add_argument(
        "--min-hits",
        type=int,
        default=1,
        help="Minimum hits required to promote a target (default: 1)",
    )
    args = parser.parse_args(argv)

    cfg_dir = Path(args.cfg_dir)
    # Check fallback relative to TLKRecomp if not found
    if not cfg_dir.exists():
        alt = Path(__file__).resolve().parent.parent / "recomp"
        if alt.exists():
            cfg_dir = alt

    all_targets: List[CoverageTarget] = []
    all_indirects: List[IndirectTarget] = []

    json_logs = [p for p in args.logs if p.endswith((".json", ".jsonl"))]
    other_logs = [p for p in args.logs if p not in json_logs]
    batches: List[Tuple[List[CoverageTarget], List[IndirectTarget]]] = []
    upstream = load_with_upstream(json_logs) if json_logs else None
    if upstream is not None:
        t, i, warnings = upstream
        print(f"Loaded {len(json_logs)} capture file(s) via snesrecomp coverage_profile.load_profiles")
        for w in warnings:
            print(f"Warning: {w}")
        batches.append((t, i))
    else:
        other_logs = list(args.logs)
        if json_logs:
            print("Note: upstream loader rejected the JSON input (no v2 identity?); using lenient parser")
    for log_file in other_logs:
        batches.append(parse_coverage_log(log_file))
    for targets, indirects in batches:
        # Filter by min_hits
        filtered_targets = [t for t in targets if t.hits >= args.min_hits]
        filtered_indirects = [i for i in indirects if i.hits >= args.min_hits]
        all_targets.extend(filtered_targets)
        all_indirects.extend(filtered_indirects)

    if SKIPPED:
        by_reason: Dict[str, int] = {}
        for _pc, why in SKIPPED:
            by_reason[why] = by_reason.get(why, 0) + 1
        print("Not promoted (needs manual analysis): " +
              ", ".join(f"{k}={v}" for k, v in sorted(by_reason.items())))
    if not all_targets and not all_indirects:
        print("No coverage discoveries found in supplied log(s).")
        return 0

    print(
        f"Parsed {len(all_targets)} function targets and "
        f"{len(all_indirects)} indirect dispatch targets from {len(args.logs)} log file(s)."
    )

    diffs = merge_coverage(
        cfg_dir,
        all_targets,
        all_indirects,
        propose_entry_mx_at=args.entry_mx_at,
    )

    if not diffs:
        print("All coverage targets are already present in configs. No changes needed.")
        return 0

    total_added = print_diff(diffs)

    if args.apply:
        for path, (_, new_lines) in diffs.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("".join(new_lines), encoding="utf-8")
        print(f"\nApplied {total_added} additions across {len(diffs)} config file(s).")
    else:
        print(
            f"\nDry run: {total_added} proposed additions across {len(diffs)} config file(s)."
            "\nRun with --apply to write changes to disk."
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
