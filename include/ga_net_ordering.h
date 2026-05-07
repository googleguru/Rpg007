#pragma once
// GA Net Ordering: Genetic algorithm that finds a net processing sequence
// that minimizes expected routing congestion and DRC count.
//
// The GA operates on permutations of net indices. Fitness uses a lightweight
// surrogate congestion model so we don't need a full TritonRoute run per eval.

#include "rba_types.h"
#include <random>
#include <functional>

namespace rba {

// Surrogate congestion estimator: given a partial routing order, estimate
// the congestion introduced by routing net i after nets 0..i-1 are done.
// This must be supplied by the caller (e.g. wrapping TritonRoute's GCell map).
using CongestionEstimator = std::function<
    float(const std::vector<net_id>& order, size_t net_idx,
          const CongestionMap& baseline_cong)>;

class GANetOrdering {
public:
    // Chromosome: a permutation of net indices [0, n_nets)
    using Chromosome = std::vector<size_t>;

    struct Individual {
        Chromosome genes;
        double fitness;  // lower = better
        bool operator<(const Individual& o) const { return fitness < o.fitness; }
    };

    explicit GANetOrdering(const RBAConfig& cfg, uint64_t seed = 42);

    // Run GA and return the optimized net ordering as an index permutation.
    // nets: flat vector of all nets to be ordered
    // cong_map: baseline GCell congestion from global routing
    // estimator: surrogate or exact fitness oracle
    std::vector<net_id> run(
        const std::vector<Net>& nets,
        const CongestionMap& cong_map,
        CongestionEstimator estimator = nullptr);

    // Access the Pareto-optimal front from last run
    const std::vector<Individual>& last_population() const { return population_; }

private:
    RBAConfig cfg_;
    std::mt19937_64 rng_;
    std::vector<Individual> population_;

    // ── GA operators ──────────────────────────────────────────────────────

    // Initialize population with random permutations + seeded heuristics:
    //   - smallest bounding box first (easy nets early)
    //   - most pins first (complex nets get more routing freedom early)
    //   - critical path order (from timing weight)
    void initialize_population(const std::vector<Net>& nets, size_t n);

    // Order Crossover (OX): produces offspring that preserves relative order
    // from parent1 for a random subsequence, fills remainder from parent2.
    Chromosome order_crossover(const Chromosome& p1, const Chromosome& p2);

    // 2-opt swap mutation: swap two random positions in the chromosome.
    // For congestion-aware routing this is more effective than random shuffle.
    void mutate_2opt(Chromosome& c);

    // Tournament selection (k=cfg_.ga_tournament_k)
    const Individual& tournament_select() const;

    // Evaluate fitness for a chromosome given the nets and congestion map.
    double evaluate(const Chromosome& c, const std::vector<Net>& nets,
                    const CongestionMap& cmap, CongestionEstimator& est) const;

    // Built-in surrogate: sum of weighted criticality scores in chromosome order
    double surrogate_fitness(const Chromosome& c,
                             const std::vector<Net>& nets,
                             const CongestionMap& cmap) const;
};

} // namespace rba
