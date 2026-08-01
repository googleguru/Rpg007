# RBA-TritonRoute: Resilient Bio-Inspired Algorithm Routing Framework

## 1. Problem Statement

Detailed routing in advanced VLSI nodes (7nm and below) faces combinatorial explosion in
design-rule complexity, routing congestion, and DRC iteration counts. TritonRoute's
negotiation-based rip-up/reroute converges reliably but its cost functions and net-ordering
heuristics are static — they cannot adapt to per-benchmark congestion topology or
dynamically penalize recurring DRC hotspots.

The **RBA-TritonRoute framework** wraps TritonRoute with a bio-inspired optimization layer
that:
- Reorders nets using a Genetic Algorithm (GA) to front-load hard-to-route nets
- Guides path search via Ant Colony Optimization (ACO) with pheromone trails seeded by
  successful historical routes
- Tunes congestion cost weights using Particle Swarm Optimization (PSO) across routing
  iterations
- Minimizes via count post-route using Artificial Bee Colony (ABC) search
- Selects rip-up candidates using a hybrid ACO+GA fitness model

This preserves TritonRoute's full DRC rule deck and LEF/DEF compatibility while adding an
adaptive decision layer above it.

---

## 2. System Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                     RBA Orchestrator (rba_orchestrator.cpp)         │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────┐  ┌──────────┐  │
│  │  GA Net      │  │  PSO Cost    │  │  ACO Path │  │  ABC Via │  │
│  │  Ordering    │  │  Tuner       │  │  Search   │  │  Minimizer│  │
│  │  (ga_net_    │  │  (pso_cost_  │  │  (aco_    │  │  (abc_   │  │
│  │  ordering)   │  │  tuner)      │  │  search)  │  │  via_min)│  │
│  └──────┬───────┘  └──────┬───────┘  └─────┬─────┘  └────┬─────┘  │
│         └─────────────────┼────────────────┼──────────────┘        │
│                           ▼                ▼                        │
│              ┌────────────────────────────────┐                    │
│              │   TritonRoute Bridge            │                    │
│              │   (triton_bridge.cpp)           │                    │
│              │   - inject net order            │                    │
│              │   - override cost weights       │                    │
│              │   - read DRC markers            │                    │
│              │   - read congestion maps        │                    │
│              │   - read/write route guides     │                    │
│              └──────────────┬─────────────────┘                    │
└─────────────────────────────┼───────────────────────────────────────┘
                              ▼
              ┌───────────────────────────────┐
              │   TritonRoute / OpenROAD       │
              │   - FlexDR (detailed router)   │
              │   - FlexPA (pin access)        │
              │   - FlexGC (geometry checker)  │
              │   - FlexRP (route planning)    │
              └───────────────────────────────┘
```

### Data Flow

```
LEF/DEF + Route Guides
        │
        ▼
[RBA Orchestrator] ──────────────────────────────────────────────────┐
        │                                                             │
        ├─ Phase 1: GA Net Ordering                                   │
        │     Population of net permutations                         │
        │     Fitness = estimated congestion + critical path delay    │
        │     Output: ordered net list → injected into TritonRoute    │
        │                                                             │
        ├─ Phase 2: PSO Cost Initialization                          │
        │     Swarm of (wire_cost, via_cost, congestion_weight)       │
        │     vectors across TritonRoute iteration space              │
        │     Output: initial cost weights for FlexDR                 │
        │                                                             │
        ├─ Phase 3: ACO-Guided Routing Iteration                      │
        │     Per-net path search on routing graph                    │
        │     Pheromone ∝ route quality (WL, via, DRC-free)          │
        │     Feedback: DRC markers → pheromone evaporation           │
        │                                                             │
        ├─ Phase 4: Rip-Up Candidate Selection (ACO+GA hybrid)        │
        │     Score nets by (DRC_count × congestion × fanout)         │
        │     GA selects rip-up set that maximizes expected repair     │
        │                                                             │
        └─ Phase 5: ABC Via Minimization                              │
              Post-route via reduction without DRC introduction       │
              Employed/onlooker/scout bee phases                      │
              Output: minimized DEF with updated via locations        │
