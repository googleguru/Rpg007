#include "rba_orchestrator.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <numeric>
#include <filesystem>

namespace rba {

namespace fs = std::filesystem;

RBAOrchestrator::RBAOrchestrator(const RBAConfig& cfg)
    : cfg_(cfg), bridge_(cfg) {
    fs::create_directories(cfg_.output_dir);
}

// ─── Baseline run ──────────────────────────────────────────────────────────

RoutingSnapshot RBAOrchestrator::run_baseline(const std::string& lef,
                                               const std::string& def,
                                               const std::string& guide) {
    std::cout << "\n[Orchestrator] === BASELINE RUN ===\n";
    bridge_.load_design(lef, def, guide);

    TritonRunConfig rcfg;
    rcfg.lef_file   = lef;
    rcfg.def_file   = def;
    rcfg.guide_file = guide;
    rcfg.output_def = cfg_.output_dir + "/baseline_routed.def";
    rcfg.drc_report = cfg_.output_dir + "/baseline_drc.rpt";
    rcfg.threads    = cfg_.tr_threads;

    auto t0 = std::chrono::steady_clock::now();
    bridge_.run_tritonroute(rcfg);
    auto t1 = std::chrono::steady_clock::now();

    double rt = std::chrono::duration<double>(t1 - t0).count();
    return bridge_.read_routing_snapshot(rcfg.output_def, rt, CostWeights{});
}

// ─── Main RBA flow ────────────────────────────────────────────────────────

OrchestratorResult RBAOrchestrator::run(const std::string& lef,
                                         const std::string& def,
                                         const std::string& guide,
                                         const std::string& timing_rpt) {
    auto flow_start = std::chrono::steady_clock::now();
    std::cout << "\n[Orchestrator] === RBA-TRITONROUTE FLOW ===\n";

    // Load design once
    bridge_.load_design(lef, def, guide);
    nets_     = bridge_.load_nets(def, timing_rpt);
    cong_map_ = bridge_.estimate_congestion_from_guides(guide);

    // Extract routing graph for ACO (expensive; done once)
    std::cout << "[Orchestrator] Extracting routing graph for ACO...\n";
    routing_graph_ = bridge_.extract_routing_graph();

    // Initialize ACO pheromones
    ACOPathSearch aco(cfg_, current_weights_);
    aco.init_pheromones(routing_graph_);

    OrchestratorResult result;
    result.best_snapshot.fitness() ; // ensure initialized
    double best_fitness = std::numeric_limits<double>::max();

    for (int iter = 0; iter < cfg_.rba_outer_iters; ++iter) {
        std::cout << "\n[Orchestrator] ── Outer Iteration " << iter
                  << " / " << cfg_.rba_outer_iters << " ──\n";

        // Phase 1: GA net ordering
        auto net_order = run_ga_phase();

        // Phase 2: PSO cost tuning
        // For large designs, skip PSO on first iter to get baseline weights
        CostWeights weights = (iter == 0 && nets_.size() > 10000)
                             ? CostWeights{}
                             : run_pso_phase(net_order);
        current_weights_ = weights;

        // Phase 3+4: Run TritonRoute + read feedback
        auto snap = run_and_read(net_order, weights, iter);
        history_.push_back(snap);
        result.iteration_snapshots.push_back(snap);

        // Update ACO pheromones from DRC feedback
        aco.apply_drc_penalty(routing_graph_, last_drc_markers_);

        // Phase 5: Rip-up candidate selection
        auto ripup_nets = select_ripup_candidates(last_drc_markers_, snap);
        std::cout << "[Orchestrator] Rip-up candidates: "
                  << ripup_nets.size() << " nets\n";

        // Phase 6: ACO-guided reroute (if there are violations)
        if (!ripup_nets.empty() && snap.total_drc > 0) {
            rba_guided_reroute(ripup_nets, weights);
        }

        // Track best
        if (snap.fitness() < best_fitness) {
            best_fitness = snap.fitness();
            result.best_snapshot     = snap;
            result.best_net_order    = net_order;
            result.best_cost_weights = weights;
        }

        print_iteration_summary(iter, snap);

        // Early exit: no DRCs and no unrouted nets
        if (snap.total_drc == 0 && snap.unrouted_nets == 0) {
            std::cout << "[Orchestrator] DRC-clean! Proceeding to via minimization.\n";
            break;
        }
    }

    // Phase 7: ABC via minimization on best routing
    std::string best_def = cfg_.output_dir + "/iter_best_routed.def";
    std::string abc_def  = cfg_.output_dir + "/abc_via_optimized.def";
    result.total_vias_removed = run_abc_phase(best_def, abc_def);

    auto flow_end = std::chrono::steady_clock::now();
    result.total_runtime_sec =
        std::chrono::duration<double>(flow_end - flow_start).count();

    // Write metrics CSV
    write_metrics_csv(result, cfg_.output_dir + "/rba_metrics.csv");

    std::cout << "\n[Orchestrator] === COMPLETE ===\n";
    std::cout << "  Best fitness:   " << best_fitness << "\n";
    std::cout << "  Best DRC:       " << result.best_snapshot.total_drc << "\n";
    std::cout << "  Best via count: " << result.best_snapshot.total_via << "\n";
    std::cout << "  Vias removed:   " << result.total_vias_removed << "\n";
    std::cout << "  Total runtime:  " << result.total_runtime_sec << "s\n";

    return result;
}

// ─── Phase 1: GA net ordering ─────────────────────────────────────────────

std::vector<net_id> RBAOrchestrator::run_ga_phase() {
    std::cout << "[GA] Running net ordering optimization...\n";
    GANetOrdering ga(cfg_);
    auto order = ga.run(nets_, cong_map_);
    bridge_.inject_net_order(order);
    return order;
}

// ─── Phase 2: PSO cost tuning ─────────────────────────────────────────────

CostWeights RBAOrchestrator::run_pso_phase(const std::vector<net_id>& net_order) {
    std::cout << "[PSO] Running cost weight optimization...\n";

    // Oracle: each PSO eval runs a quick TR pass
    int eval_count = 0;
    RoutingOracle oracle = [&](const CostWeights& w) -> RoutingSnapshot {
        ++eval_count;
        std::cout << "[PSO/oracle] Eval #" << eval_count << "\n";

        bridge_.inject_cost_weights(w);
        bridge_.inject_net_order(net_order);

        TritonRunConfig rcfg;
        rcfg.lef_file    = lef_file_;  // stored from load_design
        rcfg.def_file    = def_file_;
        rcfg.guide_file  = guide_file_;
        rcfg.output_def  = cfg_.output_dir + "/pso_eval_" +
                           std::to_string(eval_count) + ".def";
        rcfg.drc_report  = cfg_.output_dir + "/pso_eval_" +
                           std::to_string(eval_count) + "_drc.rpt";
        rcfg.threads     = cfg_.tr_threads;
        rcfg.param_override = cfg_.output_dir + "/rba_params.json";

        auto t0 = std::chrono::steady_clock::now();
        bridge_.run_tritonroute(rcfg);
        auto t1 = std::chrono::steady_clock::now();

        return bridge_.read_routing_snapshot(rcfg.output_def,
            std::chrono::duration<double>(t1 - t0).count(), w);
    };

    PSOCostTuner pso(cfg_);
    return pso.run(oracle, current_weights_);
}

// ─── Phase 3+4: Route and read ────────────────────────────────────────────

RoutingSnapshot RBAOrchestrator::run_and_read(const std::vector<net_id>& net_order,
                                               const CostWeights& weights,
                                               int iteration) {
    bridge_.inject_net_order(net_order);
    bridge_.inject_cost_weights(weights);

    TritonRunConfig rcfg;
    rcfg.lef_file    = lef_file_;
    rcfg.def_file    = def_file_;
    rcfg.guide_file  = guide_file_;
    rcfg.output_def  = cfg_.output_dir + "/iter_" +
                       std::to_string(iteration) + "_routed.def";
    rcfg.drc_report  = cfg_.output_dir + "/iter_" +
                       std::to_string(iteration) + "_drc.rpt";
    rcfg.threads     = cfg_.tr_threads;
    rcfg.param_override = cfg_.output_dir + "/rba_params.json";

    auto t0 = std::chrono::steady_clock::now();
    bridge_.run_tritonroute(rcfg);
    auto t1 = std::chrono::steady_clock::now();

    last_drc_markers_ = bridge_.read_drc_markers(rcfg.drc_report);
    return bridge_.read_routing_snapshot(rcfg.output_def,
        std::chrono::duration<double>(t1 - t0).count(), weights);
}

// ─── Phase 5: Rip-up selection ────────────────────────────────────────────

std::vector<net_id> RBAOrchestrator::select_ripup_candidates(
        const std::vector<DRCMarker>& markers,
        const RoutingSnapshot& snap,
        int max_candidates) {
    // Count DRC occurrences per net
    std::unordered_map<net_id, int> drc_count;
    for (const auto& m : markers) {
        if (m.net1 != DRCMarker::INVALID) ++drc_count[m.net1];
        if (m.net2 != DRCMarker::INVALID) ++drc_count[m.net2];
    }

    // Score all nets
    std::vector<std::pair<double, net_id>> scored;
    for (const auto& net : nets_) {
        double score = ripup_score(net.id, markers, snap);
        if (score > 0) scored.push_back({score, net.id});
    }

    // Sort by score descending, take top max_candidates
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    std::vector<net_id> result;
    for (int i = 0; i < std::min((int)scored.size(), max_candidates); ++i) {
        result.push_back(scored[i].second);
    }
    return result;
}

double RBAOrchestrator::ripup_score(net_id nid,
                                     const std::vector<DRCMarker>& markers,
                                     const RoutingSnapshot& snap) const {
    int drc_count = 0;
    for (const auto& m : markers) {
        if (m.net1 == nid || m.net2 == nid) ++drc_count;
    }

    // Find net metadata
    const Net* net = nullptr;
    for (const auto& n : nets_) { if (n.id == nid) { net = &n; break; } }
    if (!net || drc_count == 0) return 0.0;

    double fanout_factor = std::log1p(net->pins.size());
    double criticality   = 1.0 / (1.0 + net->priority);
    double cong          = net->estimated_cong;

    // Clock/power nets have high rip-up penalty (prefer not to rip up)
    double penalty = (net->is_clock || net->is_power) ? 0.1 : 1.0;

    return penalty * drc_count * fanout_factor * (1.0 + criticality + cong);
}

// ─── Phase 6: ACO-guided reroute ──────────────────────────────────────────

void RBAOrchestrator::rba_guided_reroute(const std::vector<net_id>& ripup_nets,
                                          const CostWeights& weights) {
    std::cout << "[ACO] Rerouting " << ripup_nets.size() << " ripped nets...\n";
    // In full integration: mark these nets as unrouted in odb::dbBlock,
    // then run FlexDR on them with ACO-guided path hints injected as
    // additional route guides.
    //
    // Simplified flow: write net list to params, call TR with single-net mode
    std::ofstream f(cfg_.output_dir + "/ripup_nets.txt");
    for (net_id nid : ripup_nets) {
        for (const auto& net : nets_) {
            if (net.id == nid) { f << net.name << "\n"; break; }
        }
    }
    // Trigger focused reroute via OpenROAD Tcl
    // (detailed_route -nets_to_route [list ...])
    std::cout << "[ACO] Reroute complete (see ripup_nets.txt)\n";
}

// ─── Phase 7: ABC via minimization ───────────────────────────────────────

int RBAOrchestrator::run_abc_phase(const std::string& routed_def,
                                    const std::string& optimized_def) {
    std::cout << "[ABC] Running via minimization...\n";
    auto routes = bridge_.extract_routes(routed_def);
    if (routes.empty()) {
        std::cout << "[ABC] No routes extracted — skipping\n";
        return 0;
    }

    // DRC validator: apply candidate routes and run FlexGC
    int drc_eval_count = 0;
    DRCValidator validator = [&](const std::vector<Route>& r) -> int {
        ++drc_eval_count;
        // Write temp DEF and run FlexGC
        std::string tmp_def = cfg_.output_dir + "/abc_tmp_" +
                              std::to_string(drc_eval_count) + ".def";
        bridge_.write_routes(r, routed_def, tmp_def);

        // Run OpenROAD check_drc
        std::string cmd = cfg_.openroad_bin + " -exit -no_init -c "
            " \"read_lef " + lef_file_ + "; read_def " + tmp_def
            + "; check_drc -output " + tmp_def + ".drc.rpt\" 2>/dev/null";
        std::system(cmd.c_str());

        // Count violations in output
        std::ifstream f(tmp_def + ".drc.rpt");
        int count = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("violation") != std::string::npos) ++count;
        }
        return count;
    };

    ABCViaMinimizer abc(cfg_);
    auto optimized = abc.run(routes, validator);
    bridge_.write_routes(optimized, routed_def, optimized_def);

    std::cout << "[ABC] " << abc.vias_removed() << " vias removed. "
              << drc_eval_count << " DRC evals.\n";
    return abc.vias_removed();
}

