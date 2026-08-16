#include "waveform/macdisplaylink.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <QDebug>
#include <QWindow>
#include <QtGlobal>
#include <cmath>

// CADisplayLink is available for AppKit views since macOS 14.0. Only compile
// the implementation if the SDK we build against knows about it; the runtime
// check below (@available) takes care of older systems.
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
#define MIXXX_CADISPLAYLINK_SDK_AVAILABLE 1
#endif

#ifdef MIXXX_CADISPLAYLINK_SDK_AVAILABLE

/// Objective-C target of the CADisplayLink. The callback runs on the main run
/// loop, and the values are only read from the main thread, so plain doubles
/// (no atomics, no locking) are sufficient.
API_AVAILABLE(macos(14.0))
@interface MixxxDisplayLinkTarget : NSObject {
  @public
    double m_timestamp;
    double m_targetTimestamp;
    double m_duration;
}
@property(nonatomic, strong) CADisplayLink* displayLink;
- (instancetype)initWithView:(NSView*)view;
- (void)invalidate;
@end

@implementation MixxxDisplayLinkTarget

- (instancetype)initWithView:(NSView*)view {
    self = [super init];
    if (self) {
        m_timestamp = 0.0;
        m_targetTimestamp = 0.0;
        m_duration = 0.0;
        // The display link created from a view automatically follows the
        // display the view is shown on, including a changing refresh rate
        // (ProMotion).
        _displayLink = [view displayLinkWithTarget:self selector:@selector(onDisplayLink:)];
        [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)onDisplayLink:(CADisplayLink*)sender {
    m_timestamp = sender.timestamp;
    m_targetTimestamp = sender.targetTimestamp;
    m_duration = sender.duration;
}

- (void)invalidate {
    [_displayLink invalidate];
    _displayLink = nil;
}

@end

#endif // MIXXX_CADISPLAYLINK_SDK_AVAILABLE

namespace mixxx {

// static
bool MacDisplayLink::isAvailable() {
#ifdef MIXXX_CADISPLAYLINK_SDK_AVAILABLE
    if (__builtin_available(macOS 14.0, *)) {
        return true;
    }
#endif
    return false;
}

MacDisplayLink::MacDisplayLink(QWindow* pWindow)
        : m_pImpl(nullptr) {
#ifdef MIXXX_CADISPLAYLINK_SDK_AVAILABLE
    if (__builtin_available(macOS 14.0, *)) {
        if (!pWindow) {
            return;
        }
        // On macOS WId is the NSView* of the window. Note that winId() creates
        // the native window if it does not exist yet. The bridge cast is
        // required because this file is compiled with ARC.
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(pWindow->winId());
        if (!view) {
            qWarning() << "MacDisplayLink: window has no NSView";
            return;
        }
        MixxxDisplayLinkTarget* pTarget =
                [[MixxxDisplayLinkTarget alloc] initWithView:view];
        if (!pTarget.displayLink) {
            qWarning() << "MacDisplayLink: failed to create CADisplayLink";
            return;
        }
        m_pImpl = (__bridge_retained void*)pTarget;
        return;
    }
#endif
    Q_UNUSED(pWindow);
    qWarning() << "MacDisplayLink: CADisplayLink requires macOS 14 or later, "
                  "falling back to the refresh rate reported by QScreen";
}

MacDisplayLink::~MacDisplayLink() {
#ifdef MIXXX_CADISPLAYLINK_SDK_AVAILABLE
    if (m_pImpl) {
        if (__builtin_available(macOS 14.0, *)) {
            MixxxDisplayLinkTarget* pTarget =
                    (__bridge_transfer MixxxDisplayLinkTarget*)m_pImpl;
            [pTarget invalidate];
        }
        m_pImpl = nullptr;
    }
#endif
}

std::optional<std::chrono::microseconds> MacDisplayLink::refreshInterval() const {
#ifdef MIXXX_CADISPLAYLINK_SDK_AVAILABLE
    if (m_pImpl) {
        if (__builtin_available(macOS 14.0, *)) {
            MixxxDisplayLinkTarget* pTarget = (__bridge MixxxDisplayLinkTarget*)m_pImpl;
            const double duration = pTarget->m_duration;
            if (duration > 0.0) {
                return std::chrono::microseconds(
                        static_cast<std::chrono::microseconds::rep>(duration * 1e6));
            }
        }
    }
#endif
    return std::nullopt;
}

std::optional<std::chrono::microseconds> MacDisplayLink::timeToNextPresentation() const {
#ifdef MIXXX_CADISPLAYLINK_SDK_AVAILABLE
    if (m_pImpl) {
        if (__builtin_available(macOS 14.0, *)) {
            MixxxDisplayLinkTarget* pTarget = (__bridge MixxxDisplayLinkTarget*)m_pImpl;
            const double target = pTarget->m_targetTimestamp;
            const double duration = pTarget->m_duration;
            if (target <= 0.0 || duration <= 0.0) {
                // No callback received yet.
                return std::nullopt;
            }
            // CACurrentMediaTime() is the same time base as the display link
            // timestamps, so the difference is meaningful without having to
            // relate the Mach time base to PerformanceTimer's steady_clock.
            double delta = target - CACurrentMediaTime();
            if (delta <= 0.0) {
                // The cached target timestamp already passed. This happens if
                // the UpdateRequest is delivered before our CADisplayLink
                // callback of the same vsync, or if the main thread was
                // blocked. Extrapolate by whole refresh intervals.
                delta += std::floor(-delta / duration + 1.0) * duration;
            }
            if (delta > 4.0 * duration) {
                // Implausible, e.g. the display link was paused. Let the
                // caller fall back to the nominal interval.
                return std::nullopt;
            }
            return std::chrono::microseconds(
                    static_cast<std::chrono::microseconds::rep>(delta * 1e6));
        }
    }
#endif
    return std::nullopt;
}

} // namespace mixxx
