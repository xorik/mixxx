#!/bin/bash
# A/B series: does folding the PLL phase error symmetrically stop the period drift?
#
# The two arms, both with the drift clamp OFF:
#   A  MIXXX_BENCH_PLL_WRAP=asym   upstream behaviour: only a POSITIVE phase
#                                  error is folded back to the nearest frame
#   B  MIXXX_BENCH_PLL_WRAP=sym    fold both signs
#
# Why the clamp is not an arm here. A numerical simulation of the loop (the code
# re-implemented against synthetic timings, 12000 frames, no Mixxx involved)
# suggests the clamp makes the PHASE worse while making the period look right:
#   upstream          period 8140.5 us, phase error sd 2874 us
#   upstream + clamp  period 8250.0 us, phase error sd 3956 us
#   symmetric fold    period 8332.6 us, phase error sd 1337 us   (true 8333.33)
# Measuring a palliative that is suspected of hurting the metric that matters is
# not worth the user's screen time. That simulation is a reading of the code,
# not a measurement of the system - which is exactly why this series exists.
#
# Design notes that carry over from the drift series:
#   * 90 s per run: drift accumulates with TIME. Measured on the live profile,
#     the period moved 16682.9 -> 16588.9 -> 16565.2 -> 16558.6 us over minutes.
#     A 20 s run sees the tail of that or nothing, and both arms then report the
#     same number - the classic way to "prove" there is no difference.
#   * The arms are INTERLEAVED (A, B, A, B, ...). Running one arm to completion
#     and then the other would attribute any drift of the machine itself to the
#     change under test.
#   * One binary for the whole series; its sha256 is checked before every run.
#     If it changes, the series is void as a whole, not partially.
#
# Metrics, in order of importance: pllPeriodUs over time, then the phase error
# (phaseErr* in the telemetry, once per second), then fps. Dropped frames are
# reference only.
#
# Usage: tools/bench/series-pllwrap.sh [-g WxH+X+Y] [-r reps] [-d measure_s] [-s settle_s]
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

SERIES="$RESULTS/series-pllwrap-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$SERIES" || exit 1

SHA_AT_START=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
{
    echo "series=pllwrap"
    echo "date=$(date -Iseconds)"
    echo "bin_sha256=$SHA_AT_START"
    echo "reps=$REPS measure_s=$MEASURE settle_s=$SETTLE geometry=$GEOMETRY"
    echo "arms=asym(upstream) sym; drift clamp OFF in both"
} >"$SERIES/series.txt"

total=$((REPS * 2))
n=0
per_run_s=$((MEASURE + SETTLE + 15))
echo "series: $total runs, about $((total * per_run_s / 60)) minutes -> $SERIES"

for rep in $(seq 1 "$REPS"); do
    for arm in asym sym; do     # interleaved, never one arm after the other
        n=$((n + 1))
        sha=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
        if [ "$sha" != "$SHA_AT_START" ]; then
            echo "ABORT: the binary changed during the series ($sha != $SHA_AT_START)." >&2
            echo "The whole series is void, not just this run." >&2
            echo "binary changed at run $n" >>"$SERIES/series.txt"
            exit 1
        fi
        label="${arm}_r${rep}"
        echo
        echo "[$n/$total] $label"
        "$ROOT/tools/bench/run.sh" \
            -d "$MEASURE" -s "$SETTLE" -g "$GEOMETRY" \
            -l "$label" -o "$SERIES/$label" \
            -e "MIXXX_BENCH_PLL_WRAP=$arm" \
            -e "MIXXX_BENCH_PLL_CLAMP=0" || true
    done
done

echo
python3 "$ROOT/tools/bench/series_report.py" "$SERIES"
