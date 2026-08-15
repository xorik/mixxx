#!/bin/bash
# Performance matrix for the waveform types: the cost of the colour window as a
# function of the number of texture samples, measured at two widget heights.
#
# Raw logs only, no numbers are computed here: whoever owns the telemetry does
# the maths. What this script guarantees is provenance, so that the logs can be
# trusted:
#   * the binary is hashed before and after the series, and a mismatch
#     invalidates the WHOLE series, not the runs after the rebuild;
#   * every run records what actually arrived in the shader (BENCHHIT lines),
#     not what was asked for;
#   * radii alternate inside a height, so a slow drift of the machine cannot be
#     mistaken for an effect of the radius.
#
# REQUIRES the benchmark hooks and the telemetry patch. Without them there is
# nothing to measure and the window would come to the front.
#
# Usage: tools/wfperf.sh [repeats]

set -u

REPEATS=${1:-3}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/mixxx"
PROFILE=${WFPROFILE:-/tmp/mixxx-mix11n}
OUT=/tmp/wfperf-$(date +%Y%m%d-%H%M%S)

# 208,813 is the height of the waveform in the skin as the user has it,
# 416,605 is exactly twice the area at the same width.
HEIGHTS=("208,813" "416,605")
RADII=(0 3 7 12)

mkdir -p "$OUT"
if ! grep -rqs "MIXXX_BENCH" "$ROOT/src"; then
    echo "ERROR: the benchmark hooks are not applied, refusing to launch." >&2
    exit 1
fi

sha_before=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
echo "binary sha256 before: $sha_before" | tee "$OUT/provenance.txt"

# The window has already ended up on the wrong screen once, on the laptop the
# user works on, and it went unnoticed for the whole series because nobody
# looked. So the screen is checked on the FIRST run and the series stops right
# there if it is wrong, instead of producing thirty interruptions.
EXPECT_SCREEN=${WFSCREEN:-DELL U2417H}
check_screen() {
    local log=$1
    local line
    line=$(grep -m1 "MIXXX_WF_WINDOW=" "$log" 2>/dev/null)
    if [ -z "$line" ]; then
        echo "ERROR: the run did not report a window at all, stopping." >&2
        return 1
    fi
    echo "$line" | tee -a "$OUT/provenance.txt"
    case "$line" in
        *"screen=$EXPECT_SCREEN"*) return 0 ;;
        *)
            echo "ERROR: the window is not on '$EXPECT_SCREEN', stopping the whole series." >&2
            return 1
            ;;
    esac
}
first_run_checked=0

# vsync off, otherwise both types sit at the refresh rate and the difference
# disappears into the headroom.
sed -i '' "s/^VSync .*/VSync 0/" "$PROFILE/mixxx.cfg"

for height in "${HEIGHTS[@]}"; do
    sed -i '' "s/^stackedWaveforms_splitSize .*/stackedWaveforms_splitSize $height/" \
        "$PROFILE/mixxx.cfg"
    for repeat in $(seq 1 "$REPEATS"); do
        for radius in "${RADII[@]}"; do
            label="h${height%%,*}-r$radius-$repeat"
            echo "=== $label"
            SETTLE=${SETTLE:-35} WFPROFILE="$PROFILE" \
                "$ROOT/tools/wfshot-bg.sh" "perf-$label" 30 \
                MIXXX_WF_COLOR_SMOOTH_BINS=$radius >/dev/null 2>&1
            cp "/tmp/wfshot/perf-$label/mixxx.log" "$OUT/$label.log" 2>/dev/null ||
                cp "/tmp/wfshot/perf-$label/stdout.txt" "$OUT/$label.log" 2>/dev/null
            if [ "$first_run_checked" -eq 0 ]; then
                check_screen "$OUT/$label.log" || exit 1
                first_run_checked=1
            fi
        done
        # the stock RGB type as the reference point of every repeat
        label="h${height%%,*}-stock-$repeat"
        echo "=== $label"
        SETTLE=${SETTLE:-35} WFPROFILE="$PROFILE" \
            "$ROOT/tools/wfshot-bg.sh" "perf-$label" 12 >/dev/null 2>&1
        cp "/tmp/wfshot/perf-$label/stdout.txt" "$OUT/$label.log" 2>/dev/null
    done
done

sha_after=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
echo "binary sha256 after:  $sha_after" | tee -a "$OUT/provenance.txt"
if [ "$sha_before" != "$sha_after" ]; then
    echo "WARNING: the binary changed during the series, the WHOLE series is invalid" |
        tee -a "$OUT/provenance.txt"
fi
echo "logs: $OUT"
