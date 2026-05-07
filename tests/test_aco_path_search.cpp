#include <gtest/gtest.h>
#include "aco_path_search.h"

using namespace rba;

// Build a tiny 3×3 grid routing graph on 1 layer
static RoutingGraph make_grid_graph(int nx, int ny, int layers = 1) {
    RoutingGraph g;
    const dbu_t pitch = 100;

    // Create nodes
    for (int z = 0; z < layers; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                RoutingNode node;
                node.pos = {x * pitch, y * pitch, (layer_t)z};
                node_id nid = RoutingGraph::make_node_id(x*pitch, y*pitch, z);
                g.node_idx[nid] = g.nodes.size();
                g.nodes.push_back(node);
            }
        }
    }

    // Create edges (horizontal + vertical on each layer)
    edge_id eid = 0;
    for (int z = 0; z < layers; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                node_id cur = RoutingGraph::make_node_id(x*pitch, y*pitch, z);
                size_t cur_idx = g.node_idx.at(cur);

                auto add_edge = [&](int tx, int ty, int tz, bool is_via) {
                    if (tx < 0 || tx >= nx || ty < 0 || ty >= ny) return;
                    if (tz < 0 || tz >= layers) return;
                    node_id to = RoutingGraph::make_node_id(tx*pitch, ty*pitch, tz);
                    RoutingEdge e;
                    e.from = cur; e.to = to;
                    e.layer = z;
                    e.is_via = is_via;
                    e.wire_length = is_via ? 0 : pitch;
                    e.base_cost = 1.0f;
                    e.pheromone = 1.0f;
                    e.congestion = 0.0f;
                    g.edge_idx[eid] = g.edges.size();
                    g.nodes[cur_idx].adj.push_back(eid);
                    g.edges.push_back(e);
                    ++eid;
                };

                add_edge(x+1, y, z, false);  // right
                add_edge(x-1, y, z, false);  // left
                add_edge(x, y+1, z, false);  // up
                add_edge(x, y-1, z, false);  // down
                if (z+1 < layers) add_edge(x, y, z+1, true);  // via up
                if (z-1 >= 0)     add_edge(x, y, z-1, true);  // via down
            }
        }
    }
    return g;
}

TEST(ACOPathSearch, FindsPathOnSimpleGrid) {
    RBAConfig cfg;
    cfg.aco_n_ants    = 5;
    cfg.aco_iterations = 10;
    CostWeights weights;

    ACOPathSearch aco(cfg, weights);
    auto graph = make_grid_graph(4, 4, 1);
    aco.init_pheromones(graph);

    const dbu_t p = 100;
    node_id src = RoutingGraph::make_node_id(0, 0, 0);
    node_id dst = RoutingGraph::make_node_id(3*p, 3*p, 0);

    Route route = aco.search(graph, src, dst, 0);
    EXPECT_TRUE(route.is_complete);
    EXPECT_GT(route.path.size(), 1u);
    EXPECT_EQ(route.path.front().x, 0);
    EXPECT_EQ(route.path.back().x, 3*p);
}

TEST(ACOPathSearch, PheromoneInitialization) {
    RBAConfig cfg;
    CostWeights weights;
    ACOPathSearch aco(cfg, weights);
    auto graph = make_grid_graph(3, 3, 1);
    aco.init_pheromones(graph);

    for (const auto& e : graph.edges) {
        EXPECT_NEAR(e.pheromone, cfg.aco_tau_min, 1e-6f);
    }
}

TEST(ACOPathSearch, DRCPenaltyReducesPheromone) {
    RBAConfig cfg;
    CostWeights weights;
    ACOPathSearch aco(cfg, weights);
    auto graph = make_grid_graph(3, 3, 1);
    aco.init_pheromones(graph);

    // Set all pheromones to mid-range
    for (auto& e : graph.edges) e.pheromone = 1.0f;

    // Add a DRC marker covering the entire graph
    DRCMarker m;
    m.region = {0, 0, 300, 300};
    m.layer = 0;
    m.type = DRCType::SHORT;
    m.severity = 1.0f;
    m.net1 = m.net2 = DRCMarker::INVALID;

    aco.apply_drc_penalty(graph, {m});

    // All pheromones should be reduced
    for (const auto& e : graph.edges) {
        EXPECT_LT(e.pheromone, 1.0f);
        EXPECT_GE(e.pheromone, cfg.aco_tau_min);
    }
}

TEST(ACOPathSearch, UnreachableReturnsIncomplete) {
    RBAConfig cfg;
    cfg.aco_n_ants    = 3;
    cfg.aco_iterations = 5;
    CostWeights weights;
    ACOPathSearch aco(cfg, weights);

    // Empty graph
    RoutingGraph g;
    aco.init_pheromones(g);

    Route r = aco.search(g,
        RoutingGraph::make_node_id(0, 0, 0),
        RoutingGraph::make_node_id(100, 100, 0),
        0);
    EXPECT_FALSE(r.is_complete);
}
