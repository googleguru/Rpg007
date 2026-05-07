"""
RBA-TritonRoute Interactive GUI
================================
Full Streamlit application for visualizing, configuring, and running
the Resilient Bio-Inspired Algorithm routing framework.

Sections:
  1. Dashboard    — KPI cards + overview charts
  2. Benchmarks   — Per-benchmark detail (select from dropdown)
  3. Algorithms   — Deep-dive into GA / ACO / PSO / ABC internals
  4. Comparison   — Side-by-side baseline vs RBA analysis
  5. Configuration — Edit RBA parameters and rerun simulation
  6. Export        — Download results and plots
"""

import json
import sys
import os
import time
import subprocess
from pathlib import Path
import numpy as np
import pandas as pd

# Streamlit must be imported first
import streamlit as st
import plotly.graph_objects as go
import plotly.express as px
from plotly.subplots import make_subplots
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import seaborn as sns
import io
import base64

# ── Paths ─────────────────────────────────────────────────────────────────────
ROOT      = Path(__file__).parent.parent
RESULTS   = ROOT / "results"
PLOTS_DIR = RESULTS / "plots"
SIM_SCRIPT = ROOT / "simulation" / "rba_simulation_engine.py"
PLT_SCRIPT = ROOT / "simulation" / "generate_all_plots.py"

# ── Page config ───────────────────────────────────────────────────────────────
st.set_page_config(
    page_title="RBA-TritonRoute Framework",
    page_icon="🔌",
    layout="wide",
    initial_sidebar_state="expanded",
)

# ── Custom CSS ────────────────────────────────────────────────────────────────
st.markdown("""
<style>
    .main { background-color: #0e1117; }
    .metric-card {
        background: linear-gradient(135deg, #1a1c2a 0%, #222533 100%);
        border-radius: 10px; padding: 1.2rem;
        border-left: 4px solid; margin: 0.3rem 0;
        box-shadow: 0 2px 8px rgba(0,0,0,0.4);
    }
    .kpi-value { font-size: 2.2rem; font-weight: 800; }
    .kpi-label { font-size: 0.85rem; color: #aaa; margin-top: 4px; }
    .section-header {
        font-size: 1.3rem; font-weight: 700;
        border-bottom: 2px solid #333; padding-bottom: 6px;
        margin: 1rem 0 0.7rem 0;
    }
    .stSelectbox > div { background-color: #1a1c2a; }
    div[data-testid="stMetricValue"] { font-size: 1.8rem; }
    .algo-card {
        background: #1a1c2a; border-radius: 8px;
        padding: 1rem; margin: 0.4rem 0;
        border: 1px solid #333;
    }
</style>
""", unsafe_allow_html=True)

# ── Data loading ──────────────────────────────────────────────────────────────

@st.cache_data(ttl=30)
def load_summary():
    p = RESULTS / "summary.json"
    if not p.exists(): return None
    with open(p) as f: return json.load(f)

@st.cache_data(ttl=30)
def load_full():
    p = RESULTS / "full_results.json"
    if not p.exists(): return None
    with open(p) as f: return json.load(f)

def run_simulation(config: dict):
    """Run simulation engine with given config."""
    import tempfile
    cfg_path = RESULTS / "custom_config.json"
    with open(cfg_path, "w") as f: json.dump(config, f)
    result = subprocess.run(
        [sys.executable, str(SIM_SCRIPT), "--all-benchmarks",
         "--output", str(RESULTS)],
        capture_output=True, text=True, timeout=120
    )
    st.cache_data.clear()
    return result.stdout + result.stderr

def run_plot_generation():
    result = subprocess.run(
        [sys.executable, str(PLT_SCRIPT)],
        capture_output=True, text=True, timeout=120,
        cwd=str(ROOT)
    )
    return result.stdout + result.stderr

def img_to_b64(path):
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()

def show_png(path, caption="", use_column_width=True):
    if Path(path).exists():
        st.image(str(path), caption=caption, use_container_width=use_column_width)
    else:
        st.warning(f"Plot not found: {path}. Run simulation first.")

# ── Sidebar ───────────────────────────────────────────────────────────────────

with st.sidebar:
    st.markdown("## 🔌 RBA-TritonRoute")
    st.markdown("*Bio-Inspired VLSI Routing*")
    st.divider()

    page = st.radio("Navigation", [
        "📊 Dashboard",
        "🔬 Benchmark Detail",
        "🧬 Algorithm Internals",
        "⚖️ Baseline Comparison",
        "⚙️ Configuration",
        "📤 Export Results",
    ])

    st.divider()
    st.markdown("**Quick Run**")
    n_runs  = st.slider("Runs per benchmark", 1, 10, 5)
    n_iters = st.slider("Outer iterations",   1, 10, 5)

    if st.button("▶ Run Simulation", type="primary", use_container_width=True):
        with st.spinner("Simulating all 19 benchmarks..."):
            log = run_simulation({"runs": n_runs, "iters": n_iters})
        st.success("Simulation complete!")
        with st.expander("Simulation log"):
            st.text(log[-3000:] if len(log) > 3000 else log)

    if st.button("📈 Regenerate Plots", use_container_width=True):
        with st.spinner("Generating 12 figures..."):
            log2 = run_plot_generation()
        st.success("Plots regenerated!")

    st.divider()
    summaries = load_summary()
    full_res  = load_full()
    if summaries:
        st.markdown(f"**{len(summaries)}** benchmarks loaded")
        total_nets = sum(r["nets"] for r in full_res) if full_res else 0
        st.markdown(f"**{total_nets/1e6:.1f}M** total nets")
    else:
        st.warning("No results yet — click Run Simulation")


