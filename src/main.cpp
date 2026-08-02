// RBA-TritonRoute: Main entry point.
//
// Usage:
//   rba_router --lef <tech.lef> --def <floorplan.def> --guide <global.guide>
//              [--timing <opensta.rpt>] [--output <dir>] [--threads N]
//              [--baseline-only] [--config <rba_config.json>]

#include "rba_orchestrator.h"
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static void print_usage() {
    std::cerr <<
        "Usage: rba_router [options]\n"
        "  --lef   <file>    Technology + cell LEF (required)\n"
        "  --def   <file>    Post-placement DEF (required)\n"
        "  --guide <file>    Global routing guides (required)\n"
        "  --timing <file>   OpenSTA path report for criticality weighting\n"
        "  --output <dir>    Output directory (default: ./rba_output)\n"
        "  --threads <N>     TritonRoute threads (default: 8)\n"
        "  --config <file>   JSON config overrides\n"
        "  --openroad <bin>  Path to the openroad binary (default: openroad on PATH)\n"
        "  --seed <N>        RNG seed for GA/PSO/ACO/ABC (default: each optimizer's own fixed default)\n"
        "  --ripup_fraction <f> Rip-up candidate cap as a fraction of total nets (default: 0.10)\n"
        "  --no-ga           Disable GA net ordering (ablation; uses DEF declaration order)\n"
        "  --no-pso          Disable PSO cost tuning (ablation; uses TritonRoute default weights)\n"
        "  --no-aco          Disable ACO pheromone tracking (ablation)\n"
        "  --no-abc          Disable ABC via minimization (ablation)\n"
        "  --baseline-only   Run plain TritonRoute only (no RBA net order/cost/rip-up injection)\n"
        "  --help\n";
}

static rba::RBAConfig load_config(const std::string& path) {
    rba::RBAConfig cfg;
    if (path.empty()) return cfg;

    std::ifstream f(path);
    if (!f) {
        std::cerr << "Warning: cannot open config " << path << "\n";
        return cfg;
    }

    json j;
    f >> j;

    // GA
    if (j.count("ga")) {
        cfg.ga_population     = j["ga"].value("population",     cfg.ga_population);
        cfg.ga_generations    = j["ga"].value("generations",    cfg.ga_generations);
        cfg.ga_crossover_rate = j["ga"].value("crossover_rate", cfg.ga_crossover_rate);
        cfg.ga_mutation_rate  = j["ga"].value("mutation_rate",  cfg.ga_mutation_rate);
        cfg.ga_elite_count    = j["ga"].value("elite_count",    cfg.ga_elite_count);
    }
    // ACO
    if (j.count("aco")) {
        cfg.aco_n_ants      = j["aco"].value("n_ants",      cfg.aco_n_ants);
        cfg.aco_iterations  = j["aco"].value("iterations",  cfg.aco_iterations);
        cfg.aco_alpha       = j["aco"].value("alpha",       cfg.aco_alpha);
        cfg.aco_beta        = j["aco"].value("beta",        cfg.aco_beta);
        cfg.aco_rho         = j["aco"].value("rho",         cfg.aco_rho);
    }
    // PSO
    if (j.count("pso")) {
        cfg.pso_n_particles = j["pso"].value("n_particles", cfg.pso_n_particles);
        cfg.pso_iterations  = j["pso"].value("iterations",  cfg.pso_iterations);
        cfg.pso_active_outer_iter_lo =
            j["pso"].value("active_outer_iter_lo", cfg.pso_active_outer_iter_lo);
        cfg.pso_active_outer_iter_hi =
            j["pso"].value("active_outer_iter_hi", cfg.pso_active_outer_iter_hi);
    }
    // ABC
    if (j.count("abc")) {
        cfg.abc_n_bees      = j["abc"].value("n_bees",      cfg.abc_n_bees);
        cfg.abc_max_cycles  = j["abc"].value("max_cycles",  cfg.abc_max_cycles);
        cfg.abc_limit       = j["abc"].value("limit",       cfg.abc_limit);
    }
    // General
    cfg.rba_outer_iters  = j.value("outer_iters",  cfg.rba_outer_iters);
    cfg.tr_threads       = j.value("threads",       cfg.tr_threads);
    cfg.openroad_bin     = j.value("openroad_bin",  cfg.openroad_bin);
    cfg.output_dir       = j.value("output_dir",    cfg.output_dir);
    cfg.seed             = j.value("seed",          cfg.seed);
    cfg.enable_ga        = j.value("enable_ga",     cfg.enable_ga);
    cfg.enable_pso       = j.value("enable_pso",    cfg.enable_pso);
    cfg.enable_aco       = j.value("enable_aco",    cfg.enable_aco);
    cfg.enable_abc       = j.value("enable_abc",    cfg.enable_abc);
    cfg.ripup_fraction   = j.value("ripup_fraction",cfg.ripup_fraction);

    return cfg;
}

