#!/bin/bash
# Interleaved A/B/C/D benchmark of the current waveform renderer.
#
# Earlier runs gave contradictory answers (windowed vs fullscreen swapped
# places between sessions) because each configuration was measured once, at a
# different moment, on a machine with varying background load. This runner
# fixes that: every configuration is repeated REPS times and the order is
# interleaved, so drifting system load hits all of them equally. System load
# is sampled during each run and recorded next to the results.
#
# Configurations:
#   win      - windowed, 120 Hz target                       (current default)
#   full     - fullscreen, 120 Hz target
#   core     - windowed, 120 Hz, OpenGL 4.1 Core Profile     (vs legacy 2.1)
#   nosync   - windowed, swapInterval=0 + ST_FREE            (throughput ceiling)
#
# Does not touch system volume, windows, focus or displays.
# Mixxx runs with no audio device at all.

set -u
MEASURE=${1:-40}
SETTLE=${2:-25}
REPS=${3:-3}
PROFILE="$HOME/www/ai/mixxx/bench-profile"   # was /tmp/mixxx-perf until the repo move
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
CFG="$PROFILE/mixxx.cfg"
BASE="$HOME/www/ai/mixxx/bench-results/waveperf-ab-$(date +%Y%m%d-%H%M%S)"

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

run_one() { # config rep
    local cfg=$1 rep=$2
    local out="$BASE/${cfg}_r${rep}"; mkdir -p "$out"
    local extra=() ; local env_swap="" ; local env_core="" ; local vsync=0

    local decks=0
    case "$cfg" in
        win2)   decks=0 ;;                      # 2 visible decks = 2 GL surfaces
        win4)   decks=1 ;;                      # 4 visible decks = 4 GL surfaces
        core)   decks=0; env_core=1 ;;          # 2 decks, OpenGL 4.1 Core
        nosync) decks=0; env_swap=0; vsync=4 ;; # 2 decks, no vsync: raw ceiling
    esac
    set_cfg show_4decks "$decks"

    set_cfg WaveformType 16
    set_cfg FrameRate 120
    set_cfg VSync "$vsync"
    rm -f "$PROFILE"/mixxx.log*

    echo "--- $cfg rep$rep ---"
    MIXXX_BENCH_NO_AUDIO=1 \
    MIXXX_BENCH_SWAP_INTERVAL="$env_swap" \
    MIXXX_BENCH_CORE_PROFILE="$env_core" \
        "$BIN" --developer --log-flush-level debug --settings-path "$PROFILE" \
        ${extra[@]+"${extra[@]}"} "${TRACKS[@]}" >"$out/stdout.txt" 2>&1 &
    local pid=$!
    sleep "$SETTLE"
    kill -0 "$pid" 2>/dev/null || { echo "  FAILED"; tail -3 "$out/stdout.txt"; return 1; }

    # sample system-wide load while measuring, to explain any outliers later
    ( for _ in $(seq 1 "$MEASURE"); do
        ps -A -o %cpu | awk '{s+=$1} END {print s}'; sleep 1
      done ) >"$out/sysload.txt" &
    local loadpid=$!

    sleep "$MEASURE"
    kill "$loadpid" 2>/dev/null
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$pid" 2>/dev/null; sleep 1

    grep WAVEPERF "$out/stdout.txt" >"$out/waveperf.txt" 2>/dev/null
    grep -c "phase-locked-loop" "$out/stdout.txt" >"$out/pll_updates.txt" 2>/dev/null
    grep -m1 "OpenGL driver version string" "$out/stdout.txt" >"$out/glinfo.txt" 2>/dev/null
    echo "  samples=$(wc -l <"$out/waveperf.txt")"
}

for rep in $(seq 1 "$REPS"); do
    for cfg in win2 win4 core nosync; do
        run_one "$cfg" "$rep"
    done
done

set_cfg VSync 0
echo
echo "results: $BASE"