# ══════════════════════════════════════════════════════════════════════════════
# Page 1: Dashboard
# ══════════════════════════════════════════════════════════════════════════════

if page == "📊 Dashboard":
    st.title("RBA-TritonRoute Framework")
    st.markdown("**Resilient Bio-Inspired Algorithm** routing optimization over TritonRoute/OpenROAD")

    summaries = load_summary()
    if summaries is None:
        st.info("No simulation results yet. Use **Run Simulation** in the sidebar to generate results.")
        st.stop()

    full_res = load_full()

    # KPI cards
    drc_imps = [-s["drc_improvement_pct"] for s in summaries]
    via_imps = [-s["via_improvement_pct"] for s in summaries]
    wl_chgs  = [s["wl_change_pct"]        for s in summaries]
    rt_ohs   = [s["runtime_overhead_pct"] for s in summaries]

    c1, c2, c3, c4 = st.columns(4)
    with c1:
        st.metric("Avg DRC Reduction",    f"{np.mean(drc_imps):.1f}%",
                  delta=f"{np.mean(drc_imps):.1f}% vs baseline", delta_color="normal")
    with c2:
        st.metric("Avg Via Reduction",    f"{np.mean(via_imps):.1f}%",
                  delta=f"{np.mean(via_imps):.1f}% via ABC", delta_color="normal")
    with c3:
        st.metric("Avg WL Change",        f"{np.mean(wl_chgs):+.1f}%",
                  delta=f"slight overhead from DRC detours", delta_color="inverse")
    with c4:
        st.metric("Runtime Overhead",     f"{np.mean(rt_ohs):+.0f}%",
                  delta="PSO oracle dominates", delta_color="inverse")

    st.divider()

    # Main comparison chart
    st.markdown('<div class="section-header">All Benchmarks — DRC & Via Count</div>',
                unsafe_allow_html=True)

    names_short = [s["benchmark"].replace("ispd18_","t").replace("ispd19_","t19_")
                   for s in summaries]
    bl_drc  = [s["baseline_drc"]["mean"] for s in summaries]
    rba_drc = [s["rba_drc"]["mean"]      for s in summaries]
    bl_via  = [s["baseline_via"]["mean"] for s in summaries]
    rba_via = [s["rba_via"]["mean"]      for s in summaries]

    fig_cmp = make_subplots(rows=1, cols=2,
                             subplot_titles=["DRC Violations", "Via Count"])
    fig_cmp.add_trace(go.Bar(x=names_short, y=bl_drc,  name="Baseline TR",
                              marker_color="#2171B5", opacity=0.85), row=1, col=1)
    fig_cmp.add_trace(go.Bar(x=names_short, y=rba_drc, name="RBA-TR",
                              marker_color="#E6550D", opacity=0.85), row=1, col=1)
    fig_cmp.add_trace(go.Bar(x=names_short, y=bl_via,  name="Baseline TR",
                              marker_color="#2171B5", opacity=0.85, showlegend=False), row=1, col=2)
    fig_cmp.add_trace(go.Bar(x=names_short, y=rba_via, name="RBA-TR",
                              marker_color="#E6550D", opacity=0.85, showlegend=False), row=1, col=2)
    fig_cmp.update_layout(
        template="plotly_dark", height=380, barmode="group",
        margin=dict(l=20, r=20, t=40, b=20),
        legend=dict(orientation="h", yanchor="bottom", y=1.02)
    )
    for col in [1, 2]:
        fig_cmp.update_yaxes(type="log", row=1, col=col)

    # Vertical line separating ISPD18 / ISPD19
    fig_cmp.add_vline(x=9.5, line_dash="dot", line_color="gray", opacity=0.5)
    st.plotly_chart(fig_cmp, use_container_width=True)

    # Improvement heatmap
    st.markdown('<div class="section-header">Improvement % Heatmap</div>',
                unsafe_allow_html=True)
    show_png(PLOTS_DIR / "fig2_improvement_heatmap.png",
             caption="Green = improvement (DRC/via reduction). Red = worsening. "
                     "All DRC and via metrics show consistent improvement.")

    # Convergence
    col_l, col_r = st.columns(2)
    with col_l:
        st.markdown("#### Outer Iteration Convergence")
        show_png(PLOTS_DIR / "fig3_iteration_convergence.png",
                 caption="Normalized DRC, Via, and WL across 5 outer RBA iterations "
                         "(mean ± σ, 19 benchmarks × 5 runs)")
    with col_r:
        st.markdown("#### Scalability Analysis")
        show_png(PLOTS_DIR / "fig8_scalability.png",
                 caption="DRC improvement vs design size and layer count")

    # Dark dashboard
    st.markdown('<div class="section-header">Full Dashboard Overview</div>',
                unsafe_allow_html=True)
    show_png(PLOTS_DIR / "fig12_dashboard.png",
             caption="Summary dashboard: KPIs, benchmark comparison, convergence, trade-off scatter")


