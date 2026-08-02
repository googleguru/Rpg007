#include <gtest/gtest.h>
#include <fstream>
#include "triton_bridge.h"

using namespace rba;

namespace {

std::string write_temp(const std::string& name, const std::string& content) {
    std::string path = ::testing::TempDir() + "/rba_test_" + name;
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// Minimal but structurally real LEF: one routing layer stack + one MACRO
// with two pins, matching the exact grammar found in benchmarks/ispd18_test1.
const char* kLef = R"(VERSION 5.8 ;
UNITS
  DATABASE MICRONS 1000 ;
END UNITS
LAYER Metal1
  TYPE ROUTING ;
  DIRECTION HORIZONTAL ;
END Metal1
LAYER Metal2
  TYPE ROUTING ;
  DIRECTION VERTICAL ;
END Metal2
MACRO BUF1
  CLASS CORE ;
  SIZE 2.0 BY 2.0 ;
  PIN A
    DIRECTION INPUT ;
    PORT
      LAYER Metal1 ;
        RECT 0.0 0.0 0.2 0.2 ;
    END
  END A
  PIN Y
    DIRECTION OUTPUT ;
    PORT
      LAYER Metal1 ;
        RECT 1.8 1.8 2.0 2.0 ;
    END
  END Y
END BUF1
END LIBRARY
)";

// DEF with two placed instances and one net connecting them.
const char* kDef = R"(VERSION 5.8 ;
DESIGN test_design ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 10000 10000 ) ;
COMPONENTS 2 ;
- inst1 BUF1 + PLACED ( 0 0 ) N ;
- inst2 BUF1 + PLACED ( 4000 0 ) N ;
END COMPONENTS
PINS 0 ;
END PINS
NETS 1 ;
- net1
  ( inst1 Y ) ( inst2 A )
 ;
END NETS
END DESIGN
)";

const char* kGuide = R"(net1
(
0 0 2000 2000 Metal1
2000 0 4000 2000 Metal1
)
)";

}  // namespace

TEST(TritonBridgeRealParsing, LoadNetsResolvesRealPinCoordinates) {
    RBAConfig cfg;
    cfg.output_dir = ::testing::TempDir();
    std::string lef = write_temp("pins.lef", kLef);
    std::string def = write_temp("pins.def", kDef);
    std::string guide = write_temp("pins.guide", kGuide);

    TritonBridge bridge(cfg);
    ASSERT_TRUE(bridge.load_design(lef, def, guide));
    auto nets = bridge.load_nets(def);

    ASSERT_EQ(nets.size(), 1u);
    ASSERT_EQ(nets[0].pins.size(), 2u);

    // inst1's Y pin: macro-local centroid (1.9, 1.9) um, orientation N,
    // placed at (0,0) um -> absolute (1900, 1900) DBU at 1000 DBU/um.
    // inst2's A pin: macro-local centroid (0.1, 0.1) um, placed at
    // (4000, 0) DBU -> absolute (4100, 100) DBU.
    bool found_y = false, found_a = false;
    for (const auto& p : nets[0].pins) {
        if (p.loc.x == 1900 && p.loc.y == 1900) found_y = true;
        if (p.loc.x == 4100 && p.loc.y == 100) found_a = true;
    }
    EXPECT_TRUE(found_y) << "inst1/Y pin not resolved to its real transformed location";
    EXPECT_TRUE(found_a) << "inst2/A pin not resolved to its real transformed location";

    // Neither pin should have fallen back to the old hardcoded (0,0,0).
    for (const auto& p : nets[0].pins) {
        EXPECT_FALSE(p.loc.x == 0 && p.loc.y == 0)
            << "pin still at the old placeholder (0,0) — resolution failed";
    }
}

TEST(TritonBridgeRealParsing, CongestionMapReflectsGuideDensity) {
    RBAConfig cfg;
    cfg.output_dir = ::testing::TempDir();
    std::string guide = write_temp("congestion.guide", kGuide);

    TritonBridge bridge(cfg);
    auto cmap = bridge.estimate_congestion_from_guides(guide);

    EXPECT_GT(cmap.gcell_nx, 0);
    EXPECT_GT(cmap.gcell_ny, 0);
    EXPECT_EQ(cmap.n_layers, 1);  // only Metal1 appears in kGuide

    int total_overflow = 0;
    for (const auto& c : cmap.cells) total_overflow += c.overflow;
    EXPECT_GT(total_overflow, 0) << "guide rectangles did not register any congestion";
}

