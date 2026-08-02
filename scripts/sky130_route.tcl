# Sky130 PDK — OpenROAD detailed routing script for RBA-TritonRoute
# Usage: openroad -exit sky130_route.tcl
#
# Required env vars:
#   SKY130_PDK  — path to sky130A PDK root
#   DESIGN_DEF  — input placed DEF
#   GUIDE_FILE  — global routing guide
#   OUTPUT_DIR  — directory for outputs
#   THREADS     — number of routing threads (default 8)

# ── PDK paths ──────────────────────────────────────────────────────────────────
set pdk_root   $::env(SKY130_PDK)
set tech_lef   $pdk_root/libs.ref/sky130_fd_sc_hd/techlef/sky130_fd_sc_hd__nom.tlef
set cell_lef   $pdk_root/libs.ref/sky130_fd_sc_hd/lef/sky130_fd_sc_hd.lef

# ── Design inputs ──────────────────────────────────────────────────────────────
set input_def  $::env(DESIGN_DEF)
set guide_file $::env(GUIDE_FILE)
set output_dir $::env(OUTPUT_DIR)
set n_threads  [expr {[info exists ::env(THREADS)] ? $::env(THREADS) : 8}]

# ── Output files ──────────────────────────────────────────────────────────────
set routed_def  $output_dir/routed_sky130.def
set drc_report  $output_dir/drc_sky130.rpt
set gds_out     $output_dir/routed_sky130.gds

# ── Load technology and design ─────────────────────────────────────────────────
read_lef $tech_lef
read_lef $cell_lef
read_def $input_def

# ── Set Sky130 routing layer constraints ──────────────────────────────────────
# li1 = layer 1 (local interconnect, vertical preferred)
# met1–met5 = layers 2–6
set_routing_layers -signal "li1 met1 met2 met3 met4 met5"
set_routing_layers -clock  "met3 met4 met5"

# ── Detailed routing ──────────────────────────────────────────────────────────
# Routing layer bounds are set above via set_routing_layers; passing
# -bottom_routing_layer/-top_routing_layer to detailed_route is deprecated
# (OpenROAD errors: "use set_routing_layers command instead") and was
# removed from this call for that reason.
detailed_route \
    -guide            $guide_file \
    -output_drc       $drc_report \
    -output_maze      $output_dir/maze.log \
    -verbose          0 \
    -threads          $n_threads

# ── Write outputs ──────────────────────────────────────────────────────────────
write_def $routed_def

# ── Design metrics ─────────────────────────────────────────────────────────────
report_design_area
report_wire_length
report_check_types -max_slew -max_capacitance -max_fanout -violators

# ── Optional GDS export via KLayout ──────────────────────────────────────────
# Uncomment if klayout is available and PDK stream-out map is present:
# write_db $output_dir/routed_sky130.odb
# exec klayout -b \
#     -rd input=$routed_def \
#     -rd tech_lef=$tech_lef \
#     -rd output=$gds_out \
#     -r $pdk_root/libs.tech/klayout/sky130A.lyt

puts "\[Sky130\] Routing complete — DEF: $routed_def"
puts "\[Sky130\] DRC report:           $drc_report"
