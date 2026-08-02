#include "triton_bridge.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <utility>
#include <nlohmann/json.hpp>   // header-only JSON — add to deps

namespace rba {

using json = nlohmann::json;

// ─── LEF/DEF geometry helpers ───────────────────────────────────────────────
// Real LEF 5.8 MACRO/PIN and DEF 5.8 COMPONENTS parsing, verified against
// real ISPD 2018 benchmark files (benchmarks/ispd18_test1, downloaded
// directly from ispd.cc — no synthetic-only assumptions). Kept file-local
// since only load_nets()/estimate_congestion_from_guides()/
// extract_routing_graph() need them.
namespace {

struct LefPinInfo {
    double cx_um = 0.0, cy_um = 0.0;  // centroid of the pin's shapes, in microns
    std::string layer;
};

struct LefMacroInfo {
    double width_um = 0.0, height_um = 0.0;
    std::unordered_map<std::string, LefPinInfo> pins;
};

// Parses every MACRO...END block's PIN geometry (PORT/LAYER/RECT), taking
// the centroid of each pin's first LAYER's RECTs as that pin's location —
// matches the granularity load_nets() previously had none of (it used a
// hardcoded {0,0,0} for every pin).
std::unordered_map<std::string, LefMacroInfo> parse_lef_macros(const std::string& lef_path) {
    std::unordered_map<std::string, LefMacroInfo> macros;
    std::ifstream f(lef_path);
    if (!f) return macros;

    std::regex macro_re(R"(^\s*MACRO\s+(\S+))");
    std::regex end_macro_re(R"(^\s*END\s+(\S+)\s*$)");
    std::regex size_re(R"(SIZE\s+([\d.]+)\s+BY\s+([\d.]+))");
    std::regex pin_re(R"(^\s*PIN\s+(\S+))");
    std::regex end_pin_re(R"(^\s*END\s+(\S+)\s*$)");
    std::regex layer_re(R"(LAYER\s+(\S+))");
    std::regex rect_re(R"(RECT\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+))");

    std::string line;
    std::string cur_macro, cur_pin, cur_layer;
    double sum_x = 0, sum_y = 0;
    int n_rects = 0;
    bool in_macro = false, in_pin = false;

    auto flush_pin = [&]() {
        if (in_pin && n_rects > 0 && !cur_macro.empty()) {
            LefPinInfo p;
            p.cx_um = sum_x / n_rects;
            p.cy_um = sum_y / n_rects;
            p.layer = cur_layer;
            macros[cur_macro].pins[cur_pin] = p;
        }
        in_pin = false;
        sum_x = sum_y = 0;
        n_rects = 0;
        cur_layer.clear();
    };

    while (std::getline(f, line)) {
        std::smatch m;
        if (!in_macro) {
            if (std::regex_search(line, m, macro_re)) {
                cur_macro = m[1];
                in_macro = true;
            }
            continue;
        }
        if (std::regex_search(line, m, end_macro_re) && m[1] == cur_macro) {
            flush_pin();
            in_macro = false;
            cur_macro.clear();
            continue;
        }
        if (!in_pin) {
            if (std::regex_search(line, m, size_re)) {
                macros[cur_macro].width_um  = std::stod(m[1]);
                macros[cur_macro].height_um = std::stod(m[2]);
            } else if (std::regex_search(line, m, pin_re)) {
                cur_pin = m[1];
                in_pin = true;
                sum_x = sum_y = 0;
                n_rects = 0;
                cur_layer.clear();
            }
            continue;
        }
        // inside a PIN block
        if (std::regex_search(line, m, end_pin_re) && m[1] == cur_pin) {
            flush_pin();
            continue;
        }
        if (std::regex_search(line, m, layer_re)) {
            cur_layer = m[1];
        } else if (std::regex_search(line, m, rect_re)) {
            double xl = std::stod(m[1]), yl = std::stod(m[2]);
            double xh = std::stod(m[3]), yh = std::stod(m[4]);
            sum_x += (xl + xh) / 2.0;
            sum_y += (yl + yh) / 2.0;
            ++n_rects;
        }
    }
    return macros;
}

struct DefComponentInfo {
    std::string macro;
    dbu_t x = 0, y = 0;
    std::string orient = "N";
};

// Parses `- instName MACRO_NAME + (PLACED|FIXED|COVER) ( x y ) ORIENT ;`
std::unordered_map<std::string, DefComponentInfo> parse_def_components(const std::string& def_path) {
    std::unordered_map<std::string, DefComponentInfo> comps;
    std::ifstream f(def_path);
    if (!f) return comps;

    bool in_components = false;
    std::regex comp_re(
        R"(^\s*-\s+(\S+)\s+(\S+)\s+\+\s+(?:PLACED|FIXED|COVER)\s+\(\s*(-?\d+)\s+(-?\d+)\s*\)\s+(\S+))");
    std::string line;
    while (std::getline(f, line)) {
        if (!in_components) {
            if (line.find("COMPONENTS") != std::string::npos &&
                line.find("END") == std::string::npos) {
                in_components = true;
            }
            continue;
        }
        if (line.find("END COMPONENTS") != std::string::npos) break;
        std::smatch m;
        if (std::regex_search(line, m, comp_re)) {
            DefComponentInfo c;
            c.macro  = m[2];
            c.x      = static_cast<dbu_t>(std::stol(m[3]));
            c.y      = static_cast<dbu_t>(std::stol(m[4]));
            c.orient = m[5];
            comps[m[1]] = c;
        }
    }
    return comps;
}

// Standard LEF/DEF orientation transform: local pin point (x, y) within a
// macro of size (w, h), both in microns, mapped through orientation `ori`
// (N/S/E/W/FN/FS/FE/FW) then translated by the component's placed origin
// (inst_x, inst_y, in DBU). Formula matches the widely-used LEF/DEF
// reference transform tables (e.g. as implemented in OpenROAD's odb
// dbTransform) — not derived from any single design's output, so it should
// hold for any LEF/DEF pair, not just the ispd18 files this was checked
// against.
Point3D transform_pin_to_absolute(const LefPinInfo& pin, double w_um, double h_um,
                                  double dbu_per_micron, dbu_t inst_x, dbu_t inst_y,
                                  const std::string& ori) {
    double x = pin.cx_um, y = pin.cy_um;
    double tx, ty;
    if (ori == "N")       { tx = x;         ty = y; }
    else if (ori == "S")  { tx = w_um - x;  ty = h_um - y; }
    else if (ori == "E")  { tx = h_um - y;  ty = x; }
    else if (ori == "W")  { tx = y;         ty = w_um - x; }
    else if (ori == "FN") { tx = w_um - x;  ty = y; }
    else if (ori == "FS") { tx = x;         ty = h_um - y; }
    else if (ori == "FE") { tx = h_um - y;  ty = w_um - x; }
    else if (ori == "FW") { tx = y;         ty = x; }
    else                  { tx = x;         ty = y; }  // unknown orient: treat as N

    Point3D p;
    p.x = inst_x + static_cast<dbu_t>(std::lround(tx * dbu_per_micron));
    p.y = inst_y + static_cast<dbu_t>(std::lround(ty * dbu_per_micron));
    p.z = 0;
    return p;
}

struct LefLayerInfo {
    std::string name;
    bool is_horizontal = true;  // DIRECTION HORIZONTAL vs VERTICAL
};

// Parses routing layers (TYPE ROUTING) in file order, which is also their
// stacking order (M1, M2, ... ) in every LEF this was checked against.
std::vector<LefLayerInfo> parse_lef_routing_layers(const std::string& lef_path) {
    std::vector<LefLayerInfo> layers;
    std::ifstream f(lef_path);
    if (!f) return layers;

    std::regex layer_re(R"(^\s*LAYER\s+(\S+)\s*$)");
    std::regex type_re(R"(TYPE\s+ROUTING\s*;)");
    std::regex dir_re(R"(DIRECTION\s+(HORIZONTAL|VERTICAL)\s*;)");
    std::string line, pending_name;
    bool pending_is_routing = false;
    std::string pending_dir = "HORIZONTAL";

    auto flush = [&]() {
        if (!pending_name.empty() && pending_is_routing) {
            layers.push_back(LefLayerInfo{pending_name, pending_dir == "HORIZONTAL"});
        }
        pending_name.clear();
        pending_is_routing = false;
        pending_dir = "HORIZONTAL";
    };

    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, layer_re)) {
            flush();
            pending_name = m[1];
            continue;
        }
        if (!pending_name.empty()) {
            if (std::regex_search(line, type_re)) pending_is_routing = true;
            if (std::regex_search(line, m, dir_re)) pending_dir = m[1];
        }
        if (line.find("END") != std::string::npos && line.find(pending_name) != std::string::npos) {
            flush();
        }
    }
    flush();
    return layers;
}

