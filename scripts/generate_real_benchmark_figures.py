#!/usr/bin/env python3
"""
Generate figures from REAL benchmark statistics — net/pin counts, pin
resolution rate, and congestion density — read from
results/real_benchmark_stats.json, which is itself produced by actually
parsing the real ISPD 2018 LEF/DEF/guide files in benchmarks/ through
TritonBridge's real parsers (load_nets, estimate_congestion_from_guides,
extract_routing_graph — see src/triton_bridge.cpp).

These are NOT routing-quality results (no DRC/via/wirelength numbers here
— that needs an actual routed DEF, which doesn't exist yet; see
simulation/figures/ for the clearly-labeled synthetic placeholders for
that). This script only visualizes real preprocessing/data-plumbing
statistics: how many nets and pins a design has, how many of those pins
the LEF/DEF parser could actually resolve to real coordinates, and how
guide-derived congestion density varies by benchmark. Every number here
came from parsing real files, not from an RNG.

Usage:
    python3 scripts/generate_real_benchmark_figures.py
"""

import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

STATS_PATH = Path("results/real_benchmark_stats.json")
OUTPUT_DIR = Path("results/figures")

# Single-hue sequential ramp (magnitude data) — safe by construction with
# only one hue in play, per the dataviz color formula (sequential = one
# hue, light -> dark). White figure background matches the convention
# already used by simulation/figures/*.png elsewhere in this repo.
BLUE_DARK = "#1d4ed8"
BLUE_MID = "#3b82f6"
INK = "#1f2937"
MUTED = "#6b7280"
GRID = "#e5e7eb"


def style_axes(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(GRID)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=MUTED, labelsize=9)
    ax.yaxis.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)


def fig_scale(stats):
    names = [s["name"].replace("ispd18_", "") for s in stats]
    nets = [s["nets"] for s in stats]
    pins = [s["total_pins"] for s in stats]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 4.2))
    fig.patch.set_facecolor("white")

    for ax, vals, title, color in [
        (ax1, nets, "Net count", BLUE_DARK),
        (ax2, pins, "Pin count", BLUE_MID),
    ]:
        style_axes(ax)
        bars = ax.bar(names, vals, color=color, width=0.6, zorder=3)
        ax.set_title(title, fontsize=11, color=INK, loc="left", fontweight="bold")
        ax.set_yscale("log")
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v * 1.15, f"{v:,}",
                    ha="center", va="bottom", fontsize=8, color=INK)

    fig.suptitle(
        "Real ISPD 2018 benchmark scale (parsed from actual DEF files, not typed in)",
        fontsize=12, color=INK, y=1.02, fontweight="bold")
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "real_fig1_benchmark_scale.png", dpi=150, bbox_inches="tight")
    plt.close(fig)


def fig_pin_resolution(stats):
    names = [s["name"].replace("ispd18_", "") for s in stats]
    rates = [100.0 * s["resolved_pins"] / s["total_pins"] for s in stats]

    fig, ax = plt.subplots(figsize=(7, 4))
    fig.patch.set_facecolor("white")
    style_axes(ax)
    bars = ax.bar(names, rates, color=BLUE_DARK, width=0.55, zorder=3)
    ax.set_ylim(90, 101)
    ax.set_ylabel("Pins resolved to real (x, y) coordinates [%]", fontsize=9, color=MUTED)
    ax.set_title(
        "Real pin-coordinate resolution rate (LEF MACRO/PIN + DEF placement + orientation)",
        fontsize=11, color=INK, loc="left", fontweight="bold")
    for b, v in zip(bars, rates):
        ax.text(b.get_x() + b.get_width() / 2, v + 0.15, f"{v:.2f}%",
                ha="center", va="bottom", fontsize=8, color=INK)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "real_fig2_pin_resolution_rate.png", dpi=150, bbox_inches="tight")
    plt.close(fig)


def fig_congestion(stats):
    names = [s["name"].replace("ispd18_", "") for s in stats]
    total_overflow = [s["congestion_total_overflow"] for s in stats]
    nets = [s["nets"] for s in stats]
    per_net = [t / n for t, n in zip(total_overflow, nets)]

    fig, ax = plt.subplots(figsize=(7, 4))
    fig.patch.set_facecolor("white")
    style_axes(ax)
    bars = ax.bar(names, per_net, color=BLUE_MID, width=0.55, zorder=3)
    ax.set_ylabel("Guide-derived GCell overflow per net", fontsize=9, color=MUTED)
    ax.set_title(
        "Real routing-guide density, normalized by net count (64×64 GCell grid)",
        fontsize=11, color=INK, loc="left", fontweight="bold")
    for b, v in zip(bars, per_net):
        ax.text(b.get_x() + b.get_width() / 2, v + max(per_net) * 0.02, f"{v:.1f}",
                ha="center", va="bottom", fontsize=8, color=INK)
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / "real_fig3_guide_congestion_density.png", dpi=150, bbox_inches="tight")
    plt.close(fig)


def main():
    stats = json.loads(STATS_PATH.read_text())
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    fig_scale(stats)
    fig_pin_resolution(stats)
    fig_congestion(stats)
    print(f"[figures] wrote 3 real-data figures to {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
