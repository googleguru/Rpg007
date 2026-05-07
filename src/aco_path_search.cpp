#include "aco_path_search.h"
#include <queue>
#include <cmath>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <stdexcept>

namespace rba {

ACOPathSearch::ACOPathSearch(const RBAConfig& cfg, const CostWeights& weights,
                             uint64_t seed)
    : cfg_(cfg), weights_(weights), rng_(seed) {}

// ─── Pheromone initialization ──────────────────────────────────────────────

void ACOPathSearch::init_pheromones(RoutingGraph& graph) {
    double tau0 = cfg_.aco_tau_min;  // start low; good paths will be reinforced
    for (auto& edge : graph.edges) {
        edge.pheromone = static_cast<float>(tau0);
    }
}

// ─── DRC penalty application ───────────────────────────────────────────────

void ACOPathSearch::apply_drc_penalty(RoutingGraph& graph,
                                      const std::vector<DRCMarker>& markers) {
    // For each DRC marker, find all edges in the marker's bounding box
    // on the marker's layer and reduce their pheromone.
    for (const auto& marker : markers) {
        for (auto& edge : graph.edges) {
            if (edge.layer != marker.layer) continue;
            // Approximate edge location from nodes
            const RoutingNode& fn = graph.nodes[graph.node_idx.at(edge.from)];
            const RoutingNode& tn = graph.nodes[graph.node_idx.at(edge.to)];
            dbu_t ex = (fn.pos.x + tn.pos.x) / 2;
            dbu_t ey = (fn.pos.y + tn.pos.y) / 2;
            if (marker.region.contains(ex, ey)) {
                edge.pheromone = static_cast<float>(
                    edge.pheromone * cfg_.aco_drc_penalty);
                clamp_pheromone(edge);
            }
        }
    }
}

// ─── Main search ──────────────────────────────────────────────────────────

Route ACOPathSearch::search(const RoutingGraph& graph,
                            node_id src, node_id dst,
                            net_id net,
                            int n_ants,
                            PathDRCChecker drc_check) {
    if (graph.node_idx.find(src) == graph.node_idx.end() ||
        graph.node_idx.find(dst) == graph.node_idx.end()) {
        Route empty; empty.net = net; empty.is_complete = false;
        return empty;
    }

    int ants = (n_ants > 0) ? n_ants : cfg_.aco_n_ants;

    // Seed with Dijkstra path for first pheromone deposit
    Route best = dijkstra_route(graph, src, dst, net, weights_);
    double best_score = best.is_complete
        ? best.wirelength + best.via_count * weights_.w_via
        : std::numeric_limits<double>::max();

    for (int iter = 0; iter < cfg_.aco_iterations; ++iter) {
        for (int a = 0; a < ants; ++a) {
            AntState ant;
            ant.current = src;
            ant.path_nodes.push_back(src);
            ant.tabu.insert(src);
            ant.total_cost = 0.0;

            bool reached = construct_path(graph, ant, dst);
            if (!reached) continue;

            Route candidate = build_route(graph, ant, net);
            int drc = drc_check ? drc_check(candidate) : 0;
            candidate.drc_count = drc;

            double score = candidate.wirelength
                         + candidate.via_count * weights_.w_via
                         + drc * weights_.w_drc_hist * 100.0;

            if (score < best_score) {
                best_score = score;
                best = candidate;
            }
        }

        // Global pheromone update after each iteration
        if (best.is_complete) {
            global_update(const_cast<RoutingGraph&>(graph), best);
        }

        // Evaporation across all edges
        for (auto& edge : const_cast<RoutingGraph&>(graph).edges) {
            edge.pheromone = static_cast<float>(
                (1.0 - cfg_.aco_rho) * edge.pheromone);
            clamp_pheromone(edge);
        }
    }

    return best;
}

// ─── Path construction ─────────────────────────────────────────────────────

bool ACOPathSearch::construct_path(const RoutingGraph& graph,
                                   AntState& ant,
                                   node_id dst) {
    // Max steps: generous bound to avoid infinite loops
    const size_t max_steps = graph.nodes.size() * 2;

    for (size_t step = 0; step < max_steps; ++step) {
        if (ant.current == dst) return true;

        auto nit = graph.node_idx.find(ant.current);
        if (nit == graph.node_idx.end()) return false;
        const RoutingNode& node = graph.nodes[nit->second];

        // Filter candidates (not in tabu)
        bool any_candidate = false;
        for (edge_id eid : node.adj) {
            auto eit = graph.edge_idx.find(eid);
            if (eit == graph.edge_idx.end()) continue;
            const RoutingEdge& e = graph.edges[eit->second];
            if (ant.tabu.count(e.to) == 0) {
                any_candidate = true;
                break;
            }
        }
        if (!any_candidate) return false;  // trapped

        edge_id chosen = select_edge(graph, ant);
        auto eit = graph.edge_idx.find(chosen);
        if (eit == graph.edge_idx.end()) return false;

        const RoutingEdge& edge = graph.edges[eit->second];

        // Local pheromone update (weakens trail slightly to encourage diversity)
        local_update(const_cast<RoutingEdge&>(edge));

        ant.path_edges.push_back(chosen);
        ant.total_cost += edge_cost(edge);
        ant.current = edge.to;
        ant.path_nodes.push_back(ant.current);
        ant.tabu.insert(ant.current);
    }

    return (ant.current == dst);
}

// ─── Edge selection ────────────────────────────────────────────────────────

edge_id ACOPathSearch::select_edge(const RoutingGraph& graph,
                                   const AntState& ant) const {
    auto nit = graph.node_idx.find(ant.current);
    const RoutingNode& node = graph.nodes[nit->second];

    std::vector<std::pair<edge_id, double>> candidates;
    double total = 0.0;

    for (edge_id eid : node.adj) {
        auto eit = graph.edge_idx.find(eid);
        if (eit == graph.edge_idx.end()) continue;
        const RoutingEdge& e = graph.edges[eit->second];
        if (ant.tabu.count(e.to)) continue;

        double tau = e.pheromone;
        double eta = heuristic(e);
        double prob = std::pow(tau, cfg_.aco_alpha)
                    * std::pow(eta, cfg_.aco_beta);
        candidates.push_back({eid, prob});
        total += prob;
    }

    if (candidates.empty()) return UINT64_MAX;

    // Roulette-wheel selection
    std::uniform_real_distribution<double> dist(0.0, total);
    double r = dist(const_cast<std::mt19937_64&>(rng_));
    double cum = 0.0;
    for (auto& [eid, prob] : candidates) {
        cum += prob;
        if (r <= cum) return eid;
    }
    return candidates.back().first;
}

// ─── Heuristic and cost ────────────────────────────────────────────────────

double ACOPathSearch::heuristic(const RoutingEdge& e) const {
    double cost = edge_cost(e);
    return 1.0 / (cost + 1e-9);
}

double ACOPathSearch::edge_cost(const RoutingEdge& e) const {
    double c = 0.0;
    c += e.wire_length * weights_.w_wire;
    c += e.is_via ? weights_.w_via : 0.0;
    c += e.congestion * weights_.w_cong;
    c += e.base_cost;
    return c;
}

// ─── Pheromone updates ─────────────────────────────────────────────────────

void ACOPathSearch::local_update(RoutingEdge& e) const {
    // MMAS local update: slightly decay to encourage exploration
    e.pheromone = static_cast<float>(
        (1.0 - 0.01) * e.pheromone + 0.01 * cfg_.aco_tau_min);
    clamp_pheromone(e);
}

void ACOPathSearch::global_update(RoutingGraph& graph, const Route& best_route) {
    if (!best_route.is_complete) return;

    double deposit = cfg_.aco_Q / (best_route.wirelength + 1.0);

    // Reinforce edges on the best route
    for (size_t i = 0; i + 1 < best_route.path.size(); ++i) {
        node_id fn = RoutingGraph::make_node_id(
            best_route.path[i].x, best_route.path[i].y, best_route.path[i].z);
        node_id tn = RoutingGraph::make_node_id(
            best_route.path[i+1].x, best_route.path[i+1].y, best_route.path[i+1].z);

        // Find edge connecting fn → tn
        auto nit = graph.node_idx.find(fn);
        if (nit == graph.node_idx.end()) continue;
        const RoutingNode& node = graph.nodes[nit->second];

        for (edge_id eid : node.adj) {
            auto eit = graph.edge_idx.find(eid);
            if (eit == graph.edge_idx.end()) continue;
            if (graph.edges[eit->second].to == tn) {
                RoutingEdge& e = graph.edges[eit->second];
                e.pheromone = static_cast<float>(e.pheromone + deposit);
                clamp_pheromone(e);
                break;
            }
        }
    }
}

void ACOPathSearch::clamp_pheromone(RoutingEdge& e) const {
    if (e.pheromone < cfg_.aco_tau_min) e.pheromone = static_cast<float>(cfg_.aco_tau_min);
    if (e.pheromone > cfg_.aco_tau_max) e.pheromone = static_cast<float>(cfg_.aco_tau_max);
}

// ─── Route builder ─────────────────────────────────────────────────────────

Route ACOPathSearch::build_route(const RoutingGraph& graph,
                                 const AntState& ant, net_id net) const {
    Route r;
    r.net = net;
    r.is_complete = (ant.current == ant.path_nodes.back());
    r.via_count = 0;
    r.wirelength = 0.0;
    r.drc_count = 0;

    for (size_t i = 0; i < ant.path_nodes.size(); ++i) {
        auto nit = graph.node_idx.find(ant.path_nodes[i]);
        if (nit == graph.node_idx.end()) continue;
        r.path.push_back(graph.nodes[nit->second].pos);
    }

    for (edge_id eid : ant.path_edges) {
        auto eit = graph.edge_idx.find(eid);
        if (eit == graph.edge_idx.end()) { r.is_via.push_back(false); continue; }
        const RoutingEdge& e = graph.edges[eit->second];
        r.is_via.push_back(e.is_via);
        if (e.is_via) ++r.via_count;
        else r.wirelength += e.wire_length;
    }

    return r;
}

// ─── Pheromone snapshot ────────────────────────────────────────────────────

std::vector<std::pair<edge_id, float>> ACOPathSearch::pheromone_snapshot(
        const RoutingGraph& graph) const {
    std::vector<std::pair<edge_id, float>> snap;
    snap.reserve(graph.edges.size());
    for (size_t i = 0; i < graph.edges.size(); ++i) {
        // Find edge_id from edge_idx reverse map
        for (auto& [eid, idx] : graph.edge_idx) {
            if (idx == i) { snap.push_back({eid, graph.edges[i].pheromone}); break; }
        }
    }
    return snap;
}

// ─── Dijkstra seeding ─────────────────────────────────────────────────────

Route dijkstra_route(const RoutingGraph& graph,
                     node_id src, node_id dst,
                     net_id net,
                     const CostWeights& weights) {
    // Standard Dijkstra on the routing graph with weighted edge cost
    using PQItem = std::pair<double, node_id>;
    std::priority_queue<PQItem, std::vector<PQItem>, std::greater<PQItem>> pq;
    std::unordered_map<node_id, double> dist;
    std::unordered_map<node_id, std::pair<node_id, edge_id>> prev;

    dist[src] = 0.0;
    pq.push({0.0, src});

    auto edge_cost_fn = [&](const RoutingEdge& e) -> double {
        return e.wire_length * weights.w_wire
             + (e.is_via ? weights.w_via : 0.0)
             + e.congestion * weights.w_cong
             + e.base_cost;
    };

    while (!pq.empty()) {
        auto [d, u] = pq.top(); pq.pop();
        if (d > dist[u]) continue;
        if (u == dst) break;

        auto nit = graph.node_idx.find(u);
        if (nit == graph.node_idx.end()) continue;
        const RoutingNode& node = graph.nodes[nit->second];

        for (edge_id eid : node.adj) {
            auto eit = graph.edge_idx.find(eid);
            if (eit == graph.edge_idx.end()) continue;
            const RoutingEdge& e = graph.edges[eit->second];
            double nd = d + edge_cost_fn(e);
            if (!dist.count(e.to) || nd < dist[e.to]) {
                dist[e.to] = nd;
                prev[e.to] = {u, eid};
                pq.push({nd, e.to});
            }
        }
    }

    Route r;
    r.net = net;
    r.via_count = 0;
    r.wirelength = 0.0;
    r.drc_count = 0;
    r.is_complete = dist.count(dst) > 0;

    if (!r.is_complete) return r;

    // Reconstruct path
    node_id cur = dst;
    std::vector<node_id> path_rev;
    std::vector<edge_id> edges_rev;
    while (cur != src) {
        path_rev.push_back(cur);
        auto& [par, eid] = prev[cur];
        edges_rev.push_back(eid);
        cur = par;
    }
    path_rev.push_back(src);
    std::reverse(path_rev.begin(), path_rev.end());
    std::reverse(edges_rev.begin(), edges_rev.end());

    for (node_id nid : path_rev) {
        auto nit = graph.node_idx.find(nid);
        if (nit != graph.node_idx.end())
            r.path.push_back(graph.nodes[nit->second].pos);
    }
    for (edge_id eid : edges_rev) {
        auto eit = graph.edge_idx.find(eid);
        if (eit == graph.edge_idx.end()) { r.is_via.push_back(false); continue; }
        const RoutingEdge& e = graph.edges[eit->second];
        r.is_via.push_back(e.is_via);
        if (e.is_via) ++r.via_count;
        else r.wirelength += e.wire_length;
    }

    return r;
}

} // namespace rba