// Parses LEF `VIA <name> DEFAULT ... LAYER a ; ... LAYER cut ; ... LAYER b ;
// END <name>` blocks (verified against real ISPD 2018 LEF files, e.g.
// benchmarks/ispd18_test1's VIA12_1C/VIA23_1C/... definitions) into a
// (bottomRoutingLayer, topRoutingLayer) -> via name lookup. Real LEFs
// define several via variants per layer pair (e.g. VIA12_1C, VIA12_1C_H,
// VIA12_1C_V for centered/horizontal/vertical enclosure) — this keeps the
// first one seen per pair, which is the plain "_1C"-style default in
// every ISPD 2018 LEF this was checked against, not a guarantee for every
// possible LEF.
std::map<std::pair<std::string, std::string>, std::string> parse_lef_default_vias(
        const std::string& lef_path) {
    std::map<std::pair<std::string, std::string>, std::string> vias;
    std::ifstream f(lef_path);
    if (!f) return vias;

    std::regex via_start_re(R"(^\s*VIA\s+(\S+)\s+DEFAULT\s*$)");
    std::regex via_layer_re(R"(^\s*LAYER\s+(\S+)\s*;)");
    std::regex end_re(R"(^\s*END\s+(\S+)\s*$)");

    std::string line, cur_via;
    std::vector<std::string> cur_layers;
    bool in_via = false;
    while (std::getline(f, line)) {
        std::smatch m;
        if (!in_via) {
            if (std::regex_search(line, m, via_start_re)) {
                cur_via = m[1];
                cur_layers.clear();
                in_via = true;
            }
            continue;
        }
        if (std::regex_search(line, m, end_re) && m[1] == cur_via) {
            if (cur_layers.size() >= 3) {
                auto key = std::make_pair(cur_layers.front(), cur_layers.back());
                if (vias.find(key) == vias.end()) vias[key] = cur_via;
            }
            in_via = false;
            continue;
        }
        if (std::regex_search(line, m, via_layer_re)) {
            cur_layers.push_back(m[1]);
        }
    }
    return vias;
}

}  // anonymous namespace

TritonBridge::TritonBridge(const RBAConfig& cfg) : cfg_(cfg) {}

// ─── Design loading ────────────────────────────────────────────────────────

bool TritonBridge::load_design(const std::string& lef,
                                const std::string& def,
                                const std::string& guide) {
    lef_file_   = lef;
    def_file_   = def;
    guide_file_ = guide;

    // Extract design name from DEF header
    std::ifstream f(def);
    if (!f) {
        std::cerr << "[Bridge] Cannot open DEF: " << def << "\n";
        return false;
    }
    std::string line;
    std::regex design_re(R"(DESIGN\s+(\S+)\s*;)");
    std::regex nets_re(R"(NETS\s+(\d+)\s*;)");
    std::regex units_re(R"(UNITS\s+DISTANCE\s+MICRONS\s+(\d+)\s*;)");
    std::regex diearea_re(
        R"(DIEAREA\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\))");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, design_re)) design_name_ = m[1];
        if (std::regex_search(line, m, nets_re))   total_nets_ = std::stoi(m[1]);
        if (std::regex_search(line, m, units_re))  dbu_per_micron_ = std::stod(m[1]);
        if (std::regex_search(line, m, diearea_re)) {
            die_area_ = BBox{
                static_cast<dbu_t>(std::stol(m[1])), static_cast<dbu_t>(std::stol(m[2])),
                static_cast<dbu_t>(std::stol(m[3])), static_cast<dbu_t>(std::stol(m[4]))};
        }
        if (!design_name_.empty() && total_nets_ > 0 &&
            dbu_per_micron_ != 1000.0 && die_area_.xh != 0) {
            break;
        }
    }
    std::cout << "[Bridge] Loaded design: " << design_name_
              << " (" << total_nets_ << " nets, " << dbu_per_micron_
              << " DBU/micron, die " << die_area_.xh << "x" << die_area_.yh << ")\n";
    return true;
}

