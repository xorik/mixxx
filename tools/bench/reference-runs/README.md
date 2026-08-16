# Reference runs

Kept because a check was written against them, not because the numbers matter.

## 20260814-222500-acceptance

The harness acceptance run: window on the built-in display, focus sampled
twice a second and never once on Mixxx, no audio device opened, 70 telemetry
lines.

It is here because the summary check that compares the locked PLL period
against the refresh rate of the screen the window is actually on was written
afterwards, and this run is what it was validated against: re-scoring it turns
the original VALID into

    INVALID - the PLL locked to 16667 us but the screen it is on runs at
    120 Hz (8333 us)

which is correct. The lock happened while the window was still on the 60 Hz
main display and was never revisited after the window moved. If that check is
ever changed, re-score this directory first: it should stay INVALID for that
reason and no other.
