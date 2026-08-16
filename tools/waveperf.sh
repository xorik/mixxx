#!/bin/bash
# Waveform rendering performance harness for Mixxx (macOS).
#
# Records the per-second WAVEPERF lines emitted by the instrumented build
# (see WaveformWidgetFactory::renderSelf, active only under --developer).
#
# WHAT THIS SCRIPT DOES *NOT* DO, BY CONSTRUCTION:
#   - never sleeps or wakes any display
#   - never creates, moves or closes any window
#   - never activates an application or moves keyboard focus
# The one unavoidable side effect is that launching Mixxx opens its window,
# which macOS will bring to the front. ALWAYS get the user's go-ahead first.
#
# AUDIO SAFETY: system volume is never touched. Output is pointed at the HDMI
# monitor (not the Mac speakers) only so Mixxx does not raise its blocking
# "no output device" dialog. No deck is ever started, so nothing is ever
# written to the audio device.
#
# Usage: tools/waveperf.sh [seconds] [settle_seconds]

set -u

DURATION=${1:-60}
SETTLE=${2:-30}
PROFILE="$HOME/www/ai/mixxx/bench-profile"   # was /tmp/mixxx-perf until the repo move
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
LOG="$PROFILE/mixxx.log"
OUT="$HOME/www/ai/mixxx/bench-results/waveperf-$(date +%Y%m%d-%H%M%S)"

TRACKS=(
    "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
    "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
    "/Users/andrey/Music/set/fullon/System Nipel - Artificial Dream.mp3"
    "/Users/andrey/Music/set/disco/MJ - Thriller.mp3"
)

mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "ERROR: $BIN missing" >&2; exit 1; }

rm -f "$LOG" "$PROFILE"/mixxx.log.*

MIXXX_BENCH_NO_AUDIO=1 "$BIN" --developer --log-flush-level debug --settings-path "$PROFILE" "${TRACKS[@]}" >"$OUT/stdout.txt" 2>&1 &
MIXXX_PID=$!
echo "pid=$MIXXX_PID  settling ${SETTLE}s, then measuring ${DURATION}s ..."
echo "$(date +%s.%N) launched" >"$OUT/phases.txt"

sleep "$SETTLE"
kill -0 "$MIXXX_PID" 2>/dev/null || {
    echo "ERROR: mixxx died during startup" >&2; tail -20 "$OUT/stdout.txt" >&2; exit 1; }

echo "$(date +%s.%N) measure_start" >>"$OUT/phases.txt"
sleep "$DURATION"
echo "$(date +%s.%N) measure_end" >>"$OUT/phases.txt"

kill -TERM "$MIXXX_PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$MIXXX_PID" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$MIXXX_PID" 2>/dev/null

cp "$LOG" "$OUT/mixxx.log" 2>/dev/null
grep WAVEPERF "$OUT/mixxx.log" >"$OUT/waveperf.txt" 2>/dev/null

echo "results: $OUT"
echo "WAVEPERF samples: $(wc -l <"$OUT/waveperf.txt" 2>/dev/null || echo 0)"