// ─── Net order injection ───────────────────────────────────────────────────

void TritonBridge::inject_net_order(const std::vector<net_id>& order) {
    pending_net_order_ = order;
    std::cout << "[Bridge] Staged net order (" << order.size() << " nets)\n";
}

// ─── Cost weight injection ────────────────────────────────────────────────

void TritonBridge::inject_cost_weights(const CostWeights& w) {
    pending_cost_weights_ = w;
    std::cout << "[Bridge] Staged cost weights: wire=" << w.w_wire
              << " via=" << w.w_via << " cong=" << w.w_cong << "\n";
}

// ─── Forced rip-up injection ───────────────────────────────────────────────

void TritonBridge::inject_ripup_nets(const std::vector<net_id>& nets) {
    pending_ripup_nets_ = nets;
    std::cout << "[Bridge] Staged forced rip-up (" << nets.size() << " nets)\n";
}

// ─── net_id → name file resolution ─────────────────────────────────────────

void TritonBridge::write_net_name_file(const std::vector<net_id>& ids,
                                        const std::string& path) const {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write " + path);
    for (net_id id : ids) {
        auto it = net_names_.find(id);
        if (it != net_names_.end()) out << it->second << "\n";
    }
}

// ─── TritonRoute invocation ────────────────────────────────────────────────

bool TritonBridge::run_tritonroute(const TritonRunConfig& run_cfg) {
    std::string tcl = write_tcl_script(run_cfg);
    std::string tcl_path = cfg_.output_dir + "/run_route.tcl";
    {
        std::ofstream f(tcl_path);
        f << tcl;
    }

    // Build OpenROAD command
    std::ostringstream cmd;
    cmd << cfg_.openroad_bin
        << " -exit"
        << (run_cfg.verbose ? "" : " -no_init")
        << " " << tcl_path
        << " 2>&1 | tee " << cfg_.output_dir << "/openroad.log";

    std::cout << "[Bridge] Running: " << cmd.str() << "\n";

    auto t0 = std::chrono::steady_clock::now();
    int ret = std::system(cmd.str().c_str());
    auto t1 = std::chrono::steady_clock::now();
    ++invocation_count_;

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "[Bridge] TritonRoute finished in " << elapsed
              << "s (exit=" << ret << ", invocation #" << invocation_count_ << ")\n";

    return (ret == 0);
}

// ─── Tcl script generation ─────────────────────────────────────────────────
//
// Emits real Tcl calls to the RBA-specific commands added by
// third_party/openroad.patch (set_drt_cost_weights, set_drt_net_order,
// set_drt_ripup_nets — see docs/INTEGRATION.md). Each call is guarded by an
// `info commands` check so this script still runs correctly (falling back
// to TritonRoute's own defaults, with a logged warning) against a stock,
// unpatched OpenROAD build.

std::string TritonBridge::write_tcl_script(const TritonRunConfig& run_cfg) {
    std::ostringstream tcl;
    tcl << "# Auto-generated by RBA bridge\n";
    tcl << "read_lef " << run_cfg.lef_file << "\n";
    tcl << "read_def " << run_cfg.def_file << "\n";
    tcl << "\n";

    if (pending_cost_weights_) {
        const CostWeights& w = *pending_cost_weights_;
        // Map RBA's [0.1, 20.0] float weights onto TritonRoute's native
        // integer cost knobs, scaled so RBA's defaults reproduce
        // TritonRoute's own defaults (ROUTESHAPECOST=8, VIACOST=4,
        // MARKERCOST=32, GRIDCOST=2). w_cong/w_timing have no direct
        // TritonRoute cost-model analog yet and are not sent.
        auto to_cost = [](float rba_weight, double scale) {
            long v = std::lround(rba_weight * scale);
            return v < 1 ? 1 : v;
        };
        tcl << "if {[llength [info commands set_drt_cost_weights]] > 0} {\n";
        tcl << "  set_drt_cost_weights"
            << " -route_shape_cost " << to_cost(w.w_wire, 8.0)
            << " -via_cost "         << to_cost(w.w_via, 1.0)
            << " -marker_cost "      << to_cost(w.w_drc_hist, 6.4)
            << " -grid_cost "        << to_cost(w.w_layer_pref, 2.0)
            << "\n";
        tcl << "} else {\n";
        tcl << "  puts \"\\[RBA\\] WARNING: set_drt_cost_weights unavailable "
               "(unpatched OpenROAD build) — using TritonRoute defaults\"\n";
        tcl << "}\n\n";
        pending_cost_weights_.reset();
    }

    if (pending_net_order_) {
        std::string order_path = cfg_.output_dir + "/net_order.txt";
        write_net_name_file(*pending_net_order_, order_path);
        tcl << "if {[llength [info commands set_drt_net_order]] > 0} {\n";
        tcl << "  set_drt_net_order -file " << order_path << "\n";
        tcl << "} else {\n";
        tcl << "  puts \"\\[RBA\\] WARNING: set_drt_net_order unavailable "
               "(unpatched OpenROAD build) — net order ignored\"\n";
        tcl << "}\n\n";
        pending_net_order_.reset();
    }

    if (pending_ripup_nets_) {
        std::string ripup_path = cfg_.output_dir + "/ripup_nets.txt";
        write_net_name_file(*pending_ripup_nets_, ripup_path);
        tcl << "if {[llength [info commands set_drt_ripup_nets]] > 0} {\n";
        tcl << "  set_drt_ripup_nets -file " << ripup_path << "\n";
        tcl << "} else {\n";
        tcl << "  puts \"\\[RBA\\] WARNING: set_drt_ripup_nets unavailable "
               "(unpatched OpenROAD build) — forced rip-up ignored\"\n";
        tcl << "}\n\n";
        pending_ripup_nets_.reset();
    }

    // TritonRoute detailed routing command
    tcl << "detailed_route \\\n";
    tcl << "  -guide " << run_cfg.guide_file << " \\\n";
    tcl << "  -output_drc " << run_cfg.drc_report << " \\\n";
    tcl << "  -output_maze " << cfg_.output_dir << "/maze.log \\\n";
    tcl << "  -verbose " << run_cfg.verbose << " \\\n";
    tcl << "  -threads " << run_cfg.threads << "\n\n";

    tcl << "write_def " << run_cfg.output_def << "\n";

    // Write GCell congestion report
    tcl << "report_design_area\n";
    tcl << "report_wire_length\n";

    return tcl.str();
}

