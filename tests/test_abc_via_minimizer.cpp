#include <gtest/gtest.h>
#include "abc_via_minimizer.h"

using namespace rba;

static std::vector<Route> make_test_routes(int n_routes, int vias_each) {
    std::vector<Route> routes;
    for (int i = 0; i < n_routes; ++i) {
        Route r;
        r.net = i;
        r.is_complete = true;
        r.drc_count = 0;
        r.wirelength = 1000.0;
        r.via_count = 0;

        // Simple path: (0,0,0) → (10,0,1) → (20,0,1) with vias interspersed
        int layer = 0;
        for (int j = 0; j <= vias_each * 2; ++j) {
            Point3D p{(dbu_t)(j * 10), 0, (layer_t)layer};
            r.path.push_back(p);
            if (j > 0) {
                bool via = (j % 2 == 1);  // odd steps are vias
                r.is_via.push_back(via);
                if (via) { ++r.via_count; layer = 1 - layer; }
            }
        }
        routes.push_back(r);
    }
    return routes;
}

TEST(ABCViaMinimizer, ReducesViaCount) {
    RBAConfig cfg;
    cfg.abc_n_bees    = 10;
    cfg.abc_max_cycles = 20;
    cfg.abc_limit     = 5;

    ABCViaMinimizer abc(cfg);

    auto routes = make_test_routes(3, 4);
    int initial_vias = 0;
    for (const auto& r : routes) initial_vias += r.via_count;

    // Validator that always returns 0 DRCs (all via removals are safe)
    DRCValidator val = [](const std::vector<Route>&) -> int { return 0; };

    auto result = abc.run(routes, val);

    int final_vias = 0;
    for (const auto& r : result) final_vias += r.via_count;

    EXPECT_LE(final_vias, initial_vias);
    EXPECT_GE(abc.vias_removed(), 0);
}

TEST(ABCViaMinimizer, RespectsStrictDRCValidator) {
    RBAConfig cfg;
    cfg.abc_n_bees    = 10;
    cfg.abc_max_cycles = 10;

    ABCViaMinimizer abc(cfg);
    auto routes = make_test_routes(2, 2);
    int initial_vias = 0;
    for (const auto& r : routes) initial_vias += r.via_count;

    // Validator that rejects ALL via removals
    DRCValidator strict_val = [](const std::vector<Route>&) -> int { return 99; };

    auto result = abc.run(routes, strict_val);
    int final_vias = 0;
    for (const auto& r : result) final_vias += r.via_count;

    // No vias should be removed
    EXPECT_EQ(final_vias, initial_vias);
    EXPECT_EQ(abc.vias_removed(), 0);
}

TEST(ABCViaMinimizer, HandlesEmptyRoutes) {
    RBAConfig cfg;
    ABCViaMinimizer abc(cfg);
    DRCValidator val = [](const std::vector<Route>&) -> int { return 0; };

    EXPECT_NO_THROW(abc.run({}, val));
    EXPECT_EQ(abc.vias_removed(), 0);
}
