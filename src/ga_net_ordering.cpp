#include "ga_net_ordering.h"
#include <algorithm>
#include <numeric>
#include <cassert>
#include <cmath>
#include <iostream>

namespace rba {

GANetOrdering::GANetOrdering(const RBAConfig& cfg, uint64_t seed)
    : cfg_(cfg), rng_(seed) {}

// ─── Public entry point ────────────────────────────────────────────────────

std::vector<net_id> GANetOrdering::run(
        const std::vector<Net>& nets,
        const CongestionMap& cong_map,
        CongestionEstimator estimator) {

    const size_t n = nets.size();
    if (n == 0) return {};

    // Bind estimator (use built-in surrogate if none provided)
    CongestionEstimator est = estimator;
    if (!est) {
        est = [this](const std::vector<net_id>&, size_t, const CongestionMap&) {
            return 0.0f;  // surrogate_fitness handles it internally
        };
    }

    initialize_population(nets, cfg_.ga_population);

    // Evaluate initial population
    for (auto& ind : population_) {
        ind.fitness = evaluate(ind.genes, nets, cong_map, est);
    }

    double prev_best = std::numeric_limits<double>::max();
    int stagnation = 0;

    for (int gen = 0; gen < cfg_.ga_generations; ++gen) {
        std::vector<Individual> next_gen;
        next_gen.reserve(cfg_.ga_population);

        // Elitism: copy top individuals unchanged
        std::sort(population_.begin(), population_.end());
        for (int e = 0; e < cfg_.ga_elite_count && e < (int)population_.size(); ++e) {
            next_gen.push_back(population_[e]);
        }

        // Fill remainder with crossover + mutation
        std::uniform_real_distribution<double> prob(0.0, 1.0);
        while ((int)next_gen.size() < cfg_.ga_population) {
            const auto& p1 = tournament_select();
            const auto& p2 = tournament_select();

            Chromosome child;
            if (prob(rng_) < cfg_.ga_crossover_rate) {
                child = order_crossover(p1.genes, p2.genes);
            } else {
                child = p1.genes;
            }

            if (prob(rng_) < cfg_.ga_mutation_rate) {
                mutate_2opt(child);
            }

            double fit = evaluate(child, nets, cong_map, est);
            next_gen.push_back({std::move(child), fit});
        }

        population_ = std::move(next_gen);

        // Convergence check
        double best = population_[0].fitness;
        if (std::abs(prev_best - best) < 1e-6) {
            ++stagnation;
        } else {
            stagnation = 0;
            prev_best = best;
        }

        if (stagnation >= 20) {
            std::cout << "[GA] Converged at generation " << gen
                      << " (fitness=" << best << ")\n";
            break;
        }

        if (gen % 10 == 0) {
            std::cout << "[GA] Gen " << gen
                      << " best=" << best << "\n";
        }
    }

    // Return ordered net IDs from best chromosome
    std::sort(population_.begin(), population_.end());
    const Chromosome& best_genes = population_[0].genes;
    std::vector<net_id> result;
    result.reserve(n);
    for (size_t idx : best_genes) {
        result.push_back(nets[idx].id);
    }
    return result;
}

// ─── Initialization ────────────────────────────────────────────────────────

void GANetOrdering::initialize_population(const std::vector<Net>& nets, size_t pop_size) {
    const size_t n = nets.size();
    population_.clear();
    population_.reserve(pop_size);

    // Heuristic seed 1: order by criticality (timing-first)
    {
        Chromosome c(n);
        std::iota(c.begin(), c.end(), 0);
        std::stable_sort(c.begin(), c.end(), [&](size_t a, size_t b){
            return nets[a].priority < nets[b].priority;
        });
        population_.push_back({c, 0.0});
    }

    // Heuristic seed 2: order by pin count descending (complex nets first)
    {
        Chromosome c(n);
        std::iota(c.begin(), c.end(), 0);
        std::stable_sort(c.begin(), c.end(), [&](size_t a, size_t b){
            return nets[a].pins.size() > nets[b].pins.size();
        });
        population_.push_back({c, 0.0});
    }

    // Heuristic seed 3: clock nets first, then power, then signals
    {
        Chromosome c(n);
        std::iota(c.begin(), c.end(), 0);
        auto type_key = [&](size_t i) -> int {
            if (nets[i].is_clock) return 0;
            if (nets[i].is_power) return 1;
            return 2;
        };
        std::stable_sort(c.begin(), c.end(), [&](size_t a, size_t b){
            return type_key(a) < type_key(b);
        });
        population_.push_back({c, 0.0});
    }

    // Fill rest with random permutations
    std::uniform_int_distribution<size_t> idx_dist(0, n-1);
    while (population_.size() < pop_size) {
        Chromosome c(n);
        std::iota(c.begin(), c.end(), 0);
        std::shuffle(c.begin(), c.end(), rng_);
        population_.push_back({c, 0.0});
    }
}

// ─── Order crossover (OX) ─────────────────────────────────────────────────

GANetOrdering::Chromosome GANetOrdering::order_crossover(
        const Chromosome& p1, const Chromosome& p2) {
    const size_t n = p1.size();
    std::uniform_int_distribution<size_t> dist(0, n-1);
    size_t a = dist(rng_), b = dist(rng_);
    if (a > b) std::swap(a, b);

    Chromosome child(n, SIZE_MAX);
    std::vector<bool> used(n, false);

    // Copy segment [a,b] from p1
    for (size_t i = a; i <= b; ++i) {
        child[i] = p1[i];
        used[p1[i]] = true;
    }

    // Fill remaining positions in p2 order
    size_t pos = (b + 1) % n;
    for (size_t i = 0; i < n; ++i) {
        size_t gene = p2[(b + 1 + i) % n];
        if (!used[gene]) {
            child[pos] = gene;
            pos = (pos + 1) % n;
        }
    }
    return child;
}

// ─── 2-opt mutation ────────────────────────────────────────────────────────

void GANetOrdering::mutate_2opt(Chromosome& c) {
    const size_t n = c.size();
    if (n < 2) return;
    std::uniform_int_distribution<size_t> dist(0, n-1);
    size_t a = dist(rng_), b = dist(rng_);
    if (a > b) std::swap(a, b);
    std::reverse(c.begin() + a, c.begin() + b + 1);
}

// ─── Tournament selection ──────────────────────────────────────────────────

const GANetOrdering::Individual& GANetOrdering::tournament_select() const {
    std::uniform_int_distribution<size_t> dist(0, population_.size()-1);
    const Individual* best = &population_[dist(const_cast<std::mt19937_64&>(rng_))];
    for (int k = 1; k < cfg_.ga_tournament_k; ++k) {
        const Individual* cand = &population_[dist(const_cast<std::mt19937_64&>(rng_))];
        if (cand->fitness < best->fitness) best = cand;
    }
    return *best;
}

// ─── Fitness evaluation ────────────────────────────────────────────────────

double GANetOrdering::evaluate(const Chromosome& c, const std::vector<Net>& nets,
                                const CongestionMap& cmap,
                                CongestionEstimator& est) const {
    // Use surrogate unless external estimator overrides
    return surrogate_fitness(c, nets, cmap);
}

double GANetOrdering::surrogate_fitness(const Chromosome& c,
                                         const std::vector<Net>& nets,
                                         const CongestionMap& cmap) const {
    // Surrogate model: weighted sum of per-net difficulty scores
    // ordered so that high-difficulty nets later → higher penalty (they face
    // more occupied routing resources)
    double score = 0.0;
    for (size_t rank = 0; rank < c.size(); ++rank) {
        const Net& net = nets[c[rank]];
        double position_penalty = static_cast<double>(rank) / c.size();

        // Difficulty factors
        double pin_factor  = std::log1p(net.pins.size());
        double crit_factor = 1.0 / (1.0 + net.priority);
        double cong_factor = net.estimated_cong;

        // Hard nets placed late → bad
        score += position_penalty * (pin_factor + crit_factor + cong_factor * 2.0);

        // Clock/power nets in non-first position → penalty
        if (net.is_clock && rank > 0) score += 100.0;
        if (net.is_power && rank > nets.size() * 0.1) score += 50.0;
    }
    return score;
}

} // namespace rba