```

---

## 3. TritonRoute Integration Points

| Signal              | Direction     | RBA Use                                     |
|---------------------|---------------|---------------------------------------------|
| `net_order[]`       | RBA → TR      | GA-optimized net processing sequence        |
| `cost_weights{}`    | RBA → TR      | PSO-tuned (wire, via, congestion) weights   |
| `drc_markers[]`     | TR → RBA      | Pheromone evaporation hotspots (ACO)        |
| `congestion_map[]`  | TR → RBA      | PSO particle fitness evaluation             |
| `route_guides[]`    | TR ↔ RBA      | ACO path constraints + via-free corridor id |
| `routing_graph`     | TR → RBA      | ACO pheromone graph substrate               |
| `via_locations[]`   | RBA → TR      | ABC-minimized via placement                 |

---

## 4. Algorithm Design

### 4.1 GA Net Ordering

**Encoding:** Chromosome = permutation of net indices `[n₀, n₁, ..., nₖ]`

**Fitness Function:**
```
F(chromosome) = w₁·ΣCongestion(nᵢ | n₀..nᵢ₋₁ routed)
              + w₂·ΣCriticalPathDelay(nᵢ)
              + w₃·EstimatedDRC(nᵢ)
              - w₄·RoutabilityScore(nᵢ)
```

**Operators:**
- Selection: Tournament (k=5) — avoids fitness scaling issues
- Crossover: Order Crossover (OX) — preserves relative ordering invariant
- Mutation: 2-opt swap (5% rate) — locally improves congestion-critical swaps
- Elitism: Top 10% preserved each generation

**Convergence:** Stop when ΔF < ε for 20 consecutive generations, or gen_max reached

### 4.2 ACO Path Search

**Graph:** G = (V, E) where V = routing grid intersections, E = track segments + vias
**Pheromone:** τ(e) initialized to τ₀ = 1/n_nets
**Heuristic:** η(e) = 1 / (wire_cost(e) + via_cost(e) + congestion_cost(e))

**Path Construction:**
```
P(eᵢⱼ | current=i) = [τ(eᵢⱼ)^α · η(eᵢⱼ)^β] / Σ[τ(eᵢₖ)^α · η(eᵢₖ)^β]
```
- α = 1.0 (pheromone weight), β = 2.0 (heuristic weight)
- Tabu list prevents revisiting within single ant path

**Pheromone Update:**
```
τ(e) ← (1-ρ)·τ(e) + Σ Δτₐ(e)
Δτₐ(e) = Q/L(path_a)  if e ∈ path_a, DRC-free
         = 0           otherwise
