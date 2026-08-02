# OpenROAD/TritonRoute Integration

RBA-TritonRoute does not run stock TritonRoute. It requires OpenROAD built
from [`third_party/openroad.patch`](../third_party/openroad.patch) applied
to the commit pinned in [`OPENROAD_COMMIT`](../OPENROAD_COMMIT). The patch
adds three Tcl commands to `drt` (TritonRoute's detailed-routing module)
that give RBA a real way to influence a routing pass, replacing an earlier,
broken mechanism that tried to `source` a JSON sidecar file as Tcl (which
cannot work — Tcl `source` evaluates its argument as a script, not data —
and meant no RBA parameter ever reached TritonRoute).

Every command is guarded on the RBA side with an `info commands` check, so
`rba_router` still runs correctly (falling back to TritonRoute's own
defaults, with a logged warning) against a stock, unpatched OpenROAD build.
The routing loop closes either way; only the bio-inspired algorithms lose
their ability to actually steer the router without the patch.

## Building the patched OpenROAD

Needs a machine with real headroom — OpenROAD's own dependency set (Boost,
LEMON, spdlog, or-tools, etc.) plus the build itself commonly wants well
beyond 8GB RAM and tens of GB of disk, and can take 1-3+ hours even on a
capable multi-core machine. This was not attempted in the sandbox this
patch was authored in (2 CPUs / 8GB RAM / 20GB disk shared with the repo
itself) for exactly that reason.

```bash
git clone https://github.com/The-OpenROAD-Project/OpenROAD.git
cd OpenROAD
git checkout "$(cat /path/to/Rpg007/OPENROAD_COMMIT)"
git apply /path/to/Rpg007/third_party/openroad.patch

# Installs OpenROAD's own dependencies system-wide (needs sudo).
# -all pulls every optional dep; see -h for a narrower set if you already
# have some of these.
./etc/DependencyInstaller.sh -all

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Binary lands at build/src/openroad by default; confirm it runs:
./build/src/openroad -version
```

The patch touches only `src/drt/src/{TritonRoute.i,TritonRoute.tcl,drt-global.h,
serialization.h,dr/FlexDR_maze.cpp,dr/FlexDR_init.cpp}` — no build-system or
SWIG-registration changes are needed beyond what those files already do,
since `TritonRoute.i`/`.tcl` are already wired into OpenROAD's Tcl build.

**Verification status**: this patch has been checked to apply cleanly
(`git apply --check`) to the pinned commit and is grounded in direct
inspection of that commit's real source (not a guess at API names — see
the "Confidence" note on each command below). It has **not** been compiled
anywhere. Likely friction points, in rough order of suspicion: (a)
`getRouterConfiguration()` visibility from the new SWIG `%inline` block —
it's a public method on `TritonRoute` so this should be fine, but SWIG
name resolution across the `drt::` namespace boundary is the first thing
to check if `set_drt_cost_weights` fails to compile or register; (b) the
`serialization.h` hunk — `RBA_NET_PRIORITY`/`RBA_FORCED_RIPUP_NETS` need
`boost/serialization/unordered_map.hpp` (added) and must round-trip
correctly if any other translation unit also serializes
`RouterConfiguration` via a different Archive type than tested; (c) the
`FlexDR_init.cpp` hunk assumes `getWorkerRegionQuery()` and
`clearRouteConnFigs()` have the exact signatures used by the neighboring
`initRipUpNetsFromMarkers()` it was modeled on — a compiler error here
would point at a drt-internal API mismatch worth diffing against the
current pinned commit again.

### Verification checklist — what to send back

Once it builds, this is what turns "the patch looks right" into "the patch
works":

1. **Build evidence**: `./build/src/openroad -version` output, and the
   tail of the build log (or just "clean build, 0 errors" if true).
2. **Tcl commands exist** — from an interactive `openroad` shell:
   ```tcl
   info commands set_drt_cost_weights
   info commands set_drt_net_order
   info commands set_drt_ripup_nets
   ```
   Each should return the command name, not an empty string.
3. **The smoke test, for real**: run
   [`tests/test_end_to_end.sh`](../tests/test_end_to_end.sh) with
   `OPENROAD_BIN=/path/to/build/src/openroad` — it should now PASS (not
   SKIP/exit 77) and produce a real routed DEF + metrics CSV on mini_test.
4. **Cost weights actually bite** (Phase 2.4 of the current plan): route
   mini_test twice with deliberately different `-via_cost` values via
   `set_drt_cost_weights` and confirm the via count in the output DEF
   changes. If it doesn't move, the hook isn't wired correctly even
   though the command exists — this is the test that actually validates
   the FlexDR_maze.cpp/FlexDR_init.cpp hunks, not just the SWIG plumbing.