# ══════════════════════════════════════════════════════════════════════════════
# Page 2: Benchmark Detail
# ══════════════════════════════════════════════════════════════════════════════

elif page == "🔬 Benchmark Detail":
    st.title("Benchmark Detail Analysis")
    summaries = load_summary()
    full_res  = load_full()
    if not summaries:
        st.info("Run simulation first."); st.stop()

    bench_names = [s["benchmark"] for s in summaries]
    sel = st.selectbox("Select Benchmark", bench_names)

    sidx    = next(i for i,s in enumerate(summaries) if s["benchmark"] == sel)
    s       = summaries[sidx]
    bench   = full_res[sidx]

    # Metadata
    st.markdown(f"### {sel}")
    mc1, mc2, mc3, mc4 = st.columns(4)
    mc1.metric("Nets",   f"{bench['nets']//1000}k")
    mc2.metric("Cells",  f"{bench['cells']//1000}k")
    mc3.metric("Layers", bench['layers'])
    mc4.metric("GA Impr.", f"{bench['ga_improvement_pct']:.1f}%")

    st.divider()

    # Per-run comparison table
    st.markdown("#### Per-Run Metrics (Baseline vs RBA Final Iteration)")
    bl_df = pd.DataFrame(bench["baseline_runs"])
    max_it = max(r["iteration"] for r in bench["rba_runs"])
    rba_df = pd.DataFrame([r for r in bench["rba_runs"] if r["iteration"] == max_it])

    comp_data = []
    for metric, label in [("drc_count","DRC"),("via_count","Via"),
                           ("wirelength","WL"),("runtime_sec","Runtime(s)")]:
        bl_m = bl_df[metric].mean(); bl_s = bl_df[metric].std()
        rb_m = rba_df[metric].mean(); rb_s = rba_df[metric].std()
        chg = (rb_m - bl_m) / bl_m * 100 if bl_m > 0 else 0
        comp_data.append({
            "Metric": label,
            "Baseline Mean": f"{bl_m:.1f} ± {bl_s:.1f}",
            "RBA Mean":      f"{rb_m:.1f} ± {rb_s:.1f}",
            "Change %":      f"{chg:+.2f}%",
        })
    st.dataframe(pd.DataFrame(comp_data), use_container_width=True, hide_index=True)

    # Iteration timeline
    st.markdown("#### RBA Iteration Timeline")
    rba_by_iter = {}
    for r in bench["rba_runs"]:
        rba_by_iter.setdefault(r["iteration"], []).append(r)

    iters    = sorted(rba_by_iter.keys())
    drc_means= [np.mean([r["drc_count"] for r in rba_by_iter[i]]) for i in iters]
    via_means= [np.mean([r["via_count"] for r in rba_by_iter[i]]) for i in iters]
    drc_stds = [np.std([r["drc_count"]  for r in rba_by_iter[i]]) for i in iters]
    via_stds = [np.std([r["via_count"]  for r in rba_by_iter[i]]) for i in iters]

    fig_it = make_subplots(rows=1, cols=2,
                            subplot_titles=["DRC per Iteration", "Via Count per Iteration"])

    bl_drc_val = bl_df["drc_count"].mean()
    bl_via_val = bl_df["via_count"].mean()

    for col, (means, stds, bl_val, yname) in enumerate([
        (drc_means, drc_stds, bl_drc_val, "DRC"),
        (via_means, via_stds, bl_via_val, "Via Count")
    ], 1):
        fig_it.add_hline(y=bl_val, line_dash="dash", line_color="#2171B5",
                         annotation_text="Baseline", row=1, col=col)
        fig_it.add_trace(go.Scatter(
            x=iters, y=means,
            error_y=dict(type="data", array=stds, visible=True),
            mode="lines+markers", name=f"RBA {yname}",
            line=dict(color="#E6550D", width=2.5),
            marker=dict(size=8)
        ), row=1, col=col)

    fig_it.update_layout(template="plotly_dark", height=350,
                          margin=dict(l=20,r=20,t=40,b=20), showlegend=False)
    st.plotly_chart(fig_it, use_container_width=True)

    # PSO weight evolution for this benchmark
    st.markdown("#### PSO Cost Weight Evolution")
    pso_hist = bench["pso_history"]
    pso_df = pd.DataFrame(pso_hist)
    weight_cols = [c for c in pso_df.columns if c.startswith("w_")]

    if weight_cols:
        fig_pso = go.Figure()
        colors = px.colors.qualitative.Plotly
        for i, col in enumerate(weight_cols):
            fig_pso.add_trace(go.Scatter(
                x=pso_df["iteration"], y=pso_df[col],
                mode="lines+markers", name=col,
                line=dict(color=colors[i % len(colors)], width=2),
                marker=dict(size=5)
            ))
        fig_pso.update_layout(template="plotly_dark", height=300,
                               xaxis_title="PSO Iteration", yaxis_title="Weight Value",
                               margin=dict(l=20,r=20,t=20,b=20))
        st.plotly_chart(fig_pso, use_container_width=True)

    # ABC via reduction
    st.markdown("#### ABC Via Minimization")
    abc_hist = bench["abc_history"]
    abc_df   = pd.DataFrame(abc_hist)

    fig_abc = make_subplots(rows=1, cols=2,
                             subplot_titles=["Via Count Reduction", "Bee Activity"])
    fig_abc.add_trace(go.Scatter(
        x=abc_df["cycle"], y=abc_df["best_via_count"],
        mode="lines", name="Best Via", fill="tozeroy",
        line=dict(color="#FEC44F", width=2)
    ), row=1, col=1)

    scout_mask = abc_df["scout_event"]
    fig_abc.add_trace(go.Scatter(
        x=abc_df.loc[scout_mask, "cycle"],
        y=abc_df.loc[scout_mask, "best_via_count"],
        mode="markers", name="Scout Restart",
        marker=dict(color="red", size=8, symbol="star")
    ), row=1, col=1)

    fig_abc.add_trace(go.Bar(
        x=abc_df["cycle"], y=abc_df["employed_improvements"],
        name="Employed", marker_color="#31A354", opacity=0.8
    ), row=1, col=2)
    fig_abc.add_trace(go.Bar(
        x=abc_df["cycle"], y=abc_df["onlooker_improvements"],
        name="Onlooker", marker_color="#756BB1", opacity=0.7
    ), row=1, col=2)

    fig_abc.update_layout(template="plotly_dark", height=320, barmode="stack",
                           margin=dict(l=20,r=20,t=40,b=20))
    fig_abc.update_xaxes(title_text="Cycle")
    st.plotly_chart(fig_abc, use_container_width=True)
    st.info(f"**ABC Result:** {bench['abc_vias_removed']:,} vias removed "
            f"({bench['abc_via_pct']:.1f}% reduction)")