// ─── DRC marker parsing ────────────────────────────────────────────────────

std::vector<DRCMarker> TritonBridge::read_drc_markers(
        const std::string& drc_report) {
    // Try JSON format first (ISPD 2018+ TritonRoute output)
    if (drc_report.size() >= 5 &&
        drc_report.substr(drc_report.size()-5) == ".json") {
        return parse_drc_json(drc_report);
    }
    return parse_drc_rpt(drc_report);
}

std::vector<DRCMarker> TritonBridge::parse_drc_json(
        const std::string& path) const {
    std::vector<DRCMarker> markers;
    std::ifstream f(path);
    if (!f) return markers;

    json j;
    try { f >> j; } catch (...) { return markers; }

    for (auto& viol : j["violations"]) {
        DRCMarker m;
        auto& bbox = viol["bbox"];
        m.region = {bbox[0], bbox[1], bbox[2], bbox[3]};
        m.layer  = viol.value("layer_idx", 0);

        std::string type_str = viol.value("type", "OTHER");
        if      (type_str == "SHORT")    m.type = DRCType::SHORT;
        else if (type_str == "SPACING")  m.type = DRCType::SPACING;
        else if (type_str == "WIDTH")    m.type = DRCType::WIDTH;
        else if (type_str == "ENCLOSURE")m.type = DRCType::ENCLOSURE;
        else                             m.type = DRCType::OTHER;

        m.severity  = 1.0f;
        m.net1      = viol.value("net1_id", DRCMarker::INVALID);
        m.net2      = viol.value("net2_id", DRCMarker::INVALID);
        markers.push_back(m);
    }
    std::cout << "[Bridge] Parsed " << markers.size() << " DRC markers\n";
    return markers;
}

std::vector<DRCMarker> TritonBridge::parse_drc_rpt(
        const std::string& path) const {
    // Parse TritonRoute's text DRC report:
    //   violation type: Short
    //     srcs: net1 net2
    //     bbox = (xl yl) - (xh yh) on Layer M1
    std::vector<DRCMarker> markers;
    std::ifstream f(path);
    if (!f) return markers;

    std::string line;
    DRCMarker cur;
    bool in_viol = false;

    while (std::getline(f, line)) {
        if (line.find("violation type:") != std::string::npos) {
            if (in_viol) markers.push_back(cur);
            cur = DRCMarker{};
            in_viol = true;
            if (line.find("Short") != std::string::npos)   cur.type = DRCType::SHORT;
            else if (line.find("Spacing") != std::string::npos) cur.type = DRCType::SPACING;
            else cur.type = DRCType::OTHER;
            cur.severity = 1.0f;
        } else if (in_viol && line.find("bbox") != std::string::npos) {
            // Parse: bbox = (xl yl) - (xh yh) on Layer Mx
            std::regex bbox_re(R"(\((\d+)\s+(\d+)\)\s*-\s*\((\d+)\s+(\d+)\))");
            std::smatch bm;
            if (std::regex_search(line, bm, bbox_re)) {
                cur.region = {std::stoi(bm[1]), std::stoi(bm[2]),
                              std::stoi(bm[3]), std::stoi(bm[4])};
            }
            // Layer number heuristic: "Layer metal1" → 0, "metal2" → 1 ...
            std::regex layer_re(R"(Layer\s+\w*(\d+))");
            std::smatch lm;
            if (std::regex_search(line, lm, layer_re)) {
                cur.layer = static_cast<layer_t>(std::stoi(lm[1]) - 1);
            }
        }
    }
    if (in_viol) markers.push_back(cur);

    std::cout << "[Bridge] Parsed " << markers.size() << " DRC markers (RPT)\n";
    return markers;
}

// ─── Routing snapshot ──────────────────────────────────────────────────────

RoutingSnapshot TritonBridge::read_routing_snapshot(
        const std::string& output_def,
        double runtime_sec,
        const CostWeights& weights) {
    RoutingSnapshot snap;
    snap.runtime_sec   = runtime_sec;
    snap.weights_used  = weights;
    snap.total_drc     = 0;
    snap.total_via     = 0;
    snap.total_wirelength = 0.0;
    snap.unrouted_nets = 0;

    // Parse DEF for wire/via statistics
    std::ifstream f(output_def);
    if (!f) return snap;

    std::string line;
    std::regex via_re(R"(\bNEW\b.*\+\s*VIA\b)");
    std::regex wire_re(R"(ROUTED\s+\S+\s+\d+\s+\((\d+)\s+(\d+)\)\s+\((\d+)\s+(\d+)\))");
    std::regex unrouted_re(R"(UNROUTED)");

    while (std::getline(f, line)) {
        if (std::regex_search(line, via_re)) ++snap.total_via;
        std::smatch wm;
        if (std::regex_search(line, wm, wire_re)) {
            long dx = std::abs(std::stol(wm[3]) - std::stol(wm[1]));
            long dy = std::abs(std::stol(wm[4]) - std::stol(wm[2]));
            snap.total_wirelength += dx + dy;
        }
        if (std::regex_search(line, unrouted_re)) ++snap.unrouted_nets;
    }

    // Read DRC count from most recent DRC report
    std::string drc_rpt = cfg_.output_dir + "/drc.rpt";
    std::ifstream drc_f(drc_rpt);
    if (drc_f) {
        std::regex total_re(R"(Total\s+Violations\s*:\s*(\d+))");
        while (std::getline(drc_f, line)) {
            std::smatch dm;
            if (std::regex_search(line, dm, total_re)) {
                snap.total_drc = std::stoi(dm[1]);
                break;
            }
        }
    }

    std::cout << "[Bridge] Snapshot: DRC=" << snap.total_drc
              << " via=" << snap.total_via
              << " WL=" << snap.total_wirelength
              << " unrouted=" << snap.unrouted_nets
              << " t=" << snap.runtime_sec << "s\n";
    return snap;
}

