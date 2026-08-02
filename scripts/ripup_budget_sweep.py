#!/usr/bin/env python3
"""
Rip-up budget sweep.

The RBA orchestrator's rip-up candidate cap (RBAConfig::ripup_fraction,
include/rba_types.h) used to be a hardcoded flat count of 50 nets with no
justification. It is now a configurable fraction of total net count
(default 0.10, itself still an arbitrary starting point). This script
sweeps that fraction across a benchmark and reports the resulting DRC/via
counts so the default can be justified or replaced with data instead of
another unjustified guess.

Usage:
    python3 scripts/ripup_budget_sweep.py \\
        --rba_bin ./build/rba_router \\
        --lef design.lef --def design.def --guide design.guide \\
        --output ./ripup_sweep_results \\
        --fractions 0.02 0.05 0.10 0.20 0.30 0.50

Requires a real openroad binary reachable by rba_router (--openroad or on
PATH) — this script only orchestrates rba_router invocations and parses
their output; it does not itself talk to OpenROAD.
"""

import argparse
import csv
import json
import subprocess
import sys
import time
from pathlib import Path


def run_one(rba_bin: str, lef: str, def_: str, guide: str,
           out_dir: Path, fraction: float, openroad_bin: str,
           seed: int, extra_config: str = "") -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        rba_bin,
        "--lef", lef, "--def", def_, "--guide", guide,
        "--output", str(out_dir),
        "--ripup_fraction", str(fraction),
        "--seed", str(seed),
    ]
    if openroad_bin:
        cmd += ["--openroad", openroad_bin]
    if extra_config:
        cmd += ["--config", extra_config]

    t0 = time.monotonic()
    row = {"ripup_fraction": fraction, "seed": seed, "success": False,
           "drc_count": None, "via_count": None, "wirelength": None,
           "unrouted_nets": None, "runtime_sec": None, "router_invocations": None}
    try:
        subprocess.run(cmd, check=True, capture_output=True, timeout=14400)
        row["success"] = True
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"[sweep] fraction={fraction} seed={seed} FAILED: {e}", file=sys.stderr)
        row["runtime_sec"] = time.monotonic() - t0
        return row
    row["runtime_sec"] = time.monotonic() - t0

    summary_path = out_dir / "run_summary.json"
    if summary_path.exists():
        with open(summary_path) as f:
            summary = json.load(f)
        row["drc_count"] = summary.get("drc_count")
        row["via_count"] = summary.get("via_count")
        row["wirelength"] = summary.get("wirelength")
        row["unrouted_nets"] = summary.get("unrouted_nets")
        row["router_invocations"] = summary.get("router_invocations")
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rba_bin", required=True)
    ap.add_argument("--lef", required=True)
    ap.add_argument("--def", dest="def_", required=True)
    ap.add_argument("--guide", required=True)
    ap.add_argument("--output", default="./ripup_sweep_results")
    ap.add_argument("--openroad", default="openroad")
    ap.add_argument("--config", default="", help="Base JSON config (ripup_fraction is overridden per point)")
    ap.add_argument("--fractions", type=float, nargs="+",
                    default=[0.02, 0.05, 0.10, 0.20, 0.30, 0.50],
                    help="Rip-up fractions to sweep (default: 0.02 0.05 0.10 0.20 0.30 0.50)")
    ap.add_argument("--seed", type=int, default=1,
                    help="Base seed; held fixed across the sweep so only ripup_fraction varies")
    args = ap.parse_args()

    out_root = Path(args.output)
    out_root.mkdir(parents=True, exist_ok=True)

    rows = []
    for frac in args.fractions:
        print(f"[sweep] ripup_fraction={frac}")
        out_dir = out_root / f"frac_{frac:g}"
        row = run_one(args.rba_bin, args.lef, args.def_, args.guide,
                     out_dir, frac, args.openroad, args.seed, args.config)
        rows.append(row)
        print(f"  -> {row}")

    csv_path = out_root / "ripup_sweep.csv"
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"[sweep] wrote {csv_path}")

    json_path = out_root / "ripup_sweep.json"
    with open(json_path, "w") as f:
        json.dump(rows, f, indent=2)
    print(f"[sweep] wrote {json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
