#!/usr/bin/env python3
"""
RBA-TritonRoute Evaluation Script
==================================
Runs the RBA router and baseline TritonRoute on all ISPD benchmark circuits,
collects metrics, and produces comparison tables and plots.

Usage:
    python3 evaluate_rba.py --benchmarks /path/to/ispd2018 \
                             --rba_bin    /path/to/rba_router \
                             --openroad   /path/to/openroad \
                             --output     ./results

Dependencies:
    pip install pandas matplotlib scipy numpy tqdm
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, List, Optional
import statistics

# Optional imports for plotting
try:
    import pandas as pd
    import matplotlib.pyplot as plt
    import matplotlib.gridspec as gridspec
    from scipy import stats
    HAS_PLOT = True
except ImportError:
    HAS_PLOT = False
    print("[Warning] pandas/matplotlib/scipy not found — skipping plots")


# ─── Data structures ──────────────────────────────────────────────────────────

@dataclass
class BenchmarkResult:
    name:           str
    method:         str        # "baseline" or "rba"
    drc_count:      int        = 0
    via_count:      int        = 0
    wirelength:     float      = 0.0
    unrouted_nets:  int        = 0
    runtime_sec:    float      = 0.0
    contest_score:  Optional[float] = None
    success:        bool       = True
    run_id:         int        = 0


@dataclass
class BenchmarkConfig:
    name:        str
    lef_file:    str
    def_file:    str
    guide_file:  str
    timing_rpt:  str = ""


# ─── Experiment analysis helpers ───────────────────────────────────────────


def get_result_value(result: Any, key: str) -> Any:
    if isinstance(result, dict):
        return result.get(key)
    return getattr(result, key, None)


def summarize_metric(values: List[float]) -> Dict[str, Any]:
    vals = [float(v) for v in values if v is not None]
    if not vals:
        return {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0}
    return {
        "mean": statistics.mean(vals),
        "std": statistics.pstdev(vals) if len(vals) > 1 else 0.0,
        "min": min(vals),
        "max": max(vals),
        "n": len(vals),
        "values": vals,
    }


def summarize_results(results: List[Any], metric: str) -> Dict[str, Any]:
    values = [get_result_value(r, metric) for r in results if get_result_value(r, metric) is not None]
    return summarize_metric(values)


def pick_best_result(results: List[Any], metric: str, budget: Optional[float] = None) -> Optional[Any]:
    if not results:
        return None
    filtered = results
    if budget is not None:
        filtered = [r for r in results if get_result_value(r, "runtime_sec") is not None and get_result_value(r, "runtime_sec") <= budget]
    if not filtered:
        filtered = results
    if metric == "contest_score":
        return max(filtered, key=lambda r: float(get_result_value(r, metric) or 0.0))
    return min(filtered, key=lambda r: float(get_result_value(r, metric) or float("inf")))


def build_experiment_report(raw_results: Dict[str, List[Any]],
                            iteration_summaries: Optional[Dict[str, Any]] = None,
                            runtime_budget_sec: Optional[float] = None,
                            compute_budget_invocations: Optional[float] = None) -> List[Dict[str, Any]]:
    """Build a benchmark-level report with absolute stats, deltas, equal-runtime and equal-compute comparisons."""
    baseline_results = raw_results.get("baseline", [])
    rba_results = raw_results.get("rba", [])

    report: List[Dict[str, Any]] = []
    benchmarks = sorted({get_result_value(r, "name") for r in baseline_results + rba_results if get_result_value(r, "name")})
    for bench in benchmarks:
        bl = [r for r in baseline_results if get_result_value(r, "name") == bench]
        rb = [r for r in rba_results if get_result_value(r, "name") == bench]
        if not bl or not rb:
            continue

        entry: Dict[str, Any] = {"benchmark": bench}
        metrics = ["drc_count", "via_count", "wirelength", "runtime_sec"]
        if any(get_result_value(r, "contest_score") is not None for r in bl + rb):
            metrics.append("contest_score")

        entry["baseline"] = {}
        entry["rba"] = {}
        for metric in metrics:
            entry["baseline"][metric] = summarize_results(bl, metric)
            entry["rba"][metric] = summarize_results(rb, metric)

        entry["delta_pct"] = {}
        for metric in metrics:
            bl_mean = entry["baseline"][metric]["mean"]
            rb_mean = entry["rba"][metric]["mean"]
            if bl_mean == 0:
                delta = 0.0
            else:
                delta = ((rb_mean - bl_mean) / bl_mean) * 100.0
            entry["delta_pct"][metric] = delta

        entry["equal_runtime"] = {}
        for metric in metrics:
            budget = runtime_budget_sec if runtime_budget_sec is not None else None
            bl_best = pick_best_result(bl, metric, budget=budget)
            rb_best = pick_best_result(rb, metric, budget=budget)
            if bl_best is None or rb_best is None:
                winner = "tie"
            else:
                bl_val = float(get_result_value(bl_best, metric) or 0.0)
                rb_val = float(get_result_value(rb_best, metric) or 0.0)
                if metric == "contest_score":
                    winner = "rba" if rb_val > bl_val else "baseline"
                else:
                    winner = "rba" if rb_val < bl_val else "baseline"
            entry["equal_runtime"][metric] = {
                "baseline": bl_best,
                "rba": rb_best,
                "winner": winner,
            }

        entry["equal_compute_budget"] = {}
        for metric in metrics:
            budget = compute_budget_invocations if compute_budget_invocations is not None else 1.0
            bl_best = pick_best_result(bl, metric, budget=budget)
            rb_best = pick_best_result(rb, metric, budget=budget)
            if bl_best is None or rb_best is None:
                winner = "tie"
            else:
                bl_val = float(get_result_value(bl_best, metric) or 0.0)
                rb_val = float(get_result_value(rb_best, metric) or 0.0)
                if metric == "contest_score":
                    winner = "rba" if rb_val > bl_val else "baseline"
                else:
                    winner = "rba" if rb_val < bl_val else "baseline"
            entry["equal_compute_budget"][metric] = {
                "baseline": bl_best,
                "rba": rb_best,
                "winner": winner,
            }

        if iteration_summaries is not None:
            entry["convergence"] = iteration_summaries.get(bench, {})

        report.append(entry)

    return report


def summarize_convergence(output_dir: str) -> Dict[str, Any]:
    """Summarize RBA iteration traces from rba_metrics.csv files."""
    summary: Dict[str, Any] = {}
    root = Path(output_dir)
    for csv_path in sorted(root.rglob("rba_metrics.csv")):
        parts = csv_path.parts
        if len(parts) < 4:
            continue
        bench = parts[-4]
        run_id = parts[-2]
        try:
            run_id_int = int(run_id)
        except ValueError:
            run_id_int = 0

        with open(csv_path, newline="") as handle:
            rows = list(csv.DictReader(handle))

        if not rows:
            continue

        bench_summary = summary.setdefault(bench, {"runs": []})
        bench_summary["runs"].append({"run_id": run_id_int, "rows": rows})

    for bench, payload in summary.items():
        metrics = ["drc_count", "via_count", "wirelength"]
        convergence = {}
        for metric in metrics:
            by_iteration: Dict[int, List[float]] = {}
            for run in payload["runs"]:
                for row in run["rows"]:
                    try:
                        iteration = int(row.get("iteration", 0))
                        value = float(row.get(metric, 0.0))
                    except (TypeError, ValueError):
                        continue
                    by_iteration.setdefault(iteration, []).append(value)
            convergence[metric] = [
                {
                    "iteration": iteration,
                    "mean": statistics.mean(values),
                    "std": statistics.pstdev(values) if len(values) > 1 else 0.0,
                    "min": min(values),
                    "max": max(values),
                }
                for iteration, values in sorted(by_iteration.items())
            ]
        summary[bench] = convergence

    return summary


def capture_provenance(output_dir: str, openroad_bin: str = "openroad") -> Dict[str, Any]:
    """Capture git and binary provenance metadata for a run."""
    provenance: Dict[str, Any] = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "python_version": sys.version.split()[0],
        "source": "evaluation scaffold",
        "notes": "Real router measurements are not present in this repository snapshot.",
    }

    try:
        git_commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=Path(__file__).resolve().parents[1], text=True).strip()
        provenance["git_commit"] = git_commit
    except Exception:
        provenance["git_commit"] = None

    try:
        version = subprocess.check_output([openroad_bin, "--version"], text=True, stderr=subprocess.STDOUT).strip()
        provenance["openroad_version"] = version
    except Exception:
        provenance["openroad_version"] = None

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    with open(output / "provenance.json", "w") as handle:
        json.dump(provenance, handle, indent=2)
    return provenance


def write_experiment_report(report: List[Dict[str, Any]], output_dir: str, convergence_summaries: Optional[Dict[str, Any]] = None) -> None:
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "experiment_report.json"
    with open(report_path, "w") as handle:
        json.dump(report, handle, indent=2)

    if convergence_summaries is not None:
        conv_path = output / "convergence_summary.json"
        with open(conv_path, "w") as handle:
            json.dump(convergence_summaries, handle, indent=2)

    if not report:
        placeholder = [{
            "benchmark": "placeholder",
            "baseline": {
                "drc_count": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
                "via_count": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
                "wirelength": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
                "runtime_sec": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
            },
            "rba": {
                "drc_count": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
                "via_count": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
                "wirelength": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
                "runtime_sec": {"mean": 0.0, "std": 0.0, "min": 0.0, "max": 0.0, "n": 0, "values": []},
            },
            "delta_pct": {"drc_count": 0.0, "via_count": 0.0, "wirelength": 0.0, "runtime_sec": 0.0},
            "equal_runtime": {},
            "equal_compute_budget": {},
            "convergence": {},
        }]
        with open(report_path, "w") as handle:
            json.dump(placeholder, handle, indent=2)
        return

    summary_rows = []
    for entry in report:
        summary_rows.append({
            "benchmark": entry["benchmark"],
            "baseline_drc": entry["baseline"]["drc_count"]["mean"],
            "rba_drc": entry["rba"]["drc_count"]["mean"],
            "drc_delta_pct": entry["delta_pct"]["drc_count"],
            "baseline_via": entry["baseline"]["via_count"]["mean"],
            "rba_via": entry["rba"]["via_count"]["mean"],
            "via_delta_pct": entry["delta_pct"]["via_count"],
            "baseline_wl": entry["baseline"]["wirelength"]["mean"],
            "rba_wl": entry["rba"]["wirelength"]["mean"],
            "wl_delta_pct": entry["delta_pct"]["wirelength"],
            "baseline_rt": entry["baseline"]["runtime_sec"]["mean"],
            "rba_rt": entry["rba"]["runtime_sec"]["mean"],
            "runtime_delta_pct": entry["delta_pct"]["runtime_sec"],
        })

    if summary_rows:
        with open(output / "summary.csv", "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(summary_rows[0].keys()))
            writer.writeheader()
            writer.writerows(summary_rows)


# ─── Benchmark discovery ──────────────────────────────────────────────────────

def discover_benchmarks(bench_dir: str) -> List[BenchmarkConfig]:
    """
    Walk bench_dir looking for ISPD 2018/2019 benchmark structure:
      <name>/
        <name>.lef  (or tech.lef + cells.lef)
        <name>.def
        <name>.guide
    """
    benchmarks = []
    bench_path = Path(bench_dir)

    for subdir in sorted(bench_path.iterdir()):
        if not subdir.is_dir():
            continue
        name = subdir.name

        # Find LEF (prefer merged lef, else first .lef)
        lef = next(subdir.glob("*.lef"), None)
        def_ = next(subdir.glob("*.def"), None)
        guide = next(subdir.glob("*.guide"), None)

        if not (lef and def_ and guide):
            print(f"[Discover] Skipping {name}: missing LEF/DEF/guide")
            continue

        timing = next(subdir.glob("*.rpt"), None)

        benchmarks.append(BenchmarkConfig(
            name=name,
            lef_file=str(lef),
            def_file=str(def_),
            guide_file=str(guide),
            timing_rpt=str(timing) if timing else ""
        ))
        print(f"[Discover] Found: {name}")

    return benchmarks


# ─── Run helpers ──────────────────────────────────────────────────────────────

def run_baseline(bench: BenchmarkConfig, openroad_bin: str,
                 output_dir: str, run_id: int) -> BenchmarkResult:
    """Run unmodified TritonRoute via OpenROAD."""
    result = BenchmarkResult(name=bench.name, method="baseline", run_id=run_id)

    out_dir = Path(output_dir) / bench.name / "baseline" / str(run_id)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Write Tcl script
    tcl_path = out_dir / "run.tcl"
    out_def  = out_dir / "routed.def"
    drc_rpt  = out_dir / "drc.rpt"

    tcl_content = f"""
