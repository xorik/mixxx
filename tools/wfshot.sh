#!/bin/bash
# Waveform screenshot harness for the shader work (MIX-11).
#
# Launches the build of this worktree on a throwaway settings profile (a copy
# of the user's profile, so the reference tracks are already analyzed), lets it
# render for a while and writes one PNG per deck straight from the OpenGL frame
# buffer of the waveform widget (see WaveformWidget::grabFrameIfRequested).
# Reading the frame buffer inside the process avoids the two traps of
# screenshots: without the Screen Recording permission a screenshot is silently
# black, and whatever covers the window ends up in the picture.
#
# No deck is ever started, so nothing can be played.
#
# Usage: tools/wfshot.sh <label> [waveform type] [env assignments...]
#   tools/wfshot.sh new 30
#   tools/wfshot.sh stock 12
#   tools/wfshot.sh nofloor 30 MIXXX_WF_AMP_FLOOR=0

set -u

LABEL=${1:?label required}
TYPE=${2:-30}
shift
shift 2>/dev/null || true

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${WFBIN:-$ROOT/build/mixxx}"
PROFILE=${WFPROFILE:-/tmp/mixxx-mix11u}
OUT=/tmp/wfshot
SETTLE=${SETTLE:-25}

# WFTRACK loads a single file instead of the default set, WFZOOM sets the zoom
# level (1 = most detail, 10 = most time on screen).
if [ -n "${WFTRACK:-}" ]; then
    TRACKS=("$WFTRACK")
else
    # Standard material: two test files where the comparison with Traktor goes
    # by segment, and two pieces of music. One run gives one frame per deck.
    TRACKS=(
        "/Users/andrey/Music/30_mixgrid.wav"
        "/Users/andrey/Music/40_chip.wav"
        "/Users/andrey/Music/zzz/xo/Spor-Powder Monkey .mp3"
        "/Users/andrey/Music/b01_pop.mp3"
    )
fi

# HARD GUARD. Without the benchmark hooks the window comes to the front and a
# profile without an output device blocks on a modal dialog. Both have already
# disturbed the user, so this is a refusal, not a warning.
if ! grep -rqs "MIXXX_BENCH" "$ROOT/src"; then
    echo "ERROR: the benchmark hooks are not applied, refusing to launch." >&2
    echo "       git apply /tmp/mixxx-bench-shared/bench-hooks.patch" >&2
    exit 1
fi

mkdir -p "$OUT/$LABEL"
[ -x "$BIN" ] || { echo "ERROR: $BIN missing" >&2; exit 1; }
if pgrep -x mixxx >/dev/null; then
    echo "ERROR: another Mixxx is running, refusing to start a second one" >&2
    exit 1
fi

# The profile is a copy of a 2.5.4 one. As long as its [Config] Version says
# 2.5.4, Mixxx runs the "move everyone to all-shader waveforms" upgrade on
# every start, which silently rewrites the waveform type (any unknown type,
# including our new one, becomes plain RGB). Pin the version to the version of
# this build so the upgrade stays out of the way.
sed -i '' "s/^Version 2\.5\..*/Version 2.7.0-alpha/" "$PROFILE/mixxx.cfg"
sed -i '' "s/^WaveformType .*/WaveformType $TYPE/" "$PROFILE/mixxx.cfg"
if [ -n "${WFZOOM:-}" ]; then
    sed -i '' "s/^DefaultZoom .*/DefaultZoom $WFZOOM/" "$PROFILE/mixxx.cfg"
fi
rm -f "$PROFILE"/mixxx.log* "$OUT/$LABEL"/*.png

env MIXXX_WF_GRAB="$OUT/$LABEL/wf" MIXXX_WF_GRAB_AFTER=200 "$@" \
    "$BIN" --developer --settings-path "$PROFILE" "${TRACKS[@]}" \
    >"$OUT/$LABEL/stdout.txt" 2>&1 &
PID=$!

# The hooks must report that they actually ran, otherwise the window jumps to
# the front. A flag that was passed but never executed looks exactly like a
# working background mode until it is too late.
hooks_ok=0
for _ in 1 2 3 4 5 6; do
    sleep 2
    if grep -q "accessoryPolicyApplied=true" "$OUT/$LABEL/stdout.txt" 2>/dev/null &&
            grep -q "BENCHHIT MIXXX_BENCH_NO_AUDIO=1" "$OUT/$LABEL/stdout.txt" 2>/dev/null; then
        hooks_ok=1
        break
    fi
    kill -0 "$PID" 2>/dev/null || break
done
if [ "$hooks_ok" -ne 1 ]; then
    echo "ERROR: the hooks did not report, killing the run before it can take focus." >&2
    kill -KILL "$PID" 2>/dev/null
    exit 1
fi

# Wait in short steps rather than one long sleep, so the run stays visible and
# interruptible, and stop as soon as the frames are on disk.
waited=0
while [ "$waited" -lt "$SETTLE" ]; do
    sleep 5
    waited=$((waited + 5))
    if ls "$OUT/$LABEL"/*.png >/dev/null 2>&1; then
        sleep 2
        break
    fi
    kill -0 "$PID" 2>/dev/null || break
done

kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$PID" 2>/dev/null

cp "$PROFILE/mixxx.log" "$OUT/$LABEL/mixxx.log" 2>/dev/null
grep -iE "compilation failed|linking failed|could not write" "$OUT/$LABEL/mixxx.log" | head -5
ls -l "$OUT/$LABEL"/*.png 2>/dev/null || echo "NO FRAMES GRABBED"
