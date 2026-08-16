#!/bin/bash
# Waveform benchmark harness for Mixxx (macOS) — ONE run of ONE configuration.
#
# ------------------------------------------------------------------ contract
# The user works on this Mac while measurements run. A run that the user can
# notice is a failed run. Therefore this script, by construction:
#   * never brings Mixxx to the front and never takes keyboard focus
#     (QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1, see FOCUS below);
#   * never produces sound (MIXXX_BENCH_NO_AUDIO=1, verified in the log);
#   * never creates, moves, resizes or closes any window other than Mixxx's own;
#   * never sleeps the display, never locks the session, never changes system
#     settings (volume, display arrangement, power mode);
#   * never touches the user's real Mixxx profile — it runs against a separate
#     --settings-path, and restores that profile's mixxx.cfg after the run;
#   * refuses to start if another Mixxx is already running (one at a time).
#
# FOCUS: build/mixxx is a bare Mach-O, not an .app bundle. A bare binary starts
# as a background-only process; it is Qt that promotes it to a foreground app
# (TransformProcessType) and thereby steals focus. Two settings together fix
# that, and both are needed:
#   QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 stops Qt's promotion, but
#     a background-only process cannot put a window on screen at all: measured,
#     it starts, loads the skin, and then renders exactly zero frames.
#   MIXXX_BENCH_BACKGROUND=1 (src/util/benchmac.mm) then moves the process to the
#     accessory activation policy: windows are displayed and rendered, the
#     process still never activates, never enters the Dock and never becomes key.
# No .app wrapper and no `open -g` are needed. The run records the frontmost
# application twice a second and the summary fails the run if it ever changed.
#
# SCREEN: -g WxH+X+Y parks the window on a display the user is not working on,
# using nothing but Mixxx's own geometry - no window is ever moved by the script.
# The screen it landed on and that screen's refresh rate are logged (BENCHSCREEN)
# and carried into the summary, because the refresh rate is the ceiling for every
# frame-rate figure of the run.
#
# Usage:
#   tools/bench/run.sh [-d measure_s] [-s settle_s] [-l label] [-o outdir]
#                      [-g WxH+X+Y]             (park the window on a given screen)
#                      [-c key=value]...        (patch profile mixxx.cfg)
#                      [-e VAR=value]...        (extra env for Mixxx)
#                      [-t track]...            (override the track set)
#                      [-- extra mixxx args]
#
# Example:
#   tools/bench/run.sh -d 30 -l base
#   tools/bench/run.sh -d 30 -l noswap -e MIXXX_BENCH_SWAP_INTERVAL=0 \
#                      -c FrameRate=120 -c WaveformType=16

set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BENCH_BIN:-$ROOT/build/mixxx}"
# The test profile lives BESIDE the repository, not in /tmp: /tmp is cleared by
# macOS and the profile is 1.7 GB with an analysed library of 1788 tracks, whose
# loss costs hours of re-analysis. Derived from the repository location so the
# next move needs no edit here. See "The test profile" in README.md.
PROFILE="${BENCH_PROFILE:-$(dirname "$ROOT")/bench-profile}"
# Results live beside the repository too, for the same reason as the profile:
# /tmp is cleared by macOS, and a clean-up in the middle of a 12-minute series
# would take the whole series with it, silently.
RESULTS="${BENCH_RESULTS:-$(dirname "$ROOT")/bench-results}"

MEASURE=20
SETTLE=20
LABEL=run
OUTDIR=""
# Where the window is parked. There is no default on purpose: the origins in
# Qt's virtual desktop change whenever a monitor is plugged or unplugged. Every
# run logs the layout as BENCHSCREENS; read it with tools/bench/screens.sh.
GEOMETRY="${BENCH_GEOMETRY:-}"
declare -a CFG_SET=() ENV_SET=() TRACKS=() EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -d) MEASURE=$2; shift 2 ;;
        -s) SETTLE=$2; shift 2 ;;
        -l) LABEL=$2; shift 2 ;;
        -o) OUTDIR=$2; shift 2 ;;
        -g) GEOMETRY=$2; shift 2 ;;
        -c) CFG_SET+=("$2"); shift 2 ;;
        -e) ENV_SET+=("$2"); shift 2 ;;
        -t) TRACKS+=("$2"); shift 2 ;;
        --) shift; EXTRA_ARGS=("$@"); break ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ ${#TRACKS[@]} -eq 0 ]; then
    TRACKS=(
        "/Users/andrey/Music/set/chillstep/Tritonal ft. Cristina Soto - Still With Me (Seven Lions Remix).mp3"
        "/Users/andrey/Music/set/chillstep/MitiS - Born.mp3"
    )
fi

[ -n "$OUTDIR" ] || OUTDIR="$RESULTS/$(date +%Y%m%d-%H%M%S)-$LABEL"
mkdir -p "$OUTDIR" || exit 1

log() { printf '%s\n' "$*"; }
die() { printf 'ABORT: %s\n' "$*" >&2; echo "$*" >"$OUTDIR/aborted.txt"; exit 1; }

# ------------------------------------------------------------------ preflight
[ -x "$BIN" ] || die "binary not found: $BIN"
case "$(file -b "$BIN")" in
    *Mach-O*) : ;;
    *) die "not a Mach-O executable: $BIN" ;;