read_lef {bench.lef_file}
read_def {bench.def_file}
detailed_route \\
    -guide {bench.guide_file} \\
    -output_drc {drc_rpt} \\
    -verbose 0 \\
    -threads 8
write_def {out_def}
"""
    tcl_path.write_text(tcl_content)

    t0 = time.monotonic()
    try:
        subprocess.run(
            [openroad_bin, "-exit", str(tcl_path)],
            check=True,
            capture_output=True,
            timeout=7200  # 2 hours max
        )
        result.success = True
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"[Baseline] {bench.name} FAILED: {e}")
        result.success = False
        result.runtime_sec = time.monotonic() - t0
        return result

    result.runtime_sec = time.monotonic() - t0

    # Parse metrics
    if out_def.exists():
        result.wirelength, result.via_count, result.unrouted_nets = \
            parse_def_metrics(str(out_def))
    if drc_rpt.exists():
        result.drc_count = count_drc_violations(str(drc_rpt))

    return result


def run_rba(bench: BenchmarkConfig, rba_bin: str,
            output_dir: str, run_id: int,
            rba_config: Optional[str] = None) -> BenchmarkResult:
    """Run RBA-TritonRoute router."""
    result = BenchmarkResult(name=bench.name, method="rba", run_id=run_id)

    out_dir = Path(output_dir) / bench.name / "rba" / str(run_id)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        rba_bin,
        "--lef",   bench.lef_file,
        "--def",   bench.def_file,
        "--guide", bench.guide_file,
        "--output", str(out_dir),
        "--threads", "8",
    ]
    if bench.timing_rpt:
        cmd += ["--timing", bench.timing_rpt]
    if rba_config:
        cmd += ["--config", rba_config]

    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, check=True, capture_output=True, timeout=14400)
        result.success = True
        # Save stdout for debugging
        (out_dir / "rba.log").write_bytes(proc.stdout)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        print(f"[RBA] {bench.name} FAILED: {e}")
        result.success = False
        result.runtime_sec = time.monotonic() - t0
        return result

    result.runtime_sec = time.monotonic() - t0

    # Parse best output DEF
    best_def = out_dir / "abc_via_optimized.def"
    if not best_def.exists():
        best_def = out_dir / "iter_best_routed.def"

    if best_def.exists():
        result.wirelength, result.via_count, result.unrouted_nets = \
            parse_def_metrics(str(best_def))

    # Parse metrics CSV if available
    metrics_csv = out_dir / "rba_metrics.csv"
    if metrics_csv.exists():
        best = parse_metrics_csv(str(metrics_csv))
        if best:
            result.drc_count = best.get("drc_count", 0)

    return result


# ─── DEF parser ───────────────────────────────────────────────────────────────

def parse_def_metrics(def_path: str):
    """Returns (wirelength_dbu, via_count, unrouted_nets) from a routed DEF."""
    wirelength = 0.0
    via_count  = 0
    unrouted   = 0

    wire_re    = re.compile(r'ROUTED\s+\S+\s+\d+\s+\((\d+)\s+(\d+)\)\s+\((\d+)\s+(\d+)\)')
    via_re     = re.compile(r'\+ VIA\b', re.IGNORECASE)
    unrouted_re = re.compile(r'\bUNROUTED\b')

    try:
        with open(def_path) as f:
            for line in f:
                for m in wire_re.finditer(line):
                    dx = abs(int(m.group(3)) - int(m.group(1)))
                    dy = abs(int(m.group(4)) - int(m.group(2)))
                    wirelength += dx + dy
                if via_re.search(line):
                    via_count += 1
                if unrouted_re.search(line):
                    unrouted += 1
    except OSError:
        pass

    return wirelength, via_count, unrouted


def count_drc_violations(rpt_path: str) -> int:
    """Count total DRC violations from TritonRoute RPT or JSON."""
    if rpt_path.endswith(".json"):
        try:
            with open(rpt_path) as f:
                j = json.load(f)
            return len(j.get("violations", []))
        except Exception:
            return 0

    total = 0
    try:
        with open(rpt_path) as f:
            for line in f:
                if "Total Violations" in line:
                    m = re.search(r'(\d+)', line)
                    if m: return int(m.group(1))
                # Count individual violation blocks
                if "violation type:" in line.lower():
                    total += 1
    except OSError:
        pass
    return total


def parse_metrics_csv(csv_path: str) -> dict:
    """Return best iteration row from RBA metrics CSV."""
    try:
        with open(csv_path) as f:
            lines = f.readlines()
        if len(lines) < 2:
            return {}
        header = [h.strip() for h in lines[0].split(",")]
        best_row = {}
        best_drc = float("inf")
        for line in lines[1:]:
            row = dict(zip(header, [v.strip() for v in line.split(",")]))
            drc = int(row.get("drc_count", 9999))
            if drc < best_drc:
                best_drc = drc
                best_row = {k: float(v) if v.replace(".","").isdigit() else v
                            for k, v in row.items()}
        return best_row
    except Exception:
        return {}


# ─── Statistical analysis ─────────────────────────────────────────────────────

def compare_methods(baseline_results: List[BenchmarkResult],
                    rba_results: List[BenchmarkResult]) -> dict:
    """Backward-compatible wrapper around the new experiment-report builder."""
    raw_results = {
        "baseline": [asdict(r) for r in baseline_results],
        "rba": [asdict(r) for r in rba_results],
    }
    report = build_experiment_report(raw_results)
    summary = {}
    for entry in report:
        summary[entry["benchmark"]] = {
            "baseline_drc": entry["baseline"]["drc_count"]["mean"],
            "rba_drc": entry["rba"]["drc_count"]["mean"],
            "drc_change%": entry["delta_pct"]["drc_count"],
            "baseline_via": entry["baseline"]["via_count"]["mean"],
            "rba_via": entry["rba"]["via_count"]["mean"],
            "via_change%": entry["delta_pct"]["via_count"],
            "baseline_wl": entry["baseline"]["wirelength"]["mean"],
            "rba_wl": entry["rba"]["wirelength"]["mean"],
            "wl_change%": entry["delta_pct"]["wirelength"],
            "baseline_rt": entry["baseline"]["runtime_sec"]["mean"],
            "rba_rt": entry["rba"]["runtime_sec"]["mean"],
            "rt_change%": entry["delta_pct"]["runtime_sec"],
        }
    return summary


def print_comparison_table(summary: dict):
    """Print formatted comparison table."""
    print("\n" + "="*100)
    print(f"{'Benchmark':<20} {'BL DRC':>8} {'RBA DRC':>8} {'ΔDRC%':>8} "
          f"{'BL Via':>8} {'RBA Via':>8} {'ΔVia%':>8} "
          f"{'ΔWL%':>8} {'ΔRT%':>8}")
    print("-"*100)

    drc_improvements, via_improvements = [], []

    for name, s in sorted(summary.items()):
        print(f"{name:<20} "
              f"{s['baseline_drc']:>8.0f} {s['rba_drc']:>8.0f} {s['drc_change%']:>+8.1f}% "
              f"{s['baseline_via']:>8.0f} {s['rba_via']:>8.0f} {s['via_change%']:>+8.1f}% "
              f"{s['wl_change%']:>+8.1f}% {s['rt_change%']:>+8.1f}%")
        drc_improvements.append(s["drc_change%"])
        via_improvements.append(s["via_change%"])

    print("-"*100)
    if drc_improvements:
        print(f"{'Average':<20} {'':>8} {'':>8} "
              f"{statistics.mean(drc_improvements):>+8.1f}% "
              f"{'':>8} {'':>8} "
              f"{statistics.mean(via_improvements):>+8.1f}%")
    print("="*100)
    print("Negative % = improvement (fewer DRCs/vias/shorter wire)\n")


def run_significance_tests(baseline_results, rba_results, summary):
    """Run Wilcoxon signed-rank tests for statistical significance."""
    if not HAS_PLOT:
        return
    print("\n── Wilcoxon Signed-Rank Tests ──")
    for metric in ["drc_count", "via_count", "wirelength"]:
        bl_vals = [getattr(r, metric) for r in baseline_results if r.success]
        rb_vals = [getattr(r, metric) for r in rba_results     if r.success]
        if len(bl_vals) < 5:
            print(f"  {metric}: insufficient data")
            continue
        n = min(len(bl_vals), len(rb_vals))
        stat, p = stats.wilcoxon(bl_vals[:n], rb_vals[:n], alternative='greater')
        sig = "✓ significant" if p < 0.05 else "✗ not significant"
        print(f"  {metric:<20}: W={stat:.1f}, p={p:.4f} ({sig} at α=0.05)")


# ─── Plotting ─────────────────────────────────────────────────────────────────

def plot_results(summary: dict, output_dir: str):
    if not HAS_PLOT:
        return

    fig = plt.figure(figsize=(16, 10))
    gs  = gridspec.GridSpec(2, 3, figure=fig)

    benches = sorted(summary.keys())
    x = range(len(benches))

    def grouped_bars(ax, key_bl, key_rba, title, ylabel):
        bl  = [summary[b][key_bl]  for b in benches]
        rba = [summary[b][key_rba] for b in benches]
        w = 0.35
        ax.bar([i - w/2 for i in x], bl,  w, label="Baseline", color="#4C72B0")
        ax.bar([i + w/2 for i in x], rba, w, label="RBA",      color="#DD8452")
        ax.set_title(title); ax.set_ylabel(ylabel)
        ax.set_xticks(list(x)); ax.set_xticklabels(benches, rotation=45, ha="right")
        ax.legend(); ax.grid(axis="y", alpha=0.3)

    # DRC count
    ax1 = fig.add_subplot(gs[0, 0])
    grouped_bars(ax1, "baseline_drc", "rba_drc", "DRC Violations", "Count")

    # Via count
    ax2 = fig.add_subplot(gs[0, 1])
    grouped_bars(ax2, "baseline_via", "rba_via", "Via Count", "Count")

    # Wirelength
    ax3 = fig.add_subplot(gs[0, 2])
    grouped_bars(ax3, "baseline_wl", "rba_wl", "Total Wirelength", "DBU")

    # Runtime comparison
    ax4 = fig.add_subplot(gs[1, 0])
    grouped_bars(ax4, "baseline_rt", "rba_rt", "Runtime", "Seconds")

    # DRC improvement % bar
    ax5 = fig.add_subplot(gs[1, 1])
    drc_pcts = [summary[b]["drc_change%"] for b in benches]
    colors = ["#2ca02c" if p <= 0 else "#d62728" for p in drc_pcts]
    ax5.bar(list(x), drc_pcts, color=colors)
    ax5.axhline(0, color="black", linewidth=0.8)
    ax5.set_title("DRC Change %"); ax5.set_ylabel("% (negative = better)")
    ax5.set_xticks(list(x)); ax5.set_xticklabels(benches, rotation=45, ha="right")
    ax5.grid(axis="y", alpha=0.3)

    # Via improvement %
    ax6 = fig.add_subplot(gs[1, 2])
    via_pcts = [summary[b]["via_change%"] for b in benches]
    colors6 = ["#2ca02c" if p <= 0 else "#d62728" for p in via_pcts]
    ax6.bar(list(x), via_pcts, color=colors6)
    ax6.axhline(0, color="black", linewidth=0.8)
    ax6.set_title("Via Change %"); ax6.set_ylabel("% (negative = better)")
    ax6.set_xticks(list(x)); ax6.set_xticklabels(benches, rotation=45, ha="right")
    ax6.grid(axis="y", alpha=0.3)

    fig.suptitle("RBA-TritonRoute vs. Baseline TritonRoute\n(ISPD Benchmarks)",
                 fontsize=13)
    plt.tight_layout()

    plot_path = Path(output_dir) / "rba_comparison.pdf"
    fig.savefig(str(plot_path), dpi=150, bbox_inches="tight")
    print(f"[Plot] Saved: {plot_path}")
    plt.close()


# ─── Main ────────────────────────────────────────────────────────────────────

def run_sky130_verification(def_file: str, output_dir: str,
                            pdk_root: str = "", run_magic: bool = False,
                            run_klayout: bool = False) -> int:
    """
    Run Sky130 PDK DRC verification on a routed DEF.
    Returns the number of violations found (0 = clean).
    Imports sky130_verification from the same scripts directory.
    """
    script = Path(__file__).parent / "sky130_verification.py"
    if not script.exists():
        print("[Sky130] sky130_verification.py not found — skipping")
        return -1

    cmd = [sys.executable, str(script),
           "--def", def_file,
           "--output", str(Path(output_dir) / "sky130_drc")]
    if pdk_root:
        cmd += ["--pdk", pdk_root]
    if run_magic:
        cmd.append("--magic")
    if run_klayout:
        cmd.append("--klayout")

    try:
        result = subprocess.run(cmd, capture_output=False, timeout=600)
        return result.returncode  # 0 = clean, 1 = violations found
    except subprocess.TimeoutExpired:
        print("[Sky130] Verification timed out")
        return -1


def main():
    parser = argparse.ArgumentParser(description="RBA-TritonRoute Evaluation")
    parser.add_argument("--benchmarks",  required=True,
                        help="Directory containing ISPD benchmark subdirectories")
    parser.add_argument("--rba_bin",     required=True, help="Path to rba_router binary")
    parser.add_argument("--openroad",    default="openroad",
                        help="Path to OpenROAD binary (for baseline)")
    parser.add_argument("--output",      default="./eval_results",
                        help="Output directory for results")
    parser.add_argument("--rba_config",  default="",
                        help="JSON config file for RBA parameters")
    parser.add_argument("--runs",        type=int, default=5,
                        help="Number of independent runs per benchmark (default: 5)")
    parser.add_argument("--skip_baseline", action="store_true",
                        help="Skip baseline runs (use existing results)")
    parser.add_argument("--benchmarks_subset", nargs="*",
                        help="Run only these benchmark names")
    # Sky130 PDK verification options
    parser.add_argument("--sky130_verify", action="store_true",
                        help="Run Sky130 PDK DRC on each RBA output DEF")
    parser.add_argument("--sky130_pdk", default=os.environ.get("SKY130_PDK", ""),
                        help="Path to sky130A PDK root (or set SKY130_PDK env)")
    parser.add_argument("--sky130_magic",   action="store_true",
                        help="Also run Magic VLSI full DRC during verification")
    parser.add_argument("--sky130_klayout", action="store_true",
                        help="Also run KLayout DRC deck during verification")
    args = parser.parse_args()

    Path(args.output).mkdir(parents=True, exist_ok=True)

    # Discover benchmarks
    all_benchmarks = discover_benchmarks(args.benchmarks)
    if args.benchmarks_subset:
        all_benchmarks = [b for b in all_benchmarks
                          if b.name in args.benchmarks_subset]

    if not all_benchmarks:
        print("No benchmarks found — check --benchmarks path")
        sys.exit(1)

    print(f"\nRunning {len(all_benchmarks)} benchmarks × {args.runs} runs each\n")

    baseline_results: List[BenchmarkResult] = []
    rba_results: List[BenchmarkResult]      = []
    raw_results: Dict[str, List[Any]] = {"baseline": [], "rba": []}

    for bench in all_benchmarks:
        print(f"\n─── {bench.name} ───")

        for run_id in range(args.runs):
            print(f"  Run {run_id+1}/{args.runs}")

            if not args.skip_baseline:
                print("    [Baseline]")
                bl = run_baseline(bench, args.openroad, args.output, run_id)
                baseline_results.append(bl)
                raw_results["baseline"].append(asdict(bl))
                print(f"      DRC={bl.drc_count} via={bl.via_count} "
                      f"WL={bl.wirelength:.0f} t={bl.runtime_sec:.1f}s")

            print("    [RBA]")
            rb = run_rba(bench, args.rba_bin, args.output, run_id, args.rba_config)
            rba_results.append(rb)
            raw_results["rba"].append(asdict(rb))
            print(f"      DRC={rb.drc_count} via={rb.via_count} "
                  f"WL={rb.wirelength:.0f} t={rb.runtime_sec:.1f}s")

            if args.sky130_verify and rb.success:
                out_dir = Path(args.output) / bench.name / "rba" / str(run_id)
                best_def = str(out_dir / "abc_via_optimized.def")
                if not Path(best_def).exists():
                    best_def = str(out_dir / "iter_best_routed.def")
                if Path(best_def).exists():
                    print("    [Sky130 Verify]")
                    run_sky130_verification(
                        best_def,
                        str(out_dir),
                        pdk_root=args.sky130_pdk,
                        run_magic=args.sky130_magic,
                        run_klayout=args.sky130_klayout,
                    )

    capture_provenance(args.output, openroad_bin=args.openroad)

    # Save raw results
    results_json = Path(args.output) / "raw_results.json"
    with open(results_json, "w") as f:
        json.dump(raw_results, f, indent=2)
    print(f"\n[Results] Raw data: {results_json}")

    # Analysis
    if baseline_results and rba_results:
        convergence_summaries = summarize_convergence(args.output)
        report = build_experiment_report(raw_results, iteration_summaries=convergence_summaries)
        write_experiment_report(report, args.output, convergence_summaries=convergence_summaries)

        summary = compare_methods(baseline_results, rba_results)
        print_comparison_table(summary)
        run_significance_tests(baseline_results, rba_results, summary)

        if HAS_PLOT:
            df = pd.DataFrame(summary).T
            df.to_csv(Path(args.output) / "summary.csv")

        plot_results(summary, args.output)


if __name__ == "__main__":
    main()
