#pragma once
// ABC Via Minimizer: Artificial Bee Colony optimization to reduce via count
// in a completed routing solution without introducing new DRC violations.
//
// Food source = a set of via-removal decisions.
// Bees search for via alternatives (wire jogs on same layer) and validate
// them against the DRC rule deck via a lightweight geometry checker.

#include "rba_types.h"
#include <random>
#include <functional>

namespace rba {

// DRC validator: given a modified route set, return DRC count introduced
// (0 = no new DRCs). Must be fast — called O(bees × cycles) times.
using DRCValidator = std::function<int(const std::vector<Route>&)>;

struct ViaCandidate {
    net_id  net;
    size_t  path_idx;   // index in Route::path where this via occurs
    Point3D via_loc;
    bool    removable;  // can be replaced by a jog on lower/upper metal
};

class ABCViaMinimizer {
public:
    explicit ABCViaMinimizer(const RBAConfig& cfg, uint64_t seed = 55443);

    // Run ABC on the given route set and return optimized routes.
    // validator: DRC oracle for proposed via removal sets
    // Returns the modified route set (vias reduced where DRC-safe).
    std::vector<Route> run(std::vector<Route> routes,
                           DRCValidator validator);

    int vias_removed() const { return vias_removed_; }

private:
    RBAConfig cfg_;
    std::mt19937_64 rng_;
    int vias_removed_ = 0;

    // ── Food source representation ─────────────────────────────────────────
    struct FoodSource {
        std::vector<bool>  remove_flags;  // one per via candidate
        double             fitness;       // 1 / (1 + via_count + drc * 1000)
        int                trial_count;   // how many cycles without improvement
    };

    std::vector<FoodSource> sources_;
    std::vector<ViaCandidate> candidates_;

    // ── Phases ────────────────────────────────────────────────────────────

    void extract_candidates(const std::vector<Route>& routes);
    void init_sources(int n_sources);

    // Employed bee: local search — try flipping one remove_flag
    void employed_bee_phase(std::vector<Route>& routes, DRCValidator& val);

    // Onlooker bee: probability-based exploitation of good sources
    void onlooker_bee_phase(std::vector<Route>& routes, DRCValidator& val);

    // Scout bee: replace abandoned sources with random restart
    void scout_bee_phase();

    // Apply remove_flags to routes and return modified copy
    std::vector<Route> apply_source(const std::vector<Route>& routes,
                                    const FoodSource& src) const;

    // Compute fitness of a route set
    double compute_fitness(const std::vector<Route>& routes,
                           DRCValidator& validator) const;

    // Greedy single-via removal: try removing each via, keep if DRC-free
    std::vector<Route> greedy_via_pass(std::vector<Route> routes,
                                       DRCValidator& validator) const;
};

} // namespace rba
