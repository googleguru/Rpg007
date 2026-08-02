#!/usr/bin/env python3
"""
Dr.CU adapter — DOCUMENTED STUB, NOT A REAL INTEGRATION.

Referee 1 Q4 asks for Dr.CU as a second baseline detailed router alongside
TritonRoute. Dr.CU (github.com/cuhk-eda/dr-cu — "Dr. CU, VLSI Detailed
Routing Tool Developed by CUHK", Chen/Pui/Li/Young, ASPDAC'19 + TCAD'20 +
ICCAD'19) is a separate C++ codebase with its own build system (CMake +
Boost + a trimmed Rsyn, optionally Cadence Innovus for its own eval step).
Building and integrating it for real is another from-source compile on top
of the OpenROAD patch build already deferred in this repo (see
docs/INTEGRATION.md) — out of scope for this pass by explicit choice, to
keep that already-large effort bounded. This file exists so the *shape* of
the integration is real and reviewable even though the integration itself
is not: every function below either raises NotImplementedError or is a
thin, honestly-labeled wrapper around a real CLI documented from Dr.CU's
own README (fetched and read directly from
https://raw.githubusercontent.com/cuhk-eda/dr-cu/master/README.md while
writing this file — the flags below are not guessed).

Real Dr.CU CLI (confirmed from its README, section 2.1):
    ispd18dr -lef <tech+cell.lef> -def <placed.def> -guide <routing.guide> \\
              -output <solution.def> -threads <N>

No -output_drc / DRC-report flag is documented for the raw binary in that
README — Dr.CU's own DRC evaluation goes through its run.py wrapper's
`eval` step, which requires a Cadence Innovus license
("Innovus®, version 17.1, optional, for design rule checking and
evaluation"). That is a real, separate integration cost: getting DRC/via/
wirelength numbers out of Dr.CU comparable to RBA-TritonRoute's own
metrics either needs Innovus, or a from-scratch parser for whatever
solution-DEF format `ispd18dr -output` produces — neither is done here.

To make this real:
  1. Build Dr.CU: `git clone https://github.com/cuhk-eda/dr-cu && cd dr-cu
     && scripts/build.py -o release` (needs GCC>=5.5, CMake>=2.8,
     Boost>=1.58; Innovus optional for its own eval step).
  2. Replace run_drcu_baseline() below with a real subprocess.run(...) call
     shaped like run_baseline()/run_rba() in evaluate_rba.py.
  3. Replace parse_drcu_solution() with a real parser for Dr.CU's solution
     DEF (likely reusable as-is from evaluate_rba.py's own
     parse_def_metrics(), since both tools emit standard DEF) and a real
     DRC counter (either an Innovus-based check_drc equivalent, or a
     from-scratch geometric checker matching Dr.CU's rule set).
  4. Wire both into evaluate_rba.py as a third `method` value ("drcu")
     alongside "baseline" and "rba", extending BenchmarkResult.method's
     implicit enum and every place that currently assumes exactly two
     methods (build_experiment_report's `bl`/`rb` split, compare_methods,
     print_comparison_table, plot_results).
"""

from dataclasses import dataclass
from typing import Optional


DRCU_REPO_URL = "https://github.com/cuhk-eda/dr-cu"


@dataclass
class DrCuRunConfig:
    lef_file: str
    def_file: str
    guide_file: str
    output_def: str
    threads: int = 8
    drcu_bin: str = "ispd18dr"  # matches the real binary name in Dr.CU's `run` dir


def build_drcu_command(cfg: DrCuRunConfig) -> list:
    """Returns the real Dr.CU CLI invocation for `cfg` — this part IS
    accurate (transcribed from Dr.CU's own README), even though nothing
    in this file actually runs it yet."""
    return [
        cfg.drcu_bin,
        "-lef", cfg.lef_file,
        "-def", cfg.def_file,
        "-guide", cfg.guide_file,
        "-output", cfg.output_def,
        "-threads", str(cfg.threads),
    ]


def run_drcu_baseline(cfg: DrCuRunConfig) -> "Optional[dict]":
    """NOT IMPLEMENTED. Would subprocess.run(build_drcu_command(cfg), ...)
    and return a dict shaped like evaluate_rba.BenchmarkResult, matching
    run_baseline()/run_rba()'s contract. Raises unconditionally so this
    stub can never be silently mistaken for a working integration —
    calling code must catch NotImplementedError and skip Dr.CU comparison
    rather than getting a fabricated result."""
    raise NotImplementedError(
        "Dr.CU integration is a documented stub (see this file's module "
        f"docstring) — build Dr.CU from {DRCU_REPO_URL} and implement this "
        "function for real before using it as a baseline.")


def parse_drcu_solution(solution_def: str) -> "Optional[dict]":
    """NOT IMPLEMENTED. Would parse Dr.CU's -output solution DEF into
    {wirelength, via_count, unrouted_nets} at minimum, and DRC counts if
    an Innovus-free checker is written (see module docstring, item 3)."""
    raise NotImplementedError(
        "Dr.CU solution-DEF parsing is not implemented — see this file's "
        "module docstring for what's needed.")