esac
# An empty profile would produce a run against an empty library: it would look
# entirely valid - frames, telemetry, no errors - and measure nothing of what we
# care about. So refuse, loudly, and never create one on the fly.
[ -d "$PROFILE" ] || die "the test profile does not exist: $PROFILE
It is not created automatically on purpose: an empty profile means an empty
library, and a run against it looks perfectly valid while measuring nothing.
Re-create it as an APFS clone of the real profile - see 'The test profile' in
tools/bench/README.md - or point BENCH_PROFILE at an existing one."
[ -f "$PROFILE/mixxx.cfg" ] || die "profile has no mixxx.cfg: $PROFILE
Looks like a directory that is not a Mixxx profile. See 'The test profile' in
tools/bench/README.md."
[ -f "$PROFILE/mixxxdb.sqlite" ] || die "profile has no library database: $PROFILE
A profile without mixxxdb.sqlite has no tracks, and a run against an empty
library measures nothing while looking valid. See tools/bench/README.md."
# Guard against ever pointing the harness at the user's real profile.
case "$PROFILE" in
    "$HOME"/Library/*) die "refusing to run against the real profile: $PROFILE" ;;
esac

# One Mixxx at a time, and that means ANY Mixxx, not just ours: a neighbouring
# task builds its own binary and runs it on the same machine, and two GL
# applications on one screen influence each other through WindowServer. Match on
# the executable name, not on the command line - the agent processes themselves
# have "mixxx" in their arguments and would produce a false positive.
running=$(ps -Ao pid=,comm= | awk '{p=$1; n=$NF; sub(/.*\//, "", n);
    if (tolower(n) ~ /^mixxx/) printf "%s(%s) ", p, n}')
[ -z "$running" ] || die "a Mixxx is already running: $running
Refusing to start a second one. If that is a leftover of an earlier run, kill it;
if it belongs to another task, wait for it - measuring through someone else's
compositor load is not a measurement."

# Where the window goes must always be a deliberate decision: without it Mixxx
# restores whatever position its profile happens to hold, which once landed the
# window in the middle of the display the user was working on.
[ -n "$GEOMETRY" ] || die "no -g WxH+X+Y given: refusing to let the window land wherever
the profile last left it. Read the CURRENT layout from the BENCHSCREENS lines of
the last run - tools/bench/screens.sh - and never reuse remembered coordinates:
the origins change whenever a monitor is plugged or unplugged, and stale ones
either push the window off the desktop or let Qt place it somewhere else."

frontname() { # -> name of the frontmost application, or "?"
    local asn name
    asn=$(lsappinfo front 2>/dev/null) || return
    [ -n "$asn" ] || { echo '?'; return; }
    name=$(lsappinfo info -only name "$asn" 2>/dev/null | sed -n 's/.*"LSDisplayName"="\(.*\)"/\1/p')
    echo "${name:-?}"
}

# Track count of the profile actually in use. A run against the wrong profile -
# an old copy, an empty library - otherwise looks entirely normal, and the only
# trace would be someone's memory of which path was passed.
PROFILE_TRACKS=$(sqlite3 "$PROFILE/mixxxdb.sqlite" "select count(*) from library;" 2>/dev/null)
[ -n "$PROFILE_TRACKS" ] || PROFILE_TRACKS="unknown (sqlite3 unavailable)"

FRONT_BEFORE=$(frontname)

# Preflight says what it checked, out loud. A check that prints nothing when it
# passes cannot be told apart from a check that never ran - which has bitten this
# project four times, from black screenshots to an empty "git status | grep".
log "preflight OK"
log "  binary   $BIN"
log "           $(shasum -a 256 "$BIN" | cut -c1-16)  $(stat -f %Sm -t %FT%T "$BIN")"
log "  profile  $PROFILE"
log "           $PROFILE_TRACKS tracks in the library"
log "  window   $GEOMETRY"
log "  no other Mixxx running, front application is $FRONT_BEFORE"

# ------------------------------------------------------- environment snapshot
{
    echo "date=$(date -Iseconds)"
    echo "label=$LABEL"
    echo "bin=$BIN"
    echo "bin_mtime=$(stat -f %Sm -t %FT%T "$BIN")"
    echo "bin_sha256=$(shasum -a 256 "$BIN" | cut -d' ' -f1)"
    echo "profile=$PROFILE"
    echo "profile_tracks=$PROFILE_TRACKS"
    echo "git_head=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)"
    echo "git_dirty_sha256=$(git -C "$ROOT" diff HEAD | shasum -a 256 | cut -d' ' -f1)"
    echo "measure_s=$MEASURE"
    echo "geometry=$GEOMETRY"
    echo "settle_s=$SETTLE"
    echo "front_before=$FRONT_BEFORE"
    echo "power_before=$(pmset -g batt | head -1 | sed 's/.*drawing from //;s/'"'"'//g')"
    echo "uptime=$(uptime | sed 's/^ *//')"
    # Provenance for the "was anybody else using this machine" question. With a
    # single screen there is nowhere to put the window except where the user is
    # working, so the run stays usable but must carry that caveat in the data
    # rather than in somebody's memory.
    echo "screens_system=$(system_profiler SPDisplaysDataType 2>/dev/null |
        grep -c 'Online: Yes')"
    echo "gui_apps_before=$(lsappinfo list 2>/dev/null | grep -cE '^[0-9]+\) ')"
    for kv in ${CFG_SET+"${CFG_SET[@]}"}; do echo "cfg=$kv"; done
    for kv in ${ENV_SET+"${ENV_SET[@]}"}; do echo "env=$kv"; done
    for t in "${TRACKS[@]}"; do echo "track=$t"; done
    for a in ${EXTRA_ARGS+"${EXTRA_ARGS[@]}"}; do echo "arg=$a"; done
} >"$OUTDIR/meta.txt"
system_profiler SPDisplaysDataType 2>/dev/null >"$OUTDIR/displays.txt"