Send back whichever of these you get to (even just #1 and #2) and I'll
pick up verification/Phase 2 from there.

## Commands

### `set_drt_cost_weights`

```tcl
set_drt_cost_weights [-route_shape_cost cost] [-via_cost cost] \
                      [-marker_cost cost] [-grid_cost cost]
```

Sets four of TritonRoute's real maze-routing cost constants for the
remainder of the process (they are runtime fields on
`drt::RouterConfiguration`, not compile-time constants — confirmed against
`src/drt/src/drt-global.h` at the pinned commit). Defaults match
TritonRoute's own out-of-the-box values (`route_shape_cost=8, via_cost=4,
marker_cost=32, grid_cost=2`), so an unpatched call with no flags is a
no-op relative to stock behavior.

| Flag | `RouterConfiguration` field | Meaning |
|---|---|---|
| `-route_shape_cost` | `ROUTESHAPECOST` | cost of routing over/near existing shapes |
| `-via_cost` | `VIACOST` | cost per via |
| `-marker_cost` | `MARKERCOST` | cost of routing through a DRC marker region |
| `-grid_cost` | `GRIDCOST` | base per-grid-edge traversal cost |

RBA's `CostWeights` struct (`include/rba_types.h`) has six fields
(`w_wire, w_via, w_cong, w_drc_hist, w_layer_pref, w_timing`); the bridge
(`src/triton_bridge.cpp`, `write_tcl_script`) maps four of them onto this
command:

| RBA field | → | cost flag | scale |
|---|---|---|---|
| `w_wire` | → | `-route_shape_cost` | ×8 |
| `w_via` | → | `-via_cost` | ×1 |
| `w_drc_hist` | → | `-marker_cost` | ×6.4 |
| `w_layer_pref` | → | `-grid_cost` | ×2 |

Scales are chosen so RBA's float defaults (`w_wire=1.0, w_via=4.0,
w_drc_hist=5.0, w_layer_pref=1.0`) reproduce TritonRoute's own integer
defaults. **`w_cong` and `w_timing` are not sent** — the pinned commit's
`RouterConfiguration` has no direct per-run congestion or timing cost knob
equivalent to these; wiring them up would require a deeper patch into
`FlexGridGraph`'s cost function and is left for a follow-up.

**Confidence: high.** `drt::TritonRoute::getRouterConfiguration()` is
already a public accessor returning a mutable `RouterConfiguration*`
(`src/drt/include/drt/TritonRoute.h`), so this hook needed no changes
beyond a new SWIG `%inline` function and Tcl proc following the existing
pattern used by e.g. `detailed_route_set_default_via`.

### `set_drt_net_order`

```tcl
set_drt_net_order -file filename
```

`filename` is a plain text file, one net name per line, highest priority
first. Populates `RouterConfiguration::RBA_NET_PRIORITY` (net name → rank,
lower = higher priority).

**This is a per-worker-tile priority hint, not a single global sequential
net order.** TritonRoute's detailed router parallelizes routing across
spatial worker tiles (`FlexDRWorker`); each worker builds its own reroute
queue independently. The patch changes
`FlexDRWorker::mazeIterInit_sortRerouteQueue`
(`src/drt/src/dr/FlexDR_maze.cpp`) — the comparator that sorts each
worker's reroute queue at maze iteration 0 — to sort by `RBA_NET_PRIORITY`
rank first (when the entry is a net with a ranked name) before falling
back to TritonRoute's existing `typeId()`/`getId()` tiebreak. Nets with no
entry in the priority file keep their default position. There is no
mechanism in this patch to force a strict whole-chip net-by-net sequence —
that would require changing how work is partitioned across worker tiles,
which is out of scope here.

**Confidence: medium.** Grounded in direct reading of the real comparator
and the `RouteQueueEntry`/`route_queue_init_queue` machinery at the pinned
commit, but unlike the cost-weight hook this has not been cross-checked
against a second code path, and the interaction between per-worker
priority and TritonRoute's own iterative rip-up/reroute passes (iterations
beyond 0) has not been traced. Needs compile + smoke-test verification.

### `set_drt_ripup_nets`

```tcl
set_drt_ripup_nets -file filename
```

`filename` is a plain text file, one net name per line. Populates
`RouterConfiguration::RBA_FORCED_RIPUP_NETS`. On the next `detailed_route`
call, each worker unconditionally clears and re-routes any of its local
nets whose name appears in this set, regardless of current DRC state —
mirroring the exact pattern TritonRoute already uses for marker-driven
rip-up (`FlexDRWorker::initRipUpNetsFromMarkers`): remove the net's route
geometry from the worker's region query, clear its route connectivity
figures, mark it for rip-up, and push it onto the worker's reroute queue.
Implemented in `FlexDRWorker::route_queue_init_queue`'s `RipUpMode::DRC`
branch (`src/drt/src/dr/FlexDR_init.cpp`), which is the mode `detailed_route`
uses by default.

**Confidence: medium.** The append pattern (clear region-query entries →
`clearRouteConnFigs()` → `setRipup()` → push a `RouteQueueEntry`) is copied
directly from the existing `RipUpMode::NEARDRC` branch a few lines below in
the same function, which does the same thing for marker-flagged nets — so
the shape of the change is well precedented, but it has not been compiled
or run.

## RBA-side usage

`TritonBridge::inject_net_order` / `inject_cost_weights` / `inject_ripup_nets`
(`src/triton_bridge.cpp`) stage values that `write_tcl_script` emits as real
Tcl on the next `run_tritonroute` call, each wrapped in:

```tcl
if {[llength [info commands set_drt_cost_weights]] > 0} {
  set_drt_cost_weights ...
} else {
  puts "[RBA] WARNING: set_drt_cost_weights unavailable (unpatched OpenROAD build) — using TritonRoute defaults"
}
```

so `--baseline-only` runs (and any run against a stock OpenROAD binary)
degrade gracefully instead of failing on an undefined Tcl command.

## Known gaps / next steps

- Not compiled or run in this environment — see "Verification status" above.
- `w_cong`/`w_timing` have no real cost-model hook yet.
- Net order is per-worker-tile, not a strict global order (see above) —
  if a stricter guarantee is needed, it would require patching how
  TritonRoute partitions the design into worker tiles, which this patch
  does not attempt.
- Tcl error-message IDs `9001`–`9004` (`utl::DRT` category) were chosen to
  avoid the ID ranges already in use in `TritonRoute.tcl`/`TritonRoute.i`
  at the pinned commit, but were not checked against OpenROAD's full
  message-ID registry (`utl::MessageManager`) — verify no collision before
  upstreaming.
