#include "pso_cost_tuner.h"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace rba {

PSOCostTuner::PSOCostTuner(const RBAConfig& cfg, uint64_t seed)
    : cfg_(cfg), rng_(seed) {}

// ─── Public entry point ────────────────────────────────────────────────────

CostWeights PSOCostTuner::run(RoutingOracle oracle, const CostWeights& initial) {
    const int n = cfg_.pso_n_particles;
    init_swarm(initial, n);

    pbest_     = positions_;
    pbest_fit_.assign(n, std::numeric_limits<double>::max());
    gbest_fitness_ = std::numeric_limits<double>::max();

    // Evaluate initial positions
    std::cout << "[PSO] Evaluating initial swarm (" << n << " particles)...\n";
    for (int i = 0; i < n; ++i) {
        evaluate_particle(i, oracle);
    }
    std::cout << "[PSO] Initial gbest fitness: " << gbest_fitness_ << "\n";

    for (int iter = 0; iter < cfg_.pso_iterations; ++iter) {
        // Linearly decay inertia weight ω over iterations
        double omega = cfg_.pso_omega
                     - (cfg_.pso_omega - 0.4) * iter / cfg_.pso_iterations;

        for (int i = 0; i < n; ++i) {
            update_particle(i, omega, cfg_.pso_c1, cfg_.pso_c2);
            evaluate_particle(i, oracle);
        }

        if (iter % 5 == 0) {
            std::cout << "[PSO] Iter " << iter
                      << " gbest=" << gbest_fitness_
                      << " (w_wire=" << gbest_.w_wire
                      << " w_via=" << gbest_.w_via
                      << " w_cong=" << gbest_.w_cong << ")\n";
        }
    }

    return gbest_;
}

// ─── Initialization ────────────────────────────────────────────────────────

void PSOCostTuner::init_swarm(const CostWeights& initial, int n) {
    positions_.resize(n);
    velocities_.resize(n);

    std::normal_distribution<float> noise(0.0f, 0.5f);
    std::uniform_real_distribution<float> vel_init(-0.5f, 0.5f);

    positions_[0] = initial;  // seed first particle at initial
    velocities_[0] = CostWeights{};

    for (int i = 1; i < n; ++i) {
        CostWeights w = initial;
        w.w_wire      += noise(rng_);
        w.w_via       += noise(rng_);
        w.w_cong      += noise(rng_);
        w.w_drc_hist  += noise(rng_);
        w.w_layer_pref+= noise(rng_);
        w.w_timing    += noise(rng_);
        w.clamp();
        positions_[i] = w;

        velocities_[i] = CostWeights{
            vel_init(rng_), vel_init(rng_), vel_init(rng_),
            vel_init(rng_), vel_init(rng_), vel_init(rng_)
        };
    }
}

// ─── Particle update ──────────────────────────────────────────────────────

void PSOCostTuner::update_particle(int idx, double omega, double c1, double c2) {
    std::uniform_real_distribution<double> r01(0.0, 1.0);
    double r1 = r01(rng_), r2 = r01(rng_);

    // v = ω·v + c1·r1·(pbest-x) + c2·r2·(gbest-x)
    CostWeights& v = velocities_[idx];
    CostWeights& x = positions_[idx];

    v = add(scale(v, omega),
        add(random_scale(sub(pbest_[idx], x), c1 * r1),
            random_scale(sub(gbest_, x),     c2 * r2)));

    // Velocity clamping: max change per step = 2.0
    auto vclamp = [](float v){ return v > 2.0f ? 2.0f : v < -2.0f ? -2.0f : v; };
    v.w_wire = vclamp(v.w_wire); v.w_via = vclamp(v.w_via);
    v.w_cong = vclamp(v.w_cong); v.w_drc_hist = vclamp(v.w_drc_hist);
    v.w_layer_pref = vclamp(v.w_layer_pref); v.w_timing = vclamp(v.w_timing);

    x = add(x, v);
    x.clamp();
}

void PSOCostTuner::evaluate_particle(int idx, RoutingOracle& oracle) {
    RoutingSnapshot snap = oracle(positions_[idx]);
    double fit = snap.fitness();

    if (fit < pbest_fit_[idx]) {
        pbest_fit_[idx] = fit;
        pbest_[idx] = positions_[idx];
    }
    if (fit < gbest_fitness_) {
        gbest_fitness_ = fit;
        gbest_ = positions_[idx];
        std::cout << "[PSO] New gbest: " << gbest_fitness_
                  << " (drc=" << snap.total_drc
                  << " via=" << snap.total_via << ")\n";
    }
}

// ─── Vector arithmetic ─────────────────────────────────────────────────────

CostWeights PSOCostTuner::add(const CostWeights& a, const CostWeights& b) const {
    return {a.w_wire+b.w_wire, a.w_via+b.w_via, a.w_cong+b.w_cong,
            a.w_drc_hist+b.w_drc_hist, a.w_layer_pref+b.w_layer_pref,
            a.w_timing+b.w_timing};
}

CostWeights PSOCostTuner::sub(const CostWeights& a, const CostWeights& b) const {
    return {a.w_wire-b.w_wire, a.w_via-b.w_via, a.w_cong-b.w_cong,
            a.w_drc_hist-b.w_drc_hist, a.w_layer_pref-b.w_layer_pref,
            a.w_timing-b.w_timing};
}

CostWeights PSOCostTuner::scale(const CostWeights& a, double s) const {
    return {static_cast<float>(a.w_wire*s), static_cast<float>(a.w_via*s),
            static_cast<float>(a.w_cong*s), static_cast<float>(a.w_drc_hist*s),
            static_cast<float>(a.w_layer_pref*s), static_cast<float>(a.w_timing*s)};
}

CostWeights PSOCostTuner::random_scale(const CostWeights& a, double s) {
    // s is already c_i * r_i, so this is just scalar multiply
    return scale(a, s);
}

} // namespace rba
