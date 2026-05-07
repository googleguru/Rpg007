#!/usr/bin/env bash
# Download and prepare ISPD 2018/2019 detailed routing benchmarks.
#
# ISPD 2018: https://www.ispd.cc/contests/18/ispd18_contest.html
# ISPD 2019: https://www.ispd.cc/contests/19/
#
# The benchmark tarballs are hosted by the ISPD contest committee.
# Run this script from the rba_router/benchmarks directory.

set -euo pipefail

BENCH_DIR="${1:-$(pwd)/benchmarks}"
mkdir -p "$BENCH_DIR"
cd "$BENCH_DIR"

echo "=== ISPD 2018 Benchmark Setup ==="
echo "Benchmark tarballs must be downloaded manually from:"
echo "  https://www.ispd.cc/contests/18/ispd18_contest.html"
echo ""
echo "Expected directory structure after extraction:"
echo "  benchmarks/"
echo "  ├── ispd18_test1/"
echo "  │   ├── ispd18_test1.input.lef"
echo "  │   ├── ispd18_test1.input.def"
echo "  │   └── ispd18_test1.input.guide"
echo "  ├── ispd18_test2/"
echo "  │   ..."
echo "  └── ispd19_test1/"
echo "      ..."
echo ""
echo "After placing tarballs in $BENCH_DIR, run:"
echo "  for f in *.tar.gz; do tar xzf \$f; done"
echo ""

# Create a synthetic mini-benchmark for testing without real benchmark files
echo "=== Creating synthetic test benchmark ==="
MINI_DIR="$BENCH_DIR/mini_test"
mkdir -p "$MINI_DIR"

# Minimal LEF
cat > "$MINI_DIR/mini_test.lef" << 'LEOF'
VERSION 5.8 ;
BUSBITCHARS "[]" ;
DIVIDERCHAR "/" ;

UNITS
  DATABASE MICRONS 2000 ;
END UNITS

MANUFACTURINGGRID 0.005 ;

LAYER li1
  TYPE ROUTING ;
  DIRECTION HORIZONTAL ;
  WIDTH 0.17 ;
  PITCH 0.34 0.34 ;
  OFFSET 0.17 0.17 ;
  MINWIDTH 0.17 ;
  AREA 0.0561 ;
END li1

LAYER met1
  TYPE ROUTING ;
  DIRECTION HORIZONTAL ;
  WIDTH 0.14 ;
  PITCH 0.28 0.28 ;
  OFFSET 0.14 0.14 ;
  MINWIDTH 0.14 ;
END met1

LAYER met2
  TYPE ROUTING ;
  DIRECTION VERTICAL ;
  WIDTH 0.14 ;
  PITCH 0.28 0.28 ;
  OFFSET 0.14 0.14 ;
  MINWIDTH 0.14 ;
END met2

LAYER mcon
  TYPE CUT ;
  WIDTH 0.17 ;
  SPACING 0.19 ;
  ENCLOSURE BELOW 0.03 0.06 ;
  ENCLOSURE ABOVE 0.03 0.06 ;
END mcon

LAYER via
  TYPE CUT ;
  WIDTH 0.15 ;
  SPACING 0.17 ;
  ENCLOSURE BELOW 0.055 0.085 ;
  ENCLOSURE ABOVE 0.055 0.085 ;
END via

END LIBRARY
LEOF

# Minimal DEF with a few nets
cat > "$MINI_DIR/mini_test.def" << 'DEOF'
VERSION 5.8 ;
DIVIDERCHAR "/" ;
BUSBITCHARS "[]" ;
DESIGN mini_test ;
UNITS DISTANCE MICRONS 2000 ;
DIEAREA ( 0 0 ) ( 20000 20000 ) ;

COMPONENTS 0 ;
END COMPONENTS

PINS 4 ;
  - P_A + NET net_a + DIRECTION INPUT
    + LAYER met1 ( -100 -100 ) ( 100 100 )
    + FIXED ( 2000 2000 ) N ;
  - P_B + NET net_b + DIRECTION INPUT
    + LAYER met1 ( -100 -100 ) ( 100 100 )
    + FIXED ( 2000 10000 ) N ;
  - P_Y + NET net_a + DIRECTION OUTPUT
    + LAYER met2 ( -100 -100 ) ( 100 100 )
    + FIXED ( 18000 2000 ) N ;
  - P_Z + NET net_b + DIRECTION OUTPUT
    + LAYER met2 ( -100 -100 ) ( 100 100 )
    + FIXED ( 18000 10000 ) N ;
END PINS

NETS 2 ;
  - net_a ( PIN P_A ) ( PIN P_Y ) ;
  - net_b ( PIN P_B ) ( PIN P_Z ) ;
END NETS

END DESIGN
DEOF

# Minimal guide file (rectangular guides for each net)
cat > "$MINI_DIR/mini_test.guide" << 'GEOF'
net_a
(
( 0 0 20000 20000 met1 )
( 0 0 20000 20000 met2 )
)
net_b
(
( 0 0 20000 20000 met1 )
( 0 0 20000 20000 met2 )
)
GEOF

echo "Created synthetic mini-benchmark at: $MINI_DIR"
echo ""
echo "Test with:"
echo "  ./rba_router --lef $MINI_DIR/mini_test.lef \\"
echo "               --def $MINI_DIR/mini_test.def \\"
echo "               --guide $MINI_DIR/mini_test.guide \\"
echo "               --output /tmp/rba_mini_out"
