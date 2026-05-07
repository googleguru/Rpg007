#pragma once
// ACO Path Search: Ant Colony Optimization over the detailed routing graph.
//
// Each "ant" constructs a path from source pin to target pin for a given net.
// Pheromone trails accumulate on high-quality (DRC-free, low-congestion) edges.
// DRC markers from TritonRoute cause forced pheromone evaporation on hot edges.
//
// Implements MAX-MIN Ant System (MMAS) to prevent pheromone stagnation.

#include "rba_types.h"
#include <random>
#include <unordered_set>
#include <functional>

namespace rba {

// Callback invoked after each ant constructs a complete path.
// Allows the caller to check DRC for the proposed path without a full TR run.
using PathDRCChecker = std::function<int(const Route&)>;  // returns DRC count

class ACOPathSearch {
public:
    explicit ACOPathSearch(const RBAConfig& cfg, const CostWeights& weights,
                           uint64_t seed = 12345);

    // Initialize pheromone trails from the routing graph.
    // Must be called once before search().
    void init_pheromones(RoutingGraph& graph);

    // Update pheromone evaporation on edges covered by DRC markers.
    // Call after each TritonRoute pass with the new DRC set.
    void apply_drc_penalty(RoutingGraph& graph,
                           const std::vector<DRCMarker>& markers);

    // Find the best path from src to dst for a given net.
    // n_ants: number of ants to run (overrides cfg if > 0)
    // Returns the best Route found, or an incomplete route if unreachable.
    Route search(const RoutingGraph& graph,
                 node_id src, node_id dst,
                 net_id net,
                 int n_ants = 0,
                 PathDRCChecker drc_check = nullptr);

    // Global pheromone update: reinforce best-so-far path.
    void global_update(RoutingGraph& graph, const Route& best_route);

    // Update cost weights (called by PSO tuner between iterations).
    void set_weights(const CostWeights& w) { weights_ = w; }

    // Export pheromone snapshot for visualization / debugging.
    std::vector<std::pair<edge_id, float>> pheromone_snapshot(
        const RoutingGraph& graph) const;

private:
    RBAConfig   cfg_;
    CostWeights weights_;
    std::mt19937_64 rng_;

    // ── Single ant path construction ───────────────────────────────────────

    struct AntState {
        node_id current;
        std::vector<node_id> path_nodes;
        std::vector<edge_id> path_edges;
        double total_cost;
        std::unordered_set<node_id> tabu;  // visited nodes
    };

    // Construct one ant's path using ACO probability rule.
    // Returns true if the ant reached the destination.
    bool construct_path(const RoutingGraph& graph,
                        AntState& ant,
                        node_id dst);

    // Compute transition probability denominator (normalized) for node i.
    double compute_denom(const RoutingGraph& graph,
                         const AntState& ant) const;

    // Select next edge probabilistically: τ^α · η^β / Σ(τ^α · η^β)
    edge_id select_edge(const RoutingGraph& graph,
                        const AntState& ant) const;

    // Edge heuristic: η(e) = 1 / edge_cost(e)
    double heuristic(const RoutingEdge& e) const;

    // Edge cost combining all weighted factors
    double edge_cost(const RoutingEdge& e) const;

    // Local pheromone update (during construction, MMAS variant)
    void local_update(RoutingEdge& e) const;

    // Convert path_nodes + path_edges into a Route struct
    Route build_route(const RoutingGraph& graph,
                      const AntState& ant,
                      net_id net) const;

    // MMAS: clamp pheromone to [τ_min, τ_max]
    void clamp_pheromone(RoutingEdge& e) const;
};

// ─── Helper: Dijkstra for ACO path initialization ────────────────────────────
// Used to seed the first pheromone trail before ants run.
Route dijkstra_route(const RoutingGraph& graph,
                     node_id src, node_id dst,
                     net_id net,
                     const CostWeights& weights);

} // namespace rba