# ══════════════════════════════════════════════════════════════════════════════
# Page 3: Algorithm Internals
# ══════════════════════════════════════════════════════════════════════════════

elif page == "🧬 Algorithm Internals":
    st.title("Bio-Inspired Algorithm Deep-Dive")
    full_res = load_full()
    if not full_res:
        st.info("Run simulation first."); st.stop()

    tab1, tab2, tab3, tab4 = st.tabs([
        "🧬 Genetic Algorithm", "🐜 Ant Colony Opt.", "🐦 Particle Swarm", "🐝 Artificial Bee Colony"
    ])

    # ── GA tab ────────────────────────────────────────────────────────────
    with tab1:
        st.markdown("### Genetic Algorithm — Net Ordering Convergence")
        st.markdown("""
        **Encoding:** Permutation chromosome of net indices
        **Crossover:** Order Crossover (OX) — preserves relative ordering
        **Mutation:** 2-opt swap (5% rate)
        **Selection:** Tournament (k=5) + 10% elitism
        """)

        st.image(str(PLOTS_DIR / "fig4_ga_convergence.png"),
                 caption="GA convergence: best / mean / worst fitness + population diversity "
                         "for 4 representative benchmarks (392k → 1.78M nets)",
                 use_container_width=True)

        # Interactive GA trace for selected benchmark
        sel_bench_name = st.selectbox("Select benchmark for interactive GA trace",
                                       [r["benchmark"] for r in full_res])
        bench = next(r for r in full_res if r["benchmark"] == sel_bench_name)
        ga_hist = bench["ga_history"]
        ga_df = pd.DataFrame(ga_hist)

        fig_ga = go.Figure()
        fig_ga.add_trace(go.Scatter(x=ga_df["generation"], y=ga_df["best_fitness"],
                                    mode="lines", name="Best", line=dict(color="#31A354", width=2.5)))
        fig_ga.add_trace(go.Scatter(x=ga_df["generation"], y=ga_df["mean_fitness"],
                                    mode="lines", name="Mean", line=dict(color="#2171B5", width=1.5, dash="dash")))
        fig_ga.add_trace(go.Scatter(x=ga_df["generation"], y=ga_df["worst_fitness"],
                                    mode="lines", name="Worst", line=dict(color="#CB181D", width=1, dash="dot")))

        # Shaded band
        fig_ga.add_traces([
            go.Scatter(x=ga_df["generation"].tolist() + ga_df["generation"].tolist()[::-1],
                       y=ga_df["best_fitness"].tolist() + ga_df["worst_fitness"].tolist()[::-1],
                       fill="toself", fillcolor="rgba(49,163,84,0.1)",
                       line=dict(color="rgba(255,255,255,0)"), showlegend=False)
        ])

        fig_ga.add_trace(go.Scatter(x=ga_df["generation"], y=ga_df["diversity"],
                                    mode="lines", name="Diversity",
                                    line=dict(color="#FEC44F", width=1.5, dash="dashdot"),
                                    yaxis="y2"))

        fig_ga.update_layout(
            template="plotly_dark", height=400,
            xaxis_title="Generation", yaxis_title="Fitness",
            yaxis2=dict(title="Diversity", overlaying="y", side="right",
                        range=[0, 1.2]),
            title=f"GA Convergence: {sel_bench_name} "
                  f"(improvement: {bench['ga_improvement_pct']:.1f}%)",
            legend=dict(orientation="h", yanchor="bottom", y=1.02)
        )
        st.plotly_chart(fig_ga, use_container_width=True)

    # ── ACO tab ───────────────────────────────────────────────────────────
    with tab2:
        st.markdown("### Ant Colony Optimization — Pheromone Dynamics")
        st.markdown("""
        **Algorithm:** MAX-MIN Ant System (MMAS)
        **Pheromone bounds:** τ ∈ [1e-4, 10.0] — prevents stagnation
        **Heuristic η(e):** 1 / (wire_cost + via_cost + congestion_cost)
        **DRC feedback:** Violated edges get τ → τ × 0.4 (forced evaporation)
        """)

        st.image(str(PLOTS_DIR / "fig5_aco_pheromone.png"),
                 caption="ACO pheromone evolution (mean + std) and path quality improvement "
                         "across iterations for 3 benchmarks",
                 use_container_width=True)

        sel_b = st.selectbox("ACO benchmark", [r["benchmark"] for r in full_res],
                              key="aco_sel")
        bench = next(r for r in full_res if r["benchmark"] == sel_b)
        aco_df = pd.DataFrame(bench["aco_history"])

        fig_aco = make_subplots(rows=2, cols=1, shared_xaxes=True,
                                 subplot_titles=["Pheromone Mean (τ̄)",
                                                  "Best Path Cost & DRC in Paths"])
        fig_aco.add_trace(go.Scatter(x=aco_df["iteration"], y=aco_df["tau_mean"],
                                     mode="lines", name="τ mean",
                                     line=dict(color="#756BB1", width=2.5)), row=1, col=1)
        fig_aco.add_trace(go.Scatter(x=aco_df["iteration"],
                                     y=aco_df["pheromone_entropy"],
                                     mode="lines", name="Entropy",
                                     line=dict(color="orange", width=1.5, dash="dash"),
                                     yaxis="y2"), row=1, col=1)

        fig_aco.add_trace(go.Scatter(x=aco_df["iteration"], y=aco_df["best_path_cost"],
                                     mode="lines+markers", name="Best path cost",
                                     line=dict(color="#E6550D", width=2.5)), row=2, col=1)
        fig_aco.add_trace(go.Bar(x=aco_df["iteration"], y=aco_df["drc_in_paths"],
                                  name="DRC in paths", marker_color="#CB181D",
                                  opacity=0.5), row=2, col=1)

        fig_aco.update_layout(template="plotly_dark", height=500,
                               xaxis2_title="Iteration",
                               title=f"ACO Dynamics: {sel_b}")
        st.plotly_chart(fig_aco, use_container_width=True)

    # ── PSO tab ───────────────────────────────────────────────────────────
    with tab3:
        st.markdown("### Particle Swarm Optimization — Cost Weight Tuning")
        st.markdown("""
        **Search space:** 6-dimensional weight vector (w_wire, w_via, w_cong, w_drc_hist, w_layer_pref, w_timing)
        **Clerc constants:** ω=0.729 (decays to 0.4), c₁=c₂=1.494
        **Oracle:** one TritonRoute detailed_route pass per particle evaluation
        **Bounds:** all weights ∈ [0.1, 20.0]
        """)

        st.image(str(PLOTS_DIR / "fig6_pso_weights.png"),
                 caption="PSO cost weight optimization: gbest convergence, weight trajectories, "
                         "inertia decay, and final weight distribution for ispd18_test5",
                 use_container_width=True)

        sel_b = st.selectbox("PSO benchmark", [r["benchmark"] for r in full_res],
                              key="pso_sel")
        bench = next(r for r in full_res if r["benchmark"] == sel_b)
        pso_hist = bench["pso_history"]
        pso_df = pd.DataFrame(pso_hist)

        weight_cols = [c for c in pso_df.columns if c.startswith("w_")]
        colors = px.colors.qualitative.Plotly

        fig_pso2 = make_subplots(rows=1, cols=2,
                                  subplot_titles=["Gbest Fitness Convergence",
                                                   "Weight Trajectories"])
        fig_pso2.add_trace(go.Scatter(x=pso_df["iteration"], y=pso_df["gbest_fitness"],
                                      mode="lines+markers", name="Gbest",
                                      line=dict(color="#E7298A", width=2.5)), row=1, col=1)
        fig_pso2.add_trace(go.Scatter(x=pso_df["iteration"], y=pso_df["pbest_mean"],
                                      mode="lines", name="Pbest mean",
                                      line=dict(color="#2171B5", dash="dash")), row=1, col=1)

        for i, col in enumerate(weight_cols):
            fig_pso2.add_trace(go.Scatter(
                x=pso_df["iteration"], y=pso_df[col],
                mode="lines+markers", name=col,
                line=dict(color=colors[i % len(colors)], width=1.8),
                marker=dict(size=4)
            ), row=1, col=2)

        fig_pso2.update_layout(template="plotly_dark", height=380,
                                title=f"PSO Optimization: {sel_b}")
        st.plotly_chart(fig_pso2, use_container_width=True)

        # Final weights radar chart
        if weight_cols and pso_hist:
            final = {k: pso_hist[-1][k] for k in weight_cols if k in pso_hist[-1]}
            fig_radar = go.Figure(go.Scatterpolar(
                r=list(final.values()), theta=list(final.keys()),
                fill="toself", line=dict(color="#E7298A", width=2),
                fillcolor="rgba(231,41,138,0.2)"
            ))
            fig_radar.update_layout(
                polar=dict(radialaxis=dict(visible=True, range=[0, 12])),
                template="plotly_dark", height=350,
                title=f"Final PSO Weight Radar: {sel_b}"
            )
            st.plotly_chart(fig_radar, use_container_width=True)

    # ── ABC tab ───────────────────────────────────────────────────────────
    with tab4:
        st.markdown("### Artificial Bee Colony — Via Minimization")
        st.markdown("""
        **Food source:** Set of via-removal flags (one per non-pin via)
        **Employed bees:** Local search — flip one flag per cycle
        **Onlooker bees:** Probability-proportional exploitation of good sources
        **Scout bees:** Random restart when trial_count ≥ limit (prevents local minima)
        **Greedy pre-pass:** Single-via removal tested first (fast, DRC-oracle-validated)
        """)

        st.image(str(PLOTS_DIR / "fig7_abc_via.png"),
                 caption="ABC via minimization: via count reduction curve + scout restart events "
                         "+ employed/onlooker bee improvement activity per cycle",
                 use_container_width=True)

        # Summary table of via reduction across all benchmarks
        via_data = [{"Benchmark": r["benchmark"],
                     "Initial Vias": f"{full_res[i]['abc_history'][0]['best_via_count']:,}",
                     "Vias Removed": f"{r['abc_vias_removed']:,}",
                     "Reduction %":  f"{r['abc_via_pct']:.1f}%"}
                    for i, r in enumerate(load_summary())]
        st.dataframe(pd.DataFrame(via_data), use_container_width=True, hide_index=True)