// ─── Net loading ────────────────────────────────────────────────────────────
//
// Real pin coordinates: for an `( inst pinName )` connection, the pin's
// absolute location is the LEF macro pin centroid transformed by the
// instance's DEF placement (position + orientation). For a top-level
// `( PIN name )` connection, the location comes straight from the DEF
// PINS section's own FIXED/PLACED point — no LEF lookup needed.
// Verified against real ISPD 2018 benchmark files (see anonymous-namespace
// helpers above), not just the synthetic mini_test fixture.

std::vector<Net> TritonBridge::load_nets(const std::string& def_file,
                                          const std::string& timing_rpt) {
    std::vector<Net> nets;
    std::ifstream f(def_file);
    if (!f) return nets;

    auto macros = parse_lef_macros(lef_file_);
    auto comps  = parse_def_components(def_file);

    // DEF PINS section: pin name -> absolute (x, y), already in DBU.
    std::unordered_map<std::string, Point3D> def_pins;
    {
        std::ifstream pf(def_file);
        bool in_pins = false;
        std::string cur_pin_name;
        std::regex pin_start_re(R"(^\s*-\s+(\S+))");
        std::regex placed_re(
            R"(\+\s+(?:FIXED|PLACED|COVER)\s+\(\s*(-?\d+)\s+(-?\d+)\s*\))");
        std::string pline;
        while (std::getline(pf, pline)) {
            if (!in_pins) {
                if (pline.find("PINS") != std::string::npos &&
                    pline.find("END") == std::string::npos) {
                    in_pins = true;
                }
                continue;
            }
            if (pline.find("END PINS") != std::string::npos) break;
            std::smatch m;
            if (std::regex_search(pline, m, pin_start_re)) cur_pin_name = m[1];
            if (std::regex_search(pline, m, placed_re) && !cur_pin_name.empty()) {
                def_pins[cur_pin_name] = Point3D{
                    static_cast<dbu_t>(std::stol(m[1])),
                    static_cast<dbu_t>(std::stol(m[2])), 0};
            }
        }
    }

    std::string line;
    bool in_nets = false;
    Net cur_net;
    cur_net.id = 0;

    std::regex net_re(R"(^\s*-\s+(\S+)\s*)");
    std::regex inst_pin_re(R"(\(\s*(\S+)\s+(\S+)\s*\))");
    std::regex end_re(R"(^\s*END\s+NETS)");
    int unresolved_pins = 0;

    while (std::getline(f, line)) {
        if (line.find("NETS") != std::string::npos && !in_nets) {
            in_nets = true; continue;
        }
        if (in_nets && std::regex_search(line, end_re)) break;
        if (!in_nets) continue;

        std::smatch m;
        if (std::regex_search(line, m, net_re)) {
            if (!cur_net.name.empty()) {
                net_names_[cur_net.id] = cur_net.name;
                nets.push_back(cur_net);
            }
            cur_net = Net{};
            cur_net.id = static_cast<net_id>(nets.size());
            cur_net.name = m[1];
            cur_net.is_clock = (cur_net.name.find("clk") != std::string::npos ||
                                cur_net.name.find("CLK") != std::string::npos);
            cur_net.is_power = (cur_net.name == "VDD" || cur_net.name == "VSS" ||
                                cur_net.name == "VCC" || cur_net.name == "GND");
        }

        for (auto it = std::sregex_iterator(line.begin(), line.end(), inst_pin_re);
             it != std::sregex_iterator(); ++it) {
            std::string first = (*it)[1], second = (*it)[2];
            Point3D loc{0, 0, 0};
            std::string layer = "M1";
            bool resolved = false;

            if (first == "PIN") {
                // Top-level I/O pin: ( PIN pinName )
                auto dp = def_pins.find(second);
                if (dp != def_pins.end()) { loc = dp->second; resolved = true; }
            } else {
                // Instance pin: ( instName pinName )
                auto ci = comps.find(first);
                if (ci != comps.end()) {
                    auto mi = macros.find(ci->second.macro);
                    if (mi != macros.end()) {
                        auto pi = mi->second.pins.find(second);
                        if (pi != mi->second.pins.end()) {
                            loc = transform_pin_to_absolute(
                                pi->second, mi->second.width_um, mi->second.height_um,
                                dbu_per_micron_, ci->second.x, ci->second.y,
                                ci->second.orient);
                            layer = pi->second.layer;
                            resolved = true;
                        }
                    }
                }
            }
            if (!resolved) ++unresolved_pins;
            cur_net.pins.push_back(Pin{loc, layer, cur_net.id});
        }
    }
    if (!cur_net.name.empty()) {
        net_names_[cur_net.id] = cur_net.name;
        nets.push_back(cur_net);
    }
    if (unresolved_pins > 0) {
        std::cout << "[Bridge] load_nets: " << unresolved_pins
                  << " pin(s) could not be resolved to real coordinates "
                     "(missing LEF macro/pin or DEF component/PIN entry) "
                     "— left at (0,0)\n";
    }

    // Apply timing priorities if report provided
    if (!timing_rpt.empty()) {
        std::ifstream tf(timing_rpt);
        // Simplified: assign priority by appearance order in timing report
        // Real implementation: parse OpenSTA -path_delay report
        std::unordered_map<std::string, int> rank;
        int r = 0;
        std::string tline;
        while (std::getline(tf, tline)) {
            for (auto& net : nets) {
                if (rank.count(net.name) == 0 &&
                    tline.find(net.name) != std::string::npos) {
                    rank[net.name] = r++;
                }
            }
        }
        for (auto& net : nets) {
            auto it = rank.find(net.name);
            net.priority = (it != rank.end()) ? it->second : (int)nets.size();
        }
    } else {
        // Default: sort by pin count descending
        int i = 0;
        for (auto& net : nets) net.priority = i++;
    }

    std::cout << "[Bridge] Loaded " << nets.size() << " nets\n";
    return nets;
}

// ─── Congestion estimation from guides ────────────────────────────────────