# ------------------------------------------------------------ profile patching
cp "$PROFILE/mixxx.cfg" "$OUTDIR/mixxx.cfg.orig"
restore_cfg() { cp "$OUTDIR/mixxx.cfg.orig" "$PROFILE/mixxx.cfg" 2>/dev/null; }
set_cfg() {
    local k=$1 v=$2
    if grep -qE "^$k " "$PROFILE/mixxx.cfg"; then
        sed -i '' "s|^$k .*|$k $v|" "$PROFILE/mixxx.cfg"
    else
        printf '%s %s\n' "$k" "$v" >>"$PROFILE/mixxx.cfg"
    fi
}
for kv in ${CFG_SET+"${CFG_SET[@]}"}; do set_cfg "${kv%%=*}" "${kv#*=}"; done

# A profile whose [Config] Version is older than 2.6.0 makes Mixxx run its
# upgrade path on EVERY start, and that path silently rewrites WaveformType:
# a value it does not recognise is replaced by plain RGB. The run then looks
# perfect and measures a different renderer. Confirmed on the user's own
# profile: Version 2.5.4, WaveformType 17 (not a value of the enum at all),
# telemetry reporting type=12.
cfg_version=$(awk '/^\[Config\]/{f=1;next} /^\[/{f=0} f&&/^Version /{print $2; exit}' \
    "$PROFILE/mixxx.cfg")
bin_version=$("$BIN" --version 2>/dev/null | sed -n 's/^Mixxx \(.*\)/\1/p' | head -1)
echo "cfg_version_before=${cfg_version:-none}" >>"$OUTDIR/meta.txt"
case "$cfg_version" in
    2.[0-5].*|2.[0-5]|"")
        [ -n "$bin_version" ] || die "cannot read the version of $BIN"
        log "profile Version '${cfg_version:-none}' predates 2.6.0 and would trigger the"
        log "upgrade path that rewrites WaveformType; setting it to $bin_version for this run"
        set_cfg Version "$bin_version"
        ;;
