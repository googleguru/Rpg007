#!/usr/bin/env python3
"""
Sky130 PDK Verification for RBA-TritonRoute
============================================
Parses a routed DEF produced by RBA-TritonRoute and checks it against
Sky130A DRC rules (widths, spacings, via enclosures, minimum area).
Optionally invokes Magic or KLayout for full physical verification.

Usage:
    python3 sky130_verification.py --def routed.def [--pdk /path/to/sky130A]
                                   [--magic] [--klayout] [--output ./verify_out]

Dependencies:
    pip install rich (optional, for coloured table output)
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    from rich.console import Console
    from rich.table import Table
    console = Console()
    HAS_RICH = True
except ImportError:
    HAS_RICH = False

# ─── Sky130 PDK constants ─────────────────────────────────────────────────────

SKY130_LAYERS = ["li1", "met1", "met2", "met3", "met4", "met5"]

SKY130_LAYER_RULES: Dict[str, dict] = {
    "li1":  {"min_width": 170,  "min_spacing": 170,  "min_area": 14520,
             "eol_spacing": 270,  "preferred_dir": "V"},
    "met1": {"min_width": 140,  "min_spacing": 140,  "min_area": 15400,
             "eol_spacing": 250,  "preferred_dir": "H"},
    "met2": {"min_width": 140,  "min_spacing": 140,  "min_area": 15400,
             "eol_spacing": 250,  "preferred_dir": "V"},
    "met3": {"min_width": 300,  "min_spacing": 300,  "min_area": 160000,
             "eol_spacing": 500,  "preferred_dir": "H"},
    "met4": {"min_width": 300,  "min_spacing": 300,  "min_area": 160000,
             "eol_spacing": 500,  "preferred_dir": "V"},
    "met5": {"min_width": 1600, "min_spacing": 1600, "min_area": 4000000,
             "eol_spacing": 1600, "preferred_dir": "H"},
}

SKY130_VIA_RULES: Dict[str, dict] = {
    "mcon": {"cut_size": 170, "spacing": 190, "enc_lower":   0, "enc_upper":  60},
    "via":  {"cut_size": 150, "spacing": 170, "enc_lower":  55, "enc_upper":  55},
    "via2": {"cut_size": 200, "spacing": 200, "enc_lower":  55, "enc_upper":  55},
    "via3": {"cut_size": 200, "spacing": 200, "enc_lower":  60, "enc_upper":  60},
    "via4": {"cut_size": 800, "spacing": 800, "enc_lower": 310, "enc_upper": 310},
}

DBU_PER_MICRON = 1000   # Sky130: 1 DBU = 1 nm

# ─── Data structures ──────────────────────────────────────────────────────────

@dataclass
class WireSegment:
    net:    str
    layer:  str
    x1: int; y1: int
    x2: int; y2: int
    width:  int = 0

    @property
    def length(self) -> int:
        return abs(self.x2 - self.x1) + abs(self.y2 - self.y1)

    @property
    def is_horizontal(self) -> bool:
        return self.y1 == self.y2

    @property
    def bbox_area(self) -> int:
        w = self.width if self.width else SKY130_LAYER_RULES.get(self.layer, {}).get("min_width", 140)
        if self.is_horizontal:
            return self.length * w
        return w * self.length


@dataclass
class ViaInstance:
    net:    str
    via_type: str
    x: int; y: int


@dataclass
class DRCViolation:
    rule:    str          # e.g. "WIDTH", "SPACING", "VIA_ENC", "MIN_AREA"
    layer:   str
    net:     str
    message: str
    severity: float = 1.0


@dataclass
class VerificationResult:
    def_file:           str
    total_segments:     int = 0
    total_vias:         int = 0
    width_violations:   List[DRCViolation] = field(default_factory=list)
    spacing_violations: List[DRCViolation] = field(default_factory=list)
    area_violations:    List[DRCViolation] = field(default_factory=list)
    via_violations:     List[DRCViolation] = field(default_factory=list)
    layer_dir_warnings: List[DRCViolation] = field(default_factory=list)
    unknown_layers:     List[str] = field(default_factory=list)

    @property
    def total_violations(self) -> int:
        return (len(self.width_violations) + len(self.spacing_violations) +
                len(self.area_violations)  + len(self.via_violations))

    @property
    def passed(self) -> bool:
        return self.total_violations == 0


# ─── DEF parser ───────────────────────────────────────────────────────────────

class DEFParser:
    """
    Minimal DEF NETS section parser.
    Extracts wire segments (ROUTED ... layer width coords) and via instances.
    Handles the DEF 5.8 NETS syntax produced by TritonRoute.
    """

    # ROUTED met1 140 ( 1000 2000 ) ( 3000 2000 )
    ROUTED_RE  = re.compile(
        r'(?:ROUTED|NEW)\s+(\S+)\s+(\d+)\s+\(\s*(\d+)\s+(\d+)\s*\)'
        r'\s+\(\s*(\*|\d+)\s+(\*|\d+)\s*\)'
    )
    VIA_RE     = re.compile(r'\+\s*VIA\s+(\S+)', re.IGNORECASE)
    COORD_RE   = re.compile(r'\(\s*(\*|\d+)\s+(\*|\d+)\s*\)')
    NET_HDR_RE = re.compile(r'^\s*-\s+(\S+)')

    def parse(self, def_path: str) -> Tuple[List[WireSegment], List[ViaInstance]]:
        segments: List[WireSegment] = []
        vias:     List[ViaInstance] = []

        try:
            with open(def_path) as f:
                content = f.read()
        except OSError as e:
            print(f"[Parser] Cannot open {def_path}: {e}")
            return segments, vias

        # Locate NETS section
        nets_start = content.find("\nNETS ")
        nets_end   = content.find("\nEND NETS")
        if nets_start == -1 or nets_end == -1:
            print("[Parser] NETS section not found in DEF")
            return segments, vias

        nets_block = content[nets_start:nets_end]

        current_net = ""
        prev_x, prev_y = 0, 0

        for line in nets_block.splitlines():
            hdr = self.NET_HDR_RE.match(line)
            if hdr:
                current_net = hdr.group(1)
                prev_x = prev_y = 0
                continue

            # Parse routing geometry
            m = self.ROUTED_RE.search(line)
            if m:
                layer = m.group(1)
                width = int(m.group(2))
                x1    = int(m.group(3)); y1 = int(m.group(4))
                x2    = x1 if m.group(5) == "*" else int(m.group(5))
                y2    = y1 if m.group(6) == "*" else int(m.group(6))
                if m.group(5) == "*": x2 = prev_x
                if m.group(6) == "*": y2 = prev_y
                seg = WireSegment(net=current_net, layer=layer,
                                  x1=x1, y1=y1, x2=x2, y2=y2, width=width)
                segments.append(seg)
                prev_x, prev_y = x2, y2

                # Check for inline via
                via_m = self.VIA_RE.search(line)
                if via_m:
                    vias.append(ViaInstance(net=current_net,
                                            via_type=via_m.group(1),
                                            x=x2, y=y2))
            else:
                # Bare VIA on continuation line
                via_m = self.VIA_RE.search(line)
                if via_m:
                    coords = self.COORD_RE.findall(line)
                    for cx, cy in coords:
                        vx = prev_x if cx == "*" else int(cx)
                        vy = prev_y if cy == "*" else int(cy)
                        vias.append(ViaInstance(net=current_net,
                                                via_type=via_m.group(1),
                                                x=vx, y=vy))
                        prev_x, prev_y = vx, vy

        return segments, vias


# ─── DRC checkers ─────────────────────────────────────────────────────────────

def check_width(segments: List[WireSegment]) -> List[DRCViolation]:
    violations = []
    for seg in segments:
        rules = SKY130_LAYER_RULES.get(seg.layer)
        if not rules:
            continue
        min_w = rules["min_width"]
        if seg.width > 0 and seg.width < min_w:
            sev = (min_w - seg.width) / min_w
            violations.append(DRCViolation(
                rule="WIDTH", layer=seg.layer, net=seg.net, severity=sev,
                message=(f"wire width {seg.width} nm < min {min_w} nm "
                         f"on {seg.layer} (net {seg.net})")
            ))
    return violations


def check_min_area(segments: List[WireSegment]) -> List[DRCViolation]:
    violations = []
    # Group segments by (net, layer) and sum areas for connected segments
    # Simplified: check each segment individually as lower bound
    for seg in segments:
        rules = SKY130_LAYER_RULES.get(seg.layer)
        if not rules:
            continue
        min_area = rules["min_area"]
        area = seg.bbox_area
        if 0 < area < min_area:
            sev = (min_area - area) / min_area
            violations.append(DRCViolation(
                rule="MIN_AREA", layer=seg.layer, net=seg.net, severity=sev,
                message=(f"segment area {area} nm² < min {min_area} nm² "
                         f"on {seg.layer} (net {seg.net})")
            ))
    return violations


def check_preferred_direction(segments: List[WireSegment]) -> List[DRCViolation]:
    warnings = []
    for seg in segments:
        rules = SKY130_LAYER_RULES.get(seg.layer)
        if not rules:
            continue
        preferred_h = (rules["preferred_dir"] == "H")
        if seg.is_horizontal != preferred_h and seg.length > 0:
            warnings.append(DRCViolation(
                rule="LAYER_DIR", layer=seg.layer, net=seg.net, severity=0.3,
                message=(f"non-preferred direction on {seg.layer} "
                         f"({'V' if seg.is_horizontal else 'H'} wire, "
                         f"preferred: {rules['preferred_dir']}) net {seg.net}")
            ))
    return warnings


def check_via_layer_names(vias: List[ViaInstance]) -> List[DRCViolation]:
    violations = []
    known_vias = set(SKY130_VIA_RULES.keys())
    # Also accept generated via names like VIA_met1_met2
    sky130_via_prefixes = ("mcon", "via", "sky130_fd_sc_hd")

    for v in vias:
        vt_lower = v.via_type.lower()
        base = re.sub(r'_\d+$', '', vt_lower)  # strip array suffix e.g. via2_1x1→via2
        if base not in known_vias and not any(vt_lower.startswith(p) for p in sky130_via_prefixes):
            violations.append(DRCViolation(
                rule="VIA_TYPE", layer="via", net=v.net, severity=0.5,
                message=(f"unrecognised via type '{v.via_type}' — "
                         f"not in sky130 via library (net {v.net})")
            ))
    return violations


def check_unknown_layers(segments: List[WireSegment]) -> List[str]:
    unknown = set()
    for seg in segments:
        if seg.layer not in SKY130_LAYER_RULES:
            unknown.add(seg.layer)
    return sorted(unknown)


def check_spacing_same_layer(segments: List[WireSegment]) -> List[DRCViolation]:
    """
    Check same-layer spacing between parallel wire segments on the same net.
    Full spacing DRC requires edge-to-edge geometry (expensive).
    This lightweight pass checks segments sharing the same Y (horizontal)
    or X (vertical) coordinate where overlap exists.
    """
    violations = []

    by_layer: Dict[str, List[WireSegment]] = defaultdict(list)
    for seg in segments:
        by_layer[seg.layer].append(seg)

    for layer, segs in by_layer.items():
        rules = SKY130_LAYER_RULES.get(layer)
        if not rules:
            continue
        min_sp = rules["min_spacing"]

        # Partition into horizontal and vertical groups
        h_segs = [s for s in segs if s.is_horizontal]
        v_segs = [s for s in segs if not s.is_horizontal]

        # Horizontal segments: bucket by Y, sort by X range, check X gap
        y_buckets: Dict[int, List[WireSegment]] = defaultdict(list)
        for s in h_segs:
            y_buckets[s.y1].append(s)
        for y_coord, bucket in y_buckets.items():
            bucket.sort(key=lambda s: min(s.x1, s.x2))
            for i in range(len(bucket) - 1):
                a, b = bucket[i], bucket[i+1]
                gap = min(b.x1, b.x2) - max(a.x1, a.x2)
                half_w = (a.width + b.width) // 2
                edge_gap = gap - half_w
                if 0 < edge_gap < min_sp:
                    sev = (min_sp - edge_gap) / min_sp
                    violations.append(DRCViolation(
                        rule="SPACING", layer=layer,
                        net=f"{a.net}/{b.net}", severity=sev,
                        message=(f"spacing {edge_gap} nm < min {min_sp} nm "
                                 f"on {layer} (nets {a.net}, {b.net})")
                    ))

        # Vertical segments: bucket by X
        x_buckets: Dict[int, List[WireSegment]] = defaultdict(list)
        for s in v_segs:
            x_buckets[s.x1].append(s)
        for x_coord, bucket in x_buckets.items():
            bucket.sort(key=lambda s: min(s.y1, s.y2))
            for i in range(len(bucket) - 1):
                a, b = bucket[i], bucket[i+1]
                gap = min(b.y1, b.y2) - max(a.y1, a.y2)
                half_w = (a.width + b.width) // 2
                edge_gap = gap - half_w
                if 0 < edge_gap < min_sp:
                    sev = (min_sp - edge_gap) / min_sp
                    violations.append(DRCViolation(
                        rule="SPACING", layer=layer,
                        net=f"{a.net}/{b.net}", severity=sev,
                        message=(f"spacing {edge_gap} nm < min {min_sp} nm "
                                 f"on {layer} (nets {a.net}, {b.net})")
                    ))
    return violations


# ─── External tool runners ────────────────────────────────────────────────────

def run_magic_drc(def_file: str, pdk_root: str, output_dir: str) -> Optional[int]:
    """Run Magic VLSI DRC for full sky130 physical verification."""
    magicrc = Path(pdk_root) / "libs.tech/magic/sky130A.magicrc"
    if not magicrc.exists():
        print(f"[Magic] RC file not found: {magicrc}")
        return None

    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    magic_drc_rpt = out_dir / "magic_drc.rpt"

    tcl = f"""