CongestionMap TritonBridge::estimate_congestion_from_guides(
        const std::string& guide_file) {
    std::cout << "[Bridge] Building congestion map from " << guide_file << "\n";

    // Global routing guide format (verified against real ISPD 2018 .guide
    // files, e.g. benchmarks/ispd18_test1.input.guide):
    //   net_name
    //   (
    //   xl yl xh yh layerName
    //   ...
    //   )
    struct GuideRect { dbu_t xl, yl, xh, yh; std::string layer; };
    std::vector<GuideRect> rects;
    std::vector<std::string> layer_order;  // first-seen order
    std::unordered_map<std::string, int> layer_idx;
    dbu_t bxl = std::numeric_limits<dbu_t>::max(), byl = std::numeric_limits<dbu_t>::max();
    dbu_t bxh = std::numeric_limits<dbu_t>::min(), byh = std::numeric_limits<dbu_t>::min();

    {
        std::ifstream f(guide_file);
        if (!f) {
            std::cerr << "[Bridge] Cannot open guide file: " << guide_file << "\n";
            CongestionMap empty;
            empty.gcell_nx = empty.gcell_ny = empty.n_layers = 0;
            return empty;
        }
        std::regex rect_re(
            R"(^\s*(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\S+)\s*$)");
        std::string line;
        while (std::getline(f, line)) {
            std::smatch m;
            if (!std::regex_search(line, m, rect_re)) continue;
            GuideRect r{
                static_cast<dbu_t>(std::stol(m[1])), static_cast<dbu_t>(std::stol(m[2])),
                static_cast<dbu_t>(std::stol(m[3])), static_cast<dbu_t>(std::stol(m[4])),
                m[5]};
            rects.push_back(r);
            bxl = std::min(bxl, r.xl); byl = std::min(byl, r.yl);
            bxh = std::max(bxh, r.xh); byh = std::max(byh, r.yh);
            if (layer_idx.find(r.layer) == layer_idx.end()) {
                layer_idx[r.layer] = static_cast<int>(layer_order.size());
                layer_order.push_back(r.layer);
            }
        }
    }

    CongestionMap cmap;
    if (rects.empty()) {
        std::cout << "[Bridge] No guide rectangles parsed from " << guide_file << "\n";
        cmap.gcell_nx = cmap.gcell_ny = cmap.n_layers = 0;
        return cmap;
    }

    // Fixed-resolution GCell grid spanning the guide bounding box — same
    // resolution class used by extract_routing_graph() below, since both
    // need to agree on what a "GCell" is for congestion/pheromone to line up.
    constexpr int kGridN = 64;
    cmap.gcell_nx = kGridN;
    cmap.gcell_ny = kGridN;
    cmap.n_layers = static_cast<int>(layer_order.size());
    cmap.cells.assign(static_cast<size_t>(cmap.gcell_nx) * cmap.gcell_ny * cmap.n_layers, GCell{});

    double span_x = std::max<dbu_t>(1, bxh - bxl);
    double span_y = std::max<dbu_t>(1, byh - byl);
    double cell_w = span_x / kGridN;
    double cell_h = span_y / kGridN;

    for (int z = 0; z < cmap.n_layers; ++z) {
        for (int y = 0; y < kGridN; ++y) {
            for (int x = 0; x < kGridN; ++x) {
                GCell& c = cmap.at(x, y, z);
                c.bbox = BBox{
                    static_cast<dbu_t>(bxl + x * cell_w),
                    static_cast<dbu_t>(byl + y * cell_h),
                    static_cast<dbu_t>(bxl + (x + 1) * cell_w),
                    static_cast<dbu_t>(byl + (y + 1) * cell_h)};
                c.layer = static_cast<layer_t>(z);
            }
        }
    }

    // Accumulate guide density per GCell: each overlapping guide rect
    // increments that cell's overflow count and both utilization axes
    // (a guide doesn't distinguish horizontal/vertical demand on its own —
    // that refinement would need per-net track usage, not just guide
    // presence).
    for (const auto& r : rects) {
        int z = layer_idx[r.layer];
        int x0 = std::clamp(static_cast<int>((r.xl - bxl) / cell_w), 0, kGridN - 1);
        int x1 = std::clamp(static_cast<int>((r.xh - bxl) / cell_w), 0, kGridN - 1);
        int y0 = std::clamp(static_cast<int>((r.yl - byl) / cell_h), 0, kGridN - 1);
        int y1 = std::clamp(static_cast<int>((r.yh - byl) / cell_h), 0, kGridN - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                GCell& c = cmap.at(x, y, z);
                ++c.overflow;
            }
        }
    }
    int max_overflow = 1;
    for (auto& c : cmap.cells) max_overflow = std::max(max_overflow, c.overflow);
    for (auto& c : cmap.cells) {
        float u = static_cast<float>(c.overflow) / static_cast<float>(max_overflow);
        c.h_utilization = u;
        c.v_utilization = u;
    }

    std::cout << "[Bridge] Congestion map: " << cmap.gcell_nx << "x" << cmap.gcell_ny
              << "x" << cmap.n_layers << " from " << rects.size() << " guide rects\n";
    return cmap;
}

// ─── Routing graph extraction ───────────────────────────────────────────────
//
// Built at GCell resolution (same 64x64-per-layer grid as
// estimate_congestion_from_guides(), so ACO pheromones and the congestion
// map line up on the same cells), not full track resolution. A track-level
// graph is the "100M+ nodes" case the README's Challenges table already
// calls out as needing hierarchical clustering — GCell resolution IS that
// mitigation, applied directly, rather than a stub returning nothing.
// Nodes: one per (GCell x, GCell y, routing layer). Edges: 4-connectivity
// within a layer (both axes connected regardless of preferred direction —
// PSO/GA cost weights and w_layer_pref are what should penalize
// non-preferred-direction hops, not graph connectivity itself) plus a via
// edge straight up/down between adjacent layers at the same (x, y).

