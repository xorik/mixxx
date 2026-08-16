#!/bin/bash
# Everything that can be tested WITHOUT rewriting the renderer and WITHOUT
# merging the per-deck GL surfaces into one.
#
# All configurations run at the DOUBLED waveform height (416), because on an
# idle machine at the normal size every variant already sits at ~0 drops and
# the comparison carries no information. The doubled widget is the stress case
# and also what the user actually wants to be smooth.
#
#   ref208   - normal height, as shipped: reference point
#   base     - doubled height, as shipped
#   noswap   - swapInterval=0, let the PLL alone pace frames
#   nodone   - no doneCurrent() between surfaces
#   core     - OpenGL 4.1 Core Profile + GLSL 410 instead of legacy 2.1/GLSL 120
#   triple   - triple buffering
#   timer    - VSyncThread ST_TIMER instead of ST_PLL (config only)
#   all      - noswap + nodone + core + triple
#
# Interleaved and repeated; power source recorded per second so a battery blip
# can be excluded. Fullscreen, playback, faders at zero (gain untouched).

set -u
MEASURE=${1:-20}
SETTLE=${2:-20}
REPS=${3:-3}
PROFILE="$HOME/www/ai/mixxx/bench-profile"   # was /tmp/mixxx-perf until the repo move
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
CFG="$PROFILE/mixxx.cfg"
BASE="$HOME/www/ai/mixxx/bench-results/waveperf-full-$(date +%Y%m%d-%H%M%S)"

TRACKS=(
    "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
    "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
)
mkdir -p "$BASE"

set_cfg() {
    if grep -qE "^$1 " "$CFG"; then sed -i '' "s|^$1 .*|$1 $2|" "$CFG"
    else echo "$1 $2" >>"$CFG"; fi
}

run_one() { # config rep
    local cfg=$1 rep=$2
    local out="$BASE/${cfg}_r${rep}"; mkdir -p "$out"
    local env_swap="" env_done="" env_core="" env_behav="" split="416,605" vsync=0

    # PASS/FAIL metric: with vsync on, fps saturates at the refresh rate and
    # carries no information. What matters is whether a launch achieves ZERO
    # dropped frames for the whole window, so we report a pass rate over many
    # launches instead of averaging frame rates.
    set_cfg waveform_options 2
    local env_pllfix="" env_noreattach=""
    case "$cfg" in
        dl_helper) env_noreattach=1 ;;  # driver stays on the 3x3 shared window
        dl_real)   ;;                   # driver moved to a real waveform window
    esac
    set_cfg VSync 7

    set_cfg WaveformType 16
    set_cfg FrameRate 120
    set_cfg show_4decks 0
    set_cfg stackedWaveforms_splitSize "208,813"
    rm -f "$PROFILE"/mixxx.log*

    echo "--- $cfg rep$rep ---"
    MIXXX_BENCH_NO_AUDIO=1 \
    MIXXX_BENCH_AUTOPLAY=12 \
    MIXXX_BENCH_SWAP_INTERVAL="$env_swap" \
    MIXXX_BENCH_NO_DONECURRENT="$env_done" \
    MIXXX_BENCH_CORE_PROFILE="$env_core" \
    MIXXX_BENCH_SWAP_BEHAVIOR="$env_behav" \
    MIXXX_BENCH_PLL_FIX="$env_pllfix" \
    MIXXX_BENCH_NO_REATTACH="$env_noreattach" \
        "$BIN" --developer --log-flush-level debug --settings-path "$PROFILE" \
        --full-screen "${TRACKS[@]}" >"$out/stdout.txt" 2>&1 &
    local pid=$!
    sleep "$SETTLE"
    kill -0 "$pid" 2>/dev/null || { echo "  FAILED to start"; tail -3 "$out/stdout.txt"; return 1; }

    ( for _ in $(seq 1 "$MEASURE"); do
        printf '%s %s\n' "$(date +%H:%M:%S)" "$(pmset -g batt | grep -o 'AC Power\|Battery Power')"
        sleep 1
      done ) >"$out/power.txt" &
    local lp=$!
    sleep "$MEASURE"
    kill "$lp" 2>/dev/null
    # Validity check: a broken shader still yields perfect telemetry while
    # drawing nothing, so capture the screen before closing and verify later
    # that the waveform area is not just black.
    screencapture -x "$out/screen.png" 2>/dev/null
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$pid" 2>/dev/null; sleep 1

    grep WAVEPERF "$out/stdout.txt" >"$out/waveperf.txt" 2>/dev/null
    grep -c "phase-locked-loop" "$out/stdout.txt" >"$out/pll_updates.txt" 2>/dev/null
    grep -m1 "OpenGL driver version string" "$out/stdout.txt" >"$out/glinfo.txt" 2>/dev/null
    grep -ci "QOpenGLShader::compile.*ERROR" "$out/stdout.txt" >"$out/shader_errors.txt" 2>/dev/null
    # pass = no second in the measured window had a dropped frame
    local maxdrop
    maxdrop=$(grep -o 'dropsDelta=[0-9]*' "$out/waveperf.txt" | cut -d= -f2 | sort -n | tail -1)
    local lock
    lock=$(grep -oE "PLL locked to [0-9.]+|phase-locked-loop: [0-9]+ [0-9.]+" "$out/stdout.txt" | tail -1)
    echo "${maxdrop:-?}" >"$out/maxdrop.txt"
    echo "  maxDropsInAnySecond=${maxdrop:-?}  $lock"
}

for rep in $(seq 1 "$REPS"); do
    for cfg in dl_helper dl_real; do
        run_one "$cfg" "$rep"
    done
done

set_cfg VSync 0
set_cfg stackedWaveforms_splitSize "208,813"
echo
echo "results: $BASE"