```
- ρ = 0.1 (evaporation rate), Q = quality constant
- DRC markers ⟹ forced evaporation: τ(e) ← τ(e) · DRC_penalty_factor

### 4.3 PSO Congestion Cost Tuning

**Particle:** x = (w_wire, w_via, w_cong, w_drc_hist, w_layer_pref)
**Velocity Update:**
```
v ← ω·v + c₁·r₁·(pbest - x) + c₂·r₂·(gbest - x)
x ← x + v
```
- ω = 0.729, c₁ = 1.494, c₂ = 1.494 (standard Clerc constants)
- Bounds: all weights ∈ [0.1, 10.0]
- Fitness: -(DRC_count + 0.1·via_count + 0.01·total_wirelength) after one TR pass

### 4.4 ABC Via Minimization

**Food source:** S = set of via placement decisions (keep / substitute with wire jog)
**Fitness:** fit(S) = 1 / (1 + via_count(S) + DRC_introduced(S) × 1000)

**Phases:**
1. **Employed bee:** Local search — try removing one via at a time, check DRC
2. **Onlooker bee:** Select food source by probability p(S) = fit(S) / Σfit
3. **Scout bee:** Abandon exhausted sources, random restart with different jog strategy

**Limit:** If a food source is not improved in `limit` trials, scout replaces it

---

## 5. Evaluation Methodology

The evaluation flow in this repository is designed to satisfy the manuscript requirements listed below:

- Report absolute DRC counts, via counts, wirelength, runtime, and contest score for every benchmark.
- Compare baseline TritonRoute and RBA-TritonRoute under identical wall-clock and equal-compute budgets.
- Report multi-seed summaries with best, worst, mean, and standard deviation for each benchmark.
- Add convergence analysis from the iteration-wise metrics emitted by the orchestrator.
- Preserve provenance data for OpenROAD/TritonRoute version and Git commit used for each experiment.

The analysis script [scripts/evaluate_rba.py](../scripts/evaluate_rba.py) writes:
- [results/experiment_report.json](../results/experiment_report.json) for benchmark-level absolute statistics and deltas,
- [results/convergence_summary.json](../results/convergence_summary.json) for iteration-wise convergence traces,
- and [results/summary.csv](../results/summary.csv) for a compact table suitable for manuscript inclusion.

### 5.1 Experimental Comparisons

The report builder computes:
- mean, std, min, max, and sample count for each metric,
- percentage deltas between baseline and RBA for each benchmark,
- equal-runtime winner selection using the same wall-clock budget,
- equal-compute-budget winner selection using a comparable routing invocation budget,
- and convergence summaries over outer iterations.

### 5.2 Contest-Score Evaluation

When a contest score is available for a routed DEF, it is carried through the evaluation pipeline and included in the report. The contest score is treated as an additional absolute metric alongside DRC, via count, wirelength, and runtime, enabling direct comparison against the contest metric rather than only normalized ratios.

### 5.3 Reproducibility Package

The reproducibility package includes:
- [Dockerfile](../Dockerfile) for a repeatable runtime environment,
- [scripts/rba_config_ispd18.json](../scripts/rba_config_ispd18.json) and [scripts/rba_config_sky130.json](../scripts/rba_config_sky130.json) for the parameter sidecars,
- [scripts/sky130_route.tcl](../scripts/sky130_route.tcl) and the Tcl wrappers used by the bridge layer,
- and benchmark preparation guidance from [scripts/setup_ispd_benchmarks.sh](../scripts/setup_ispd_benchmarks.sh).

For every run, the operator should record the exact OpenROAD/TritonRoute binary path, Git commit, and Tcl/API sequence used in the run log and preserve it alongside the output directory.

### Benchmarks
- ISPD 2018 Detailed Routing Contest (ispd18_test1–10)
- ISPD 2019 Obstacle-Aware Routing (ispd19_test1–9)
- ISPD 2021 Routability-Driven Placement benchmarks (routed)
- DAC 2012 / ICCAD 2012 routing benchmarks (for comparison baseline)

### Metrics

| Metric                  | Measurement Method                                |
|-------------------------|---------------------------------------------------|
| Total Wirelength (WL)   | Sum of routed net lengths (DEF parser)            |
| Via Count               | Count of VIA statements in output DEF             |
| DRC Violation Count     | TritonRoute FlexGC output after final route       |
| Routing Success Rate    | % of nets fully routed (0 unrouted pins)          |
| Runtime (seconds)       | Wall clock time for full flow                     |
| Memory Peak (MB)        | `/usr/bin/time -v` RSS peak                       |
| RBA Overhead (seconds)  | Bio-inspired computation time only                |

### Comparison Baseline
- Vanilla TritonRoute with default parameters
- Same LEF/DEF/guide inputs
- Same machine, single-threaded for fairness (multi-threaded separately noted)

### Statistical Validation
- 5 independent runs per benchmark (GA/PSO/ACO have stochastic elements)
- Report mean ± std for all metrics
- Wilcoxon signed-rank test for significance

---

## 6. Expected Challenges and Mitigations

| Challenge                              | Mitigation                                          |
|----------------------------------------|-----------------------------------------------------|
| TritonRoute not designed for injection | Use OpenROAD Tcl API + programmatic guide generation|
| ACO graph too large (100M+ nodes)      | Hierarchical ACO on congestion clusters only        |
| GA fitness requires TR run (slow)      | Lightweight surrogate model (congestion estimator)  |
| PSO weight space discontinuous         | Restart strategy + penalty for degenerate weights   |
| ABC DRC checking is TR-dependent       | Batch via removal candidates, single TR-GC call     |
| Convergence premature (ACO)            | Pheromone bounds [τ_min, τ_max] (MAX-MIN AS)        |
| Net ordering changes timing            | Tie-break by timing criticality from OpenSTA        |

---

## 7. Future Improvements

1. **Reinforcement Learning layer:** Replace PSO cost tuning with a DQN agent that learns
   cost adjustments across benchmark classes (transfer learning across technology nodes)
2. **GPU-accelerated ACO:** Parallelize ant path construction on CUDA for 100M+ node graphs
3. **Multi-objective Pareto front:** Simultaneously optimize (WL, via, DRC) as Pareto set
   instead of weighted sum — expose trade-off curves to the designer
4. **Technology-aware pheromone seeding:** Pre-train ACO pheromone maps on training circuits,
   transfer to unseen benchmarks (few-shot routing)
5. **Timing-driven extension:** Integrate OpenSTA slack into GA fitness and ACO heuristic η(e)
6. **Power-aware via selection:** Model via resistance/current density in ABC fitness function
7. **Layout symmetry exploitation:** Detect repeated circuit structures, clone optimal routes
