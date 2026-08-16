#include "vsyncthread.h"

#include <QGuiApplication>
#include <QScreen>
#include <algorithm>

#include "moc_vsyncthread.cpp"
#include "util/math.h"
#include "util/performancetimer.h"

namespace {

constexpr int kNumStableDeltasRequired = 20;

/// How long the PLL may keep rejecting deltas as implausible before it stops
/// cross-checking against the display and locks on the median alone.
constexpr double kPllCrossCheckTimeoutMicros = 2e6;

/// Frame interval, in microseconds, of the screen the render window is on, or
/// 0 when that is not known. It must NOT be guessed from the primary screen:
/// on a laptop with a 120 Hz panel and a 60 Hz external monitor as the primary
/// one, every check based on it rejects the true interval of the other screen.
/// The primary screen is only used when there is exactly one screen, where the
/// answer is unambiguous.
double displayFrameIntervalMicros(double reportedHz) {
    double hz = reportedHz;
    if (hz <= 1.0) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        if (screens.size() != 1) {
            return 0.0;
        }
        hz = screens.first()->refreshRate();
    }
    return hz > 1.0 ? 1e6 / hz : 0.0;
}

VSyncThread::VSyncMode defaultVSyncMode() {
#ifdef __APPLE__
    // ST_PLL, but with the initialisation bug fixed (see updatePLL): the old
    // code compared each sampled interval against a running mean the sample
    // itself had just been folded into, so one short startup frame could drag
    // the locked period ~2% off and the app then dropped ~15 frames/s for the
    // whole session.
    //
    // ST_DISPLAY_LINK (VSync 7) is the architecturally correct successor - it
    // takes the cadence from the system instead of measuring it, so it follows
    // the window across monitors - but it is NOT ready: after the first frames
    // the update requests stop arriving and the waveform freezes.
    return VSyncThread::ST_PLL;
#else
    return VSyncThread::ST_TIMER;
#endif
}

} // namespace

VSyncThread::VSyncThread(QObject* pParent, VSyncThread::VSyncMode vSyncMode)
        : QThread(pParent),
          m_bDoRendering(true),
          m_syncIntervalTimeMicros(33333), // 30 FPS
          m_waitToSwapMicros(0),
          m_vSyncMode(vSyncMode == VSyncThread::ST_DEFAULT ? defaultVSyncMode() : vSyncMode),
          m_syncOk(false),
          m_droppedFrames(0),
          m_swapWait(0),
          m_displayFrameRate(60.0),
          m_vSyncPerRendering(1),
          m_pllInitCnt(0),
          m_pllInitSum(0.0),
          m_pllPhaseOut(0.0),
          m_pllDeltaOut(16666.6),
          m_pllDisplayHz(0.0),
          m_pllPhaseErrCount(0),
          m_pllPhaseErrSum(0.0),
          m_pllPhaseErrSumSq(0.0),
          m_pllPhaseErrWorst(0.0),
          m_pllRejectedSinceMicros(0.0),
          m_pllRejectedCount(0),
          m_pllCrossCheckOff(false),
          m_pllLogging(0.0) {
    m_pllTimer.start();
    // In ST_DISPLAY_LINK mode run() is never entered, so start the timer here.
    // In all other modes run() restarts it anyway.
    m_timer.start();
}

VSyncThread::~VSyncThread() {
    m_bDoRendering = false;
    if (m_vSyncMode != ST_DISPLAY_LINK) {
        // In ST_DISPLAY_LINK mode the thread has never been started and
        // nobody is waiting for the semaphore.
        m_semaVsyncSlot.release(m_vSyncMode == ST_PLL ? 1 : 2); // Two slots, one for PLL
    }
    wait();
    //delete m_glw;
}

