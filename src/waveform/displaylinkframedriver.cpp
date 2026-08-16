#include "waveform/displaylinkframedriver.h"

#include <QDebug>
#include <QScreen>
#include <algorithm>
#include <cmath>

#include "moc_displaylinkframedriver.cpp"
#include "util/assert.h"
#include "waveform/vsyncthread.h"

#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
#include "waveform/macdisplaylink.h"
#endif

#ifdef MIXXX_USE_QOPENGL
#include "widget/openglwindow.h"
#endif

namespace {

constexpr int kDefaultRefreshIntervalMicros = 16667; // 60 Hz
// If no update request is delivered for this long we assume that the platform
// stopped serving them (e.g. because the window got occluded or the display
// link was stopped) and drive a frame ourselves, so that the GUI tick, the VU
// meters and the spinnies keep running.
constexpr int kWatchdogIntervalMs = 100;

} // anonymous namespace

DisplayLinkFrameDriver::DisplayLinkFrameDriver(
        QWindow* pWindow, VSyncThread* pVSyncThread, QObject* pParent)
        : QObject(pParent),
          m_pWindow(pWindow),
          m_pVSyncThread(pVSyncThread),
          m_running(false),
          m_updateRequestSeen(false),
          m_hasDrivenFrame(false),
          m_frameRate(60),
          m_refreshIntervalMicros(kDefaultRefreshIntervalMicros),
          m_divisor(1),
          m_skipCounter(0) {
    VERIFY_OR_DEBUG_ASSERT(pWindow) {
        return;
    }

#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
    auto pMacDisplayLink = std::make_unique<mixxx::MacDisplayLink>(pWindow);
    if (pMacDisplayLink->isValid()) {
        m_pMacDisplayLink = std::move(pMacDisplayLink);
    }
#endif

    connect(pWindow,
            &QWindow::screenChanged,
            this,
            &DisplayLinkFrameDriver::slotScreenChanged);

    m_watchdogTimer.setTimerType(Qt::CoarseTimer);
    m_watchdogTimer.setInterval(kWatchdogIntervalMs);
    connect(&m_watchdogTimer,
            &QTimer::timeout,
            this,
            &DisplayLinkFrameDriver::slotWatchdog);

    updateRefreshInterval();
    attachToWindow();
}

DisplayLinkFrameDriver::~DisplayLinkFrameDriver() {
    stop();
    detachFromWindow();
}

void DisplayLinkFrameDriver::attachToWindow() {
#ifdef MIXXX_USE_QOPENGL
    auto* pOpenGLWindow = qobject_cast<OpenGLWindow*>(m_pWindow.data());
    VERIFY_OR_DEBUG_ASSERT(pOpenGLWindow) {
        // Without this the update requests are never forwarded to us and only
        // the watchdog would drive frames.
        qWarning() << "DisplayLinkFrameDriver: window is not an OpenGLWindow";
        return;
    }
    pOpenGLWindow->setFrameDriver(this);
#endif
}

void DisplayLinkFrameDriver::detachFromWindow() {
#ifdef MIXXX_USE_QOPENGL
    // m_pWindow is a QPointer, so this is a no-op if the window is gone.
    auto* pOpenGLWindow = qobject_cast<OpenGLWindow*>(m_pWindow.data());
    if (pOpenGLWindow) {
        pOpenGLWindow->setFrameDriver(nullptr);
    }
#endif
}