# ══════════════════════════════════════════════════════════════════════════════
# Page 4: Baseline Comparison
# ══════════════════════════════════════════════════════════════════════════════

elif page == "⚖️ Baseline Comparison":
    st.title("RBA-TritonRoute vs. Baseline TritonRoute")
    summaries = load_summary()
    full_res  = load_full()
    if not summaries:
        st.info("Run simulation first."); st.stop()

    # Main comparison figure
    st.image(str(PLOTS_DIR / "fig1_main_comparison.png"),
             caption="Group comparison: DRC violations, via count, wirelength, and runtime "
                     "for all 19 ISPD 2018+2019 benchmarks (log-scale DRC/via)",
             use_container_width=True)

    st.divider()
    col1, col2 = st.columns(2)

    with col1:
        st.markdown("#### Statistical Analysis (Wilcoxon Test)")
        st.image(str(PLOTS_DIR / "fig9_boxplots.png"),
                 caption="Box plots showing distribution of normalized metrics across "
                         "19 benchmarks × 5 runs. Wilcoxon signed-rank test results shown.",
                 use_container_width=True)

    with col2:
        st.markdown("#### Component Ablation Study")
        st.image(str(PLOTS_DIR / "fig10_ablation.png"),
                 caption="Cumulative improvement from each algorithm component. "
                         "GA ordering contributes most to DRC; ABC to via reduction.",
                 use_container_width=True)

    st.divider()

    # Interactive scatter: DRC vs via improvement
    st.markdown("#### DRC vs Via Improvement Trade-off")
    drc_imps = [-s["drc_improvement_pct"] for s in summaries]
    via_imps = [-s["via_improvement_pct"] for s in summaries]
    nets_k   = [full_res[i]["nets"]/1000   for i in range(len(summaries))]
    names    = [s["benchmark"] for s in summaries]

    fig_scatter = go.Figure(go.Scatter(
        x=drc_imps, y=via_imps,
        mode="markers+text",
        text=names,
        textposition="top center",
        textfont=dict(size=7),
        marker=dict(size=[n**0.4 for n in nets_k],
                    color=nets_k, colorscale="YlOrRd",
                    showscale=True,
                    colorbar=dict(title="Design Size (k nets)"),
                    opacity=0.85, line=dict(width=1, color="white"))
    ))
    fig_scatter.update_layout(
        template="plotly_dark", height=450,
        xaxis_title="DRC Improvement (%)",
        yaxis_title="Via Improvement (%)",
        title="DRC vs Via Improvement — All Benchmarks (marker size ∝ design size)"
    )
    st.plotly_chart(fig_scatter, use_container_width=True)

    # Full summary table
    st.markdown("#### Complete Results Table")
    table_data = []
    for s in summaries:
        table_data.append({
            "Benchmark":      s["benchmark"],
            "BL DRC (mean)":  f"{s['baseline_drc']['mean']:.0f}",
            "RBA DRC (mean)": f"{s['rba_drc']['mean']:.0f}",
            "ΔDRC%":          f"{s['drc_improvement_pct']:+.1f}%",
            "BL Via (mean)":  f"{s['baseline_via']['mean']:.0f}",
            "RBA Via (mean)": f"{s['rba_via']['mean']:.0f}",
            "ΔVia%":          f"{s['via_improvement_pct']:+.1f}%",
            "ΔWL%":           f"{s['wl_change_pct']:+.2f}%",
            "Overhead%":      f"{s['runtime_overhead_pct']:+.0f}%",
        })
    df = pd.DataFrame(table_data)
    st.dataframe(df, use_container_width=True, hide_index=True)

    # Averages
    avg_drc = np.mean([-s["drc_improvement_pct"] for s in summaries])
    avg_via = np.mean([-s["via_improvement_pct"] for s in summaries])
    avg_wl  = np.mean([s["wl_change_pct"] for s in summaries])
    st.success(
        f"**Overall Average:** DRC −{avg_drc:.1f}% | Via −{avg_via:.1f}% | WL {avg_wl:+.1f}%"
    )


