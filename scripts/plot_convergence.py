#!/usr/bin/env python3
"""
Plot per-iteration convergence curves from rba_metrics.csv outputs.
Shows how DRC count, via count, and wirelength evolve across outer iterations.

Usage:
    python3 plot_convergence.py --results_dir ./eval_results \
                                 --output ./convergence_plots
"""
import argparse
import json
import os
from pathlib import Path
import re

try:
    import pandas as pd
    import matplotlib.pyplot as plt
    import matplotlib.cm as cm
    import numpy as np
except ImportError:
    raise SystemExit("pip install pandas matplotlib numpy")


def load_metrics(results_dir: str):
    """
    Walk results_dir for rba_metrics.csv files.
    Returns dict: {benchmark_name: [{run_id: int, data: DataFrame}]}
    """
    data = {}
    root = Path(results_dir)

    for csv_path in sorted(root.rglob("rba_metrics.csv")):
        parts = csv_path.parts
        # Expected path: results_dir/<bench>/rba/<run_id>/rba_metrics.csv
        if len(parts) < 4:
            continue
        bench_name = parts[-4]
        run_id_str = parts[-2]
        try:
            run_id = int(run_id_str)
        except ValueError:
            run_id = 0

        df = pd.read_csv(str(csv_path))
        if bench_name not in data:
            data[bench_name] = []
        data[bench_name].append({"run_id": run_id, "df": df})

    return data


def plot_convergence(data: dict, output_dir: str):
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    metrics = ["drc_count", "via_count", "wirelength"]
    titles  = ["DRC Violations", "Via Count", "Total Wirelength (DBU)"]

    for bench_name, runs in data.items():
        fig, axes = plt.subplots(1, 3, figsize=(15, 4))
        fig.suptitle(f"RBA Convergence: {bench_name}", fontsize=12)

        colors = cm.tab10(np.linspace(0, 1, max(len(runs), 1)))

        for ax, metric, title in zip(axes, metrics, titles):
            for i, run in enumerate(runs):
                df = run["df"]
                if metric not in df.columns:
                    continue
                ax.plot(df["iteration"], df[metric],
                        color=colors[i % len(colors)],
                        alpha=0.7, linewidth=1.5,
                        label=f"run {run['run_id']}")

            ax.set_xlabel("Outer Iteration")
            ax.set_ylabel(title)
            ax.set_title(title)
            ax.grid(alpha=0.3)

        axes[0].legend(fontsize=7, ncol=2)
        plt.tight_layout()

        out_path = Path(output_dir) / f"convergence_{bench_name}.pdf"
        fig.savefig(str(out_path), dpi=150, bbox_inches="tight")
        plt.close()
        print(f"[Plot] {out_path}")


def plot_weight_evolution(data: dict, output_dir: str):
    """Plot how PSO-tuned cost weights evolve across iterations."""
    weight_cols = ["w_wire", "w_via", "w_cong"]

    for bench_name, runs in data.items():
        for run in runs[:1]:  # first run only
            df = run["df"]
            if not all(c in df.columns for c in weight_cols):
                continue

            fig, ax = plt.subplots(figsize=(8, 4))
            for col in weight_cols:
                ax.plot(df["iteration"], df[col], marker="o", label=col)
            ax.set_xlabel("Outer Iteration")
            ax.set_ylabel("Weight Value")
            ax.set_title(f"PSO Weight Evolution: {bench_name}")
            ax.legend()
            ax.grid(alpha=0.3)
            plt.tight_layout()

            out_path = Path(output_dir) / f"weights_{bench_name}.pdf"
            fig.savefig(str(out_path), dpi=150, bbox_inches="tight")
            plt.close()
            print(f"[Plot] {out_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--results_dir", required=True)
    parser.add_argument("--output", default="./convergence_plots")
    args = parser.parse_args()

    data = load_metrics(args.results_dir)
    if not data:
        print(f"No rba_metrics.csv files found in {args.results_dir}")
        return

    print(f"Found data for {len(data)} benchmarks")
    plot_convergence(data, args.output)
    plot_weight_evolution(data, args.output)
    print("Done.")


if __name__ == "__main__":
    main()
