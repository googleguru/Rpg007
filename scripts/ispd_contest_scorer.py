#!/usr/bin/env python3
"""
ISPD 2019 contest scorer, implementing the official formula from
"ISPD19 Contest: Evaluation Metrics and Ranking Method" (William Chow,
Gracieli Posser, Stefanus Mantik, Yixiao Ding, Wen-Hao Liu, Cadence Design
Systems, Inc., 2018-12-15), published at
https://www.ispd.cc/contests/19/metrics_and_ranking.pdf — fetched and read
directly from that URL while implementing this file. Not derived from
memory or guesswork; every weight below is quoted from that document.
The ISPD 2018 contest reused the same detailed-routing problem/metric
family; this scorer targets the ISPD19 formula, which is the one this
repo's benchmarks (ispd18_test*/ispd19_test*, per setup_ispd_benchmarks.sh)
were actually evaluated under.

    original_score =
          500 * short_metal_area_per_m2_pitch
        + 500 * num_short_violations
        + 500 * num_spacing_violations
        + 500 * num_min_area_violations
        +   1 * wirelength_outside_guides_m2_pitch
        +   1 * num_vias_outside_guides
        + 0.5 * offtrack_wire_length_m2_pitch
        +   1 * num_offtrack_vias
        +   1 * wrongway_wire_length_m2_pitch
        +   4 * num_single_cut_vias
        +   2 * num_multi_cut_vias
        + 0.5 * total_wire_length_m2_pitch

    scaled_score = original_score * (1 + nondeterministic_penalty + runtime_factor)
    runtime_factor = clip(0.02 * log2(wall_time / median_wall_time), -0.1, 0.1)
    nondeterministic_penalty = 0.03 if nondeterministic else 0.0

Lower scaled_score is better. Any open (unconnected) net makes a solution
INVALID, ranked worse than every valid solution regardless of score.

IMPORTANT — measurement fidelity: the official formula needs 12 distinct
geometric quantities (short area, off-track/wrong-way wire lengths,
single- vs multi-cut via counts, etc.). RBA-TritonRoute's current DEF/DRC
parsers (src/triton_bridge.cpp, scripts/evaluate_rba.py) do not yet
distinguish most of these — see ScoreComponents.unmeasured for exactly
which fields a given call filled in as 0 for lack of data, versus measured
for real. Never report contest_score without checking that list; a score
computed mostly from zeros is not comparable to a real contest submission.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field, fields
from typing import Dict, List, Optional, Tuple


# Official per-component weights (metrics_and_ranking.pdf, slide 4).
WEIGHTS = {
    "short_metal_area_per_m2_pitch": 500.0,
    "num_short_violations": 500.0,
    "num_spacing_violations": 500.0,
    "num_min_area_violations": 500.0,
    "wirelength_outside_guides_m2_pitch": 1.0,
    "num_vias_outside_guides": 1.0,
    "offtrack_wire_length_m2_pitch": 0.5,
    "num_offtrack_vias": 1.0,
    "wrongway_wire_length_m2_pitch": 1.0,
    "num_single_cut_vias": 4.0,
    "num_multi_cut_vias": 2.0,
    "total_wire_length_m2_pitch": 0.5,
}


@dataclass
class ScoreComponents:
    """One field per official metric, in M2-pitch units where the formula
    calls for it (see WEIGHTS keys). Fields left at their 0.0 default
    because a caller had no way to measure them are recorded in
    `unmeasured` by from_partial()."""
    short_metal_area_per_m2_pitch: float = 0.0
    num_short_violations: float = 0.0
    num_spacing_violations: float = 0.0
    num_min_area_violations: float = 0.0
    wirelength_outside_guides_m2_pitch: float = 0.0
    num_vias_outside_guides: float = 0.0
    offtrack_wire_length_m2_pitch: float = 0.0
    num_offtrack_vias: float = 0.0
    wrongway_wire_length_m2_pitch: float = 0.0
    num_single_cut_vias: float = 0.0
    num_multi_cut_vias: float = 0.0
    total_wire_length_m2_pitch: float = 0.0
    open_nets: int = 0            # any open net -> INVALID, not just penalized
    unmeasured: Tuple[str, ...] = field(default_factory=tuple)

    @classmethod
    def from_partial(cls, measured: Dict[str, float], open_nets: int = 0) -> "ScoreComponents":
        """Build components from whatever subset of the 12 fields the
        caller actually measured; every field not present in `measured`
        stays 0.0 and is listed in `unmeasured`."""
        valid_fields = {f.name for f in fields(cls)} - {"open_nets", "unmeasured"}
        unknown = set(measured) - valid_fields
        if unknown:
            raise ValueError(f"unknown score component(s): {sorted(unknown)}")
        unmeasured = tuple(sorted(valid_fields - set(measured)))
        return cls(**measured, open_nets=open_nets, unmeasured=unmeasured)

    def is_valid(self) -> bool:
        return self.open_nets == 0


def original_score(components: ScoreComponents) -> float:
    """Weighted sum per the official formula. Does not itself check
    is_valid() — callers must handle open_nets (INVALID) separately, per
    the spec: any open net makes the score meaningless, not just large."""
    total = 0.0
    for name, weight in WEIGHTS.items():
        total += weight * getattr(components, name)
    return total


def runtime_factor(wall_time_sec: float, median_wall_time_sec: float) -> float:
    """Runtime_factor = min(0.1, max(-0.1, 0.02 * log2(wall_time / median))).
    A router 8x faster/slower than the field median gets a +/-6% adjustment,
    per the source document's own worked example."""
    if wall_time_sec <= 0 or median_wall_time_sec <= 0:
        return 0.0
    raw = 0.02 * math.log2(wall_time_sec / median_wall_time_sec)
    return max(-0.1, min(0.1, raw))


