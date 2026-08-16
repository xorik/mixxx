#!/bin/bash
# Print the display layout in the coordinate system that -g of run.sh expects.
#
# The numbers come from Qt itself (BENCHSCREENS lines that every run logs), not
# from system_profiler, because only Qt's virtual-desktop coordinates can be fed
# back into MIXXX_BENCH_GEOMETRY. Re-check this whenever the user re-arranges
# monitors: a stale origin would park the benchmark window on the screen the
# user is working on.
#
# Usage: tools/bench/screens.sh [run_dir]     (default: the most recent run)
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RESULTS="${BENCH_RESULTS:-$(dirname "$ROOT")/bench-results}"
DIR="${1:-$(ls -dt "$RESULTS"/*/ 2>/dev/null | head -1)}"

[ -n "${DIR:-}" ] && [ -d "$DIR" ] || {
    echo "No run to read the layout from. Do one run first, e.g." >&2
    echo "  tools/bench/run.sh -d 5 -s 20 -l layout" >&2
    exit 1
}

echo "layout as seen by Qt in $DIR:"
grep -h BENCHSCREENS "$DIR/mixxx.log" "$DIR/stdout.txt" 2>/dev/null | sed 's/.*BENCHSCREENS /  /' | sort -u
echo
echo "window of that run:"
grep -h BENCHSCREEN\  "$DIR/mixxx.log" "$DIR/stdout.txt" 2>/dev/null | sed 's/.*BENCHSCREEN /  /' | sort -u
echo
echo "Park the window with e.g.  tools/bench/run.sh -g 1400x900+<X>+<Y>"
echo "where X,Y are inside the geometry of the screen you picked."