void DisplayLinkFrameDriver::reattachTo(QWindow* pWindow) {
    VERIFY_OR_DEBUG_ASSERT(pWindow) {
        return;
    }
    if (m_pWindow.data() == pWindow) {
        return;
    }
    detachFromWindow();
    if (m_pWindow) {
        disconnect(m_pWindow.data(), nullptr, this, nullptr);
    }
    m_pWindow = pWindow;

#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
    m_pMacDisplayLink.reset();
    auto pMacDisplayLink = std::make_unique<mixxx::MacDisplayLink>(pWindow);
    if (pMacDisplayLink->isValid()) {
        m_pMacDisplayLink = std::move(pMacDisplayLink);
    }
#endif

    connect(pWindow,
            &QWindow::screenChanged,
            this,
            &DisplayLinkFrameDriver::slotScreenChanged);

    updateRefreshInterval();
    attachToWindow();
    qDebug() << "DisplayLinkFrameDriver: re-attached to a visible window, "
                "refresh interval"
             << m_refreshIntervalMicros << "us";
    if (m_running) {
        m_updateRequestSeen = false;
        pWindow->requestUpdate();
    }
}

void DisplayLinkFrameDriver::start() {
    if (m_running) {
        return;
    }
    m_running = true;
    m_skipCounter = 0;
    m_updateRequestSeen = false;
    m_hasDrivenFrame = false;
    updateRefreshInterval();
    qDebug() << "DisplayLinkFrameDriver: starting with a refresh interval of"
             << m_refreshIntervalMicros << "us, rendering every" << m_divisor
             << "update request(s) for a configured frame rate of" << m_frameRate << "Hz";
    m_watchdogTimer.start();
    requestNextUpdate();
}

void DisplayLinkFrameDriver::stop() {
    m_running = false;
    m_watchdogTimer.stop();
}

void DisplayLinkFrameDriver::setFrameRate(int frameRate) {
    m_frameRate = std::max(1, frameRate);
    updateDivisor();
}

void DisplayLinkFrameDriver::slotScreenChanged(QScreen* pScreen) {
    Q_UNUSED(pScreen);
    // The window has been moved to another display, which may have a different
    // refresh rate. Note that the macOS CADisplayLink created from the NSView
    // follows the view to the new display by itself.
    updateRefreshInterval();
    qDebug() << "DisplayLinkFrameDriver: window changed screen, refresh interval is now"
             << m_refreshIntervalMicros << "us, divisor" << m_divisor;
}

void DisplayLinkFrameDriver::updateRefreshInterval() {
    double refreshRate = 0.0;
    if (m_pWindow && m_pWindow->screen()) {
        // Deliberately the screen of the window, not the primary screen: with
        // multiple displays with different refresh rates only the screen the
        // window is actually on is relevant.
        refreshRate = m_pWindow->screen()->refreshRate();
    }
    if (refreshRate >= 1.0) {
        m_refreshIntervalMicros = static_cast<int>(std::lround(1e6 / refreshRate));
    } else {
        m_refreshIntervalMicros = kDefaultRefreshIntervalMicros;
    }

#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
    if (m_pMacDisplayLink) {
        // Prefer the interval reported by the display link, it is the actual
        // one (ProMotion displays change it at runtime and QScreen only
        // reports the mode's nominal rate).
        const auto interval = m_pMacDisplayLink->refreshInterval();
        if (interval && interval->count() > 0) {
            m_refreshIntervalMicros = static_cast<int>(interval->count());
        }
    }
#endif

    updateDivisor();
}

void DisplayLinkFrameDriver::updateDivisor() {
    DEBUG_ASSERT(m_refreshIntervalMicros > 0);
    const int configuredIntervalMicros = static_cast<int>(1e6 / m_frameRate);
    // Nearest integer number of display frames per rendered frame. This is the
    // same rounding the PLL mode applies, so that the configured frame rate is
    // interpreted identically in both modes.
    // Note that for a configured rate that is not an integer divisor of the
    // display rate this renders at the nearest divisor, which may be *above*
    // the configured rate (e.g. 100 Hz configured on a 120 Hz display results
    // in 120 Hz, because 120/1 is closer to 100 than 120/2 = 60).
    m_divisor = std::max(1,
            (configuredIntervalMicros + m_refreshIntervalMicros / 2) /
                    m_refreshIntervalMicros);
}

