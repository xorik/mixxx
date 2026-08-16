#!/bin/bash
# Honest benchmark: FULLSCREEN, DURING PLAYBACK, 2 decks.
#
# Earlier runs measured a frozen screen, which understates the cost: while a
# track plays the waveform scrolls, the visible range is recomputed and the
# renderer re-uploads waveform data every frame.
#
# AUDIO: the benchmark hook pulls every deck's VOLUME FADER to zero before
# starting playback. pregain/gain are deliberately left alone because they
# scale the drawn waveform itself. System volume is never touched.
#
# Configurations (interleaved, repeated, to survive background-load drift):
#   base     - as shipped
#   noswap   - swapInterval=0: let the PLL alone pace frames instead of
#              blocking a second time inside swapBuffers
#   nodone   - skip doneCurrent() between surfaces
#   both     - noswap + nodone
#
# Does not touch windows, focus or displays.

set -u
MEASURE=${1:-40}
SETTLE=${2:-30}
REPS=${3:-3}
PROFILE=/tmp/mixxx-perf
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
CFG="$PROFILE/mixxx.cfg"
BASE=/tmp/waveperf-play-$(date +%Y%m%d-%H%M%S)

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
    local env_swap="" env_done="" split="208,813"
    # stackedWaveforms_splitSize: first number is the waveform area height.
    # 208 is the user's real layout; 416 is exactly twice the area (width fixed).
    case "$cfg" in
        base208)   split="208,813" ;;
        noswap208) split="208,813"; env_swap=0 ;;
        base416)   split="416,605" ;;
        noswap416) split="416,605"; env_swap=0 ;;
    esac
    set_cfg stackedWaveforms_splitSize "$split"

    set_cfg WaveformType 16
    set_cfg FrameRate 120
    set_cfg VSync 0
    set_cfg show_4decks 0
    rm -f "$PROFILE"/mixxx.log*

    echo "--- $cfg rep$rep ---"
    MIXXX_BENCH_NO_AUDIO=1 \
    MIXXX_BENCH_AUTOPLAY=12 \
    MIXXX_BENCH_SWAP_INTERVAL="$env_swap" \
    MIXXX_BENCH_NO_DONECURRENT="$env_done" \
        "$BIN" --developer --log-flush-level debug --settings-path "$PROFILE" \
        --full-screen "${TRACKS[@]}" >"$out/stdout.txt" 2>&1 &
    local pid=$!
    sleep "$SETTLE"
    kill -0 "$pid" 2>/dev/null || { echo "  FAILED"; tail -3 "$out/stdout.txt"; return 1; }

    ( for _ in $(seq 1 "$MEASURE"); do
        printf '%s %s\n' "$(date +%H:%M:%S)" "$(pmset -g batt | grep -o "AC Power\|Battery Power")"
        sleep 1
      done ) >"$out/power.txt" &
    local loadpid=$!
    sleep "$MEASURE"
    kill "$loadpid" 2>/dev/null
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$pid" 2>/dev/null; sleep 1

    grep WAVEPERF "$out/stdout.txt" >"$out/waveperf.txt" 2>/dev/null
    grep -c "phase-locked-loop" "$out/stdout.txt" >"$out/pll_updates.txt" 2>/dev/null
    grep -q "BENCH: deck faders at 0" "$out/stdout.txt" && echo yes >"$out/playing.txt" || echo no >"$out/playing.txt"
    echo "  samples=$(wc -l <"$out/waveperf.txt") playing=$(cat "$out/playing.txt")"
}

for rep in $(seq 1 "$REPS"); do
    for cfg in base208 noswap208 base416 noswap416; do
        run_one "$cfg" "$rep"
    done
done
echo
echo "results: $BASE"
