#include "util/benchmac.h"

#import <AppKit/NSApplication.h>

#include <QtDebug>
#include <QtGlobal>

namespace mixxx {

void benchApplyMacActivationPolicy() {
    if (qEnvironmentVariableIsEmpty("MIXXX_BENCH_BACKGROUND")) {
        return;
    }
    // NSApplicationActivationPolicyAccessory: the process may show windows, but
    // it is not activated by showing them and it cannot become the frontmost
    // application on its own.
    //
    // Report the state, not the call. When the process already runs from a
    // bundle marked LSUIElement it is accessory from the start, and setting the
    // policy again returns NO - which looks exactly like a failure while the
    // mode is in fact active. What matters is where we end up.
    //
    // This wording is shared with MIX-11 (bench-hooks-mix11.patch) on purpose:
    // one hook, one log format, one parser.
    const auto policyName = [](NSApplicationActivationPolicy policy) {
        switch (policy) {
        case NSApplicationActivationPolicyRegular:
            return "regular";
        case NSApplicationActivationPolicyAccessory:
            return "accessory";
        case NSApplicationActivationPolicyProhibited:
            return "prohibited";
        }
        return "unknown";
    };
    const NSApplicationActivationPolicy before = [NSApp activationPolicy];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    const NSApplicationActivationPolicy after = [NSApp activationPolicy];
    const bool applied = after == NSApplicationActivationPolicyAccessory;
    qDebug().nospace() << "BENCHHIT MIXXX_BENCH_BACKGROUND=1 policyWas=" << policyName(before)
                       << " policyNow=" << policyName(after) << " applied=" << applied
                       << " reason="
                       << (before == NSApplicationActivationPolicyAccessory ? "already-accessory"
                                                                           : "set-by-hook")
                       << " isActive=" << static_cast<bool>([NSApp isActive]);
}

} // namespace mixxx
