#!/bin/bash
# Validity check: can waveform performance be measured while the Mac is locked?
#
# INTRUSIVE ON PURPOSE - only run with the user's explicit go-ahead.
# It puts the display to sleep, and because the screen-lock delay on this
# machine is "immediate", that locks the session. The user must log back in.
# Nothing else is touched: no window is created, moved or closed, no
# application is activated, system volume is never changed.
#
# Phase A: screen on   - baseline
# Phase B: screen off / session locked
# If both phases agree, unattended background measurement is trustworthy.
#
# Usage: tools/waveperf-locked.sh [phase_seconds] [settle_seconds]

set -u

PHASE=${1:-45}
SETTLE=${2:-30}
PROFILE=/tmp/mixxx-perf
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
LOG="$PROFILE/mixxx.log"
OUT=/tmp/waveperf-locked-$(date +%Y%m%d-%H%M%S)

TRACKS=(
    "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
    "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
    "/Users/andrey/Music/set/fullon/System Nipel - Artificial Dream.mp3"
    "/Users/andrey/Music/set/disco/MJ - Thriller.mp3"
)

mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "ERROR: $BIN missing" >&2; exit 1; }
rm -f "$LOG" "$PROFILE"/mixxx.log.*

MIXXX_BENCH_NO_AUDIO=1 "$BIN" --developer --log-flush-level debug --settings-path "$PROFILE" "${TRACKS[@]}" \
    >"$OUT/stdout.txt" 2>&1 &
MIXXX_PID=$!
mark() { echo "$(date +%s.%N) $1" >>"$OUT/phases.txt"; }

sleep "$SETTLE"
kill -0 "$MIXXX_PID" 2>/dev/null || {
    echo "ERROR: mixxx died during startup" >&2; tail -20 "$OUT/stdout.txt" >&2; exit 1; }

mark A_screen_on_start
sleep "$PHASE"
mark A_screen_on_end

# ---- lock ----
pmset displaysleepnow
sleep 3
mark B_locked_start
sleep "$PHASE"
mark B_locked_end

kill -TERM "$MIXXX_PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$MIXXX_PID" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$MIXXX_PID" 2>/dev/null

cp "$LOG" "$OUT/mixxx.log" 2>/dev/null
grep WAVEPERF "$OUT/mixxx.log" >"$OUT/waveperf.txt" 2>/dev/null
echo "results: $OUT"
echo "WAVEPERF samples: $(wc -l <"$OUT/waveperf.txt" 2>/dev/null || echo 0)"
