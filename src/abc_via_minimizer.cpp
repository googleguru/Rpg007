#include "abc_via_minimizer.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <iostream>

namespace rba {

ABCViaMinimizer::ABCViaMinimizer(const RBAConfig& cfg, uint64_t seed)
    : cfg_(cfg), rng_(seed) {}

// ─── Public entry point ────────────────────────────────────────────────────

std::vector<Route> ABCViaMinimizer::run(std::vector<Route> routes,
                                         DRCValidator validator) {
    // Phase 0: greedy single-via pass (fast pre-optimization)
    routes = greedy_via_pass(routes, validator);

    extract_candidates(routes);
    if (candidates_.empty()) return routes;

    init_sources(cfg_.abc_n_bees / 2);

    std::cout << "[ABC] Starting via minimization: "
              << candidates_.size() << " via candidates, "
              << sources_.size() << " food sources\n";

    FoodSource best_source = sources_[0];
    double best_fit = compute_fitness(apply_source(routes, best_source), validator);

    for (int cycle = 0; cycle < cfg_.abc_max_cycles; ++cycle) {
        employed_bee_phase(routes, validator);
        onlooker_bee_phase(routes, validator);
        scout_bee_phase();

        // Track best source
        for (const auto& src : sources_) {
            if (src.fitness > best_fit) {
                best_fit = src.fitness;
                best_source = src;
            }
        }

        if (cycle % 20 == 0) {
            int removed = std::count(best_source.remove_flags.begin(),
                                     best_source.remove_flags.end(), true);
            std::cout << "[ABC] Cycle " << cycle
                      << " best_fitness=" << best_fit
                      << " vias_removed=" << removed << "\n";
        }
    }

    // Apply best solution
    std::vector<Route> result = apply_source(routes, best_source);
    vias_removed_ = std::count(best_source.remove_flags.begin(),
                               best_source.remove_flags.end(), true);
    std::cout << "[ABC] Final: " << vias_removed_ << " vias removed\n";
    return result;
}

// ─── Candidate extraction ──────────────────────────────────────────────────

void ABCViaMinimizer::extract_candidates(const std::vector<Route>& routes) {
    candidates_.clear();
    for (const auto& r : routes) {
        for (size_t i = 0; i < r.is_via.size() && i < r.path.size(); ++i) {
            if (r.is_via[i]) {
                // Mark as candidate if not at pin (first/last segment)
                bool at_pin = (i == 0) || (i == r.path.size() - 1);
                candidates_.push_back({r.net, i, r.path[i], !at_pin});
            }
        }
    }
}

// ─── Source initialization ─────────────────────────────────────────────────

void ABCViaMinimizer::init_sources(int n_sources) {
    sources_.resize(n_sources);
    const size_t nc = candidates_.size();
    std::uniform_int_distribution<int> flip(0, 1);

    for (auto& src : sources_) {
        src.remove_flags.resize(nc, false);
        // Random initialization: remove ~20% of non-pin vias
        for (size_t j = 0; j < nc; ++j) {
            if (candidates_[j].removable && flip(rng_) > 3) {  // 20% chance
                src.remove_flags[j] = true;
            }
        }
        src.fitness = 0.0;
        src.trial_count = 0;
    }
}

// ─── Employed bee phase ────────────────────────────────────────────────────

void ABCViaMinimizer::employed_bee_phase(std::vector<Route>& routes,
                                          DRCValidator& val) {
    std::uniform_int_distribution<size_t> idx_dist(0, candidates_.size()-1);

    for (auto& src : sources_) {
        // Try flipping one random removable via
        size_t j = idx_dist(rng_);
        if (!candidates_[j].removable) continue;

        FoodSource neighbor = src;
        neighbor.remove_flags[j] = !neighbor.remove_flags[j];

        auto candidate_routes = apply_source(routes, neighbor);
        double new_fit = compute_fitness(candidate_routes, val);

        if (new_fit > src.fitness) {
            src = neighbor;
            src.fitness = new_fit;
            src.trial_count = 0;
        } else {
            ++src.trial_count;
        }
    }
}

// ─── Onlooker bee phase ────────────────────────────────────────────────────

void ABCViaMinimizer::onlooker_bee_phase(std::vector<Route>& routes,
                                          DRCValidator& val) {
    // Compute selection probabilities proportional to fitness
    double total_fit = 0.0;
    for (const auto& src : sources_) total_fit += src.fitness + 1e-9;

    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<size_t> idx_dist(0, candidates_.size()-1);

    for (int bee = 0; bee < (int)sources_.size(); ++bee) {
        // Roulette-wheel select a food source
        double r = prob(rng_) * total_fit;
        double cum = 0.0;
        size_t chosen = 0;
        for (size_t s = 0; s < sources_.size(); ++s) {
            cum += sources_[s].fitness + 1e-9;
            if (r <= cum) { chosen = s; break; }
        }

        // Exploit chosen source: try a different via removal
        size_t j = idx_dist(rng_);
        if (!candidates_[j].removable) continue;

        FoodSource neighbor = sources_[chosen];
        neighbor.remove_flags[j] = !neighbor.remove_flags[j];

        auto candidate_routes = apply_source(routes, neighbor);
        double new_fit = compute_fitness(candidate_routes, val);

        if (new_fit > sources_[chosen].fitness) {
            sources_[chosen] = neighbor;
            sources_[chosen].fitness = new_fit;
            sources_[chosen].trial_count = 0;
        }
    }
}

// ─── Scout bee phase ──────────────────────────────────────────────────────

void ABCViaMinimizer::scout_bee_phase() {
    const size_t nc = candidates_.size();
    std::uniform_int_distribution<int> flip(0, 4);

    for (auto& src : sources_) {
        if (src.trial_count >= cfg_.abc_limit) {
            // Abandon and reinitialize
            src.remove_flags.assign(nc, false);
            for (size_t j = 0; j < nc; ++j) {
                if (candidates_[j].removable && flip(rng_) == 0)
                    src.remove_flags[j] = true;
            }
            src.fitness = 0.0;
            src.trial_count = 0;
        }
    }
}

// ─── Apply source to routes ────────────────────────────────────────────────

std::vector<Route> ABCViaMinimizer::apply_source(
        const std::vector<Route>& routes, const FoodSource& src) const {
    // Build a set of (net, path_idx) to remove
    std::unordered_map<net_id, std::vector<size_t>> to_remove;
    for (size_t j = 0; j < src.remove_flags.size() && j < candidates_.size(); ++j) {
        if (src.remove_flags[j]) {
            to_remove[candidates_[j].net].push_back(candidates_[j].path_idx);
        }
    }

    std::vector<Route> result = routes;
    for (auto& r : result) {
        auto it = to_remove.find(r.net);
        if (it == to_remove.end()) continue;
        for (size_t pidx : it->second) {
            if (pidx < r.is_via.size() && r.is_via[pidx]) {
                r.is_via[pidx] = false;
                --r.via_count;
                // Merge the two segments on same layer (simplified: just mark removal)
                // A real implementation would recalculate the wire geometry here
            }
        }
    }
    return result;
}

// ─── Fitness computation ───────────────────────────────────────────────────

double ABCViaMinimizer::compute_fitness(const std::vector<Route>& routes,
                                         DRCValidator& validator) const {
    int total_via = 0;
    for (const auto& r : routes) total_via += r.via_count;

    int new_drc = validator(routes);
    // Higher fitness = fewer vias + no new DRCs
    return 1.0 / (1.0 + total_via + new_drc * 1000.0);
}

// ─── Greedy pre-pass ──────────────────────────────────────────────────────

std::vector<Route> ABCViaMinimizer::greedy_via_pass(
        std::vector<Route> routes, DRCValidator& validator) const {
    bool improved = true;
    int pass = 0;

    while (improved) {
        improved = false;
        ++pass;
        for (auto& r : routes) {
            for (size_t i = 0; i < r.is_via.size(); ++i) {
                if (!r.is_via[i]) continue;
                if (i == 0 || i == r.path.size()-1) continue; // skip pin vias

                // Try removing this via
                r.is_via[i] = false;
                --r.via_count;

                int drc = validator(routes);
                if (drc > 0) {
                    // Restore — introduced a DRC
                    r.is_via[i] = true;
                    ++r.via_count;
                } else {
                    improved = true;
                }
            }
        }
        std::cout << "[ABC/greedy] Pass " << pass << "\n";
    }
    return routes;
}

} // namespace rba
