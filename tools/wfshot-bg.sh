#!/bin/bash
# Background variant of tools/wfshot.sh. Uses the benchmark hooks of
# @mixxx-render (MIXXX_BENCH_NO_AUDIO, MIXXX_BENCH_BACKGROUND) so that the
# window is drawn but the application never activates, and places the window on
# the second DELL (U2417H at +2048+0) so it never covers the screen the user is
# working on. All three environment variables are needed together: without
# BACKGROUND the process cannot show a window at all and no frame is grabbed.
#
# Nothing is played: decks load paused and no deck is ever started. The audio
# output is the built-in speakers, which are always present (an HDMI output
# disappears when the monitor sleeps, and Mixxx then blocks on a modal dialog).
#
# REQUIRES the benchmark hooks patch of @mixxx-render, which is deliberately
# NOT part of this branch (debug code does not belong in a feature branch):
#   git apply /tmp/mixxx-bench-shared/bench-hooks.patch
# Without it the two MIXXX_BENCH variables do nothing, the window takes focus
# and a profile without an output device blocks on a modal dialog.
#
# Usage: tools/wfshot-bg.sh <label> [waveform type] [env assignments...]

set -u

LABEL=${1:?label required}
TYPE=${2:-30}
shift
shift 2>/dev/null || true

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${WFBIN:-$ROOT/build/mixxx}"
PROFILE=${WFPROFILE:-/tmp/mixxx-mix11n}
OUT=/tmp/wfshot
SETTLE=${SETTLE:-40}

# Window on the second DELL, 1900x1000 at +2068+40 (QWidget::saveGeometry blob).
GEOMETRY='AdnQywADAAAAAAgUAAAAKAAAD38AAAQPAAAIFAAAAEQAAA9/AAAEDwAAAAIAAAAAB4AAAAgUAAAARAAAD38AAAQP'

# WFTRACKS overrides the material, colon separated. Only the first two decks
# are rendered by the LateNight skin, so put what matters in decks 1 and 2.
if [ -n "${WFTRACKS:-}" ]; then
    IFS=':' read -r -a TRACKS <<< "$WFTRACKS"
else
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

# Built-in speakers, never absent. Written by Mixxx itself, so valid by
# construction.
cp /tmp/mixxx-bench-shared/soundconfig-silent.xml "$PROFILE/soundconfig.xml"

# Keep the upgrade path out of the way (an old [Config] Version makes Mixxx
# silently rewrite the waveform type on every start), then set type, zoom and
# the window position.
sed -i '' "s/^Version 2\.5\..*/Version 2.7.0-alpha/" "$PROFILE/mixxx.cfg"
sed -i '' "s/^WaveformType .*/WaveformType $TYPE/" "$PROFILE/mixxx.cfg"
sed -i '' "s|^geometry .*|geometry $GEOMETRY|" "$PROFILE/mixxx.cfg"
if [ -n "${WFZOOM:-}" ]; then
    sed -i '' "s/^DefaultZoom .*/DefaultZoom $WFZOOM/" "$PROFILE/mixxx.cfg"
fi
rm -f "$PROFILE"/mixxx.log* "$OUT/$LABEL"/*.png

env MIXXX_BENCH_NO_AUDIO=1 MIXXX_BENCH_BACKGROUND=1 \
    QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 \
    MIXXX_WF_GRAB="$OUT/$LABEL/wf" MIXXX_WF_GRAB_AFTER=200 "$@" \
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

waited=0
while [ "$waited" -lt "$SETTLE" ]; do
    sleep 5
    waited=$((waited + 5))
    if ls "$OUT/$LABEL"/*.png >/dev/null 2>&1; then
        sleep 2
        break
    fi
done

kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$PID" 2>/dev/null

cp "$PROFILE/mixxx.log" "$OUT/$LABEL/mixxx.log" 2>/dev/null
grep -E "BENCHFLAG|BENCHHIT" "$OUT/$LABEL/stdout.txt" | head -5
ls -l "$OUT/$LABEL"/*.png 2>/dev/null || echo "NO FRAMES GRABBED"
