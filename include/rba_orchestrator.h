#pragma once
// RBA Orchestrator: Top-level controller for the bio-inspired routing framework.
//
// Execution sequence per outer iteration:
//   1. GA  → optimized net order → inject into TritonRoute
//   2. PSO → optimized cost weights → inject into TritonRoute
//   3. Run TritonRoute
//   4. Read DRC markers → ACO pheromone update
//   5. Select rip-up candidates (ACO+GA hybrid score)
//   6. Run TritonRoute rip-up/reroute on selected nets
//   7. ABC via minimization on final routes
//   8. Emit metrics snapshot

#include "rba_types.h"
#include "ga_net_ordering.h"
#include "aco_path_search.h"
#include "pso_cost_tuner.h"
#include "abc_via_minimizer.h"
#include "triton_bridge.h"
#include <cstdint>
#include <vector>
#include <string>

namespace rba {

struct OrchestratorResult {
    std::vector<RoutingSnapshot> iteration_snapshots;
    RoutingSnapshot best_snapshot;
    std::vector<net_id> best_net_order;
    CostWeights best_cost_weights;
    int total_vias_removed;
    double total_runtime_sec;
};

class RBAOrchestrator {
public:
    explicit RBAOrchestrator(const RBAConfig& cfg);

    // Main entry point: run the full RBA-TritonRoute flow.
    OrchestratorResult run(const std::string& lef,
                           const std::string& def,
                           const std::string& guide,
                           const std::string& timing_rpt = "");

    // Run only the baseline (plain TritonRoute — no RBA injection) for comparison.
    RoutingSnapshot run_baseline(const std::string& lef,
                                 const std::string& def,
                                 const std::string& guide);

    // Number of real openroad invocations consumed so far by this
    // orchestrator instance (baseline + full-flow calls combined). Used to
    // give a plain-TritonRoute tuning run the same "compute budget" RBA
    // consumed — see scripts/evaluate_rba.py equal_compute_budget handling.
    long router_invocation_count() const { return bridge_.invocation_count(); }

private:
    RBAConfig       cfg_;
    TritonBridge    bridge_;
    RoutingGraph    routing_graph_;
    std::vector<Net> nets_;
    CongestionMap   cong_map_;
    CostWeights     current_weights_;
    std::string     current_lef_file_;
    std::string     current_def_file_;
    std::string     current_guide_file_;

    // Per-iteration state
    std::vector<DRCMarker>   last_drc_markers_;
    std::vector<RoutingSnapshot> history_;

    // ── Phase drivers ──────────────────────────────────────────────────────

    // Phase 1: GA net ordering
    std::vector<net_id> run_ga_phase();

    // Phase 2: PSO cost tuning
    // Uses a lightweight oracle (1 TR pass per PSO eval for small designs,
    // surrogate for large designs)
    CostWeights run_pso_phase(const std::vector<net_id>& net_order);

    // Phase 3+4: Route + read feedback
    RoutingSnapshot run_and_read(const std::vector<net_id>& net_order,
                                 const CostWeights& weights,
                                 int iteration);

    // Phase 5: Rip-up candidate selection
    // Returns subset of net IDs to rip up based on DRC markers + congestion.
    // max_candidates = -1 (default) derives the cap from
    // cfg_.ripup_fraction * total net count instead of a hardcoded count —
    // see scripts/ripup_budget_sweep.py for sweeping that fraction.
    std::vector<net_id> select_ripup_candidates(
        const std::vector<DRCMarker>& markers,
        const RoutingSnapshot& snap,
        int max_candidates = -1);

    // Phase 6: ACO-guided reroute
    void rba_guided_reroute(const std::vector<net_id>& ripup_nets,
                            const CostWeights& weights);

    // Phase 7: ABC via minimization
    int run_abc_phase(const std::string& routed_def,
                      const std::string& optimized_def);

    // Derives a phase-specific seed from cfg_.seed (when set, >= 0) so
    // --seed/--num_seeds produce distinguishable, reproducible runs across
    // GA/PSO/ACO/ABC instead of every run reusing the same hardcoded
    // per-optimizer default. Falls back to that optimizer's own default
    // seed when cfg_.seed is unset (-1), preserving prior behavior exactly.
    uint64_t phase_seed(int phase_offset, uint64_t default_seed) const;

    // ── Rip-up scoring ────────────────────────────────────────────────────

    // Score a net for rip-up desirability:
    // Higher score = more beneficial to rip up and reroute
    double ripup_score(net_id nid, const std::vector<DRCMarker>& markers,
                       const RoutingSnapshot& snap) const;

    // ── Output helpers ────────────────────────────────────────────────────

    void write_metrics_csv(const OrchestratorResult& result,
                           const std::string& path) const;

    void print_iteration_summary(int iter,
                                 const RoutingSnapshot& snap) const;
};

} // namespace rba