def scaled_score(components: ScoreComponents, wall_time_sec: float,
                 median_wall_time_sec: float, nondeterministic: bool = False
                 ) -> Optional[float]:
    """Returns None (the paper's "infinity") if the solution is invalid
    (any open net) — never a finite-but-huge sentinel, so callers can't
    accidentally average it into a mean."""
    if not components.is_valid():
        return None
    penalty = 0.03 if nondeterministic else 0.0
    rt = runtime_factor(wall_time_sec, median_wall_time_sec)
    return original_score(components) * (1.0 + penalty + rt)


def rank_teams(scores: Dict[str, Dict[str, Optional[float]]]) -> Dict[str, float]:
    """Official ranking method (metrics_and_ranking.pdf, slide 8):
    scores[benchmark][team] = scaled_score or None for a failed/invalid run.

    For each benchmark, rank teams by scaled_score ascending (lower is
    better); a failed run gets the worst rank on that benchmark (tied
    among all failures, at len(teams)). Each team's per-benchmark worst
    rank is then dropped, and the remaining ranks are averaged; the team
    with the smallest average wins. Ties are broken by the average
    including the dropped worst rank.

    Returns {team: avg_rank_without_worst}, lower is better — matches the
    "Avg without the outlier" row in the source document's worked example.
    """
    teams = sorted({t for bench in scores.values() for t in bench})
    if not teams:
        return {}

    per_team_ranks: Dict[str, List[int]] = {t: [] for t in teams}
    for bench, team_scores in scores.items():
        n = len(teams)
        # Sort teams present in this benchmark by score; missing teams
        # (not run on this benchmark at all) are simply skipped for it.
        present = [t for t in teams if t in team_scores]
        ranked = sorted(present, key=lambda t: (team_scores[t] is None,
                                                team_scores[t] if team_scores[t] is not None else 0.0))
        # Standard competition ranking ("1,2,2,4"): teams tied on score
        # share the same (lower) rank, and the next distinct score's rank
        # skips ahead by the tie-group size — matches the worked example
        # in metrics_and_ranking.pdf exactly (two teams tied at 200 both
        # get rank 3, and the next team gets rank 5, not 4).
        position = 1
        i = 0
        while i < len(ranked):
            if team_scores[ranked[i]] is None:
                # all remaining are failures once we hit the first None,
                # since the sort key puts (True, ...) after every real score
                for t in ranked[i:]:
                    per_team_ranks[t].append(n)
                break
            j = i
            while j < len(ranked) and team_scores[ranked[j]] == team_scores[ranked[i]]:
                j += 1
            for t in ranked[i:j]:
                per_team_ranks[t].append(position)
            position += (j - i)
            i = j

    result = {}
    for t in teams:
        ranks = per_team_ranks[t]
        if not ranks:
            continue
        if len(ranks) > 1:
            worst = max(ranks)
            trimmed = ranks.copy()
            trimmed.remove(worst)
            result[t] = sum(trimmed) / len(trimmed)
        else:
            result[t] = float(ranks[0])
    return result


def classify_drc_markers(markers: List[dict]) -> Dict[str, float]:
    """Best-effort bucketing of RBA's generic DRCMarker list (see
    include/rba_types.h DRCType: SHORT/SPACING/ENCLOSURE/WIDTH/AREA/...)
    into the score components that actually have a matching category.
    Only SHORT/SPACING/AREA map cleanly onto official metrics; ENCLOSURE/
    WIDTH/VIA_ENCL/MIN_STEP/END_OF_LINE/OTHER have no official-metric
    equivalent and are deliberately dropped here rather than folded into
    an unrelated bucket. This does NOT give you short_metal_area (that is
    an area measurement, not a count) — it stays unmeasured.
    Each marker dict is expected to have a "type" key with a DRCType name."""
    counts = {"num_short_violations": 0.0, "num_spacing_violations": 0.0,
             "num_min_area_violations": 0.0}
    type_map = {"SHORT": "num_short_violations", "SPACING": "num_spacing_violations",
               "AREA": "num_min_area_violations"}
    for m in markers:
        key = type_map.get(str(m.get("type", "")).upper())
        if key:
            counts[key] += 1
    return counts