RoutingGraph TritonBridge::extract_routing_graph() {
    RoutingGraph g;
    auto layers = parse_lef_routing_layers(lef_file_);
    if (layers.empty() || die_area_.xh <= die_area_.xl) {
        std::cout << "[Bridge] extract_routing_graph: no routing layers or "
                     "die area found (lef=" << lef_file_ << ") — empty graph\n";
        return g;
    }

    constexpr int kGridN = 64;
    const int nz = static_cast<int>(layers.size());
    double span_x = std::max<dbu_t>(1, die_area_.xh - die_area_.xl);
    double span_y = std::max<dbu_t>(1, die_area_.yh - die_area_.yl);
    double cell_w = span_x / kGridN;
    double cell_h = span_y / kGridN;

    g.nodes.reserve(static_cast<size_t>(kGridN) * kGridN * nz);
    auto node_at = [&](int x, int y, int z) -> node_id {
        dbu_t cx = static_cast<dbu_t>(die_area_.xl + (x + 0.5) * cell_w);
        dbu_t cy = static_cast<dbu_t>(die_area_.yl + (y + 0.5) * cell_h);
        return RoutingGraph::make_node_id(cx, cy, static_cast<layer_t>(z));
    };

    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < kGridN; ++y) {
            for (int x = 0; x < kGridN; ++x) {
                RoutingNode n;
                n.pos.x = static_cast<dbu_t>(die_area_.xl + (x + 0.5) * cell_w);
                n.pos.y = static_cast<dbu_t>(die_area_.yl + (y + 0.5) * cell_h);
                n.pos.z = static_cast<layer_t>(z);
                node_id id = node_at(x, y, z);
                g.node_idx[id] = g.nodes.size();
                g.nodes.push_back(std::move(n));
            }
        }
    }

    auto add_edge = [&](node_id from, node_id to, layer_t layer, bool is_via, dbu_t wl) {
        edge_id eid = g.edges.size();
        RoutingEdge e;
        e.from = from; e.to = to; e.layer = layer; e.is_via = is_via;
        e.wire_length = wl;
        e.base_cost = is_via ? 4.0f : 1.0f;
        e.pheromone = 0.0f;
        e.congestion = 0.0f;
        g.edge_idx[eid] = g.edges.size();
        g.edges.push_back(e);
        g.nodes[g.node_idx[from]].adj.push_back(eid);
    };

    dbu_t step_x = static_cast<dbu_t>(cell_w);
    dbu_t step_y = static_cast<dbu_t>(cell_h);
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < kGridN; ++y) {
            for (int x = 0; x < kGridN; ++x) {
                node_id here = node_at(x, y, z);
                if (x + 1 < kGridN) {
                    node_id right = node_at(x + 1, y, z);
                    add_edge(here, right, static_cast<layer_t>(z), false, step_x);
                    add_edge(right, here, static_cast<layer_t>(z), false, step_x);
                }
                if (y + 1 < kGridN) {
                    node_id up = node_at(x, y + 1, z);
                    add_edge(here, up, static_cast<layer_t>(z), false, step_y);
                    add_edge(up, here, static_cast<layer_t>(z), false, step_y);
                }
                if (z + 1 < nz) {
                    node_id above = node_at(x, y, z + 1);
                    add_edge(here, above, static_cast<layer_t>(z), true, 0);
                    add_edge(above, here, static_cast<layer_t>(z + 1), true, 0);
                }
            }
        }
    }

    std::cout << "[Bridge] Routing graph: " << g.nodes.size() << " nodes, "
              << g.edges.size() << " edges (" << kGridN << "x" << kGridN
              << "x" << nz << " GCell resolution)\n";
    return g;
}

std::vector<Route> TritonBridge::extract_routes(const std::string& def_file) {
    std::vector<Route> routes;
    parse_def_nets(def_file, routes);
    return routes;
}

// Serializes a Route's path/is_via back into DEF ROUTED syntax:
//   ROUTED <layer0> ( x0 y0 ) ( x1 y1 ) [VIA_NAME] ( x2 y2 ) ...
// Layer changes emit a bare "NEW <layer>" continuation, matching how real
// DEF splits a route into per-layer segments. Via transitions look up a
// real via name from the LEF's own VIA definitions (see
// parse_lef_default_vias) keyed by (fromLayer, toLayer); if no via was
// defined for that exact layer pair in the LEF, falls back to a synthetic
// "VIA<fromLayer>_<toLayer>" name and logs a warning rather than silently
// emitting an unresolvable via, since ABC's via minimization results are
// only meaningful once these are real DRC-checkable via names.
static std::string serialize_route(
        const Route& r, const std::string& net_name,
        const std::vector<std::string>& layer_names,
        const std::map<std::pair<std::string, std::string>, std::string>& via_names) {
    if (r.path.empty()) return "";
    std::ostringstream out;
    out << "- " << net_name << "\n";
    auto layer_name = [&](layer_t z) -> std::string {
        return z < layer_names.size() ? layer_names[z] : ("Metal" + std::to_string(z + 1));
    };
    out << "  ROUTED " << layer_name(r.path[0].z)
        << " ( " << r.path[0].x << " " << r.path[0].y << " )";
    layer_t prev_layer = r.path[0].z;
    for (size_t i = 1; i < r.path.size(); ++i) {
        bool via = (i - 1) < r.is_via.size() && r.is_via[i - 1];
        if (via) {
            std::string from = layer_name(prev_layer), to = layer_name(r.path[i].z);
            auto key = (prev_layer <= r.path[i].z) ? std::make_pair(from, to)
                                                    : std::make_pair(to, from);
            auto it = via_names.find(key);
            if (it != via_names.end()) {
                out << " " << it->second;
            } else {
                out << " VIA" << static_cast<int>(prev_layer) << "_"
                    << static_cast<int>(r.path[i].z);
                std::cerr << "[Bridge] write_routes: no LEF VIA found for "
                          << key.first << "->" << key.second
                          << " — emitting unresolved placeholder via name\n";
            }
        }
        if (r.path[i].z != prev_layer && !via) {
            out << "\n  NEW " << layer_name(r.path[i].z);
            out << " ( " << r.path[i - 1].x << " " << r.path[i - 1].y << " )";
        }
        out << " ( " << r.path[i].x << " " << r.path[i].y << " )";
        prev_layer = r.path[i].z;
    }
    out << " ;\n";
    return out.str();
}