void VSyncThread::run() {
    QThread::currentThread()->setObjectName("VSyncThread");

    m_waitToSwapMicros = m_syncIntervalTimeMicros;
    m_timer.start();

    //qDebug() << "VSyncThread::run()";
    switch (m_vSyncMode) {
    case ST_FREE:
        m_syncIntervalTimeMicros = 1000;
        m_waitToSwapMicros = m_syncIntervalTimeMicros;
        runFree();
        break;
    case ST_PLL:
        runPLL();
        break;
    case ST_TIMER:
        runTimer();
        break;
    case ST_DISPLAY_LINK:
        // Frames are driven by DisplayLinkFrameDriver on the GUI thread.
        // This thread is not supposed to be started at all in this mode.
        DEBUG_ASSERT(!"VSyncThread must not be started in ST_DISPLAY_LINK mode");
        break;
    default:
        DEBUG_ASSERT(!"unsupported sync mode");
        break;
    }
}

void VSyncThread::runFree() {
    DEBUG_ASSERT(m_vSyncMode == ST_FREE);
    while (m_bDoRendering) {
        // for benchmark only!

        // renders the waveform, Possible delayed due to anti tearing
        emit vsyncRender();
        m_semaVsyncSlot.acquire();

        emit vsyncSwap(); // swaps the new waveform to front
        m_semaVsyncSlot.acquire();

        m_sinceLastSwap = m_timer.restart();
        usleep(m_waitToSwapMicros);
    }
}

void VSyncThread::runPLL() {
    DEBUG_ASSERT(m_vSyncMode == ST_PLL);
    qint64 offsetAdjustedAt = 0;
    qint64 offset = 0;
    qint64 nextSwapMicros = 0;
    qint64 multiplierAdjustedPllPhaseOut = 0;
    while (m_bDoRendering) {
        // Use a phase-locked-loop on the QOpenGLWindow::frameSwapped signal
        // to determine when the vsync occurs

        qint64 pllPhaseOut;
        qint64 pllDeltaOut;
        qint64 multiplierAdjustedPllDeltaOut;
        qint64 now;

        {
            std::scoped_lock lock(m_pllMutex);
            // last estimated vsync
            pllPhaseOut = std::llround(m_pllPhaseOut);
            // estimated frame interval
            pllDeltaOut = std::llround(m_pllDeltaOut);
            now = m_pllTimer.elapsed().toIntegerMicros();

            // Calculate the nearest integer number of PLL intervals that
            // correspond with the interval based on the frame rate from the
            // user settings. E.g. if the PLL is running at 60 fps, and the user
            // settings is between 25 and 40 fps, we do run effectively at 30
            // fps (multiplier is 2)
            //
            // Note this also is applied when running at 120 fps (ProMotion)
            const auto multiplier = std::max<qint64>(1,
                    (m_syncIntervalTimeMicros + pllDeltaOut / 2) / pllDeltaOut);

            // Update the multiplierAdjustedPllPhaseOut to pllPhaseOut, if pllPhaseOut has increased
            // multiplier * pllDeltaOut intervals.
            if (multiplierAdjustedPllPhaseOut == 0 ||
                    ((pllPhaseOut - multiplierAdjustedPllPhaseOut +
                             pllDeltaOut / 2) /
                            pllDeltaOut) %
                                    multiplier ==
                            0) {
                multiplierAdjustedPllPhaseOut = pllPhaseOut;
            }

            multiplierAdjustedPllDeltaOut = pllDeltaOut * multiplier;
        }

        if (multiplierAdjustedPllPhaseOut > nextSwapMicros) {
            // We received a new pll phase
            nextSwapMicros = multiplierAdjustedPllPhaseOut;
        } else {
            // We didn't receive a new pll phase out, so freewheel to estimated
            // next with the current delta.
            nextSwapMicros += multiplierAdjustedPllDeltaOut;
        }

        qint64 sleepUntilSwap = (nextSwapMicros + offset - now) % multiplierAdjustedPllDeltaOut;
        if (sleepUntilSwap < 0) {
            sleepUntilSwap += multiplierAdjustedPllDeltaOut;
        }
        usleep(sleepUntilSwap);

        m_sinceLastSwap = m_timer.restart();
        m_waitToSwapMicros = multiplierAdjustedPllDeltaOut;

        // Signal to swap the gl widgets (waveforms, spinnies, vumeters)
        // and render them for the next swap
        if (!pllInitializing() || m_pllPendingUpdate) {
            m_emitTimer.start();
            emit vsyncSwapAndRender();
            m_semaVsyncSlot.acquire();
            m_pllPendingUpdate = false;
        }

        if (m_sinceLastSwap.toIntegerMicros() > multiplierAdjustedPllDeltaOut * 3 / 2) {
            // Too much time passed since last swap: consider frame dropped.
            // Automatically adjust the time offset between the PLL and our signal,
            // ideally settling on an offset with no or little frame drops.
            const auto sinceLastOffsetAdjust = now - offsetAdjustedAt;
            // Don't adjust too often (max once every 100 ms)
            if (sinceLastOffsetAdjust > 100000) {
                // And don't adjust (immediately) if we have been running
                // without drops for over 1 second
                if (sinceLastOffsetAdjust < 1000000) {
                    offset = (offset + pllDeltaOut / 8) % multiplierAdjustedPllDeltaOut;
                }
                offsetAdjustedAt = now;
            }
            m_droppedFrames++;
            qDebug() << "DROP";
        }
    }
}