esac
echo "cfg_version_used=$(awk '/^\[Config\]/{f=1;next} /^\[/{f=0} f&&/^Version /{print $2; exit}' \
    "$PROFILE/mixxx.cfg")" >>"$OUTDIR/meta.txt"

cp "$PROFILE/mixxx.cfg" "$OUTDIR/mixxx.cfg.used"

rm -f "$PROFILE"/mixxx.log*

# ---------------------------------------------------------------------- launch
MIXXX_PID=""
MON_PID=""
cleanup() {
    [ -n "$MON_PID" ] && kill "$MON_PID" 2>/dev/null
    if [ -n "$MIXXX_PID" ] && kill -0 "$MIXXX_PID" 2>/dev/null; then
        kill -TERM "$MIXXX_PID" 2>/dev/null
        for _ in $(seq 1 40); do kill -0 "$MIXXX_PID" 2>/dev/null || break; sleep 0.25; done
        kill -KILL "$MIXXX_PID" 2>/dev/null
    fi
    restore_cfg
}
trap 'cleanup; exit 130' INT TERM

env_args=(
    "QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1"  # Qt must not promote us
    "MIXXX_BENCH_BACKGROUND=1"                           # ... we go accessory instead
    "MIXXX_BENCH_NO_AUDIO=1"                             # never make a sound
)
[ -n "$GEOMETRY" ] && env_args+=("MIXXX_BENCH_GEOMETRY=$GEOMETRY")
for kv in ${ENV_SET+"${ENV_SET[@]}"}; do env_args+=("$kv"); done

# The frontmost-application sampler runs for the whole life of the process,
# at 2 Hz, so a focus theft lasting a single frame still shows up.
(
    while :; do
        printf '%s\t%s\t%s\n' "$(date +%s.%N)" "$(frontname)" \
            "$(pmset -g batt | head -1 | grep -o "AC Power\|Battery Power")"
        sleep 0.5
    done
) >"$OUTDIR/frontapp.tsv" 2>/dev/null &
MON_PID=$!

log "launching: $LABEL  (settle ${SETTLE}s, measure ${MEASURE}s)"
env "${env_args[@]}" "$BIN" \
    --developer --log-flush-level debug \
    --settings-path "$PROFILE" \
    ${EXTRA_ARGS+"${EXTRA_ARGS[@]}"} \
    "${TRACKS[@]}" >"$OUTDIR/stdout.txt" 2>&1 &
MIXXX_PID=$!
echo "$(date +%s.%N) launched pid=$MIXXX_PID" >"$OUTDIR/phases.txt"

sleep "$SETTLE"
kill -0 "$MIXXX_PID" 2>/dev/null || {
    tail -20 "$OUTDIR/stdout.txt" >&2
    cleanup
    die "Mixxx died during startup"
}

echo "$(date +%s.%N) measure_start" >>"$OUTDIR/phases.txt"
sleep "$MEASURE"
echo "$(date +%s.%N) measure_end" >>"$OUTDIR/phases.txt"

# --------------------------------------------------------------------- collect
kill "$MON_PID" 2>/dev/null; MON_PID=""
kill -TERM "$MIXXX_PID" 2>/dev/null
for _ in $(seq 1 40); do kill -0 "$MIXXX_PID" 2>/dev/null || break; sleep 0.25; done
kill -KILL "$MIXXX_PID" 2>/dev/null
MIXXX_PID=""
restore_cfg

cp "$PROFILE/mixxx.log" "$OUTDIR/mixxx.log" 2>/dev/null
grep -h WAVEPERF "$OUTDIR/mixxx.log" "$OUTDIR/stdout.txt" 2>/dev/null | sort -u >"$OUTDIR/waveperf.txt"
echo "front_after=$(frontname)" >>"$OUTDIR/meta.txt"
echo "gui_apps_after=$(lsappinfo list 2>/dev/null | grep -cE '^[0-9]+\) ')" >>"$OUTDIR/meta.txt"
# Which applications were up, so an outlier can be traced to something concrete
# instead of being explained away.
lsappinfo list 2>/dev/null | sed -nE 's/^[0-9]+\) "([^"]*)".*/\1/p' | sort >"$OUTDIR/apps.txt"

python3 "$ROOT/tools/bench/summarize.py" "$OUTDIR"
