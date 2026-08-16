#!/bin/bash
# Build Mixxx and refresh build/mixxx, the bare binary the harness measures.
#
# The CMake target produces "build/Mixxx alpha.app". The harness deliberately
# runs a BARE copy of that executable instead of the bundle: a bare Mach-O is a
# background-only process, which is exactly what lets it render without ever
# activating (see tools/bench/run.sh). Copying, not symlinking, is essential -
# a symlink into the .app would give the process bundle identity again.
#
# Usage: tools/bench/build.sh
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT/build"

# Re-running CMake (any CMakeLists.txt edit does) insists on BUILDENV_URL even
# though the dependency tree is already unpacked in buildenv/. Point it at the
# archive that is already there, so nothing is ever downloaded.
if [ -z "${BUILDENV_URL:-}" ]; then
    envzip=$(ls -1 "$ROOT"/buildenv/mixxx-deps-*.zip 2>/dev/null | head -1)
    [ -n "$envzip" ] || { echo "ERROR: no buildenv archive in $ROOT/buildenv" >&2; exit 1; }
    export BUILDENV_URL="file://$envzip"
fi

ninja mixxx

APP_BIN="$ROOT/build/Mixxx alpha.app/Contents/MacOS/Mixxx alpha"
[ -x "$APP_BIN" ] || { echo "ERROR: $APP_BIN not built" >&2; exit 1; }

if pgrep -f "$ROOT/build/mixxx" >/dev/null; then
    echo "ERROR: a benchmark Mixxx is still running; refusing to replace the binary" >&2
    exit 1
fi

cp "$APP_BIN" "$ROOT/build/mixxx"
echo "build/mixxx  $(stat -f %Sm -t %FT%T "$ROOT/build/mixxx")  $(shasum -a 256 "$ROOT/build/mixxx" | cut -c1-16)"