TEST(TritonBridgeRealParsing, ExtractRoutingGraphBuildsRealNodesAndEdges) {
    RBAConfig cfg;
    cfg.output_dir = ::testing::TempDir();
    std::string lef = write_temp("graph.lef", kLef);
    std::string def = write_temp("graph.def", kDef);
    std::string guide = write_temp("graph.guide", kGuide);

    TritonBridge bridge(cfg);
    ASSERT_TRUE(bridge.load_design(lef, def, guide));
    auto graph = bridge.extract_routing_graph();

    // 64x64 grid x 2 routing layers (Metal1, Metal2) from kLef.
    EXPECT_EQ(graph.nodes.size(), 64u * 64u * 2u);
    EXPECT_GT(graph.edges.size(), 0u);

    // Every edge must reference nodes that actually exist in node_idx.
    for (const auto& e : graph.edges) {
        EXPECT_TRUE(graph.node_idx.count(e.from)) << "edge references unknown 'from' node";
        EXPECT_TRUE(graph.node_idx.count(e.to)) << "edge references unknown 'to' node";
    }

    // At least one via edge should connect layer 0 to layer 1.
    bool found_via = false;
    for (const auto& e : graph.edges) {
        if (e.is_via) { found_via = true; break; }
    }
    EXPECT_TRUE(found_via) << "no via edges found between routing layers";
}

TEST(TritonBridgeRealParsing, ParseDefNetsHandlesWildcardsAndVias) {
    // Output-style DEF: a routed net using the '*' wildcard (repeat the
    // previous coordinate) and a via token mid-path — exercises the exact
    // off-by-one case that was wrong in an earlier draft of this parser
    // (the via must apply to the transition it precedes, not the one
    // before it).
    const char* def_routed = R"(VERSION 5.8 ;
DESIGN test_design ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 10000 10000 ) ;
COMPONENTS 0 ;
END COMPONENTS
PINS 0 ;
END PINS
NETS 1 ;
- net1
  ROUTED Metal1 ( 100 200 ) ( 300 * ) VIA12 ( * 400 )
 ;
END NETS
END DESIGN
)";
    RBAConfig cfg;
    cfg.output_dir = ::testing::TempDir();
    std::string lef = write_temp("routed.lef", kLef);
    std::string def = write_temp("routed.def", def_routed);

    TritonBridge bridge(cfg);
    ASSERT_TRUE(bridge.load_design(lef, def, ""));
    bridge.load_nets(def);  // populates net_names_ so net1 -> net_id resolves

    auto routes = bridge.extract_routes(def);
    ASSERT_EQ(routes.size(), 1u);
    const Route& r = routes[0];

    ASSERT_EQ(r.path.size(), 3u);
    EXPECT_EQ(r.path[0].x, 100); EXPECT_EQ(r.path[0].y, 200);
    EXPECT_EQ(r.path[1].x, 300); EXPECT_EQ(r.path[1].y, 200);  // '*' repeats y=200
    EXPECT_EQ(r.path[2].x, 300); EXPECT_EQ(r.path[2].y, 400);  // '*' repeats x=300

    ASSERT_EQ(r.is_via.size(), 2u);
    EXPECT_FALSE(r.is_via[0]) << "first transition (100,200)->(300,200) has no via token, must be false";
    EXPECT_TRUE(r.is_via[1]) << "VIA12 token precedes the (300,200)->(300,400) transition — must be true";
}

TEST(TritonBridgeRealParsing, WriteRoutesReplacesOnlyTargetedNetGeometry) {
    RBAConfig cfg;
    cfg.output_dir = ::testing::TempDir();
    std::string lef = write_temp("wr.lef", kLef);
    std::string def = write_temp("wr.def", kDef);

    TritonBridge bridge(cfg);
    ASSERT_TRUE(bridge.load_design(lef, def, ""));
    bridge.load_nets(def);  // populates net_names_ for net1 -> id 0

    Route r;
    r.net = 0;
    r.path = {Point3D{1000, 1000, 0}, Point3D{2000, 1000, 0}};
    r.is_via = {false};
    r.is_complete = true;

    std::string out_def = ::testing::TempDir() + "/rba_test_wr_out.def";
    ASSERT_TRUE(bridge.write_routes({r}, def, out_def));

    std::ifstream in(out_def);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1000 1000"), std::string::npos)
        << "written route geometry not found in output DEF";
    EXPECT_NE(content.find("END DESIGN"), std::string::npos)
        << "output DEF is missing content after the NETS section — write_routes truncated the file";
}
