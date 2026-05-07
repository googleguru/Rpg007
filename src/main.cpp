// RBA-TritonRoute: Main entry point.
//
// Usage:
//   rba_router --lef <tech.lef> --def <floorplan.def> --guide <global.guide>
//              [--timing <opensta.rpt>] [--output <dir>] [--threads N]
//              [--baseline-only] [--config <rba_config.json>]

#include "rba_orchestrator.h"
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
        "  --baseline-only   Run unmodified TritonRoute only\n"
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

    return cfg;
}

int main(int argc, char* argv[]) {
    std::string lef, def, guide, timing, output, config_path;
    bool baseline_only = false;
    int threads = 8;

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
        else if (arg == "--baseline-only") baseline_only = true;
    }

    if (lef.empty() || def.empty() || guide.empty()) {
        std::cerr << "Error: --lef, --def, --guide are required\n";
        print_usage();
        return 1;
    }

    rba::RBAConfig cfg = load_config(config_path);
    if (!output.empty()) cfg.output_dir = output;
    cfg.tr_threads = threads;

    std::cout << "=== RBA-TritonRoute Routing Framework ===\n";
    std::cout << "  LEF:    " << lef   << "\n";
    std::cout << "  DEF:    " << def   << "\n";
    std::cout << "  Guide:  " << guide << "\n";
    std::cout << "  Output: " << cfg.output_dir << "\n\n";

    rba::RBAOrchestrator orchestrator(cfg);

    if (baseline_only) {
        auto snap = orchestrator.run_baseline(lef, def, guide);
        std::cout << "\n=== BASELINE RESULT ===\n";
        printf("  DRC:         %d\n",   snap.total_drc);
        printf("  Via count:   %d\n",   snap.total_via);
        printf("  Wirelength:  %.0f\n", snap.total_wirelength);
        printf("  Unrouted:    %d\n",   snap.unrouted_nets);
        printf("  Runtime:     %.1fs\n",snap.runtime_sec);
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

    return 0;
}