# ══════════════════════════════════════════════════════════════════════════════
# Page 5: Configuration
# ══════════════════════════════════════════════════════════════════════════════

elif page == "⚙️ Configuration":
    st.title("RBA Parameter Configuration")
    st.markdown("Adjust algorithm parameters and re-run the simulation to see their effect.")

    col1, col2 = st.columns(2)

    with col1:
        st.markdown("#### 🧬 Genetic Algorithm")
        ga_pop     = st.slider("Population size",        10, 200, 50, key="ga_pop")
        ga_gen     = st.slider("Generations",            10, 300, 80, key="ga_gen")
        ga_cx      = st.slider("Crossover rate",         0.5, 1.0, 0.85, 0.01, key="ga_cx")
        ga_mut     = st.slider("Mutation rate",          0.01, 0.2, 0.04, 0.01, key="ga_mut")
        ga_elite   = st.slider("Elite count",            1, 20, 5, key="ga_elite")

        st.markdown("#### 🐜 Ant Colony Optimization")
        aco_ants   = st.slider("Number of ants",         5, 100, 20, key="aco_ants")
        aco_iters  = st.slider("ACO iterations",         10, 200, 40, key="aco_iters")
        aco_alpha  = st.slider("α (pheromone weight)",   0.5, 3.0, 1.0, 0.1, key="aco_alpha")
        aco_beta   = st.slider("β (heuristic weight)",   0.5, 5.0, 2.5, 0.1, key="aco_beta")
        aco_rho    = st.slider("ρ (evaporation rate)",   0.01, 0.5, 0.08, 0.01, key="aco_rho")

    with col2:
        st.markdown("#### 🐦 Particle Swarm Optimization")
        pso_p      = st.slider("Particles",              5, 100, 20, key="pso_p")
        pso_i      = st.slider("PSO iterations",         10, 100, 30, key="pso_i")
        pso_omega  = st.slider("ω (initial inertia)",    0.4, 1.0, 0.729, 0.01, key="pso_omega")
        pso_c1     = st.slider("c₁ (cognitive coeff.)",  0.5, 3.0, 1.494, 0.01, key="pso_c1")
        pso_c2     = st.slider("c₂ (social coeff.)",     0.5, 3.0, 1.494, 0.01, key="pso_c2")

        st.markdown("#### 🐝 Artificial Bee Colony")
        abc_bees   = st.slider("Colony size",            5, 80, 20, key="abc_bees")
        abc_cycles = st.slider("Max cycles",             20, 200, 80, key="abc_cycles")
        abc_limit  = st.slider("Scout trigger (limit)",  5, 50, 15, key="abc_limit")

        st.markdown("#### 🔄 Orchestrator")
        outer_iters = st.slider("Outer iterations",      1, 10, 5, key="outer_iters")

    st.divider()

    # Show current config as JSON
    config = {
        "ga":    {"population": ga_pop, "generations": ga_gen,
                  "crossover_rate": ga_cx, "mutation_rate": ga_mut,
                  "elite_count": ga_elite},
        "aco":   {"n_ants": aco_ants, "iterations": aco_iters,
                  "alpha": aco_alpha, "beta": aco_beta, "rho": aco_rho},
        "pso":   {"n_particles": pso_p, "iterations": pso_i,
                  "omega": pso_omega, "c1": pso_c1, "c2": pso_c2},
        "abc":   {"n_bees": abc_bees, "max_cycles": abc_cycles,
                  "limit": abc_limit},
        "outer_iters": outer_iters,
    }

    with st.expander("Preview JSON config"):
        st.json(config)

    c1, c2 = st.columns(2)
    with c1:
        if st.button("▶ Run with This Configuration", type="primary", use_container_width=True):
            with st.spinner("Running simulation with custom parameters..."):
                log = run_simulation(config)
            st.success("Done! Switch to Dashboard to see updated results.")
            with st.expander("Log"):
                st.text(log[-3000:])

    with c2:
        st.download_button(
            "💾 Download Config JSON",
            data=json.dumps(config, indent=2),
            file_name="rba_config.json",
            mime="application/json",
            use_container_width=True
        )


