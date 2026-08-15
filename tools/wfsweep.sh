#!/bin/bash
# The whole series of runs for MIX-11 in one go: re-shoot of the current state
# plus three one-knob-at-a-time sweeps. Every run loads the same four tracks
# (two test files and two pieces of music) and writes one frame per deck, so
# eight runs cover everything.
#
# Takes the launcher as the first argument, so the same series can be run
# through whatever mechanism is allowed at the time:
#   tools/wfsweep.sh tools/wfshot-bg.sh
#
# Nothing is ever played.

set -u

SHOOT=${1:?pass the shot script, e.g. tools/wfshot-bg.sh}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export WFPROFILE=${WFPROFILE:-/tmp/mixxx-mix11n}
export WFZOOM=${WFZOOM:-10}
export SETTLE=${SETTLE:-40}

# deck 1 mixgrid, deck 2 chip, deck 3 Powder Monkey, deck 4 b01_pop
SEEK=MIXXX_WF_SEEK=12,14,55,240
SEEK_B=MIXXX_WF_SEEK=34,14,55,240

run() {
    label=$1
    shift
    echo "=== $label"
    "$ROOT/$SHOOT" "$label" 30 "$@" | tail -5
}

# Current defaults: smooth 7, amp floor 0.19, level floor 0.03,
# band gain 1.068/1/7.111, gamma 1, soft edge 3.
run n3-def   "$SEEK"
run n3-gridb "$SEEK_B"

run amp010 "$SEEK" MIXXX_WF_AMP_FLOOR=0.10
run amp028 "$SEEK" MIXXX_WF_AMP_FLOOR=0.28

run lvl001 "$SEEK" MIXXX_WF_COLOR_LEVEL_FLOOR=0.01
run lvl008 "$SEEK" MIXXX_WF_COLOR_LEVEL_FLOOR=0.08

run sm03 "$SEEK" MIXXX_WF_COLOR_SMOOTH_BINS=3
run sm12 "$SEEK" MIXXX_WF_COLOR_SMOOTH_BINS=12

echo "=== all runs done"
