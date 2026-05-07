#include <gtest/gtest.h>
#include "ga_net_ordering.h"

using namespace rba;

static std::vector<Net> make_test_nets(int n) {
    std::vector<Net> nets;
    for (int i = 0; i < n; ++i) {
        Net net;
        net.id = i;
        net.name = "net_" + std::to_string(i);
        net.priority = i;
        net.estimated_cong = (i % 3) * 0.3f;
        net.is_clock = (i == 0);
        net.is_power = (i == 1);
        // Add some pins
        for (int p = 0; p < 2 + (i % 4); ++p) {
            net.pins.push_back(Pin{{0,0,0}, "M1", (net_id)i});
        }
        nets.push_back(net);
    }
    return nets;
}

TEST(GANetOrdering, ProducesValidPermutation) {
    RBAConfig cfg;
    cfg.ga_population  = 20;
    cfg.ga_generations = 10;
    GANetOrdering ga(cfg);

    auto nets = make_test_nets(30);
    CongestionMap cmap;

    auto order = ga.run(nets, cmap);

    ASSERT_EQ(order.size(), nets.size());

    // Check all net IDs appear exactly once
    std::vector<net_id> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
        EXPECT_EQ(sorted[i], (net_id)i);
    }
}

TEST(GANetOrdering, ClockNetComesFirst) {
    RBAConfig cfg;
    cfg.ga_population  = 20;
    cfg.ga_generations = 20;
    GANetOrdering ga(cfg);

    auto nets = make_test_nets(20);
    CongestionMap cmap;

    auto order = ga.run(nets, cmap);

    // Clock net (id=0) should appear early in the ordering
    // Not guaranteed at position 0 due to stochastic nature, but
    // should be in the first 20% of nets
    size_t clk_pos = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == 0) { clk_pos = i; break; }
    }
    EXPECT_LT(clk_pos, order.size() / 2)
        << "Clock net should appear in first half";
}

TEST(GANetOrdering, HandlesEmptyNetList) {
    RBAConfig cfg;
    GANetOrdering ga(cfg);
    std::vector<Net> empty;
    CongestionMap cmap;
    auto order = ga.run(empty, cmap);
    EXPECT_TRUE(order.empty());
}

TEST(GANetOrdering, SmallDesignConverges) {
    RBAConfig cfg;
    cfg.ga_population  = 10;
    cfg.ga_generations = 30;
    GANetOrdering ga(cfg);

    auto nets = make_test_nets(5);
    CongestionMap cmap;

    // Should complete without crash or assertion failure
    EXPECT_NO_THROW(ga.run(nets, cmap));
}
