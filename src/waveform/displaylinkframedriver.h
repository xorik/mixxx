#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <chrono>
#include <memory>

#if defined(Q_OS_MACOS)
// CADisplayLink based timing is only available on macOS. Everywhere else the
// frame interval is derived from QScreen::refreshRate().
#define MIXXX_HAS_MAC_DISPLAY_LINK
#endif

class QScreen;
class VSyncThread;

#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
namespace mixxx {
class MacDisplayLink;
}
#endif

/// Drives waveform frames from the platform display link instead of from a
/// dedicated thread with a hand rolled phase locked loop.
///
/// The driver is attached to the QWindow of the shared OpenGL context (the
/// same window WaveformWidgetFactory::swapAndRender() already updates every
/// frame). It calls QWindow::requestUpdate(), which on macOS is served by the
/// CVDisplayLink the Cocoa platform plugin runs anyway, and reacts to the
/// resulting QEvent::UpdateRequest (delivered by OpenGLWindow::event()) by
/// emitting frameDue() on the GUI thread.
///
/// There is no estimation, no sleeping and no cross thread signal involved.
class DisplayLinkFrameDriver : public QObject {
    Q_OBJECT
  public:
    /// The driver registers itself at the window (OpenGLWindow::setFrameDriver)
    /// and unregisters in its destructor. It is safe to destroy the window
    /// first.
    /// \param pWindow window of the shared GL context, must be an OpenGLWindow
    /// so that it can forward the update requests.
    /// \param pVSyncThread the (unstarted) VSyncThread that acts as the
    /// VSyncTimeProvider for the renderers. May be nullptr in tests.
    DisplayLinkFrameDriver(QWindow* pWindow, VSyncThread* pVSyncThread, QObject* pParent);
    ~DisplayLinkFrameDriver() override;

    /// Starts requesting update events.
    void start();
    void stop();

    /// Move the driver to another window. The shared GL context lives in a 3x3
    /// helper widget, and the compositor does not necessarily serve update
    /// requests for such a window at the display cadence. Re-attaching to a
    /// real, visible waveform window makes the requests arrive on the vsync
    /// boundary instead of free running.
    void reattachTo(QWindow* pWindow);

    /// The frame rate configured by the user ([Waveform] FrameRate). If the
    /// display refreshes faster, updates are skipped to approximate it.
    void setFrameRate(int frameRate);

    /// Called by OpenGLWindow::event() when a QEvent::UpdateRequest for the
    /// shared GL window arrives (GUI thread).
    void handleUpdateRequest();

    /// The measured/reported refresh interval of the display the window is on.
    std::chrono::microseconds refreshInterval() const {
        return std::chrono::microseconds(m_refreshIntervalMicros);
    }
    /// Number of update requests per rendered frame (frame rate limiting).
    int divisor() const {
        return m_divisor;
    }

  signals:
    /// Emitted for every frame that should be swapped and rendered.
    void frameDue();

  private slots:
    void slotScreenChanged(QScreen* pScreen);
    void slotWatchdog();

  private:
    void attachToWindow();
    void detachFromWindow();
    void updateRefreshInterval();
    void updateDivisor();
    void requestNextUpdate();
    void driveFrame();

    QPointer<QWindow> m_pWindow;
    VSyncThread* m_pVSyncThread;
#ifdef MIXXX_HAS_MAC_DISPLAY_LINK
    std::unique_ptr<mixxx::MacDisplayLink> m_pMacDisplayLink;
#endif
    QTimer m_watchdogTimer;

    bool m_running;
    /// Set in handleUpdateRequest(), cleared by the watchdog. Used to detect
    /// that the platform stopped delivering update requests.
    bool m_updateRequestSeen;
    /// False until the first frame has been driven, used to suppress a bogus
    /// dropped frame report for the very first frame.
    bool m_hasDrivenFrame;
    int m_frameRate;
    int m_refreshIntervalMicros;
    int m_divisor;
    int m_skipCounter;
};