void VSyncThread::runTimer() {
    DEBUG_ASSERT(m_vSyncMode == ST_TIMER);

    while (m_bDoRendering) {
        emit vsyncRender(); // renders the new waveform.

        // wait until rendering was scheduled. It might be delayed due a
        // pending swap (depends one driver vSync settings)
        m_semaVsyncSlot.acquire();

        // qDebug() << "ST_TIMER                      " << lastMicros << restMicros;
        int remainingForSwap = m_waitToSwapMicros -
                static_cast<int>(m_timer.elapsed().toIntegerMicros());
        // waiting for interval by sleep
        if (remainingForSwap > 100) {
            usleep(remainingForSwap);
        }

        // swaps the new waveform to front in case of gl-wf
        emit vsyncSwap();

        // wait until swap occurred. It might be delayed due to driver vSync
        // settings.
        m_semaVsyncSlot.acquire();

        // <- Assume we are VSynced here ->
        m_sinceLastSwap = m_timer.restart();
        int lastSwapTime = static_cast<int>(m_sinceLastSwap.toIntegerMicros());
        if (remainingForSwap < 0) {
            // Our swapping call was already delayed
            // The real swap might happens on the following VSync, depending on driver settings
            m_droppedFrames++; // Count as Real Time Error
        }
        // try to stay in right intervals
        m_waitToSwapMicros = m_syncIntervalTimeMicros +
                ((m_waitToSwapMicros - lastSwapTime) % m_syncIntervalTimeMicros);
    }
}

int VSyncThread::elapsed() {
    return static_cast<int>(m_timer.elapsed().toIntegerMicros());
}

void VSyncThread::setSyncIntervalTimeMicros(int syncTime) {
    if (m_vSyncMode != ST_FREE) {
        m_syncIntervalTimeMicros = syncTime;
        m_vSyncPerRendering = static_cast<int>(
                round(m_displayFrameRate * m_syncIntervalTimeMicros / 1000));
    }
}

std::chrono::microseconds VSyncThread::fromTimerToNextSync(const PerformanceTimer& timer) {
    int difference = static_cast<int>(m_timer.difference(timer).toIntegerMicros());
    // int math is fine here, because we do not expect times > 4.2 s
    int toNextSync = difference + m_waitToSwapMicros;
    while (toNextSync < 0) {
        // this function is called during rendering. A negative value indicates
        // an attempt to render an outdated frame. Render the next frame instead
        toNextSync += m_syncIntervalTimeMicros;
    }
    return std::chrono::microseconds(toNextSync);
}

void VSyncThread::setDisplayLinkTiming(std::chrono::microseconds untilPresentation) {
    DEBUG_ASSERT(m_vSyncMode == ST_DISPLAY_LINK);
    // Same contract as the other modes: m_timer marks the reference point of
    // the current frame and m_waitToSwapMicros the time from that reference
    // point until the content rendered during this frame is presented.
    m_sinceLastSwap = m_timer.restart();
    m_waitToSwapMicros = static_cast<int>(untilPresentation.count());
}

int VSyncThread::droppedFrames() {
    return m_droppedFrames;
}

void VSyncThread::vsyncSlotFinished() {
    m_semaVsyncSlot.release();
}