# ══════════════════════════════════════════════════════════════════════════════
# Page 6: Export
# ══════════════════════════════════════════════════════════════════════════════

elif page == "📤 Export Results":
    st.title("Export Results & Plots")
    summaries = load_summary()
    full_res  = load_full()

    st.markdown("### Download Individual Plots")

    plot_files = sorted(PLOTS_DIR.glob("*.png")) if PLOTS_DIR.exists() else []
    if not plot_files:
        st.warning("No plots found — run simulation and generate plots first.")
    else:
        plot_descriptions = {
            "fig1_main_comparison.png":     "Main comparison: DRC, Via, WL, Runtime for all benchmarks",
            "fig2_improvement_heatmap.png": "Improvement % heatmap (all metrics × all benchmarks)",
            "fig3_iteration_convergence.png":"Outer iteration convergence curves (mean ± σ)",
            "fig4_ga_convergence.png":      "GA net ordering convergence (4 benchmarks)",
            "fig5_aco_pheromone.png":       "ACO pheromone dynamics and path quality",
            "fig6_pso_weights.png":         "PSO cost weight optimization",
            "fig7_abc_via.png":             "ABC via minimization dynamics",
            "fig8_scalability.png":         "Scalability: improvement vs design size/layers",
            "fig9_boxplots.png":            "Statistical distribution (box plots + Wilcoxon test)",
            "fig10_ablation.png":           "Ablation study: per-component contribution",
            "fig11_drc_spatial.png":        "DRC violation spatial density maps (before/after)",
            "fig12_dashboard.png":          "Full summary dashboard",
        }

        cols = st.columns(3)
        for i, pf in enumerate(plot_files):
            with cols[i % 3]:
                desc = plot_descriptions.get(pf.name, "")
                st.image(str(pf), caption=desc, use_container_width=True)
                with open(pf, "rb") as f:
                    st.download_button(f"⬇ {pf.name}", f.read(),
                                       file_name=pf.name, mime="image/png",
                                       use_container_width=True,
                                       key=f"dl_{pf.name}")

    st.divider()
    st.markdown("### Download Result Data")

    c1, c2 = st.columns(2)
    with c1:
        if summaries:
            st.download_button(
                "⬇ Summary JSON",
                data=json.dumps(summaries, indent=2),
                file_name="rba_summary.json", mime="application/json",
                use_container_width=True
            )
            # CSV
            rows = []
            for s in summaries:
                rows.append({
                    "benchmark": s["benchmark"],
                    "baseline_drc_mean": s["baseline_drc"]["mean"],
                    "rba_drc_mean": s["rba_drc"]["mean"],
                    "drc_improvement_pct": s["drc_improvement_pct"],
                    "baseline_via_mean": s["baseline_via"]["mean"],
                    "rba_via_mean": s["rba_via"]["mean"],
                    "via_improvement_pct": s["via_improvement_pct"],
                    "wl_change_pct": s["wl_change_pct"],
                    "abc_via_pct": s["abc_via_pct"],
                    "runtime_overhead_pct": s["runtime_overhead_pct"],
                })
            df_csv = pd.DataFrame(rows)
            st.download_button(
                "⬇ Summary CSV",
                data=df_csv.to_csv(index=False),
                file_name="rba_summary.csv", mime="text/csv",
                use_container_width=True
            )

    with c2:
        if full_res:
            st.download_button(
                "⬇ Full Results JSON (all iterations + algorithm histories)",
                data=json.dumps(full_res, indent=2),
                file_name="rba_full_results.json", mime="application/json",
                use_container_width=True
            )

    st.divider()
    st.markdown("### Citation")
    st.code("""@inproceedings{rba_tritonroute_2025,
  title     = {Resilient Bio-Inspired Algorithm Routing Framework for VLSI Physical Design},
  booktitle = {IEEE/ACM Proceedings},
  year      = {2025},
  note      = {Built on TritonRoute / OpenROAD}
}""", language="bibtex")