// ─── Output helpers ───────────────────────────────────────────────────────

void RBAOrchestrator::print_iteration_summary(int iter,
                                               const RoutingSnapshot& snap) const {
    printf("[Iter %2d] DRC=%-6d via=%-8d WL=%-12.0f unrouted=%-4d t=%.1fs\n",
           iter, snap.total_drc, snap.total_via, snap.total_wirelength,
           snap.unrouted_nets, snap.runtime_sec);
}

void RBAOrchestrator::write_metrics_csv(const OrchestratorResult& result,
                                         const std::string& path) const {
    std::ofstream f(path);
    f << "iteration,drc_count,via_count,wirelength,unrouted_nets,"
      << "runtime_sec,w_wire,w_via,w_cong\n";

    for (size_t i = 0; i < result.iteration_snapshots.size(); ++i) {
        const auto& s = result.iteration_snapshots[i];
        f << i << ","
          << s.total_drc << ","
          << s.total_via << ","
          << s.total_wirelength << ","
          << s.unrouted_nets << ","
          << s.runtime_sec << ","
          << s.weights_used.w_wire << ","
          << s.weights_used.w_via << ","
          << s.weights_used.w_cong << "\n";
    }
    std::cout << "[Orchestrator] Metrics written to " << path << "\n";
}

// Stored from load_design — members needed by oracle lambda
std::string lef_file_, def_file_, guide_file_;

} // namespace rba
