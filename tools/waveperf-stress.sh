#!/bin/bash
# Compare rendering variants under REDUCED RESOURCES.
#
# Why: with vsync on and ~3x headroom every variant reaches the refresh rate
# and the comparison carries no information. Two ways to make differences
# visible, both supported here:
#
#   MODE=ecore   - taskpolicy -b puts Mixxx in background QoS, so macOS
#                  schedules it on the efficiency cores. Reproducible, needs
#                  no hardware change, unlike unplugging the charger.
#   MODE=nosync  - vsync off entirely: measures raw throughput (fps is a real
#                  number again instead of saturating at the refresh rate).
#   MODE=load    - N spinning processes competing for CPU.
#
# Metric depends on the mode: for ecore/load it is "launches with ZERO dropped
# frames"; for nosync it is achieved fps.
#
# Never touches system volume, windows, focus or displays.

set -u
MODE=${1:-ecore}
MEASURE=${2:-15}
SETTLE=${3:-20}
REPS=${4:-5}
LOADPROCS=${5:-8}

PROFILE=/tmp/mixxx-perf
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
CFG="$PROFILE/mixxx.cfg"
BASE=/tmp/waveperf-stress-$MODE-$(date +%Y%m%d-%H%M%S)

TRACKS=(
    "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
    "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
)
mkdir -p "$BASE"

set_cfg() {
    if grep -qE "^$1 " "$CFG"; then sed -i '' "s|^$1 .*|$1 $2|" "$CFG"
    else echo "$1 $2" >>"$CFG"; fi
}

run_one() { # variant rep
    local cfg=$1 rep=$2
    local out="$BASE/${cfg}_r${rep}"; mkdir -p "$out"
    local env_pllfix="" env_swap="" env_done="" vsync=0

    case "$cfg" in
        base)    ;;
        pllfix)  env_pllfix=1 ;;
        noswap)  env_swap=0 ;;
        nodone)  env_done=1 ;;
        # NOTE: noswap+nodone together render a BLANK waveform - with a
        # non-blocking swap and no doneCurrent the context is never released,
        # makeCurrentIfNeeded() believes it is already current and drawing goes
        # to the wrong surface. Verified by screenshot: waveform band
        # brightness 14.9 vs 69.1. Do not combine them.
        best)    env_pllfix=1; env_swap=0 ;;
    esac
    [ "$MODE" = "nosync" ] && { vsync=4; env_swap=0; }

    set_cfg WaveformType 16
    set_cfg FrameRate 120
    set_cfg VSync "$vsync"
    set_cfg show_4decks 0
    set_cfg waveform_options 2
    rm -f "$PROFILE"/mixxx.log*

    echo "--- $MODE / $cfg rep$rep ---"
    MIXXX_BENCH_NO_AUDIO=1 MIXXX_BENCH_AUTOPLAY=12 \
    MIXXX_BENCH_PLL_FIX="$env_pllfix" \
    MIXXX_BENCH_SWAP_INTERVAL="$env_swap" \
    MIXXX_BENCH_NO_DONECURRENT="$env_done" \
        "$BIN" --developer --log-flush-level debug --settings-path "$PROFILE" \
        --full-screen "${TRACKS[@]}" >"$out/stdout.txt" 2>&1 &
    local pid=$!
    sleep 5
    kill -0 "$pid" 2>/dev/null || { echo "  FAILED"; return 1; }

    local loadpids=()
    if [ "$MODE" = "ecore" ]; then
        # background QoS -> efficiency cores
        taskpolicy -b -p "$pid" 2>/dev/null && echo "  (moved to efficiency cores)"
    elif [ "$MODE" = "load" ]; then
        for _ in $(seq 1 "$LOADPROCS"); do
            ( while :; do :; done ) & loadpids+=($!)
        done
        echo "  ($LOADPROCS competing processes)"
    fi

    sleep "$((SETTLE - 5))"
    sleep "$MEASURE"
    screencapture -x "$out/screen.png" 2>/dev/null

    for lp in ${loadpids[@]+"${loadpids[@]}"}; do kill "$lp" 2>/dev/null; done
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$pid" 2>/dev/null; sleep 1

    grep WAVEPERF "$out/stdout.txt" >"$out/waveperf.txt" 2>/dev/null
    local maxdrop
    maxdrop=$(grep -o 'dropsDelta=[0-9]*' "$out/waveperf.txt" | cut -d= -f2 | sort -n | tail -1)
    echo "${maxdrop:-?}" >"$out/maxdrop.txt"
    echo "  maxDropsInAnySecond=${maxdrop:-?}"
}

VARIANTS="base pllfix noswap nodone best"
[ "$MODE" = "nosync" ] && VARIANTS="base nodone"

for rep in $(seq 1 "$REPS"); do
    for cfg in $VARIANTS; do
        run_one "$cfg" "$rep"
    done
done

set_cfg VSync 0
echo
echo "results: $BASE"
