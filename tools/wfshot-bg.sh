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

# Window placement comes from MIXXX_BENCH_GEOMETRY (geometry.patch), not from
# the settings: writing a QWidget::saveGeometry blob into [MainWindow] geometry
# does nothing at all. Qt silently rejects a rectangle it does not consider
# sane for the current virtual desktop, and every run of this harness spent its
# life on the laptop screen of the user while the config said otherwise.
# Empty means "wherever it opens", which is the only option while the external
# monitors are switched off.
WFGEOMETRY=${WFGEOMETRY:-}

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

# Anything that makes Mixxx touch a protected folder or open an input device
# asks the user for permission, and none of it has anything to do with drawing
# a waveform. The external libraries scan folders at start-up (the Traktor one
# lives in ~/Documents), and enabled MIDI controllers open input devices, which
# is what the "receive keystrokes" prompt is about.
sed -i '' -E "s/^Show(ITunes|Rekordbox|Serato|Traktor|Rhythmbox|Banshee)Library .*/Show\1Library 0/" \
    "$PROFILE/mixxx.cfg"
awk '/^\[Controller\]/{c=1;print;next} /^\[/{c=0} c && NF {sub(/ [0-9]+$/, " 0")} {print}' \
    "$PROFILE/mixxx.cfg" >"$PROFILE/mixxx.cfg.tmp" && mv "$PROFILE/mixxx.cfg.tmp" "$PROFILE/mixxx.cfg"

# Keep the upgrade path out of the way (an old [Config] Version makes Mixxx
# silently rewrite the waveform type on every start), then set type, zoom and
# the window position.
sed -i '' "s/^Version 2\.5\..*/Version 2.7.0-alpha/" "$PROFILE/mixxx.cfg"
sed -i '' "s/^WaveformType .*/WaveformType $TYPE/" "$PROFILE/mixxx.cfg"
if [ -n "${WFZOOM:-}" ]; then
    sed -i '' "s/^DefaultZoom .*/DefaultZoom $WFZOOM/" "$PROFILE/mixxx.cfg"
fi
rm -f "$PROFILE"/mixxx.log* "$OUT/$LABEL"/*.png

# Two different things have to be switched off. The benchmark hooks stop the
# application from ACTIVATING (taking keyboard focus); "open -g" stops its
# window from being ORDERED IN FRONT. We were treating the first as if it
# covered the second, which is why the window kept appearing.
# Bundle only. A bare binary can be launched, but its window is ordered in
# front of everything, and there is no way to ask macOS not to. "open -g" is
# the only thing that controls the ORDER, so it is not optional here.
if true; then
    BUNDLE=/tmp/mixxx-bg/MixxxBg.app
    [ -d "$BUNDLE" ] || { echo "ERROR: $BUNDLE missing" >&2; exit 1; }
    envargs=(--env "MIXXXBG_BINARY=$BIN"
             --env MIXXX_BENCH_NO_AUDIO=1
             ${WFGEOMETRY:+--env "MIXXX_BENCH_GEOMETRY=$WFGEOMETRY"}
             --env MIXXX_BENCH_BACKGROUND=1
             --env QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1
             --env "MIXXX_WF_GRAB=$OUT/$LABEL/wf"
             --env MIXXX_WF_GRAB_AFTER=200)
    for assignment in "$@"; do
        envargs+=(--env "$assignment")
    done
    open -g "$BUNDLE" "${envargs[@]}" --args \
        --developer --log-flush-level debug --resource-path "$ROOT/res" \
        --settings-path "$PROFILE" "${TRACKS[@]}"
    PID=""
    # "open" returns at once and the process writes no stdout of ours, so the
    # log of the profile is the only place to look.
    LOG="$PROFILE/mixxx.log"
fi

# The hooks must report that they actually ran, otherwise the window jumps to
# the front. A flag that was passed but never executed looks exactly like a
# working background mode until it is too late.
hooks_ok=0
for _ in $(seq 1 12); do
    sleep 2
    # What counts is the state we ended up in, not who put us there: when the
    # bundle is marked LSUIElement the process is accessory before the hook
    # runs, so "alreadyAccessory" is a success and only "FAILED" is not.
    # Two spellings, because the shared hook patch has carried both: hers
    # ("accessoryPolicy=applied|alreadyAccessory") and the one that reports the
    # state it ended up in ("policyNow=accessory"). Matching only one of them
    # killed a run that had already done its work.
    if grep -qE "accessoryPolicy=(applied|alreadyAccessory)|policyNow=accessory" "$LOG" 2>/dev/null &&
            grep -q "BENCHHIT MIXXX_BENCH_NO_AUDIO=1" "$LOG" 2>/dev/null; then
        hooks_ok=1
        break
    fi
    [ -z "$PID" ] || kill -0 "$PID" 2>/dev/null || break
done
if [ "$hooks_ok" -ne 1 ]; then
    echo "ERROR: the hooks did not report, killing the run before it can take focus." >&2
    [ -z "$PID" ] || kill -KILL "$PID" 2>/dev/null
    pkill -9 -x mixxx 2>/dev/null
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

if [ -n "$PID" ]; then
    kill -TERM "$PID" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$PID" 2>/dev/null
else
    pkill -x mixxx 2>/dev/null
    sleep 2
    pkill -9 -x mixxx 2>/dev/null
fi

cp "$PROFILE/mixxx.log" "$OUT/$LABEL/mixxx.log" 2>/dev/null
grep -E "BENCHFLAG|BENCHHIT" "$LOG" | head -6
ls -l "$OUT/$LABEL"/*.png 2>/dev/null || echo "NO FRAMES GRABBED"
