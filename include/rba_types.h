#pragma once
// RBA-TritonRoute: Core type definitions for the bio-inspired routing framework.
// All physical coordinates use TritonRoute's integer DBU (database units) system.

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <limits>

namespace rba {

// ─── Coordinate types ────────────────────────────────────────────────────────

using dbu_t   = int32_t;   // database units (typically 1nm or 0.5nm per unit)
using layer_t = uint8_t;   // metal layer index (0=M1, 1=M2, ...)
using net_id  = uint32_t;
using node_id = uint64_t;  // encodes (x, y, layer) as packed integer
using edge_id = uint64_t;

struct Point3D {
    dbu_t x, y;
    layer_t z;
    bool operator==(const Point3D& o) const { return x==o.x && y==o.y && z==o.z; }
};

struct BBox {
    dbu_t xl, yl, xh, yh;
    bool contains(dbu_t x, dbu_t y) const {
        return x>=xl && x<=xh && y>=yl && y<=yh;
    }
    dbu_t area() const { return (xh-xl) * (yh-yl); }
};

// ─── Routing graph ────────────────────────────────────────────────────────────

struct RoutingEdge {
    node_id from, to;
    layer_t layer;
    bool is_via;
    dbu_t  wire_length;   // 0 for vias
    float  base_cost;     // LUT-populated from TritonRoute's cost table
    float  pheromone;     // ACO: τ(e), initialized to τ₀
    float  congestion;    // normalized [0,1]: from TritonRoute GCell map
};

struct RoutingNode {
    Point3D pos;
    std::vector<edge_id> adj;  // outgoing edges
};

struct RoutingGraph {
    std::vector<RoutingNode> nodes;
    std::vector<RoutingEdge> edges;
    std::unordered_map<node_id, size_t> node_idx;
    std::unordered_map<edge_id, size_t> edge_idx;

    // Pack (x/track_pitch, y/track_pitch, layer) into node_id
    static node_id make_node_id(dbu_t x, dbu_t y, layer_t z) {
        return (static_cast<node_id>(z) << 48)
             | (static_cast<node_id>(static_cast<uint32_t>(x)) << 16)
             | static_cast<node_id>(static_cast<uint16_t>(y));
    }
};

// ─── Net representation ───────────────────────────────────────────────────────

struct Pin {
    Point3D  loc;
    std::string layer_name;
    net_id   net;
};

struct Net {
    net_id          id;
    std::string     name;
    std::vector<Pin> pins;
    int             priority;      // timing criticality rank (0=most critical)
    float           estimated_cong;
    bool            is_clock;
    bool            is_power;
};

struct Route {
    net_id              net;
    std::vector<Point3D> path;     // ordered waypoints
    std::vector<bool>    is_via;   // true if transition i→i+1 is via
    double              wirelength;
    int                 via_count;
    int                 drc_count;
    bool                is_complete;
};

// ─── DRC markers ─────────────────────────────────────────────────────────────

enum class DRCType {
    SHORT, SPACING, ENCLOSURE, WIDTH, AREA,
    VIA_ENCL, MIN_STEP, END_OF_LINE, OTHER
};

struct DRCMarker {
    BBox     region;
    layer_t  layer;
    DRCType  type;
    net_id   net1, net2;     // involved nets (net2 = INVALID if self-violation)
    float    severity;       // normalized [0,1]
    static constexpr net_id INVALID = std::numeric_limits<net_id>::max();
};

// ─── Congestion map ───────────────────────────────────────────────────────────

struct GCell {
    BBox   bbox;
    layer_t layer;
    float  h_utilization;  // horizontal track usage [0,1]
    float  v_utilization;  // vertical track usage [0,1]
    int    overflow;       // number of overflowing routes
};

struct CongestionMap {
    int gcell_nx, gcell_ny, n_layers;
    std::vector<GCell> cells;
    GCell& at(int x, int y, int z) {
        return cells[z * gcell_nx * gcell_ny + y * gcell_nx + x];
    }
};

// ─── Cost weight vector (PSO particle position) ───────────────────────────────

struct CostWeights {
    float w_wire      = 1.0f;   // per-unit wirelength cost multiplier
    float w_via       = 4.0f;   // per-via cost (via ≈ 4 track segments)
    float w_cong      = 2.0f;   // congestion overflow penalty
    float w_drc_hist  = 5.0f;   // penalty for historically DRC-hot edges
    float w_layer_pref = 1.0f;  // preferred layer direction bonus
    float w_timing    = 0.5f;   // timing criticality weight

