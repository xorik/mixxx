#!/bin/bash
# Focused profiling of the CURRENT waveform renderer (Stacked / AllShader).
#
# Goal is not to compare waveform types but to find where the frame budget
# goes, so the current renderer can be optimised. Each WAVEPERF line now
# breaks the frame down into preRenderMs / renderMs / extrasMs / swapMs.
#
# Each run is validated: "phase-locked-loop" lines must keep appearing (~1 per
# 10 s). If they stop, presentation was not really happening, the render loop
# was free-wheeling on a timer and the drop counter is meaningless -> INVALID.
#
# Requires: display ON, session UNLOCKED (brightness may be at zero).
# Does not touch system volume, windows, focus or displays. Mixxx runs with no
# audio device at all (MIXXX_BENCH_NO_AUDIO=1).

set -u

MEASURE=${1:-60}
SETTLE=${2:-30}
PROFILE=/tmp/mixxx-perf
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
CFG="$PROFILE/mixxx.cfg"
LOG="$PROFILE/mixxx.log"
BASE=/tmp/waveperf-matrix-$(date +%Y%m%d-%H%M%S)

TRACKS=(
    "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
    "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
    "/Users/andrey/Music/set/fullon/System Nipel - Artificial Dream.mp3"
    "/Users/andrey/Music/set/disco/MJ - Thriller.mp3"
)

mkdir -p "$BASE"
[ -x "$BIN" ] || { echo "ERROR: $BIN missing" >&2; exit 1; }

set_cfg() {
    if grep -qE "^$1 " "$CFG"; then sed -i '' "s|^$1 .*|$1 $2|" "$CFG"
    else echo "$1 $2" >>"$CFG"; fi
}

run_one() { # label fps [extra args...]
    local label=$1 fps=$2; shift 2
    local out="$BASE/$label"; mkdir -p "$out"
    set_cfg WaveformType 16     # Stacked - the renderer under study
    set_cfg FrameRate "$fps"
    rm -f "$LOG" "$PROFILE"/mixxx.log.*

    echo "=== $label (fps=$fps $*) ==="
    MIXXX_BENCH_NO_AUDIO=1 "$BIN" --developer --log-flush-level debug \
        --settings-path "$PROFILE" "$@" "${TRACKS[@]}" >"$out/stdout.txt" 2>&1 &
    local pid=$!
    sleep "$SETTLE"
    kill -0 "$pid" 2>/dev/null || { echo "  FAILED"; tail -5 "$out/stdout.txt"; return 1; }
    sleep "$MEASURE"
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$pid" 2>/dev/null; sleep 1

    grep WAVEPERF "$out/stdout.txt" >"$out/waveperf.txt" 2>/dev/null
    local pll; pll=$(grep -c "phase-locked-loop" "$out/stdout.txt" 2>/dev/null || echo 0)
    echo "$pll" >"$out/pll_updates.txt"
    echo "  samples=$(wc -l <"$out/waveperf.txt") pllUpdates=$pll"
}

run_one baseline_120        120
run_one baseline_60          60
run_one fullscreen_120      120 --full-screen

echo
echo "results: $BASE"
