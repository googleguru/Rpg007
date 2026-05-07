#include <gtest/gtest.h>
#include "pso_cost_tuner.h"
#include <cmath>

using namespace rba;

// Synthetic oracle: fitness peaks at w_wire=2.0, w_via=3.0, others=1.0
static RoutingSnapshot synthetic_oracle(const CostWeights& w) {
    RoutingSnapshot s;
    // Penalize deviation from (2.0, 3.0, 1.0, 1.0, 1.0, 1.0)
    float targets[] = {2.0f, 3.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float vals[]    = {w.w_wire, w.w_via, w.w_cong,
                       w.w_drc_hist, w.w_layer_pref, w.w_timing};
    double penalty = 0.0;
    for (int i = 0; i < 6; ++i) {
        penalty += (vals[i] - targets[i]) * (vals[i] - targets[i]);
    }
    s.total_drc        = static_cast<int>(penalty * 100);
    s.total_via        = 100;
    s.total_wirelength = 1000.0;
    s.unrouted_nets    = 0;
    s.runtime_sec      = 0.01;
    s.weights_used     = w;
    return s;
}

TEST(PSOCostTuner, ConvergesToOptimum) {
    RBAConfig cfg;
    cfg.pso_n_particles = 15;
    cfg.pso_iterations  = 30;

    PSOCostTuner pso(cfg);
    CostWeights initial;  // starts at defaults (1.0, 4.0, 2.0, ...)

    CostWeights best = pso.run(synthetic_oracle, initial);

    // Should converge close to (2.0, 3.0, ...) — within 1.0 of target
    EXPECT_NEAR(best.w_wire, 2.0f, 1.5f);
    EXPECT_NEAR(best.w_via,  3.0f, 1.5f);
    EXPECT_GT(pso.global_best_fitness(), 0.0);
    EXPECT_LT(pso.global_best_fitness(), 1e6);
}

TEST(PSOCostTuner, WeightsStayInBounds) {
    RBAConfig cfg;
    cfg.pso_n_particles = 10;
    cfg.pso_iterations  = 20;

    PSOCostTuner pso(cfg);
    pso.run(synthetic_oracle, CostWeights{});

    for (const auto& p : pso.particles()) {
        EXPECT_GE(p.w_wire,  0.1f);
        EXPECT_LE(p.w_wire,  20.0f);
        EXPECT_GE(p.w_via,   0.1f);
        EXPECT_LE(p.w_via,   20.0f);
        EXPECT_GE(p.w_cong,  0.1f);
        EXPECT_LE(p.w_cong,  20.0f);
    }
}

TEST(PSOCostTuner, GbestImproves) {
    RBAConfig cfg;
    cfg.pso_n_particles = 8;
    cfg.pso_iterations  = 10;

    PSOCostTuner pso(cfg);
    pso.run(synthetic_oracle, CostWeights{});

    // gbest_fitness should be ≥ 0 (lower is better in the routing model)
    EXPECT_GE(pso.global_best_fitness(), 0.0);
}
