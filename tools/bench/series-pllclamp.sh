#!/bin/bash
# A/B series: does clamping the PLL period to the display interval stop the drift?
#
# NOT THE CURRENT SERIES. Kept because the design notes below still apply, but a
# simulation of the loop suggests the clamp improves the period while making the
# PHASE worse, i.e. it is a palliative. The series being run is
# series-pllwrap.sh, which compares the upstream asymmetric phase-error fold
# against a symmetric one - the candidate root cause.
#
# Design, and why it is this and not something shorter or simpler:
#   * 90 s of measurement per run. The drift accumulates with TIME - measured on
#     the live profile: lock at 16682.9 us, 16588.9 after 2.5 min, 16565.2,
#     16558.6. A 20 s run sees the tail of that movement or nothing at all, and
#     both arms then report the same number: the classic way to "prove" there is
#     no difference where there is one.
#   * The arms are INTERLEAVED (clamp1, clamp0, clamp1, ...). Running one arm to
#     completion and then the other would attribute any drift of the machine
#     itself - thermals, other load - to the change under test.
#   * One binary for the whole series. Its sha256 is checked before every run;
#     if it ever changes, the series is void as a whole, not partially.
#
# Both arms come from the same build: MIXXX_BENCH_PLL_CLAMP=0 disables the clamp
# at run time and each run logs BENCHHIT with the arm it really used.
#
# Usage: tools/bench/series-pllclamp.sh [-g WxH+X+Y] [-r reps] [-d measure_s] [-s settle_s]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BENCH_BIN:-$ROOT/build/mixxx}"
RESULTS="${BENCH_RESULTS:-$(dirname "$ROOT")/bench-results}"

GEOMETRY="${BENCH_GEOMETRY:-}"
REPS=3
MEASURE=90
SETTLE=20

while [ $# -gt 0 ]; do
    case "$1" in
        -g) GEOMETRY=$2; shift 2 ;;
        -r) REPS=$2; shift 2 ;;
        -d) MEASURE=$2; shift 2 ;;
        -s) SETTLE=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$GEOMETRY" ] || { echo "ERROR: -g WxH+X+Y is required (see tools/bench/screens.sh)" >&2; exit 2; }

SERIES="$RESULTS/series-pllclamp-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$SERIES" || exit 1

SHA_AT_START=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
{
    echo "series=pllclamp"
    echo "date=$(date -Iseconds)"
    echo "bin_sha256=$SHA_AT_START"
    echo "reps=$REPS measure_s=$MEASURE settle_s=$SETTLE geometry=$GEOMETRY"
} >"$SERIES/series.txt"

total=$((REPS * 2))
n=0
per_run_s=$((MEASURE + SETTLE + 15))
echo "series: $total runs, about $((total * per_run_s / 60)) minutes -> $SERIES"

for rep in $(seq 1 "$REPS"); do
    for arm in 1 0; do          # interleaved, never one arm after the other
        n=$((n + 1))
        sha=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
        if [ "$sha" != "$SHA_AT_START" ]; then
            echo "ABORT: the binary changed during the series ($sha != $SHA_AT_START)." >&2
            echo "The whole series is void, not just this run." >&2
            echo "binary changed at run $n" >>"$SERIES/series.txt"
            exit 1
        fi
        label="clamp${arm}_r${rep}"
        echo
        echo "[$n/$total] $label"
        "$ROOT/tools/bench/run.sh" \
            -d "$MEASURE" -s "$SETTLE" -g "$GEOMETRY" \
            -l "$label" -o "$SERIES/$label" \
            -e "MIXXX_BENCH_PLL_CLAMP=$arm" || true
    done
done

echo
python3 "$ROOT/tools/bench/series_report.py" "$SERIES"
