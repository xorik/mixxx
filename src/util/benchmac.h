#pragma once

// TEMPORARY BENCHMARK HOOK - not part of any product change.
//
// build/mixxx is a bare Mach-O, not an .app bundle, so macOS starts it as a
// background-only process and it is Qt that promotes it to a foreground
// application (and thereby steals the user's keyboard focus).
// QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 suppresses that promotion,
// but a background-only process cannot put windows on the screen at all, so
// nothing is ever rendered and there is nothing to measure.
//
// With MIXXX_BENCH_BACKGROUND=1 this hook moves the process to the accessory
// activation policy instead: its windows are displayed and rendered normally,
// while the process never activates, never appears in the Dock and never takes
// keyboard focus - which is what lets the waveform benchmark run while the user
// keeps working. See tools/bench/run.sh.

namespace mixxx {
void benchApplyMacActivationPolicy();
} // namespace mixxx
