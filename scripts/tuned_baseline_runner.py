#!/usr/bin/env python3
"""
Tuned-baseline runner — the fairness control for Referee 3 Q3.

Plain TritonRoute run once with default settings is not a fair comparison
against RBA's PSO-tuned cost weights: RBA gets many router invocations to
search for good weights, while a single baseline run gets one. This script
gives plain TritonRoute the *same number of router invocations* RBA
consumed on a given benchmark, via random search over the same cost-weight
knobs RBA tunes (set_drt_cost_weights — see third_party/openroad.patch and
docs/INTEGRATION.md), and reports the best result found. If the answer
"does RBA actually help" flips once TritonRoute gets an equal budget, that
is the real result.

On a stock (unpatched) OpenROAD build, set_drt_cost_weights doesn't exist,
so every trial in the search is identical (TritonRoute ignores the guard
branch and always uses its own defaults) — the search still runs and still
consumes the same number of invocations, but every trial is the same run,
which itself is useful evidence that a fair comparison requires the patch.

Usage:
    python3 scripts/tuned_baseline_runner.py \\
        --lef design.lef --def design.def --guide design.guide \\
        --output ./tuned_baseline_results \\
        --budget_invocations 40 \\
        --openroad openroad

Budget can also be read from an RBA run's run_summary.json:
    --rba_run_summary ./rba_output/run_summary.json
"""

import argparse
import json
import random
import re
import subprocess
import sys
import time
from pathlib import Path


def fitness(drc: int, via: int, wl: float, unrouted: int) -> float:
    # Same formula as RoutingSnapshot::fitness() in include/rba_types.h —
    # lower is better. Kept identical so results are comparable to RBA's
    # own best_snapshot selection.
    return drc * 1000.0 + unrouted * 10000.0 + via * 1.0 + wl * 0.001


def parse_def_metrics(def_path: Path):
    wire_re = re.compile(r'ROUTED\s+\S+\s+\d+\s+\((\d+)\s+(\d+)\)\s+\((\d+)\s+(\d+)\)')
    via_re = re.compile(r'\bNEW\b.*\+\s*VIA\b', re.IGNORECASE)
    unrouted_re = re.compile(r'\bUNROUTED\b')
    wl, via, unrouted = 0.0, 0, 0
    if not def_path.exists():
        return wl, via, unrouted
    text = def_path.read_text(errors="replace")
    for m in wire_re.finditer(text):
        x1, y1, x2, y2 = map(int, m.groups())
        wl += abs(x2 - x1) + abs(y2 - y1)
    via = len(via_re.findall(text))
    unrouted = len(unrouted_re.findall(text))
    return wl, via, unrouted


def count_drc(rpt_path: Path) -> int:
    if not rpt_path.exists():
        return 0
    text = rpt_path.read_text(errors="replace")
    m = re.search(r"Total Violations:\s*(\d+)", text)
    if m:
        return int(m.group(1))
    return len(re.findall(r"violation", text, re.IGNORECASE))


