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
BIN="$ROOT/build/mixxx"
PROFILE=/tmp/mixxx-mix11u
OUT=/tmp/wfshot
SETTLE=${SETTLE:-25}

TRACKS=(
    "/Users/andrey/Music/zzz/xo/Spor-Powder Monkey .mp3"
    "/Users/andrey/Music/b01_pop.mp3"
    "/Users/andrey/Music/b02_dub.mp3"
)

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
rm -f "$PROFILE"/mixxx.log* "$OUT/$LABEL"/*.png

env MIXXX_WF_GRAB="$OUT/$LABEL/wf" MIXXX_WF_GRAB_AFTER=200 "$@" \
    "$BIN" --developer --settings-path "$PROFILE" "${TRACKS[@]}" \
    >"$OUT/$LABEL/stdout.txt" 2>&1 &
PID=$!
sleep "$SETTLE"

kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$PID" 2>/dev/null

cp "$PROFILE/mixxx.log" "$OUT/$LABEL/mixxx.log" 2>/dev/null
grep -iE "compilation failed|linking failed|could not write" "$OUT/$LABEL/mixxx.log" | head -5
ls -l "$OUT/$LABEL"/*.png 2>/dev/null || echo "NO FRAMES GRABBED"
