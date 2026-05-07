#pragma once
// Sky130 PDK technology constants for RBA-TritonRoute verification.
// Source: SkyWater sky130A PDK — all dimensions in DBU (1 DBU = 1 nm for sky130).

#include "rba_types.h"
#include <array>
#include <string>

namespace rba {
namespace sky130 {

// ─── Technology parameters ────────────────────────────────────────────────────

constexpr int    DBU_PER_MICRON = 1000;   // 1 DBU = 1 nm
constexpr int    NUM_METAL_LAYERS = 6;    // li1 + met1..met5
constexpr double TECH_NODE_NM = 130.0;

// ─── Layer indices ────────────────────────────────────────────────────────────

enum Layer : layer_t {
    LI1  = 0,   // Local interconnect (polysilicide-based)
    MET1 = 1,
    MET2 = 2,
    MET3 = 3,
    MET4 = 4,
    MET5 = 5,
};

inline const std::array<std::string, NUM_METAL_LAYERS> LAYER_NAMES = {
    "li1", "met1", "met2", "met3", "met4", "met5"
};

inline const std::array<std::string, NUM_METAL_LAYERS> VIA_NAMES = {
    "mcon",   // li1  → met1
    "via",    // met1 → met2
    "via2",   // met2 → met3
    "via3",   // met3 → met4
    "via4",   // met4 → met5
    ""        // met5 — no via above
};

// Preferred routing direction per layer (true = horizontal)
inline const std::array<bool, NUM_METAL_LAYERS> PREFERRED_HORIZONTAL = {
    false,  // li1  — vertical preferred
    true,   // met1 — horizontal preferred
    false,  // met2 — vertical preferred
    true,   // met3 — horizontal preferred
    false,  // met4 — vertical preferred
    true,   // met5 — horizontal preferred
};

// ─── DRC rule tables (all values in DBU = nm) ─────────────────────────────────

struct LayerDRCRules {
    dbu_t min_width;      // minimum wire width
    dbu_t min_spacing;    // minimum edge-to-edge spacing (same-net excluded)
    dbu_t min_area;       // minimum enclosed area (nm²)
    dbu_t eol_spacing;    // end-of-line spacing
    dbu_t eol_width;      // EOL trigger width threshold
    dbu_t min_step;       // minimum step distance
};

// Sky130A DRC rules — refer to sky130 pdk docs/rules for authoritative values
inline const std::array<LayerDRCRules, NUM_METAL_LAYERS> LAYER_RULES = {{
    //  width  space  area    eol_sp eol_w  step
    {   170,   170,   14520,  270,   270,   160 },   // li1
    {   140,   140,   15400,  250,   250,   0   },   // met1
    {   140,   140,   15400,  250,   250,   0   },   // met2
    {   300,   300,  160000,  500,   500,   0   },   // met3
    {   300,   300,  160000,  500,   500,   0   },   // met4
    {  1600,  1600, 4000000, 1600,  1600,   0   },   // met5
}};

// Via enclosure rules (per via type, lower layer enclosure)
struct ViaDRCRules {
    dbu_t via_width;       // via cut size
    dbu_t via_height;
    dbu_t via_spacing;     // cut-to-cut spacing
    dbu_t enc_lower;       // lower metal enclosure around via
    dbu_t enc_upper;       // upper metal enclosure around via
};

inline const std::array<ViaDRCRules, 5> VIA_RULES = {{
    // cut_w  cut_h  spacing  enc_lo enc_hi
    {  170,   170,   190,     0,      60  },  // mcon (li1-met1)
    {  150,   150,   170,     55,     55  },  // via  (met1-met2)
    {  200,   200,   200,     55,     55  },  // via2 (met2-met3)
    {  200,   200,   200,     60,     60  },  // via3 (met3-met4)
    {  800,   800,   800,     310,    310 },  // via4 (met4-met5)
}};

// ─── Utility helpers ──────────────────────────────────────────────────────────

inline const std::string& layer_name(layer_t z) {
    static const std::string unknown = "unknown";
    return (z < NUM_METAL_LAYERS) ? LAYER_NAMES[z] : unknown;
}

// Convert a layer name string from LEF/DEF to layer index (-1 if unknown)
inline int layer_index(const std::string& name) {
    for (int i = 0; i < NUM_METAL_LAYERS; ++i)
        if (LAYER_NAMES[i] == name) return i;
    return -1;
}

inline bool is_preferred_direction_horizontal(layer_t z) {
    return (z < NUM_METAL_LAYERS) && PREFERRED_HORIZONTAL[z];
}

inline const LayerDRCRules& drc_rules(layer_t z) {
    static const LayerDRCRules fallback{140, 140, 15400, 250, 250, 0};
    return (z < NUM_METAL_LAYERS) ? LAYER_RULES[z] : fallback;
}

// Returns the severity [0,1] of a width violation as a function of shortfall
inline float width_violation_severity(layer_t z, dbu_t actual_width) {
    dbu_t min_w = drc_rules(z).min_width;
    if (actual_width >= min_w) return 0.0f;
    return static_cast<float>(min_w - actual_width) / min_w;
}

// Returns the severity [0,1] of a spacing violation
inline float spacing_violation_severity(layer_t z, dbu_t actual_spacing) {
    dbu_t min_s = drc_rules(z).min_spacing;
    if (actual_spacing >= min_s) return 0.0f;
    return static_cast<float>(min_s - actual_spacing) / min_s;
}

} // namespace sky130
} // namespace rba