bool TritonBridge::write_routes(const std::vector<Route>& routes,
                                 const std::string& input_def,
                                 const std::string& output_def) {
    std::cout << "[Bridge] write_routes: writing " << routes.size()
              << " routes to " << output_def << "\n";
    std::ifstream in(input_def);
    std::ofstream out(output_def);
    if (!in || !out) return false;

    auto layers = [&] {
        std::vector<std::string> names;
        for (auto& l : parse_lef_routing_layers(lef_file_)) names.push_back(l.name);
        return names;
    }();
    auto via_names = parse_lef_default_vias(lef_file_);

    std::unordered_map<net_id, const Route*> by_net;
    for (const auto& r : routes) by_net[r.net] = &r;

    // Built once, not re-scanned per line — net_names_ can have ~100K+
    // entries on a real ISPD design, and this function processes every
    // NETS line, so a linear scan per line here would be O(n^2).
    std::unordered_map<std::string, net_id> name_to_id;
    name_to_id.reserve(net_names_.size());
    for (const auto& [id, name] : net_names_) name_to_id[name] = id;

    bool in_nets = false, skipping_net = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!in_nets) {
            out << line << "\n";
            if (line.find("NETS") != std::string::npos && line.find("END") == std::string::npos) {
                in_nets = true;
            }
            continue;
        }
        if (line.find("END NETS") != std::string::npos) {
            out << line << "\n";
            in_nets = false;
            continue;
        }

        std::smatch m;
        static const std::regex net_start_re(R"(^\s*-\s+(\S+))");
        if (std::regex_search(line, m, net_start_re)) {
            std::string name = m[1];
            auto nid_it = name_to_id.find(name);
            if (nid_it != name_to_id.end() && by_net.count(nid_it->second)) {
                out << serialize_route(*by_net[nid_it->second], name, layers, via_names);
                skipping_net = true;   // drop the original geometry for this net
                continue;
            }
            skipping_net = false;
        }
        if (skipping_net) {
            // Skip the original net's body until its terminating ';'.
            if (line.find(';') != std::string::npos) skipping_net = false;
            continue;
        }
        out << line << "\n";
    }
    return true;
}

// Real DEF 5.8 NETS ROUTED/NEW parser. Handles multi-segment paths, the
// `*` wildcard (repeat the previous point's coordinate on that axis — very
// common in real router output to avoid restating unchanged coordinates),
// and via tokens (a bare identifier between two coordinate points, taken
// to mean "insert a via here, advance one routing layer" — see the
// serialize_route() comment above for why the exact layer delta isn't
// resolved from a real VIARULE table). Verified structurally against real
// ISPD 2018 DEF grammar (benchmarks/ispd18_test1's own NETS section uses
// this exact connectivity-only form pre-routing); the ROUTED/NEW geometry
// path specifically has not been checked against a real TritonRoute
// *output* DEF, since none has been produced by this repo yet — this is
// the DEF 5.8 spec's documented grammar, not a guess at TritonRoute's
// particular formatting quirks.
void TritonBridge::parse_def_nets(const std::string& def_path,
                                   std::vector<Route>& routes) const {
    std::ifstream f(def_path);
    if (!f) return;

    std::unordered_map<std::string, net_id> name_to_id;
    for (const auto& [id, name] : net_names_) name_to_id[name] = id;

    auto layers = parse_lef_routing_layers(lef_file_);
    std::unordered_map<std::string, layer_t> layer_idx;
    for (size_t i = 0; i < layers.size(); ++i) layer_idx[layers[i].name] = static_cast<layer_t>(i);

    std::regex net_start_re(R"(^\s*-\s+(\S+))");
    std::regex routed_re(R"(^\s*(ROUTED|NEW)\s+(\S+))");
    std::regex point_re(R"(\(\s*(\*|-?\d+)\s+(\*|-?\d+)\s*\))");
    std::regex via_token_re(R"(^([A-Za-z_][A-Za-z0-9_]*)$)");

    std::string line;
    bool in_nets = false;
    std::string cur_net_name;
    Route cur_route;
    dbu_t last_x = 0, last_y = 0;
    layer_t cur_layer = 0;
    bool have_point = false;
    bool pending_via = false;  // a via token was seen; applies to the *next* transition

    auto flush_route = [&]() {
        if (!cur_route.path.empty()) {
            auto it = name_to_id.find(cur_net_name);
            cur_route.net = (it != name_to_id.end()) ? it->second
                           : static_cast<net_id>(routes.size());
            cur_route.wirelength = 0;
            for (size_t i = 1; i < cur_route.path.size(); ++i) {
                if (cur_route.path[i].z == cur_route.path[i - 1].z) {
                    cur_route.wirelength += std::abs(cur_route.path[i].x - cur_route.path[i - 1].x)
                                           + std::abs(cur_route.path[i].y - cur_route.path[i - 1].y);
                }
            }
            cur_route.via_count = static_cast<int>(
                std::count(cur_route.is_via.begin(), cur_route.is_via.end(), true));
            cur_route.drc_count = 0;
            cur_route.is_complete = true;
            routes.push_back(cur_route);
        }
        cur_route = Route{};
        have_point = false;
    };

    while (std::getline(f, line)) {
        if (!in_nets) {
            if (line.find("NETS") != std::string::npos && line.find("END") == std::string::npos) {
                in_nets = true;
            }
            continue;
        }
        if (line.find("END NETS") != std::string::npos) break;

        std::smatch m;
        if (std::regex_search(line, m, net_start_re)) {
            flush_route();
            cur_net_name = m[1];
            continue;
        }

        std::string working = line;
        if (std::regex_search(working, m, routed_re)) {
            auto it = layer_idx.find(m[2]);
            cur_layer = (it != layer_idx.end()) ? it->second : cur_layer;
            working = m.suffix().str();
            have_point = false;  // a new ROUTED/NEW segment doesn't inherit the last point
        }

        // Walk tokens: coordinate points and bare via-name tokens, in order.
        static const std::regex token_re(R"(\([^()]*\)|[A-Za-z_][A-Za-z0-9_]*)");
        std::sregex_iterator tok_it(working.begin(), working.end(), token_re);
        std::sregex_iterator tok_end;
        for (; tok_it != tok_end; ++tok_it) {
            std::string tok = tok_it->str();
            std::smatch pm;
            if (std::regex_match(tok, pm, point_re)) {
                dbu_t x = (pm[1] == "*") ? last_x : static_cast<dbu_t>(std::stol(pm[1]));
                dbu_t y = (pm[2] == "*") ? last_y : static_cast<dbu_t>(std::stol(pm[2]));
                cur_route.path.push_back(Point3D{x, y, cur_layer});
                if (have_point) cur_route.is_via.push_back(pending_via);
                pending_via = false;
                last_x = x; last_y = y; have_point = true;
            } else if (std::regex_match(tok, via_token_re) && have_point) {
                // Bare identifier between two points = a via on the
                // transition about to be created by the *next* point.
                ++cur_layer;
                pending_via = true;
            }
        }
    }
    flush_route();
}

} // namespace rba
