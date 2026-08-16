# Waveform benchmark harness

Measures Mixxx waveform rendering while the user keeps working on the same Mac.
A run that the user could notice is a failed run, so every guarantee below is
either enforced by construction or checked afterwards and reported as a failure.

The older `tools/waveperf*.sh` scripts from the first session are superseded by
this directory: they launch Mixxx as a foreground application (it takes the
user's focus), some of them pass switches the binary does not read, and their
screenshot check silently produced black images.

## Files

| file | purpose |
| --- | --- |
| `build.sh` | builds the `mixxx` target and refreshes `build/mixxx`, the **bare** binary that is measured |
| `run.sh` | one run of one configuration: launch, monitor, kill, collect |
| `summarize.py` | metrics **and the validity verdict** for one run directory |
| `screens.sh` | display layout in the coordinates `run.sh -g` expects |

## How the "no focus, no sound" launch works

`build/mixxx` is a bare Mach-O, not an `.app` bundle, so `open -g` does not
apply to it. Two settings together give an invisible launch, and both are
needed:

* `QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1` stops Qt from promoting
  the process to a foreground application (that promotion is what takes the
  keyboard focus). On its own this is useless for us: a background-only process
  cannot put a window on the screen, and a run measured this way starts,
  loads the skin, and renders **zero** frames.
* `MIXXX_BENCH_BACKGROUND=1` (`src/util/benchmac.mm`) then moves the process to
  the *accessory* activation policy. Windows are displayed and rendered
  normally, while the process never activates, never appears in the Dock and
  never becomes key. The hook logs
  `BENCHHIT MIXXX_BENCH_BACKGROUND=1 accessoryPolicyApplied=true isActive=false`,
  and a run without that line is rejected.

Sound: `MIXXX_BENCH_NO_AUDIO=1` starts Mixxx with no audio output at all and
suppresses the blocking "no output device" dialog. The summary fails a run in
which any real audio device was opened.

The window is parked on a display the user is not working on with
`-g WxH+X+Y` (`MIXXX_BENCH_GEOMETRY`). Mixxx positions its own window; no window
is ever moved, resized or closed by the harness, and no system setting is
touched. **`-g` is mandatory**: without it Mixxx restores whatever position its
profile happens to hold, which once put the window in the middle of the display
the user was working on.

The geometry is applied **before the window is first shown**, not after startup.
This is not cosmetic: measured, the vsync PLL locks to the refresh rate of the
screen the window starts on and never re-locks when the window moves, so a run
that starts on a 60 Hz screen stays capped at 60 fps even after the window has
been parked on the 120 Hz one. Placing it late also flashes the window across
the user's main display for two seconds at every launch.

**Never reuse remembered coordinates.** The layout changes whenever a monitor is
plugged or unplugged - it already did once during this work, from three screens
(built-in at -1512,107 next to a 60 Hz primary) to a single built-in one, which
moves its origin. Read the current layout from the `BENCHSCREENS` lines of the
last run (`screens.sh`) and derive the coordinates from those. Stale coordinates
either push the window off the desktop or let Qt place it wherever it likes.

## Usage

```sh
tools/bench/build.sh                        # build + refresh build/mixxx
tools/bench/run.sh -d 30 -s 25 -l base -g 1400x900+-1450+150
tools/bench/run.sh -d 30 -l noswap -e MIXXX_BENCH_SWAP_INTERVAL=0
tools/bench/screens.sh                      # where the screens are, per Qt
```

Every run writes `/tmp/waveperf/<timestamp>-<label>/` containing `meta.txt`
(binary hash, git head, hash of the working-tree diff, flags, tracks),
`mixxx.log`, `stdout.txt`, `waveperf.txt`, `frontapp.tsv` (frontmost app and
power source at 2 Hz), `phases.txt` and `summary.json`.

## What makes a run valid

`summarize.py` prints `VALID`, `SUSPECT` or `INVALID`. It fails a run when

* the frontmost application changed at any point (the user did something, or —
  worse — Mixxx came forward);
* the power source changed, or the run was on battery (warning);
* a real audio device was opened;
* there is no WAVEPERF telemetry, fewer telemetry seconds than the measured
  window, or a frame rate of zero (an occluded or unmapped window);
* the accessory-policy hook did not run, or AppKit refused the policy. Being
  *already* accessory (a bundle with `LSUIElement`) is not a failure and is
  reported separately - `setActivationPolicy:` returns NO in that case too;
* a `MIXXX_BENCH_*` switch was requested that this binary does not even read —
  i.e. the change under test is not in the build.

It warns when a switch is read by the binary but logs no `BENCHHIT`, so that
"the patch was in the build" is never confused with "the patch was executed".

The same "requested versus effective" rule is applied to the configuration, not
just to environment switches. Mixxx can silently substitute what it renders:
with a `[Config] Version` older than 2.6.0 it runs its upgrade path on every
start, and that path replaces a `WaveformType` it does not recognise with plain
RGB. Nothing looks wrong afterwards - frames flow, no error is logged, and the
telemetry is perfect for the wrong renderer. (Confirmed on the user's own
profile: `Version 2.5.4`, `WaveformType 17` - not a value of the enum at all -
and telemetry reporting `type=12`, i.e. RGB.) Therefore:

* `run.sh` reads `[Config] Version` of the test profile and, if it predates
  2.6.0, raises it to the binary's own version for the run, recording both in
  `meta.txt`;
* every WAVEPERF line carries the *effective* `type=` and `options=`, and
  `summarize.py` fails the run if either differs from the `WaveformType` /
  `waveform_options` the profile was started with. `waveform_options` decides
  between the textured and the geometric signal renderer, i.e. between two
  different pieces of code.

The measured frame rate is compared with the refresh rate of the screen the
window actually landed on (`BENCHSCREEN`), because that refresh rate is the
ceiling for every frame-rate figure and runs on different screens are not
comparable.

A run must also prove that the vsync PLL **finished locking, and to the right
rate**. Without that proof the numbers describe the wrong synchronisation while
looking perfectly normal: if the plausibility check in the PLL initialisation
keeps rejecting samples, the loop never leaves initialisation and the period
stays at the startup default. That is not hypothetical on this machine - the
built-in ProMotion panel reports its current mode while the real interval floats
between 24 and 120 Hz, and the system lowers the rate when the window is
inactive, which is exactly how a benchmark window runs. So `summarize.py` fails
a run when there is no `PLL locked to` line, or when the locked period differs
from the interval of the screen the window is on by more than 5%. Applied to the
earlier acceptance run, this check catches it: locked to 16667 us on a 120 Hz
screen.

## Switches available to a run (`-e`)

| switch | effect |
| --- | --- |
| `MIXXX_BENCH_PLL_CLAMP=0` | disables the PLL drift clamp, so the drift limit can be A/B-ed inside one binary; logs `BENCHHIT ... driftClamp=on\|off` |
| `MIXXX_BENCH_PLL_WRAP=sym` | folds the PLL phase error back to the nearest frame for BOTH signs instead of only positive ones (default `asym` = upstream); logs `BENCHHIT ... phaseErrorWrapped=...` |
| `MIXXX_BENCH_PIXELPROBE=<n>` | pixel readback every *n* frames (see below) |
| `MIXXX_BENCH_SWAP_INTERVAL`, `MIXXX_BENCH_NO_DONECURRENT`, `MIXXX_BENCH_CORE_PROFILE`, `MIXXX_BENCH_SWAP_BEHAVIOR`, `MIXXX_BENCH_AUTOPLAY` | pre-existing experiment switches |

Two telemetry fields are easy to confuse, and one of them used to be named so
badly that it invited wrong conclusions:

* `syncIntervalUs` (was `pllDeltaUs`) - the interval that was **asked for**,
  `1e6 / FrameRate`. It reads 8333 even while the loop is running at 16667.
* `pllPeriodUs` - the period the PLL has actually **settled on**. This is the
  one to look at for drift.

## Open question: the drift clamp is a palliative

`MIXXX_BENCH_PLL_CLAMP` holds the PLL period near the interval the display
reports. That stops the period from wandering, but it does not explain why it
wanders. The current candidate for the root cause is the **asymmetric phase-error
fold** in `updatePLL()`: upstream folds the error back to the nearest frame only
when it is positive, so a negative error of any size reaches the loop filter in
full and the filter moves the period with it. The user's log shows large negative
phase errors (-3760, -7561, -4975 us) together with a period drifting downwards
(16682 -> 16558 us against a true 16667 us).

This is a hypothesis, not a result: the sign of the phase error also depends on
accumulated phase offset. `MIXXX_BENCH_PLL_WRAP=sym` exists to test it
(`pll-symmetric-wrap.patch`).

**Do not treat the drift question as closed if the clamp measures well.** A
clamp that hides a bug and a fix that removes it look the same in a frame-rate
column.

## Proof that pixels were really drawn

Screenshots cannot provide it: `screencapture` returns an all-black image unless
the invoking process has the Screen Recording permission (verified: a 3024x1964
capture from an earlier session has a mean pixel value of exactly 0), and
granting that permission means a system dialog in the user's face. Worst kind of
check - it looks like it works and always passes.

Instead, `MIXXX_BENCH_PIXELPROBE=<n>` reads the frame back inside the process,
which needs no permission:

* off unless asked for - a normal build never issues an extra `glReadPixels`;
* every *n* rendered frames (`1` = the default of about one second), after all
  waveforms of the frame are rendered and before the swap, for **every visible
  deck**;
* logs, per deck, `BENCHPIXELS group=... mean=... sd=... checksum=...`. The mean
  alone proves nothing - an empty area can share the background's mean - the
  spread is what separates a working renderer from a broken one.

`glReadPixels` stalls the CPU until the GPU has caught up, so **frame times from
a probing run are not comparable with anything**. Use it to validate a
configuration, then measure without it.