    // Clamp to safe operational range
    void clamp() {
        auto clamp1 = [](float v){ return v < 0.1f ? 0.1f : v > 20.0f ? 20.0f : v; };
        w_wire = clamp1(w_wire); w_via = clamp1(w_via);
        w_cong = clamp1(w_cong); w_drc_hist = clamp1(w_drc_hist);
        w_layer_pref = clamp1(w_layer_pref); w_timing = clamp1(w_timing);
    }
};

// ─── Routing result snapshot (used for fitness evaluation) ───────────────────

struct RoutingSnapshot {
    int    total_drc;
    int    total_via;
    double total_wirelength;
    int    unrouted_nets;
    double runtime_sec;
    CostWeights weights_used;

    double fitness() const {
        // Lower is better
        return total_drc * 1000.0
             + unrouted_nets * 10000.0
             + total_via * 1.0
             + total_wirelength * 0.001;
    }
};

// ─── Configuration ────────────────────────────────────────────────────────────

struct RBAConfig {
    // GA parameters
    int    ga_population     = 50;
    int    ga_generations    = 100;
    double ga_crossover_rate = 0.8;
    double ga_mutation_rate  = 0.05;
    int    ga_tournament_k   = 5;
    int    ga_elite_count    = 5;

    // ACO parameters
    int    aco_n_ants        = 20;
    int    aco_iterations    = 50;
    double aco_alpha         = 1.0;   // pheromone exponent
    double aco_beta          = 2.0;   // heuristic exponent
    double aco_rho           = 0.1;   // evaporation rate
    double aco_Q             = 100.0; // pheromone deposit constant
    double aco_tau_min       = 1e-4;
    double aco_tau_max       = 10.0;
    double aco_drc_penalty   = 0.5;   // pheromone factor on DRC edges

    // PSO parameters
    int    pso_n_particles   = 30;
    int    pso_iterations    = 40;
    double pso_omega         = 0.729;
    double pso_c1            = 1.494;
    double pso_c2            = 1.494;

    // PSO budget redesign (Tier 2.1): at the old defaults (20 particles x
    // 30 iterations x 5 outer = 3,000 router passes/design/seed), a single
    // seed on one ispd18-scale design was 50-100+ hours — infeasible. PSO
    // now only runs on outer iterations in [pso_active_outer_iter_lo,
    // pso_active_outer_iter_hi] (0-indexed); other outer iterations reuse
    // the weights PSO last converged to instead of re-tuning. Combined with
    // pso_n_particles=8, pso_iterations=5 (see scripts/rba_config_*.json),
    // this yields ~8*5*2=80 router passes for PSO instead of 3,000. The
    // actual pass count is logged for real (RBAOrchestrator::
    // router_invocation_count(), written to run_summary.json) rather than
    // asserted, so the runtime-overhead figure in any report is measured.
    int    pso_active_outer_iter_lo = 1;
    int    pso_active_outer_iter_hi = 2;

    // ABC parameters
    int    abc_n_bees        = 20;
    int    abc_max_cycles    = 100;
    int    abc_limit         = 20;    // scout trigger threshold

    // Orchestration
    int    rba_outer_iters   = 5;     // full RBA→TR cycles
    int    tr_threads        = 8;
    bool   enable_timing     = true;
    std::string openroad_bin = "openroad";
    std::string output_dir   = "./rba_output";

    // RNG seed for GA/PSO/ACO/ABC. -1 (default) means "use each optimizer's
    // own built-in default seed" (unchanged behavior — every run has always
    // been deterministic per-phase via those defaults, just not
    // user-controllable or distinguishable across --runs). >=0 derives a
    // distinct per-phase seed so --seed/--num_seeds produce genuinely
    // different, reproducible runs — see RBAOrchestrator's phase_seed().
    int    seed               = -1;

    // Component-disable ablation flags (Tier 2.3): when false, that phase
    // is skipped entirely rather than run with default/no-op parameters,
    // so ablation numbers come from actual runs, not assumed contributions.
    bool   enable_ga          = true;
    bool   enable_pso         = true;
    bool   enable_aco         = true;
    bool   enable_abc         = true;

    // Rip-up candidate cap as a fraction of total net count (Tier 2.4),
    // replacing the old hardcoded flat count of 50. 0.10 was an arbitrary
    // unjustified default before; kept as the default here pending the
    // sweep in scripts/ripup_budget_sweep.py, but is now a real,
    // configurable knob instead of a hardcoded literal.
    double ripup_fraction     = 0.10;
};

} // namespace rba