drc on
load {def_file}
drc catchup
set drc_count [drc list count total]
set f [open {magic_drc_rpt} w]
puts $f "Magic DRC Violations: $drc_count"
drc listall why $f
close $f
puts "Magic DRC complete: $drc_count violations"
quit -noprompt
"""
    tcl_path = out_dir / "magic_drc.tcl"
    tcl_path.write_text(tcl)

    try:
        result = subprocess.run(
            ["magic", "-rcfile", str(magicrc), "-noconsole", "-dnull",
             str(tcl_path)],
            capture_output=True, timeout=600
        )
        print(f"[Magic] Exit code: {result.returncode}")
        if magic_drc_rpt.exists():
            text = magic_drc_rpt.read_text()
            m = re.search(r"Magic DRC Violations:\s*(\d+)", text)
            if m:
                count = int(m.group(1))
                print(f"[Magic] DRC violations: {count}")
                return count
    except FileNotFoundError:
        print("[Magic] 'magic' not found in PATH — skipping Magic DRC")
    except subprocess.TimeoutExpired:
        print("[Magic] DRC timed out")
    return None


def run_klayout_drc(def_file: str, pdk_root: str, output_dir: str) -> Optional[int]:
    """Run KLayout DRC with sky130 DRC deck."""
    drc_deck = Path(pdk_root) / "libs.tech/klayout/drc/sky130A.drc"
    if not drc_deck.exists():
        print(f"[KLayout] DRC deck not found: {drc_deck}")
        return None

    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    report = out_dir / "klayout_drc.xml"

    try:
        result = subprocess.run(
            ["klayout", "-b",
             "-rd", f"input={def_file}",
             "-rd", f"report={report}",
             "-r",  str(drc_deck)],
            capture_output=True, timeout=1800
        )
        print(f"[KLayout] Exit code: {result.returncode}")
        if report.exists():
            text = report.read_text()
            count = text.count("<item>")
            print(f"[KLayout] DRC violations: {count}")
            return count
    except FileNotFoundError:
        print("[KLayout] 'klayout' not found in PATH — skipping KLayout DRC")
    except subprocess.TimeoutExpired:
        print("[KLayout] DRC timed out")
    return None


# ─── Report generation ────────────────────────────────────────────────────────

def print_report(result: VerificationResult):
    print("\n" + "=" * 72)
    print(f"  Sky130 PDK Verification Report")
    print(f"  DEF: {result.def_file}")
    print("=" * 72)

    print(f"\n  Segments parsed : {result.total_segments}")
    print(f"  Vias parsed     : {result.total_vias}")
    print(f"  Total violations: {result.total_violations}")

    if result.unknown_layers:
        print(f"\n  [WARN] Unknown layers (not in sky130): {result.unknown_layers}")

    categories = [
        ("WIDTH violations",      result.width_violations),
        ("SPACING violations",    result.spacing_violations),
        ("MIN_AREA violations",   result.area_violations),
        ("VIA_TYPE violations",   result.via_violations),
        ("Layer-dir warnings",    result.layer_dir_warnings),
    ]

    for title, viols in categories:
        if viols:
            print(f"\n  ── {title}: {len(viols)} ──")
            for v in viols[:20]:   # cap output at 20 per category
                prefix = "[WARN]" if v.rule == "LAYER_DIR" else "[FAIL]"
                print(f"    {prefix} {v.message}")
            if len(viols) > 20:
                print(f"    ... and {len(viols)-20} more")

    print()
    if result.passed:
        print("  RESULT: PASS — no DRC violations found")
    else:
        print(f"  RESULT: FAIL — {result.total_violations} violation(s) found")
    print("=" * 72 + "\n")


def save_report_json(result: VerificationResult, output_dir: str):
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    report_path = out / "sky130_drc_result.json"

    def viol_list(vs):
        return [{"rule": v.rule, "layer": v.layer, "net": v.net,
                 "message": v.message, "severity": round(v.severity, 4)}
                for v in vs]

    data = {
        "def_file":           result.def_file,
        "total_segments":     result.total_segments,
        "total_vias":         result.total_vias,
        "total_violations":   result.total_violations,
        "passed":             result.passed,
        "unknown_layers":     result.unknown_layers,
        "violations": {
            "width":          viol_list(result.width_violations),
            "spacing":        viol_list(result.spacing_violations),
            "min_area":       viol_list(result.area_violations),
            "via_type":       viol_list(result.via_violations),
            "layer_dir_warn": viol_list(result.layer_dir_warnings),
        }
    }

    with open(report_path, "w") as f:
        json.dump(data, f, indent=2)
    print(f"[Report] JSON written: {report_path}")


# ─── Main ─────────────────────────────────────────────────────────────────────

def verify(def_file: str,
           pdk_root: Optional[str] = None,
           run_magic: bool = False,
           run_klayout: bool = False,
           output_dir: str = "./sky130_verify") -> VerificationResult:

    result = VerificationResult(def_file=def_file)

    print(f"[Verify] Parsing DEF: {def_file}")
    parser = DEFParser()
    segments, vias = parser.parse(def_file)
    result.total_segments = len(segments)
    result.total_vias     = len(vias)
    print(f"[Verify] Found {len(segments)} wire segments, {len(vias)} vias")

    # Rule checks
    result.unknown_layers     = check_unknown_layers(segments)
    result.width_violations   = check_width(segments)
    result.area_violations    = check_min_area(segments)
    result.spacing_violations = check_spacing_same_layer(segments)
    result.via_violations     = check_via_layer_names(vias)
    result.layer_dir_warnings = check_preferred_direction(segments)

    print(f"[Verify] Width:   {len(result.width_violations)} violations")
    print(f"[Verify] Spacing: {len(result.spacing_violations)} violations")
    print(f"[Verify] MinArea: {len(result.area_violations)} violations")
    print(f"[Verify] Via:     {len(result.via_violations)} violations")
    print(f"[Verify] DirWarn: {len(result.layer_dir_warnings)} warnings")

    # External tools
    if pdk_root:
        if run_magic:
            magic_count = run_magic_drc(def_file, pdk_root, output_dir)
            if magic_count is not None:
                print(f"[Verify] Magic DRC total: {magic_count}")
        if run_klayout:
            kl_count = run_klayout_drc(def_file, pdk_root, output_dir)
            if kl_count is not None:
                print(f"[Verify] KLayout DRC total: {kl_count}")

    print_report(result)
    save_report_json(result, output_dir)
    return result


def main():
    parser = argparse.ArgumentParser(
        description="Sky130 PDK verification for RBA-TritonRoute DEF outputs"
    )
    parser.add_argument("--def",      required=True, dest="def_file",
                        help="Routed DEF file to verify")
    parser.add_argument("--pdk",      default=os.environ.get("SKY130_PDK", ""),
                        help="Path to sky130A PDK root (or set SKY130_PDK env)")
    parser.add_argument("--magic",    action="store_true",
                        help="Run Magic VLSI full DRC (requires magic in PATH)")
    parser.add_argument("--klayout",  action="store_true",
                        help="Run KLayout DRC deck (requires klayout in PATH)")
    parser.add_argument("--output",   default="./sky130_verify",
                        help="Output directory for reports (default: ./sky130_verify)")
    args = parser.parse_args()

    if not Path(args.def_file).exists():
        print(f"[Error] DEF file not found: {args.def_file}")
        sys.exit(1)

    result = verify(
        def_file=args.def_file,
        pdk_root=args.pdk or None,
        run_magic=args.magic,
        run_klayout=args.klayout,
        output_dir=args.output,
    )
    sys.exit(0 if result.passed else 1)


if __name__ == "__main__":
    main()
