#pragma once
// PSO Cost Tuner: Particle Swarm Optimization to find the optimal
// TritonRoute cost weight vector that minimizes DRC + via count + wirelength.
//
// Each particle explores the 6-dimensional weight space.
// The fitness oracle calls TritonRoute via the bridge and reads the result.

#include "rba_types.h"
#include <random>
#include <functional>

namespace rba {

// Oracle: given a CostWeights vector, run one TritonRoute pass and return
// a RoutingSnapshot. Expensive — called at most pso_particles × pso_iterations.
using RoutingOracle = std::function<RoutingSnapshot(const CostWeights&)>;

class PSOCostTuner {
public:
    explicit PSOCostTuner(const RBAConfig& cfg, uint64_t seed = 99887);

    // Run PSO and return the best CostWeights found.
    // oracle: callable that evaluates a weight vector via TritonRoute
    // initial: starting weight vector (used to seed the swarm)
    CostWeights run(RoutingOracle oracle, const CostWeights& initial = CostWeights{});

    // Access final swarm state (for warm-starting next PSO call)
    const std::vector<CostWeights>& particles() const { return positions_; }
    const CostWeights& global_best() const { return gbest_; }
    double global_best_fitness() const { return gbest_fitness_; }

private:
    RBAConfig cfg_;
    std::mt19937_64 rng_;

    std::vector<CostWeights> positions_;
    std::vector<CostWeights> velocities_;
    std::vector<CostWeights> pbest_;
    std::vector<double>      pbest_fit_;
    CostWeights gbest_;
    double      gbest_fitness_ = std::numeric_limits<double>::max();

    // Initialize swarm around the initial position with Gaussian perturbation
    void init_swarm(const CostWeights& initial, int n);

    // Apply velocity + position update for one particle
    void update_particle(int idx, double omega, double c1, double c2);

    // Evaluate fitness via oracle, update pbest/gbest
    void evaluate_particle(int idx, RoutingOracle& oracle);

    // CostWeights arithmetic helpers (treat as 6-dim vector)
    CostWeights add(const CostWeights& a, const CostWeights& b) const;
    CostWeights sub(const CostWeights& a, const CostWeights& b) const;
    CostWeights scale(const CostWeights& a, double s) const;
    CostWeights random_scale(const CostWeights& a, double s);
};

} // namespace rba