void VSyncThread::getAvailableVSyncTypes(QList<QPair<int, QString>>* pList) {
    for (int i = (int)VSyncThread::ST_TIMER; i < (int)VSyncThread::ST_COUNT; i++) {
        //if (isAvailable(type))  // TODO
        {
            enum VSyncMode mode = (enum VSyncMode)i;

            QString name;
            switch (mode) {
            case VSyncThread::ST_TIMER:
                name = tr("Timer (Fallback)");
                break;
            case VSyncThread::ST_MESA_VBLANK_MODE_1_DEPRECATED:
                name = tr("MESA vblank_mode = 1");
                break;
            case VSyncThread::ST_SGI_VIDEO_SYNC_DEPRECATED:
                name = tr("Wait for Video sync");
                break;
            case VSyncThread::ST_OML_SYNC_CONTROL_DEPRECATED:
                name = tr("Sync Control");
                break;
            case VSyncThread::ST_FREE:
                name = tr("Free + 1 ms (for benchmark only)");
                break;
            case VSyncThread::ST_PLL:
                name = tr("frameSwapped-signal driven phase locked loop");
                break;
            case VSyncThread::ST_DISPLAY_LINK:
                name = tr("Platform display link (requestUpdate)");
                break;
            default:
                break;
            }
            QPair<int, QString > pair = QPair<int, QString >(i, name);
            pList->append(pair);
        }
    }
}

mixxx::Duration VSyncThread::sinceLastSwap() const {
    return m_sinceLastSwap;
}

bool VSyncThread::pllInitializing() const {
    return m_pllInitCnt < kNumStableDeltasRequired;
}

VSyncThread::PhaseErrorStats VSyncThread::takePhaseErrorStats() {
    std::scoped_lock lock(m_pllMutex);
    PhaseErrorStats stats;
    stats.count = m_pllPhaseErrCount;
    if (m_pllPhaseErrCount > 0) {
        const double n = static_cast<double>(m_pllPhaseErrCount);
        stats.meanUs = m_pllPhaseErrSum / n;
        stats.sdUs = std::sqrt(std::max(0.0,
                m_pllPhaseErrSumSq / n - stats.meanUs * stats.meanUs));
        stats.worstUs = m_pllPhaseErrWorst;
    }
    m_pllPhaseErrCount = 0;
    m_pllPhaseErrSum = 0.0;
    m_pllPhaseErrSumSq = 0.0;
    m_pllPhaseErrWorst = 0.0;
    return stats;
}

void VSyncThread::setDisplayRefreshRate(double hz) {
    const double newHz = hz > 1.0 ? hz : 0.0;
    std::scoped_lock lock(m_pllMutex);
    if (newHz == m_pllDisplayHz) {
        return;
    }
    qDebug() << "VSyncThread: render window is on a" << newHz
             << "Hz screen (was" << m_pllDisplayHz << "Hz), re-locking the PLL";
    m_pllDisplayHz = newHz;
    // Start the lock over: keeping the accumulated samples would mix intervals
    // of two different screens, and keeping the phase would leave the loop
    // with a period from the new screen and a phase from the old one.
    m_pllInitDeltas.clear();
    m_pllInitCnt = 0;
    m_pllInitSum = 0.0;
    m_pllRejectedSinceMicros = 0.0;
    m_pllRejectedCount = 0;
    m_pllCrossCheckOff = false;
    m_pllPhaseOut = m_pllTimer.elapsed().toDoubleMicros();
    m_pllPendingUpdate = true;
}

