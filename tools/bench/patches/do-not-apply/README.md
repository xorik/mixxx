# Snapshots, not recommendations

Everything in this directory reproduces the state of a binary that was
installed at some point, so that "what exactly was in that build?" can be
answered later. None of it is a suggestion to apply.

`pll-init-fix.patch` is the version that shipped in the user's
`Mixxx alpha.app`. It can stop the waveforms from rendering altogether: a
sample the plausibility check rejects returns before `m_pllPendingUpdate` is
set, and with no timeout on that check a display whose interval it never
accepts leaves the PLL initialising for ever. No lock, no frame tick, no
waveform - only the background. Reproduced on the user's machine (ProMotion,
variable refresh) and reverted there in 6635022401.

`pll-drift-clamp.patch` clamps the period against
`QGuiApplication::primaryScreen()` rather than the screen the window is on,
so on a multi-monitor setup it holds the period at the wrong display's rate.
Reverted in de243bba44.

The usable versions of both live one directory up with a `-fixed` suffix, and
even those are unverified by measurement. See ../README.md.