def run_trial(openroad_bin: str, lef: str, def_: str, guide: str,
             out_dir: Path, trial_id: int, weights: dict, seed: int) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    out_def = out_dir / "routed.def"
    drc_rpt = out_dir / "drc.rpt"

    weights_tcl = ""
    if weights is not None:
        weights_tcl = (
            "if {[llength [info commands set_drt_cost_weights]] > 0} {\n"
            f"  set_drt_cost_weights -route_shape_cost {weights['route_shape_cost']} "
            f"-via_cost {weights['via_cost']} -marker_cost {weights['marker_cost']} "
            f"-grid_cost {weights['grid_cost']}\n"
            "} else {\n"
            "  puts \"[tuned-baseline] set_drt_cost_weights unavailable — every trial "
            "identical on this (unpatched) build\"\n"
            "}\n"
        )

    tcl = f"""
read_lef {lef}
read_def {def_}
{weights_tcl}
detailed_route \\
    -guide {guide} \\
    -output_drc {drc_rpt} \\
    -or_seed {seed} \\
    -verbose 0 \\
    -threads 8
write_def {out_def}
"""
    tcl_path = out_dir / "run.tcl"
    tcl_path.write_text(tcl)

    t0 = time.monotonic()
    row = {"trial": trial_id, "weights": weights, "success": False,
          "drc_count": None, "via_count": None, "wirelength": None,
          "unrouted_nets": None, "fitness": None, "runtime_sec": None}
    try:
        subprocess.run([openroad_bin, "-exit", str(tcl_path)],
                       check=True, capture_output=True, timeout=7200)
        row["success"] = True
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"[tuned-baseline] trial {trial_id} FAILED: {e}", file=sys.stderr)
        row["runtime_sec"] = time.monotonic() - t0
        return row
    row["runtime_sec"] = time.monotonic() - t0

    wl, via, unrouted = parse_def_metrics(out_def)
    drc = count_drc(drc_rpt)
    row.update({"drc_count": drc, "via_count": via, "wirelength": wl,
               "unrouted_nets": unrouted, "fitness": fitness(drc, via, wl, unrouted)})
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lef", required=True)
    ap.add_argument("--def", dest="def_", required=True)
    ap.add_argument("--guide", required=True)
    ap.add_argument("--output", default="./tuned_baseline_results")
    ap.add_argument("--openroad", default="openroad")
    ap.add_argument("--budget_invocations", type=int, default=None,
                    help="Number of trials to run (must equal RBA's router_invocations "
                         "for a fair comparison). Overrides --rba_run_summary if both given.")
    ap.add_argument("--rba_run_summary", default=None,
                    help="Path to an RBA run's run_summary.json; reads router_invocations "
                         "from it as the budget.")
    ap.add_argument("--seed", type=int, default=1, help="Base RNG seed for the search")
    # Search ranges mirror CostWeights' [0.1, 20.0]-scaled defaults mapped
    # through triton_bridge.cpp's scaling (see docs/INTEGRATION.md):
    # route_shape_cost default 8, via_cost default 4, marker_cost default 32,
    # grid_cost default 2. Ranges below span roughly the same [0.1, 20.0]x
    # RBA search space RBA's PSO explores for w_wire/w_via/w_drc_hist/w_layer_pref.
    ap.add_argument("--route_shape_cost_range", type=int, nargs=2, default=[1, 160])
    ap.add_argument("--via_cost_range", type=int, nargs=2, default=[1, 80])
    ap.add_argument("--marker_cost_range", type=int, nargs=2, default=[1, 640])
    ap.add_argument("--grid_cost_range", type=int, nargs=2, default=[1, 40])
    args = ap.parse_args()

    budget = args.budget_invocations
    if budget is None and args.rba_run_summary:
        with open(args.rba_run_summary) as f:
            budget = json.load(f).get("router_invocations")
    if not budget or budget < 1:
        print("error: --budget_invocations or a valid --rba_run_summary is required",
              file=sys.stderr)
        return 1

    rng = random.Random(args.seed)
    out_root = Path(args.output)
    out_root.mkdir(parents=True, exist_ok=True)

    trials = []
    print(f"[tuned-baseline] random search over {budget} trials (budget matched to RBA)")
    for i in range(budget):
        weights = {
            "route_shape_cost": rng.randint(*args.route_shape_cost_range),
            "via_cost": rng.randint(*args.via_cost_range),
            "marker_cost": rng.randint(*args.marker_cost_range),
            "grid_cost": rng.randint(*args.grid_cost_range),
        }
        trial_dir = out_root / f"trial_{i}"
        row = run_trial(args.openroad, args.lef, args.def_, args.guide,
                        trial_dir, i, weights, args.seed + i)
        trials.append(row)
        print(f"  trial {i}: {row['weights']} -> "
              f"fitness={row['fitness']} drc={row['drc_count']} via={row['via_count']}")

    successful = [t for t in trials if t["success"]]
    best = min(successful, key=lambda t: t["fitness"]) if successful else None

    summary = {
        "budget_invocations": budget,
        "trials_run": len(trials),
        "trials_succeeded": len(successful),
        "best_trial": best,
        "all_trials": trials,
    }
    summary_path = out_root / "tuned_baseline_summary.json"
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"[tuned-baseline] wrote {summary_path}")
    if best:
        print(f"[tuned-baseline] best: fitness={best['fitness']} "
              f"drc={best['drc_count']} via={best['via_count']} weights={best['weights']}")
    else:
        print("[tuned-baseline] no successful trials", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