void DisplayLinkFrameDriver::requestNextUpdate() {
    if (m_pWindow) {
        // Note: this only sets a flag in the platform window, the actual
        // QEvent::UpdateRequest is delivered by the platform's display link
        // (CVDisplayLink via QCocoaScreen on macOS).
        m_pWindow->requestUpdate();
    }
}

void DisplayLinkFrameDriver::handleUpdateRequest() {
    if (!m_running) {
        return;
    }
    m_updateRequestSeen = true;

    // Request the next update *before* doing the work: requestUpdate() only
    // raises a flag that the platform display link callback looks at, so
    // requesting it up front means that a frame that overruns its deadline
    // does not additionally lose the following display link callback.
    requestNextUpdate();

    if (++m_skipCounter < m_divisor) {
        // Frame rate limiting: the display is faster than the configured
        // frame rate, skip this update request.
        return;
    }
    m_skipCounter = 0;

    driveFrame();
}

void DisplayLinkFrameDriver::slotWatchdog() {
    if (!m_running) {
        return;
    }
    if (m_updateRequestSeen) {
        m_updateRequestSeen = false;
        return;
    }
    // No update request has been delivered during the last watchdog interval.
    // Keep the machinery alive: the same callback also drives the GUI tick,
    // the VU meters and the spinnies.
    qDebug() << "DisplayLinkFrameDriver: no update request received for"
             << kWatchdogIntervalMs << "ms, driving a frame from the watchdog";
    requestNextUpdate();
    m_skipCounter = 0;
    driveFrame();
}

void DisplayLinkFrameDriver::driveFrame() {
    // Time from now until the content rendered during this frame becomes
    // visible, i.e. the value fromTimerToNextSync() has to return for this
    // frame.
    //
    // This deliberately reproduces the contract of the ST_PLL mode: there the
    // reference point is the estimated vsync the vsync thread woke up for and
    // the value is one frame interval; here the reference point is the
    // delivery of the update request and the value is the time until the
    // presentation deadline the system reports for it, which is the same point
    // in time, only without the jitter of the estimation. Keeping this
    // identical to ST_PLL is what makes the A/B comparison meaningful.
    //
    // Note that the content rendered in this frame is only swapped in the
    // following callback, so strictly speaking it becomes visible one interval
    // after targetTimestamp. If the waveform turns out to be systematically
    // one frame behind the audio, this (pre-existing, mode independent) offset
    // is the place to fix it.
    std::chrono::microseconds untilPresentation(m_refreshIntervalMicros);

#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
    if (m_pMacDisplayLink) {
        const auto interval = m_pMacDisplayLink->refreshInterval();
        if (interval && interval->count() > 0 &&
                static_cast<int>(interval->count()) != m_refreshIntervalMicros) {
            // The display changed its refresh rate at runtime (ProMotion).
            m_refreshIntervalMicros = static_cast<int>(interval->count());
            updateDivisor();
        }
        const auto toPresentation = m_pMacDisplayLink->timeToNextPresentation();
        if (toPresentation) {
            untilPresentation = *toPresentation;
        }
    }
#endif

    // If updates are skipped, the frame rendered now is swapped m_divisor
    // display frames from now instead of on the next one.
    untilPresentation += std::chrono::microseconds(
            static_cast<std::chrono::microseconds::rep>(m_refreshIntervalMicros) *
            (m_divisor - 1));

    if (m_pVSyncThread) {
        m_pVSyncThread->setDisplayLinkTiming(untilPresentation);
        const auto sinceLastSwap = m_pVSyncThread->sinceLastSwap().toIntegerMicros();
        if (m_hasDrivenFrame &&
                sinceLastSwap >
                        static_cast<qint64>(m_refreshIntervalMicros) * m_divisor *
                                3 / 2) {
            // Too much time has passed since the last frame: consider it dropped.
            m_pVSyncThread->reportDroppedFrame();
        }
    }

    m_hasDrivenFrame = true;

    emit frameDue();
}
