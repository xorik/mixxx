#pragma once

#include <QPair>
#include <vector>
#include <QSemaphore>
#include <QThread>
#include <mutex>

#include "util/performancetimer.h"
#include "waveform/isynctimeprovider.h"

class WGLWidget;

class VSyncThread : public QThread, public VSyncTimeProvider {
    Q_OBJECT
  public:
    enum VSyncMode {
        ST_DEFAULT = 0,
        ST_MESA_VBLANK_MODE_1_DEPRECATED, // 1
        ST_SGI_VIDEO_SYNC_DEPRECATED,     // 2
        ST_OML_SYNC_CONTROL_DEPRECATED,   // 3
        ST_FREE,                          // 4
        ST_PLL,                           // 5
        ST_TIMER,                         // 6
        // Frames are driven by the platform display link via
        // QWindow::requestUpdate() on the GUI thread. In this mode the
        // VSyncThread is *not* started at all: it is only kept alive as the
        // VSyncTimeProvider for the renderers, and is fed by the
        // DisplayLinkFrameDriver via setDisplayLinkTiming().
        ST_DISPLAY_LINK,                  // 7
        ST_COUNT                          // Dummy Type at last, counting possible types
    };

    VSyncThread(QObject* pParent, VSyncMode vSyncMode);
    ~VSyncThread();

    void run() override;

    bool waitForVideoSync(WGLWidget* glw);
    int elapsed();
    void setSyncIntervalTimeMicros(int usSyncTimer);
    int droppedFrames();
    void setSwapWait(int sw);
    // VSyncTimerProvider
    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer& timer) override;
    void vsyncSlotFinished();
    void getAvailableVSyncTypes(QList<QPair<int, QString>>* list);
    void setupSync(WGLWidget* glw, int index);
    void waitUntilSwap(WGLWidget* glw);
    mixxx::Duration sinceLastSwap() const;
    // VSyncTimerProvider
    // TEMPORARY PERF INSTRUMENTATION: the period the PLL currently believes
    // in, in microseconds. The configured sync interval says what was asked
    // for; this says what the loop actually settled on, which is the whole
    // point of the drift measurements. 0 in the modes without a PLL.
    double pllPeriodMicros() const {
        return m_vSyncMode == ST_PLL ? m_pllDeltaOut : 0.0;
    }

    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(m_syncIntervalTimeMicros);
    }
    void updatePLL();
    bool pllInitializing() const;

    /// Refresh rate, in Hz, of the screen the render window is really on.
    /// Pass 0 when it is unknown: the PLL then relies on its median
    /// initialisation alone, instead of sanity-checking and clamping against a
    /// rate that does not belong to this window. Changing the rate re-locks the
    /// loop from scratch, because a period pulled to a new rate while the phase
    /// still belongs to the old one is just the old bug in new clothes.
    void setDisplayRefreshRate(double hz);

    /// ST_DISPLAY_LINK only: called on the GUI thread at the begin of every
    /// frame that is about to be swapped and rendered.
    /// \param untilPresentation time from now until the content that is
    /// rendered in this frame becomes visible on screen. This defines the
    /// value returned by fromTimerToNextSync() for this frame.
    void setDisplayLinkTiming(std::chrono::microseconds untilPresentation);
    /// ST_DISPLAY_LINK only: account a frame that was not delivered in time.
    void reportDroppedFrame() {
        m_droppedFrames++;
    }

    VSyncMode vsyncMode() const {
        return m_vSyncMode;
    }
    /// TEMPORARY INSTRUMENTATION: started immediately before the render signal
    /// is emitted, so the GUI thread can measure how long the queued signal sat
    /// in its event queue before being dispatched.
    PerformanceTimer m_emitTimer;
  signals:
    void vsyncSwapAndRender();
    void vsyncRender();
    void vsyncSwap();
  private:
    void runFree();
    void runPLL();
    void runTimer();

    bool m_bDoRendering;
    int m_syncIntervalTimeMicros;
    int m_waitToSwapMicros;
    enum VSyncMode m_vSyncMode;
    bool m_syncOk;
    int m_droppedFrames;
    int m_swapWait;
    PerformanceTimer m_timer;
    QSemaphore m_semaVsyncSlot;
    double m_displayFrameRate;
    int m_vSyncPerRendering;
    mixxx::Duration m_sinceLastSwap;
    // phase locked loop
    std::mutex m_pllMutex;
    PerformanceTimer m_pllTimer;
    std::atomic<int> m_pllInitCnt;
    std::atomic<bool> m_pllPendingUpdate;
    double m_pllInitSum;
    std::vector<double> m_pllInitDeltas;
    double m_pllPhaseOut;
    double m_pllDeltaOut;
    /// 0 = unknown, see setDisplayRefreshRate().
    double m_pllDisplayHz;
    /// Timestamp of the first of the current run of rejected deltas, 0 if the
    /// last delta was accepted. See the cross-check timeout in updatePLL().
    double m_pllRejectedSinceMicros;
    int m_pllRejectedCount;
    bool m_pllCrossCheckOff;
    double m_pllLogging;
};
