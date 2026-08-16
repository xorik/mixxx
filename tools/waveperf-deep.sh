#!/bin/bash
# Deep profiling of the current waveform renderer.
#
#   windowed_120  - the broken case: 120 Hz target in a window, ~13 drops/s.
#                   Also sampled with the macOS `sample` profiler so we can see
#                   what the GUI thread is actually doing when it misses vsync.
#   freerun       - VSync=4 (ST_FREE): no vsync at all. Shows the raw ceiling of
#                   the renderer, i.e. what this hardware can actually do.
#   fullscreen_120- the known-good case, for reference.
#
# WAVEPERF now also reports dispatchMs: how long the render signal sat in the
# GUI thread's event queue before being handled. That separates "the renderer
# is slow" from "the main thread was busy elsewhere".
#
# Does not touch system volume, windows, focus or displays.

set -u
MEASURE=${1:-60}
SETTLE=${2:-30}
PROFILE=/tmp/mixxx-perf
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
CFG="$PROFILE/mixxx.cfg"
BASE=/tmp/waveperf-deep-$(date +%Y%m%d-%H%M%S)

TRACKS=(
    "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
    "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
    "/Users/andrey/Music/set/fullon/System Nipel - Artificial Dream.mp3"
    "/Users/andrey/Music/set/disco/MJ - Thriller.mp3"
)
mkdir -p "$BASE"

set_cfg() {
    if grep -qE "^$1 " "$CFG"; then sed -i '' "s|^$1 .*|$1 $2|" "$CFG"
    else echo "$1 $2" >>"$CFG"; fi
}

run_one() { # label fps vsyncmode do_sample [extra args]
    local label=$1 fps=$2 vsync=$3 dosample=$4; shift 4
    local out="$BASE/$label"; mkdir -p "$out"
    set_cfg WaveformType 16
    set_cfg FrameRate "$fps"
    set_cfg VSync "$vsync"
    rm -f "$PROFILE"/mixxx.log*

    echo "=== $label fps=$fps vsync=$vsync $* ==="
    MIXXX_BENCH_NO_AUDIO=1 "$BIN" --developer --log-flush-level debug \
        --settings-path "$PROFILE" "$@" "${TRACKS[@]}" >"$out/stdout.txt" 2>&1 &
    local pid=$!
    sleep "$SETTLE"
    kill -0 "$pid" 2>/dev/null || { echo "  FAILED"; tail -5 "$out/stdout.txt"; return 1; }

    if [ "$dosample" = "yes" ]; then
        # macOS built-in sampling profiler: 20 s of stacks at 1 ms
        sample "$pid" 20 1 -file "$out/sample.txt" >/dev/null 2>&1 &
    fi
    sleep "$MEASURE"

    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$pid" 2>/dev/null; sleep 1

    grep WAVEPERF "$out/stdout.txt" >"$out/waveperf.txt" 2>/dev/null
    echo "$(grep -c 'phase-locked-loop' "$out/stdout.txt" 2>/dev/null || echo 0)" >"$out/pll_updates.txt"
    echo "  samples=$(wc -l <"$out/waveperf.txt")"
}

run_one windowed_120   120 0 yes
run_one freerun        120 4 no
run_one fullscreen_120 120 0 no --full-screen

set_cfg VSync 0
echo
echo "results: $BASE"
