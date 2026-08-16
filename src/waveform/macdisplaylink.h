#pragma once

#include <chrono>
#include <optional>

class QWindow;

namespace mixxx {

/// Thin C++ wrapper around a `CADisplayLink` attached to the `NSView` that
/// backs a `QWindow` (macOS 14+, see `-[NSView displayLinkWithTarget:selector:]`).
///
/// This class does *not* drive rendering. Rendering is driven by
/// `QWindow::requestUpdate()`, which the Qt Cocoa platform plugin already
/// serves from its own `CVDisplayLink`. The only purpose of this class is to
/// obtain the *system provided* presentation timestamp of the next frame
/// (`CADisplayLink.targetTimestamp`) and the actual refresh interval
/// (`CADisplayLink.duration`), so that the renderers do not have to estimate
/// them.
///
/// All methods must be called from the main (GUI) thread; the display link
/// callback is scheduled on the main run loop.
///
/// Only implemented on macOS. On every other platform this class is not
/// compiled at all (see the `MIXXX_HAS_MAC_DISPLAY_LINK` guard in
/// displaylinkframedriver.h).
class MacDisplayLink {
  public:
    /// Returns true if the running macOS version provides CADisplayLink for
    /// AppKit views (macOS 14.0+) and Mixxx was compiled against an SDK that
    /// knows about it.
    static bool isAvailable();

    /// Creates and starts a display link for the display `pWindow` is on.
    /// Check isValid() afterwards; construction fails silently (with a
    /// warning) if CADisplayLink is unavailable.
    explicit MacDisplayLink(QWindow* pWindow);
    ~MacDisplayLink();

    MacDisplayLink(const MacDisplayLink&) = delete;
    MacDisplayLink& operator=(const MacDisplayLink&) = delete;

    bool isValid() const {
        return m_pImpl != nullptr;
    }

    /// Time from now until the next presentation deadline reported by the
    /// display link (`targetTimestamp`). Returns `std::nullopt` if no
    /// timestamp has been received yet or if the cached timestamp is
    /// implausibly old.
    ///
    /// The value is derived from the most recent callback and extrapolated by
    /// whole refresh intervals, so it stays correct even if the CADisplayLink
    /// callback and Qt's UpdateRequest delivery are ordered differently within
    /// the same run loop iteration.
    std::optional<std::chrono::microseconds> timeToNextPresentation() const;

    /// The refresh interval reported by the display link (`duration`).
    std::optional<std::chrono::microseconds> refreshInterval() const;

  private:
    /// Strong (bridge-retained) reference to the Objective-C helper object
    /// that owns the CADisplayLink. void* to keep this header ObjC free.
    void* m_pImpl;
};

} // namespace mixxx
