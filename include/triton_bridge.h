#pragma once
// TritonRoute Bridge: Interface between the RBA framework and OpenROAD/TritonRoute.
//
// Interaction model:
//   - TritonRoute runs as a subprocess via OpenROAD Tcl scripts
//   - The bridge injects RBA parameters (net priority, cost weights, forced
//     rip-up nets) as real Tcl commands (set_drt_cost_weights,
//     set_drt_net_order, set_drt_ripup_nets) into the generated Tcl script.
//     These commands only exist in the patched OpenROAD build described by
//     third_party/openroad.patch (see docs/INTEGRATION.md); on a stock
//     OpenROAD build the generated Tcl detects their absence via
//     `info commands` and falls back to TritonRoute's built-in defaults
//     with a logged warning instead of failing.
//   - After each routing run, the bridge parses the output DEF and DRC report
//     to extract metrics for the bio-inspired algorithms
//
// Alternative (preferred for deeper integration):
//   - Link against OpenROAD as a library; call odb::dbBlock and
//     drt::FlexDR APIs directly from C++
//   - The bridge provides the same interface either way

#include "rba_types.h"
#include <optional>
#include <string>
#include <filesystem>
#include <unordered_map>

namespace rba {

struct TritonRunConfig {
    std::string lef_file;
    std::string def_file;
    std::string guide_file;
    std::string output_def;
    std::string drc_report;
    int         threads        = 8;
    int         verbose        = 0;
};

class TritonBridge {
public:
    explicit TritonBridge(const RBAConfig& cfg);

    // ── Setup ──────────────────────────────────────────────────────────────

    // Load LEF/DEF and build the internal routing graph.
    bool load_design(const std::string& lef, const std::string& def,
                     const std::string& guide);

    // ── Injection: RBA → TritonRoute ──────────────────────────────────────
    // Each of these stages a value to be emitted as a real Tcl command by
    // the next write_tcl_script() call (inside run_tritonroute()); they do
    // not talk to TritonRoute directly. Net names are resolved via the net
    // table populated by load_nets().

    // Stage an ordered net priority list (highest priority first).
    // Emitted as `set_drt_net_order -file <path>` (one net name per line).
    // Patched TritonRoute consults this when initializing each worker's
    // reroute queue (iteration 0 only) — see docs/INTEGRATION.md for the
    // exact semantics (this is a per-worker-tile priority hint, not a
    // single global sequential order, since TritonRoute routes via
    // spatially-parallel worker tiles).
    void inject_net_order(const std::vector<net_id>& order);

    // Stage cost weight overrides.
    // Emitted as `set_drt_cost_weights -route_shape_cost .. -via_cost ..
    // -marker_cost .. -grid_cost ..` (RBA's 6-field CostWeights are mapped
    // onto TritonRoute's 4 real cost knobs; w_cong/w_timing are not yet
    // wired to a real TritonRoute cost term — see docs/INTEGRATION.md).
    void inject_cost_weights(const CostWeights& weights);

    // Stage a forced rip-up net list.
    // Emitted as `set_drt_ripup_nets -file <path>` (one net name per line).
    // Patched TritonRoute rips up and reroutes exactly these nets on the
    // next detailed_route call, regardless of current DRC state.
    void inject_ripup_nets(const std::vector<net_id>& nets);

    // ── Execution ─────────────────────────────────────────────────────────

    // Run TritonRoute (subprocess or library call) with current config.
    // Emits any staged inject_*() values as real Tcl, then clears them.
    // Returns false if TritonRoute exits with non-zero status.
    bool run_tritonroute(const TritonRunConfig& run_cfg);

    // Number of times run_tritonroute() has actually invoked the router
    // binary (i.e. real router passes consumed), for equal-compute-budget
    // comparisons. Never reset automatically.
    long invocation_count() const { return invocation_count_; }

    // ── Extraction: TritonRoute → RBA ─────────────────────────────────────

    // Parse DRC markers from TritonRoute's JSON/RPT output.
    std::vector<DRCMarker> read_drc_markers(const std::string& drc_report);

    // Build congestion map from TritonRoute's gcell overflow data.
    CongestionMap read_congestion_map(const std::string& congestion_rpt);

    // Parse the output DEF to extract route metrics.
    RoutingSnapshot read_routing_snapshot(const std::string& output_def,
                                          double runtime_sec,
                                          const CostWeights& weights);

    // Extract full routing graph for ACO pheromone initialization.
    // Warning: large (millions of nodes for real designs) — called once.
    RoutingGraph extract_routing_graph();

    // Extract current routes for ABC via minimization.
    std::vector<Route> extract_routes(const std::string& def_file);

    // Write modified routes back to DEF after ABC via minimization.
    bool write_routes(const std::vector<Route>& routes,
                      const std::string& input_def,
                      const std::string& output_def);

    // ── Net metadata ──────────────────────────────────────────────────────

    // Load net list from DEF/timing report.
    std::vector<Net> load_nets(const std::string& def_file,
                               const std::string& timing_rpt = "");

    // Build a baseline congestion estimate from global routing guide file.
    CongestionMap estimate_congestion_from_guides(const std::string& guide_file);

    // ── Utility ───────────────────────────────────────────────────────────

    const std::string& design_name() const { return design_name_; }
    int total_nets() const { return total_nets_; }

private:
    RBAConfig    cfg_;
    std::string  design_name_;
    int          total_nets_ = 0;
    std::string  lef_file_, def_file_, guide_file_;
    long         invocation_count_ = 0;
    double       dbu_per_micron_ = 1000.0;  // from DEF "UNITS DISTANCE MICRONS n"
    BBox         die_area_{0, 0, 0, 0};     // from DEF DIEAREA, in DBU

    // net_id → net name, populated by load_nets(); used to translate
    // inject_*() net_id lists into the net *names* TritonRoute's Tcl
    // commands operate on.
    std::unordered_map<net_id, std::string> net_names_;

    // Staged (not-yet-emitted) values from the last inject_*() calls;
    // consumed and cleared by the next write_tcl_script()/run_tritonroute().
    std::optional<std::vector<net_id>> pending_net_order_;
    std::optional<CostWeights>         pending_cost_weights_;
    std::optional<std::vector<net_id>> pending_ripup_nets_;

    // Write Tcl script for OpenROAD invocation
    std::string write_tcl_script(const TritonRunConfig& run_cfg);

    // Resolve a net_id list to names and write one-per-line to `path`.
    // Unknown net_ids (no entry in net_names_) are skipped.
    void write_net_name_file(const std::vector<net_id>& ids,
                             const std::string& path) const;

    // Parse DEF NETS section for wire/via segments
    void parse_def_nets(const std::string& def_path,
                        std::vector<Route>& routes) const;

    // Parse TritonRoute's DRC JSON format (post-ISPD 2018 format)
    std::vector<DRCMarker> parse_drc_json(const std::string& path) const;

    // Parse TritonRoute's older RPT text format
    std::vector<DRCMarker> parse_drc_rpt(const std::string& path) const;
};

} // namespace rba