int main(int argc, char* argv[]) {
    std::string lef, def, guide, timing, output, config_path, openroad_bin;
    bool baseline_only = false;
    int threads = 8;
    int seed_arg = -1;
    bool no_ga = false, no_pso = false, no_aco = false, no_abc = false;
    double ripup_fraction_arg = -1.0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(); return 0; }
        else if (arg == "--lef"    && i+1 < argc) lef = argv[++i];
        else if (arg == "--def"    && i+1 < argc) def = argv[++i];
        else if (arg == "--guide"  && i+1 < argc) guide = argv[++i];
        else if (arg == "--timing" && i+1 < argc) timing = argv[++i];
        else if (arg == "--output" && i+1 < argc) output = argv[++i];
        else if (arg == "--config" && i+1 < argc) config_path = argv[++i];
        else if (arg == "--threads"&& i+1 < argc) threads = std::stoi(argv[++i]);
        else if (arg == "--openroad" && i+1 < argc) openroad_bin = argv[++i];
        else if (arg == "--seed" && i+1 < argc) seed_arg = std::stoi(argv[++i]);
        else if (arg == "--ripup_fraction" && i+1 < argc) ripup_fraction_arg = std::stod(argv[++i]);
        else if (arg == "--no-ga") no_ga = true;
        else if (arg == "--no-pso") no_pso = true;
        else if (arg == "--no-aco") no_aco = true;
        else if (arg == "--no-abc") no_abc = true;
        else if (arg == "--baseline-only") baseline_only = true;
    }

    if (lef.empty() || def.empty() || guide.empty()) {
        std::cerr << "Error: --lef, --def, --guide are required\n";
        print_usage();
        return 1;
    }

    rba::RBAConfig cfg = load_config(config_path);
    if (!output.empty()) cfg.output_dir = output;
    if (!openroad_bin.empty()) cfg.openroad_bin = openroad_bin;
    cfg.tr_threads = threads;
    if (seed_arg >= 0) cfg.seed = seed_arg;
    if (ripup_fraction_arg >= 0.0) cfg.ripup_fraction = ripup_fraction_arg;
    if (no_ga)  cfg.enable_ga  = false;
    if (no_pso) cfg.enable_pso = false;
    if (no_aco) cfg.enable_aco = false;
    if (no_abc) cfg.enable_abc = false;

    std::cout << "=== RBA-TritonRoute Routing Framework ===\n";
    std::cout << "  LEF:    " << lef   << "\n";
    std::cout << "  DEF:    " << def   << "\n";
    std::cout << "  Guide:  " << guide << "\n";
    std::cout << "  Output: " << cfg.output_dir << "\n\n";

    rba::RBAOrchestrator orchestrator(cfg);

    // Written unconditionally so evaluate_rba.py can read a real router-
    // invocation count for equal-compute-budget comparisons, regardless of
    // which path below ran (see docs — "compute budget" = number of real
    // openroad invocations consumed, not runtime).
    auto write_run_summary = [&](const json& extra) {
        json j = extra;
        j["router_invocations"] = orchestrator.router_invocation_count();
        std::ofstream f(cfg.output_dir + "/run_summary.json");
        f << j.dump(2);
    };

    if (baseline_only) {
        auto snap = orchestrator.run_baseline(lef, def, guide);
        std::cout << "\n=== BASELINE RESULT ===\n";
        printf("  DRC:         %d\n",   snap.total_drc);
        printf("  Via count:   %d\n",   snap.total_via);
        printf("  Wirelength:  %.0f\n", snap.total_wirelength);
        printf("  Unrouted:    %d\n",   snap.unrouted_nets);
        printf("  Runtime:     %.1fs\n",snap.runtime_sec);
        write_run_summary({
            {"mode", "baseline"},
            {"drc_count", snap.total_drc},
            {"via_count", snap.total_via},
            {"wirelength", snap.total_wirelength},
            {"unrouted_nets", snap.unrouted_nets},
            {"runtime_sec", snap.runtime_sec},
        });
        return 0;
    }

    // Run full RBA flow
    auto result = orchestrator.run(lef, def, guide, timing);

    // Print comparison table
    std::cout << "\n=== RESULTS SUMMARY ===\n";
    std::cout << "Iter  DRC    Via      WL             Unrouted  Time\n";
    std::cout << "────  ─────  ───────  ─────────────  ────────  ──────\n";
    for (size_t i = 0; i < result.iteration_snapshots.size(); ++i) {
        const auto& s = result.iteration_snapshots[i];
        printf("%-4zu  %-5d  %-7d  %-13.0f  %-8d  %.1fs\n",
               i, s.total_drc, s.total_via, s.total_wirelength,
               s.unrouted_nets, s.runtime_sec);
    }
    printf("\nBest: DRC=%-5d via=%-7d WL=%-13.0f vias_removed=%d\n",
           result.best_snapshot.total_drc,
           result.best_snapshot.total_via,
           result.best_snapshot.total_wirelength,
           result.total_vias_removed);
    printf("Total runtime: %.1fs\n", result.total_runtime_sec);
    printf("Router invocations: %ld\n", orchestrator.router_invocation_count());

    write_run_summary({
        {"mode", "rba"},
        {"drc_count", result.best_snapshot.total_drc},
        {"via_count", result.best_snapshot.total_via},
        {"wirelength", result.best_snapshot.total_wirelength},
        {"unrouted_nets", result.best_snapshot.unrouted_nets},
        {"vias_removed", result.total_vias_removed},
        {"runtime_sec", result.total_runtime_sec},
    });

    return 0;
}