void VSyncThread::updatePLL() {
    std::scoped_lock lock(m_pllMutex);

    m_pllPendingUpdate = false;

    // Phase-lock-looped to estimate the vsync based on the
    // QOpenGLWindow::frameSwapped signal

    const double pllPhaseIn = m_pllTimer.elapsed().toDoubleMicros();

    if (m_pllInitCnt < kNumStableDeltasRequired) {
        // Robust initialisation.
        //
        // The original code compared each delta against a running mean that
        // the delta itself had just been folded into. For the first sample
        // after a reset that difference is zero by construction, so ANY value
        // was accepted as the seed; every following normal sample then landed
        // within the +/-2000us tolerance of the mean the bad seed had dragged
        // down. A single short startup frame therefore poisoned the lock:
        // 4933 + 19*8333 over 20 samples = 8163us, i.e. 122.5Hz on a 120Hz
        // display. Measured: 5 of 10 launches locked to ~8163us instead of
        // 8333us and dropped ~15 frames/s for the whole session.
        //
        // Fix: discard implausible deltas up front using the refresh rate the
        // display actually reports, take the median rather than the mean so a
        // single outlier cannot shift the result, and snap to the display
        // interval when we are close to it.
        const double delta = pllPhaseIn - m_pllPhaseOut;
        m_pllPhaseOut = pllPhaseIn;

        const double expected = m_pllCrossCheckOff ? 0.0
                                                   : displayFrameIntervalMicros(m_pllDisplayHz);
        if (expected > 0.0 && std::abs(delta - expected) > expected * 0.25) {
            // Not a plausible frame interval for this display; ignore it rather
            // than letting it contribute to the estimate. Two safeguards, both
            // learned the hard way:
            //
            // 1. Rendering must not depend on that judgement. A rejected sample
            //    used to leave m_pllPendingUpdate false, and runPLL() only
            //    emits a frame when (!pllInitializing() || m_pllPendingUpdate),
            //    so a display whose interval the check never accepts froze the
            //    waveforms completely - background only, no renderer output at
            //    all.
            // 2. The check must be able to give up. A ProMotion panel reports
            //    its current mode while the real interval floats between 24 and
            //    120 Hz, and the system lowers the rate when the window is not
            //    active - which is exactly how a benchmark window runs. Every
            //    sample then looks implausible, the lock never completes and
            //    the period stays at the startup default forever: frames flow,
            //    but the synchronisation is wrong and nothing says so.
            //    After a couple of seconds of nothing but rejections we drop
            //    the cross-check for this lock attempt. The median
            //    initialisation is self-sufficient; the cross-check was
            //    insurance, not the foundation.
            if (m_pllRejectedSinceMicros == 0.0) {
                m_pllRejectedSinceMicros = pllPhaseIn;
            }
            ++m_pllRejectedCount;
            if (pllPhaseIn - m_pllRejectedSinceMicros > kPllCrossCheckTimeoutMicros) {
                m_pllCrossCheckOff = true;
                qDebug() << "VSyncThread: PLL display cross-check gave up after"
                         << m_pllRejectedCount << "implausible deltas in"
                         << (pllPhaseIn - m_pllRejectedSinceMicros) / 1000.0
                         << "ms (display reports" << expected
                         << "us); locking on the median alone";
            }
            m_pllPendingUpdate = true;
            return;
        }
        m_pllRejectedSinceMicros = 0.0;
        m_pllRejectedCount = 0;

        m_pllInitDeltas.push_back(delta);
        m_pllInitCnt = static_cast<int>(m_pllInitDeltas.size());

        if (m_pllInitCnt == kNumStableDeltasRequired) {
            std::vector<double> sorted = m_pllInitDeltas;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            m_pllDeltaOut = (expected > 0.0 && !m_pllCrossCheckOff &&
                                    std::abs(median - expected) < expected * 0.02)
                    ? expected
                    : median;
            qDebug() << "VSyncThread: PLL locked to" << m_pllDeltaOut
                     << "us (display reports" << expected << "us)";
        }
        return;
    }


    // inspired by https://liquidsdr.org/blog/pll-simple-howto/
    const double alpha = 0.01;               // the page above uses 0.05, but a more narrow
                                             // filter seems to work better here
    const double beta = 0.5 * alpha * alpha; // increment adjustment factor

    m_pllPhaseOut += m_pllDeltaOut;

    double pllPhaseError = pllPhaseIn - m_pllPhaseOut;

    // TEMPORARY BENCHMARK HOOK: MIXXX_BENCH_PLL_WRAP=sym|asym.
    //
    // HYPOTHESIS, NOT CONFIRMED BY MEASUREMENT. Upstream folds the phase error
    // back to the nearest frame only when it is POSITIVE. A negative error of
    // any size is never folded and reaches the loop filter in full, and the
    // filter moves the period with it (m_pllDeltaOut += beta * error). The
    // user's log shows large negative phase errors (-3760, -7561, -4975 us)
    // together with a period drifting downwards (16682 -> 16558 us against a
    // true 16667), which is consistent with that - but not proof: the sign of
    // the error also depends on accumulated phase offset, not only on the
    // period. If the hypothesis holds, clamping the period only treats the
    // symptom and the real fix is this one line.
    //
    // asym (default) = upstream behaviour. sym = fold both signs.
    static const bool symmetricWrap = qgetenv("MIXXX_BENCH_PLL_WRAP") == "sym";
    static bool wrapAnnounced = false;
    if (!wrapAnnounced) {
        wrapAnnounced = true;
        qDebug().nospace() << "BENCHHIT MIXXX_BENCH_PLL_WRAP="
                           << (symmetricWrap ? "sym" : "asym")
                           << " phaseErrorWrapped="
                           << (symmetricWrap ? "both signs" : "positive only");
    }
    if (symmetricWrap ? (pllPhaseError != 0.0) : (pllPhaseError > 0)) {
        // when off by more than a frame, jump to the nearest frame
        m_pllPhaseOut += std::round(pllPhaseError / m_pllDeltaOut) * m_pllDeltaOut;
        pllPhaseError = pllPhaseIn - m_pllPhaseOut;
    }

    // apply loop filter and correct output phase and delta
    // Record the error the loop is actually left with this frame. Only the
    // residual matters: an error folded back to the nearest frame has been
    // dealt with, one that reaches the filter has not.
    ++m_pllPhaseErrCount;
    m_pllPhaseErrSum += pllPhaseError;
    m_pllPhaseErrSumSq += pllPhaseError * pllPhaseError;
    if (std::abs(pllPhaseError) > std::abs(m_pllPhaseErrWorst)) {
        m_pllPhaseErrWorst = pllPhaseError;
    }

    m_pllPhaseOut += alpha * pllPhaseError; // adjust phase
    m_pllDeltaOut += beta * pllPhaseError;  // adjust delta

    // Keep the estimated period near the interval the display actually
    // reports. The loop filter has no anchor, so a persistent one-sided phase
    // error slowly walks the period away from the truth: measured on a 60 Hz
    // panel it drifted 16667 -> 16558us with the phase error growing to
    // -7561us (nearly half a frame), costing ~5 fps and several dropped
    // frames per second. Clamping to +/-1% keeps the loop free to track
    // jitter while preventing it from settling on a period the display does
    // not have.
    //
    // TEMPORARY BENCHMARK HOOK: MIXXX_BENCH_PLL_CLAMP=0 disables the clamp, so
    // the drift limit can be measured against the un-clamped loop in ONE
    // binary. Unset or 1 keeps it on, which is the shipped behaviour of this
    // patch. Logged once as BENCHHIT, so a run can prove which arm it measured.
    static const bool clampEnabled = qgetenv("MIXXX_BENCH_PLL_CLAMP") != "0";
    static bool clampAnnounced = false;
    if (!clampAnnounced) {
        clampAnnounced = true;
        qDebug().nospace() << "BENCHHIT MIXXX_BENCH_PLL_CLAMP="
                           << (clampEnabled ? "1" : "0")
                           << " driftClamp=" << (clampEnabled ? "on" : "off");
    }
    const double expectedInterval = displayFrameIntervalMicros(m_pllDisplayHz);
    if (clampEnabled && expectedInterval > 0.0) {
        const double lo = expectedInterval * 0.99;
        const double hi = expectedInterval * 1.01;
        m_pllDeltaOut = std::clamp(m_pllDeltaOut, lo, hi);
    }

    if (pllPhaseIn > m_pllLogging) {
        if (m_pllLogging == 0) {
            m_pllLogging = pllPhaseIn;
        } else {
            qDebug() << "phase-locked-loop:" << std::llround(m_pllPhaseOut)
                     << m_pllDeltaOut << pllPhaseError;
        }
        // log every 10 seconds
        m_pllLogging += 10000000.0;
    }

    m_pllPendingUpdate = true;
}
