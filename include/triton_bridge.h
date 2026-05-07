#pragma once
// TritonRoute Bridge: Interface between the RBA framework and OpenROAD/TritonRoute.
//
// Interaction model:
//   - TritonRoute runs as a subprocess via OpenROAD Tcl scripts
//   - The bridge writes configuration (net order, cost weights) to a JSON
//     sidecar file that a patched TritonRoute reads at startup
//   - After each routing run, the bridge parses the output DEF and DRC report
//     to extract metrics for the bio-inspired algorithms
//
// Alternative (preferred for deeper integration):
//   - Link against OpenROAD as a library; call odb::dbBlock and
//     drt::FlexDR APIs directly from C++
//   - The bridge provides the same interface either way

#include "rba_types.h"
#include <string>
#include <filesystem>

namespace rba {

struct TritonRunConfig {
    std::string lef_file;
    std::string def_file;
    std::string guide_file;
    std::string output_def;
    std::string drc_report;
    int         threads        = 8;
    int         verbose        = 0;
    std::string param_override; // path to JSON param sidecar
};

class TritonBridge {
public:
    explicit TritonBridge(const RBAConfig& cfg);

    // ── Setup ──────────────────────────────────────────────────────────────

    // Load LEF/DEF and build the internal routing graph.
    bool load_design(const std::string& lef, const std::string& def,
                     const std::string& guide);

    // ── Injection: RBA → TritonRoute ──────────────────────────────────────

    // Write ordered net list to the param sidecar JSON.
    // TritonRoute reads this on next run to process nets in given sequence.
    void inject_net_order(const std::vector<net_id>& order);

    // Write cost weight overrides to the param sidecar JSON.
    void inject_cost_weights(const CostWeights& weights);

    // ── Execution ─────────────────────────────────────────────────────────

    // Run TritonRoute (subprocess or library call) with current config.
    // Returns false if TritonRoute exits with non-zero status.
    bool run_tritonroute(const TritonRunConfig& run_cfg);

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

    // Write Tcl script for OpenROAD invocation
    std::string write_tcl_script(const TritonRunConfig& run_cfg) const;

    // Parse DEF NETS section for wire/via segments
    void parse_def_nets(const std::string& def_path,
                        std::vector<Route>& routes) const;

    // Parse TritonRoute's DRC JSON format (post-ISPD 2018 format)
    std::vector<DRCMarker> parse_drc_json(const std::string& path) const;

    // Parse TritonRoute's older RPT text format
    std::vector<DRCMarker> parse_drc_rpt(const std::string& path) const;
};

} // namespace rba
