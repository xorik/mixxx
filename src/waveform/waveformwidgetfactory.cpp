#include "waveform/waveformwidgetfactory.h"

#include "waveform/renderers/waveformrendererabstract.h"
#include "waveform/waveform.h"

#ifdef MIXXX_USE_QOPENGL
#include <QGuiApplication>
#include <QOpenGLShaderProgram>
#include <QOpenGLWindow>
#else
#include <QGLFormat>
#include <QGLShaderProgram>
#endif
#ifdef Q_OS_ANDROID
#include <GLES3/gl3.h>
#endif

#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QScreen>
#include <QStringList>
#include <QWidget>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <vector>

#include "control/controlobject.h"
#include "moc_waveformwidgetfactory.cpp"
#include "util/cmdlineargs.h"
#include "util/math.h"
#include "util/performancetimer.h"
#include "util/timer.h"
#include "waveform/displaylinkframedriver.h"
#include "waveform/guitick.h"
#include "waveform/sharedglcontext.h"
#include "waveform/visualsmanager.h"
#include "waveform/vsyncthread.h"
#ifdef MIXXX_USE_QOPENGL
#include "waveform/renderers/allshader/waveformrenderersignalbase.h"
#include "waveform/widgets/allshader/waveformwidget.h"
#include "waveform/widgets/glvsynctestwidget.h"
#include "widget/openglwindow.h"
#endif
#include "waveform/widgets/emptywaveformwidget.h"
#include "waveform/widgets/hsvwaveformwidget.h"
#include "waveform/widgets/rgbwaveformwidget.h"
#include "waveform/widgets/simplesignalwaveformwidget.h"
#include "waveform/widgets/softwarewaveformwidget.h"
#include "waveform/widgets/waveformwidgetabstract.h"
#include "widget/wvumeterbase.h"
#include "widget/wvumeterlegacy.h"
#include "widget/wwaveformviewer.h"

namespace {

// We use an AllBand gain default of 2, because default ReplayGain is "enabled" at -18 LUFS
// which gives at least 6 dB headroom with modern pop tracks.
constexpr double kVisualGainDefault[] = {2, 1, 1, 1};
constexpr bool kOverviewNormalizedDefault = false;

// Returns true if the given waveform should be rendered.
bool shouldRenderWaveform(WaveformWidgetAbstract* pWaveformWidget) {
    if (pWaveformWidget == nullptr ||
        pWaveformWidget->getWidth() == 0 ||
        pWaveformWidget->getHeight() == 0) {
        return false;
    }

    auto* glw = pWaveformWidget->getGLWidget();
    if (glw == nullptr) {
        // Not a WGLWidget. We can simply use QWidget::isVisible.
        auto* qwidget = qobject_cast<QWidget*>(pWaveformWidget->getWidget());
        return qwidget != nullptr && qwidget->isVisible();
    }

    return glw->shouldRender();
}

// --- TEMPORARY BENCHMARK HOOK: proof that pixels were really drawn ---
// A broken shader, an occluded surface or a renderer that silently draws
// nothing still produces perfect frame-time telemetry, so the numbers of a run
// mean nothing unless we can show that the waveform area really contains a
// waveform. Screenshots cannot do this: screencapture needs the Screen
// Recording permission and, without it, silently returns an all-black image.
// Reading the pixels back inside the process needs no permission at all.
//
// MIXXX_BENCH_PIXELPROBE=<n>: probe every n rendered frames (1 = use the
// default of roughly one second). Reported per deck: mean AND standard
// deviation over the sampled block plus a checksum - the mean of an empty area
// can coincide with the background, its spread cannot.
//
// glReadPixels stalls the CPU until the GPU has caught up, so a probing run is
// NOT comparable with a normal one. This is why it is off unless asked for.
void benchProbePixels(WaveformWidgetAbstract* pWaveformWidget) {
    WGLWidget* pGlw = pWaveformWidget->getGLWidget();
    if (pGlw == nullptr) {
        return;
    }
    pGlw->makeCurrentIfNeeded();
    QOpenGLContext* pContext = QOpenGLContext::currentContext();
    if (pContext == nullptr) {
        return;
    }
    const double dpr = static_cast<double>(pWaveformWidget->getDevicePixelRatio());
    const int w = static_cast<int>(pWaveformWidget->getWidth() * dpr);
    const int h = static_cast<int>(pWaveformWidget->getHeight() * dpr);
    // A block in the middle of the widget: that is where the signal is drawn,
    // and it is one readback instead of one per sample point.
    const int bw = std::min(128, w);
    const int bh = std::min(32, h);
    if (bw < 4 || bh < 4) {
        return;
    }
    const int x0 = (w - bw) / 2;
    const int y0 = (h - bh) / 2;
    std::vector<unsigned char> buf(static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4);
    // The frame is still in the back buffer; the swap happens after render().
    pContext->functions()->glReadPixels(
            x0, y0, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    pGlw->doneCurrent();

    const int n = bw * bh;
    double sum = 0.0;
    double sumSq = 0.0;
    unsigned int checksum = 2166136261u; // FNV-1a
    for (int i = 0; i < n; ++i) {
        const unsigned char r = buf[static_cast<size_t>(i) * 4];
        const unsigned char g = buf[static_cast<size_t>(i) * 4 + 1];
        const unsigned char b = buf[static_cast<size_t>(i) * 4 + 2];
        const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
        sum += lum;
        sumSq += lum * lum;
        checksum = (checksum ^ r) * 16777619u;
        checksum = (checksum ^ g) * 16777619u;
        checksum = (checksum ^ b) * 16777619u;
    }
    const double mean = sum / n;
    const double sd = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
    qDebug().nospace() << "BENCHPIXELS group=" << pWaveformWidget->getGroup()
                       << " block=" << bw << "x" << bh << "+" << x0 << "+" << y0
                       << " mean=" << QString::number(mean, 'f', 2)
                       << " sd=" << QString::number(sd, 'f', 2)
                       << " checksum=" << checksum;
}

// --- TEMPORARY PERF INSTRUMENTATION: per-frame phase accumulators (ms) ---
// Summed over one reporting second, reported as mean-per-frame in WAVEPERF.
double g_perfPreRenderMs = 0.0;
double g_perfRenderMs = 0.0;
double g_perfExtrasMs = 0.0;
double g_perfSwapMs = 0.0;
double g_perfDispatchMs = 0.0;   // queued-signal latency: emit -> slot entry
double g_perfTickMs = 0.0;       // waveformUpdateTick: repaint of all other widgets
double g_perfVisualsMs = 0.0;    // VisualsManager + GuiTick
double g_perfUpdateMs = 0.0;     // update() on the shared-context window
int g_perfEffectiveOptions = -1; // waveform_options the widgets were BUILT with
int g_perfRenderedCount = 0;     // waveforms actually rendered this second
int g_perfSwappedCount = 0;      // surfaces actually swapped this second

const QRegularExpression openGLVersionRegex(QStringLiteral("^(\\d+)\\.(\\d+).*$"));

const QString kWaveformGroup(QStringLiteral("[Waveform]"));
const ConfigKey kWaveformTypeKey =
        ConfigKey(kWaveformGroup, QStringLiteral("WaveformType"));
const ConfigKey kHardwareAccelerationKey =
        ConfigKey(kWaveformGroup, QStringLiteral("use_hardware_acceleration"));
const ConfigKey kZoomSyncKey = ConfigKey(
        kWaveformGroup, QStringLiteral("ZoomSynchronization"));
const ConfigKey kEndOfTrackWarningKey = ConfigKey(
        kWaveformGroup, QStringLiteral("EndOfTrackWarningTime"));
const ConfigKey kDefaultZoomKey =
        ConfigKey(kWaveformGroup, QStringLiteral("DefaultZoom"));
const ConfigKey kFrameRateKey =
        ConfigKey(kWaveformGroup, QStringLiteral("FrameRate"));
const ConfigKey kVSyncKey = ConfigKey(kWaveformGroup, QStringLiteral("VSync"));

ConfigKey visualGainKey(int index) {
    return ConfigKey(kWaveformGroup, QStringLiteral("VisualGain_") + QString::number(index));
}

}  // anonymous namespace

///////////////////////////////////////////

WaveformWidgetAbstractHandle::WaveformWidgetAbstractHandle()
        : m_type(WaveformWidgetType::Invalid) {
}

///////////////////////////////////////////

WaveformWidgetHolder::WaveformWidgetHolder()
        : m_waveformWidget(nullptr),
          m_waveformViewer(nullptr),
          m_skinContextCache(UserSettingsPointer(), QString()) {
}

WaveformWidgetHolder::WaveformWidgetHolder(WaveformWidgetAbstract* waveformWidget,
                                           WWaveformViewer* waveformViewer,
                                           const QDomNode& node,
                                           const SkinContext& parentContext)
    : m_waveformWidget(waveformWidget),
      m_waveformViewer(waveformViewer),
      m_skinNodeCache(node.cloneNode()),
      m_skinContextCache(&parentContext) {
}

///////////////////////////////////////////

WaveformWidgetFactory::WaveformWidgetFactory()
        // Set an empty waveform initially. We will set the correct one when skin load finishes.
        // Concretely, we want to set a non-GL waveform when loading the skin so that the window
        // loads correctly.
        : m_type(WaveformWidgetType::Empty),
          m_configType(WaveformWidgetType::Empty),
          m_config(nullptr),
          m_skipRender(false),
          m_frameRate(60),
          m_endOfTrackWarningTime(30),
          m_defaultZoom(WaveformWidgetRenderer::s_waveformDefaultZoom),
          m_zoomSync(true),
          m_overviewNormalized(kOverviewNormalizedDefault),
          m_untilMarkShowBeats(false),
          m_untilMarkShowTime(false),
          m_untilMarkAlign(Qt::AlignVCenter),
          m_untilMarkTextPointSize(24),
          m_untilMarkTextHeightLimit(toUntilMarkTextHeightLimit(0)),
          m_stemSplitTracks(false),
          m_openGlAvailable(false),
          m_openGlesAvailable(false),
          m_openGLShaderAvailable(false),
          m_beatGridAlpha(90),
          m_vsyncThread(nullptr),
          m_pDisplayLinkFrameDriver(nullptr),
          m_pGuiTick(nullptr),
          m_pVisualsManager(nullptr),
          m_frameCnt(0),
          m_actualFrameRate(0),
          m_playMarkerPosition(WaveformWidgetRenderer::s_defaultPlayMarkerPosition) {
    m_pStemSplitTracksControl = std::make_unique<ControlObject>(
            ConfigKey(kWaveformGroup, QStringLiteral("stem_split_tracks")));
    m_visualGain[AllBand] = kVisualGainDefault[AllBand];
    m_visualGain[Low] = kVisualGainDefault[Low];
    m_visualGain[Mid] = kVisualGainDefault[Mid];
    m_visualGain[High] = kVisualGainDefault[High];

#ifdef MIXXX_USE_QOPENGL
    WGLWidget* widget = SharedGLContext::getWidget();
    if (widget) {
        widget->makeCurrentIfNeeded();
        auto* pContext = QOpenGLContext::currentContext();
        if (pContext) {
            auto* glFunctions = pContext->functions();
            glFunctions->initializeOpenGLFunctions();
            QString versionString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_VERSION))));
            QString vendorString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_VENDOR))));
            QString rendererString = QString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_RENDERER))));
            qDebug().noquote() << QStringLiteral(
                    "OpenGL driver version string \"%1\", vendor \"%2\", "
                    "renderer \"%3\"")
                                          .arg(versionString, vendorString, rendererString);

            GLint majorVersion, minorVersion = GL_INVALID_ENUM;
            glFunctions->glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
            glFunctions->glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
            if (majorVersion == GL_INVALID_ENUM || minorVersion == GL_INVALID_ENUM) {
                // GL_MAJOR/MINOR_VERSION are not supported below OpenGL 3.0, so
                // parse GL_VERSION string as a fallback.
                // https://www.khronos.org/opengl/wiki/OpenGL_Context#OpenGL_version_number
                auto match = openGLVersionRegex.match(versionString);
                DEBUG_ASSERT(match.hasMatch());
                majorVersion = match.captured(1).toInt();
                minorVersion = match.captured(2).toInt();
            }

            qDebug().noquote()
                    << QStringLiteral("Supported OpenGL version: %1.%2")
                               .arg(QString::number(majorVersion), QString::number(minorVersion));

            m_openGLShaderAvailable = QOpenGLShaderProgram::hasOpenGLShaderPrograms(pContext);

            // With EGLFS there is always exactly one native window and one EGL window surface
            // OpenGL windows cannot be embedded into our QWidgets main window we already have.
            // That's why m_openGlesAvailable is not set to true. TODO: use GL Widgets for all
            // https://doc.qt.io/qt-6/embedded-linux.html
            // See https://doc.qt.io/qt-6/qguiapplication.html#platformName-prop for possible values
            bool isEglfs = QGuiApplication::platformName() == QStringLiteral("eglfs");
            bool isOpenGles = pContext->isOpenGLES();

            if (isEglfs) {
                m_openGLVersion = QStringLiteral("EGLFS ");
            } else if (isOpenGles) {
                m_openGLVersion = QStringLiteral("ES ");
            }
            // else m_openGLVersion is still empty

            //: This refers to a missing openGL version
            m_openGLVersion += majorVersion == 0 ? tr("None") : versionString;

            if (!isEglfs) {
                // Qt >= 5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                int combinedVersion = majorVersion * 100 + minorVersion;
                m_openGlesAvailable = isOpenGles && combinedVersion >= 200;
                m_openGlAvailable = !isOpenGles && combinedVersion >= 201;
            }

            if (!rendererString.isEmpty()) {
                m_openGLVersion += QStringLiteral(" (") + rendererString + QChar(')');
            }
        } else {
            qDebug() << "QOpenGLContext::currentContext() returns nullptr";
        }
        widget->doneCurrent();
        widget->hide();
    }
#else
    QGLWidget* pGlWidget = SharedGLContext::getWidget();
    if (pGlWidget && pGlWidget->isValid()) {
        // will be false if SafeMode is enabled

        pGlWidget->show();
        // Without a makeCurrent, hasOpenGLShaderPrograms returns false on Qt 5.
        // and QGLFormat::openGLVersionFlags() returns the maximum known version
        pGlWidget->makeCurrent();

        QGLFormat::OpenGLVersionFlags version = QGLFormat::openGLVersionFlags();

        auto rendererString = QString();
        if (QOpenGLContext::currentContext()) {
            auto glFunctions = QOpenGLFunctions();

            glFunctions.initializeOpenGLFunctions();
            QString versionString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_VERSION))));
            QString vendorString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_VENDOR))));
            rendererString = QString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_RENDERER))));

            // Either GL or GL ES Version is set, not both.
            qDebug() << QString("openGLVersionFlags 0x%1").arg(version, 0, 16) << versionString << vendorString << rendererString;
        } else {
            qDebug() << "QOpenGLContext::currentContext() returns nullptr";
            qDebug() << "pGlWidget->->windowHandle() =" << pGlWidget->windowHandle();
        }

        int majorGlVersion = 0;
        int minorGlVersion = 0;
        int majorGlesVersion = 0;
        int minorGlesVersion = 0;
        if (version == QGLFormat::OpenGL_Version_None) {
            m_openGLVersion = "None";
        } else if (version & QGLFormat::OpenGL_Version_4_3) {
            majorGlVersion = 4;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_4_2) {
            majorGlVersion = 4;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_4_1) {
            majorGlVersion = 4;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_4_0) {
            majorGlVersion = 4;
            minorGlVersion = 0;
        } else if (version & QGLFormat::OpenGL_Version_3_3) {
            majorGlVersion = 3;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_3_2) {
            majorGlVersion = 3;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_3_1) {
            majorGlVersion = 3;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_3_0) {
            majorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_2_1) {
            majorGlVersion = 2;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_2_0) {
            majorGlVersion = 2;
            minorGlVersion = 0;
        } else if (version & QGLFormat::OpenGL_Version_1_5) {
            majorGlVersion = 1;
            minorGlVersion = 5;
        } else if (version & QGLFormat::OpenGL_Version_1_4) {
            majorGlVersion = 1;
            minorGlVersion = 4;
        } else if (version & QGLFormat::OpenGL_Version_1_3) {
            majorGlVersion = 1;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_1_2) {
            majorGlVersion = 1;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_1_1) {
            majorGlVersion = 1;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_Version_2_0) {
            m_openGLVersion = "ES 2.0";
            majorGlesVersion = 2;
            minorGlesVersion = 0;
        } else if (version & QGLFormat::OpenGL_ES_CommonLite_Version_1_1) {
            if (version & QGLFormat::OpenGL_ES_Common_Version_1_1) {
                m_openGLVersion = "ES 1.1";
            } else {
                m_openGLVersion = "ES Common Lite 1.1";
            }
            majorGlesVersion = 1;
            minorGlesVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_Common_Version_1_1) {
            m_openGLVersion = "ES Common Lite 1.1";
            majorGlesVersion = 1;
            minorGlesVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_CommonLite_Version_1_0) {
            if (version & QGLFormat::OpenGL_ES_Common_Version_1_0) {
                m_openGLVersion = "ES 1.0";
            } else {
                m_openGLVersion = "ES Common Lite 1.0";
            }
            majorGlesVersion = 1;
            minorGlesVersion = 0;
        } else if (version & QGLFormat::OpenGL_ES_Common_Version_1_0) {
            m_openGLVersion = "ES Common Lite 1.0";
            majorGlesVersion = 1;
            minorGlesVersion = 0;
        } else {
            m_openGLVersion = QString("Unknown 0x%1")
                .arg(version, 0, 16);
        }

        if (majorGlVersion != 0) {
            m_openGLVersion = QString::number(majorGlVersion) + "."
                    + QString::number(minorGlVersion);

#if !defined(QT_NO_OPENGL) && !defined(QT_OPENGL_ES_2)
            if (majorGlVersion * 100 + minorGlVersion >= 201) {
                // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                m_openGlAvailable = true;
            }
#endif
        } else {
            if (majorGlesVersion * 100 + minorGlesVersion >= 200) {
                // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                m_openGlesAvailable = true;
            }
        }

        m_openGLShaderAvailable =
                QGLShaderProgram::hasOpenGLShaderPrograms(
                        pGlWidget->context());

        if (!rendererString.isEmpty()) {
            m_openGLVersion += " (" + rendererString + ")";
        }

        pGlWidget->hide();
    }
#endif
    evaluateWidgets();
    m_time.start();
}

WaveformWidgetFactory::~WaveformWidgetFactory() {
    if (m_pDisplayLinkFrameDriver) {
        // Unregisters itself from the window (if that still exists).
        delete m_pDisplayLinkFrameDriver;
        m_pDisplayLinkFrameDriver = nullptr;
    }
    if (m_vsyncThread) {
        delete m_vsyncThread;
    }
}

bool WaveformWidgetFactory::setConfig(UserSettingsPointer config) {
    m_config = config;
    if (!m_config) {
        return false;
    }

    bool ok = false;

    int frameRate = m_config->getValue(kFrameRateKey, m_frameRate);
    // Must match the limit in setFrameRate() and the spin box in
    // dlgprefwaveformdlg.ui, both of which allow 240. Commit 122bcdc50a
    // raised those two but missed this one, so any configured rate above 120
    // was silently reduced on every start.
    m_frameRate = math_clamp(frameRate, 1, 240);

    int endTime = m_config->getValueString(kEndOfTrackWarningKey).toInt(&ok);
    if (ok) {
        setEndOfTrackWarningTime(endTime);
    } else {
        m_config->setValue(kEndOfTrackWarningKey, m_endOfTrackWarningTime);
    }

    double defaultZoom = m_config->getValueString(kDefaultZoomKey).toDouble(&ok);
    if (ok) {
        setDefaultZoom(defaultZoom);
    } else{
        m_config->setValue(kDefaultZoomKey, m_defaultZoom);
    }

    bool zoomSync = m_config->getValue(kZoomSyncKey, m_zoomSync);
    setZoomSync(zoomSync);

    int beatGridAlpha =
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("beatGridAlpha")),
                    m_beatGridAlpha);
    setDisplayBeatGridAlpha(beatGridAlpha);

    WaveformWidgetType::Type type = static_cast<WaveformWidgetType::Type>(
            m_config->getValueString(kWaveformTypeKey).toInt(&ok));
    // Store the widget type on m_configType for later initialization.
    // We will initialize the objects later because of a problem with GL on QT 5.14.2 on Windows
    if (!ok || !setWidgetType(type, &m_configType)) {
        setWidgetType(WaveformWidgetType::RGB, &m_configType);
    }

    for (int i = 0; i < BandCount; i++) {
        m_visualGain[i] = m_config->getValue(visualGainKey(i), kVisualGainDefault[i]);
    }
    m_overviewNormalized = m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("OverviewNormalized")),
            kOverviewNormalizedDefault);

    emit visualGainChanged(
            m_visualGain[BandIndex::AllBand],
            m_visualGain[BandIndex::Low],
            m_visualGain[BandIndex::Mid],
            m_visualGain[BandIndex::High]);
    emit overviewScalingChanged();

    m_playMarkerPosition =
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("PlayMarkerPosition")),
                    WaveformWidgetRenderer::s_defaultPlayMarkerPosition);
    setPlayMarkerPosition(m_playMarkerPosition);

    int untilMarkShowBeats =
            m_config->getValueString(
                            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowBeats")))
                    .toInt(&ok);
    if (ok) {
        setUntilMarkShowBeats(static_cast<bool>(untilMarkShowBeats));
    } else {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowBeats")),
                m_untilMarkShowBeats);
    }
    int untilMarkShowTime =
            m_config->getValueString(
                            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowTime")))
                    .toInt(&ok);
    if (ok) {
        setUntilMarkShowTime(static_cast<bool>(untilMarkShowTime));
    } else {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowTime")),
                m_untilMarkShowTime);
    }

    setUntilMarkAlign(toUntilMarkAlign(
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkAlign")),
                    toUntilMarkAlignIndex(m_untilMarkAlign))));
    setUntilMarkTextPointSize(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextPointSize")),
            m_untilMarkTextPointSize));
    setUntilMarkTextHeightLimit(toUntilMarkTextHeightLimit(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextHeightLimit")),
            toUntilMarkTextHeightLimitIndex(m_untilMarkTextHeightLimit))));
    setStemReorderOnChange(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("stem_reorder_on_change")),
            true));
    setStemOpacity(static_cast<float>(
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_opacity")),
                    0.75)));
    setStemOutlineOpacity(static_cast<float>(
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_outline_opacity")),
                    0.15)));
    setStemSplitTracks(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("stem_split_tracks")),
            false));

    return true;
}

void WaveformWidgetFactory::destroyWidgets() {
    for (auto& holder : m_waveformWidgetHolders) {
        WaveformWidgetAbstract* pWidget = holder.m_waveformWidget;
        holder.m_waveformWidget = nullptr;
        delete pWidget;
    }
    m_waveformWidgetHolders.clear();
}

void WaveformWidgetFactory::addVuMeter(WVuMeterLegacy* pVuMeter) {
    // Do not hold the pointer to of timer listeners since they may be deleted.
    // We don't activate update() or repaint() directly so listener widgets
    // can decide whether to paint or not.
    connect(this,
            &WaveformWidgetFactory::waveformUpdateTick,
            pVuMeter,
            &WVuMeterLegacy::maybeUpdate,
            Qt::DirectConnection);
}

void WaveformWidgetFactory::addVuMeter(WVuMeterBase* pVuMeter) {
    // WVuMeterGLs to be rendered and swapped from the vsync thread
    connect(this,
            &WaveformWidgetFactory::renderVuMeters,
            pVuMeter,
            &WVuMeterBase::render);
    connect(this,
            &WaveformWidgetFactory::swapVuMeters,
            pVuMeter,
            &WVuMeterBase::swap);
}

void WaveformWidgetFactory::slotSkinLoaded() {
#ifdef MIXXX_USE_QOPENGL
    if (m_pDisplayLinkFrameDriver) {
        tryReattachDisplayLink(30);
    }
#endif
    setWidgetTypeFromConfig();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && defined __WINDOWS__
    // This regenerates the waveforms twice because of a bug found on Windows
    // where the first one fails.
    // The problem is that the window of the widget thinks that it is not exposed.
    // (https://doc.qt.io/qt-5/qwindow.html#exposeEvent )
    setWidgetTypeFromConfig();
#endif
}

bool WaveformWidgetFactory::setWaveformWidget(WWaveformViewer* viewer,
                                              const QDomElement& node,
                                              const SkinContext& parentContext) {
    int index = findIndexOf(viewer);
    if (index != -1) {
        qDebug() << "WaveformWidgetFactory::setWaveformWidget - "\
                    "viewer already have a waveform widget but it's not found by the factory !";
        delete viewer->getWaveformWidget();
    }

    // Cast to widget done just after creation because it can't be perform in
    // constructor (pure virtual)
    WaveformWidgetAbstract* waveformWidget = createWaveformWidget(m_type, viewer);
    viewer->setWaveformWidget(waveformWidget);
    viewer->setup(node, parentContext);

    // create new holder
    WaveformWidgetHolder holder(waveformWidget, viewer, node, &parentContext);
    if (index == -1) {
        // add holder
        m_waveformWidgetHolders.push_back(std::move(holder));
        index = static_cast<int>(m_waveformWidgetHolders.size()) - 1;
    } else {
        // update holder
        DEBUG_ASSERT(index >= 0);
        m_waveformWidgetHolders[index] = std::move(holder);
    }

    viewer->setZoom(m_defaultZoom);
    viewer->setDisplayBeatGridAlpha(m_beatGridAlpha);
    viewer->setPlayMarkerPosition(m_playMarkerPosition);
    waveformWidget->resize(viewer->width(), viewer->height());
    waveformWidget->getWidget()->show();
    viewer->update();

    qDebug() << "WaveformWidgetFactory::setWaveformWidget - waveform widget added in factory, index" << index;

    return true;
}

void WaveformWidgetFactory::setFrameRate(int frameRate) {
    m_frameRate = math_clamp(frameRate, 1, 240);
    if (m_config) {
        m_config->setValue(kFrameRateKey, m_frameRate);
    }
    if (m_vsyncThread) {
        m_vsyncThread->setSyncIntervalTimeMicros(static_cast<int>(1e6 / m_frameRate));
    }
    if (m_pDisplayLinkFrameDriver) {
        m_pDisplayLinkFrameDriver->setFrameRate(m_frameRate);
    }
}

void WaveformWidgetFactory::setEndOfTrackWarningTime(int endTime) {
    m_endOfTrackWarningTime = endTime;
    if (m_config) {
        m_config->setValue(kEndOfTrackWarningKey, m_endOfTrackWarningTime);
    }
}

bool WaveformWidgetFactory::setWidgetType(WaveformWidgetType::Type type) {
    return setWidgetType(type, &m_type);
}

bool WaveformWidgetFactory::setWidgetType(
        WaveformWidgetType::Type type,
        WaveformWidgetType::Type* pCurrentType) {
    if (type == *pCurrentType) {
        return true;
    }

    // check if type is acceptable
    int index = findHandleIndexFromType(type);
    bool isAcceptable = index > -1;
    *pCurrentType = isAcceptable ? type : WaveformWidgetType::Empty;
    if (m_config) {
        m_configType = *pCurrentType;
        // TODO do not set "Empty"?
        m_config->setValue(kWaveformTypeKey, *pCurrentType);
    }
    return isAcceptable;
}

bool WaveformWidgetFactory::widgetTypeSupportsUntilMark() const {
    switch (m_configType) {
    case WaveformWidgetType::RGB:
    case WaveformWidgetType::Filtered:
    case WaveformWidgetType::Simple:
    case WaveformWidgetType::HSV:
    case WaveformWidgetType::Stacked:
        return true;
    default:
        break;
    }
    return false;
}

bool WaveformWidgetFactory::widgetTypeSupportsStems() const {
    switch (m_configType) {
    case WaveformWidgetType::RGB:
    case WaveformWidgetType::Filtered:
    case WaveformWidgetType::Simple:
    case WaveformWidgetType::HSV:
    case WaveformWidgetType::Stacked:
        return true;
    default:
        break;
    }
    return false;
}

/// Called by MixxxMainWindow after skin loaded via slotSkinLoaded
bool WaveformWidgetFactory::setWidgetTypeFromConfig() {
    int empty = findHandleIndexFromType(WaveformWidgetType::Empty);
    int desired = findHandleIndexFromType(m_configType);
    if (desired == -1) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromConfig"
                 << " - configured type" << static_cast<int>(m_configType)
                 << "not found -- using 'EmptyWaveform'";
        desired = empty;
    }

    // Because of a previous bug, "use_hardware_acceleration" may be 0
    // which prevents loading the desired type.
    // Fix: enable acceleration if available and if type requires it.
    auto backend = getBackendFromConfig();
    // True if type and available backends support acceleration
    bool typeSupportsAcceleration = widgetTypeSupportsAcceleration(m_configType);
    bool typeSupportsSoftware = widgetTypeSupportsSoftware(m_configType);
    if (backend == WaveformWidgetBackend::None &&
            typeSupportsAcceleration && !typeSupportsSoftware) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromConfig"
                 << " - select accelerated backend because configured type"
                 << WaveformWidgetAbstractHandle::getDisplayName(m_configType)
                 << "requires it.";
        setDefaultBackend();
    }

    return setWidgetTypeFromHandle(desired, true);
}

bool WaveformWidgetFactory::setWidgetTypeFromHandle(int handleIndex, bool force) {
    if (handleIndex < 0 || handleIndex >= m_waveformWidgetHandles.size()) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromHandle"
                    " - invalid handle --> using 'EmptyWaveform'";
        // fallback empty type
        setWidgetType(WaveformWidgetType::Empty);
        return false;
    }

    WaveformWidgetAbstractHandle& handle = m_waveformWidgetHandles[handleIndex];
    if (handle.m_type == m_type && !force) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromHandle - type"
                 << handle.getDisplayName() << "already in use";
        return true;
    }

    // change the type
    setWidgetType(handle.m_type);

    m_skipRender = true;

    //re-create/setup all waveform widgets
    for (auto& holder : m_waveformWidgetHolders) {
        WaveformWidgetAbstract* previousWidget = holder.m_waveformWidget;
        TrackPointer pTrack = previousWidget->getTrackInfo();
        //previousWidget->hold();
        double previousZoom = previousWidget->getZoom();
        double previousPlayMarkerPosition = previousWidget->getPlayMarkerPosition();
        int previousbeatgridAlpha = previousWidget->getBeatGridAlpha();
        delete previousWidget;
        WWaveformViewer* viewer = holder.m_waveformViewer;
        WaveformWidgetAbstract* widget = createWaveformWidget(m_type, holder.m_waveformViewer);
        holder.m_waveformWidget = widget;
        viewer->setWaveformWidget(widget);
        viewer->setup(holder.m_skinNodeCache, holder.m_skinContextCache);
        viewer->setZoom(previousZoom);
        viewer->setPlayMarkerPosition(previousPlayMarkerPosition);
        viewer->setDisplayBeatGridAlpha(previousbeatgridAlpha);
        // resize() doesn't seem to get called on the widget. I think Qt skips
        // it since the size didn't change.
        //viewer->resize(viewer->size());
        widget->resize(viewer->width(), viewer->height());
        widget->setTrack(pTrack);
        widget->getWidget()->show();
        viewer->update();
    }

    m_skipRender = false;
    return true;
}

void WaveformWidgetFactory::setDefaultZoom(double zoom) {
    m_defaultZoom = math_clamp(zoom, WaveformWidgetRenderer::s_waveformMinZoom,
                               WaveformWidgetRenderer::s_waveformMaxZoom);
    if (m_config) {
        m_config->setValue(kDefaultZoomKey, m_defaultZoom);
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformViewer->setZoom(m_defaultZoom);
    }
}

void WaveformWidgetFactory::setZoomSync(bool sync) {
    m_zoomSync = sync;
    if (m_config) {
        m_config->setValue(kZoomSyncKey, m_zoomSync);
    }

    if (m_waveformWidgetHolders.size() == 0) {
        return;
    }

    double refZoom = m_waveformWidgetHolders[0].m_waveformWidget->getZoom();
    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformViewer->setZoom(refZoom);
    }
}

void WaveformWidgetFactory::setDisplayBeatGridAlpha(int alpha) {
    m_beatGridAlpha = alpha;
    if (m_waveformWidgetHolders.size() == 0) {
        return;
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformWidget->setDisplayBeatGridAlpha(m_beatGridAlpha);
    }
}

void WaveformWidgetFactory::setVisualGain(BandIndex index, double gain) {
    m_visualGain[index] = gain;
    if (m_config) {
        m_config->setValue(visualGainKey(index), m_visualGain[index]);
    }
    emit visualGainChanged(
            m_visualGain[BandIndex::AllBand],
            m_visualGain[BandIndex::Low],
            m_visualGain[BandIndex::Mid],
            m_visualGain[BandIndex::High]);
    if (index == BandIndex::AllBand && !m_overviewNormalized) {
        emit overviewScalingChanged();
    }
}

double WaveformWidgetFactory::getVisualGain(BandIndex index) const {
    return m_visualGain[index];
}

// static
double WaveformWidgetFactory::getVisualGainDefault(BandIndex index) {
    return kVisualGainDefault[index];
}

void WaveformWidgetFactory::setOverviewNormalized(bool normalize) {
    m_overviewNormalized = normalize;
    if (m_config) {
        m_config->set(ConfigKey(kWaveformGroup, QStringLiteral("OverviewNormalized")),
                ConfigValue(m_overviewNormalized));
    }
    emit overviewScalingChanged();
}

// static
bool WaveformWidgetFactory::isOverviewNormalizedDefault() {
    return kOverviewNormalizedDefault;
}

void WaveformWidgetFactory::setPlayMarkerPosition(double position) {
    m_playMarkerPosition = position;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("PlayMarkerPosition")),
                m_playMarkerPosition);
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformWidget->setPlayMarkerPosition(m_playMarkerPosition);
    }
}

void WaveformWidgetFactory::notifyZoomChange(WWaveformViewer* viewer) {
    WaveformWidgetAbstract* pWaveformWidget = viewer->getWaveformWidget();
    if (pWaveformWidget == nullptr || !isZoomSync()) {
        return;
    }
    double refZoom = pWaveformWidget->getZoom();

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        if (holder.m_waveformViewer != viewer) {
            holder.m_waveformViewer->setZoom(refZoom);
        }
    }
}

void WaveformWidgetFactory::renderSelf() {
    ScopedTimer t(QStringLiteral("WaveformWidgetFactory::render() %1waveforms"),
            static_cast<int>(m_waveformWidgetHolders.size()));

    // --- TEMPORARY PERF INSTRUMENTATION (WAVEPERF) ---
    // Records the wall-clock interval between consecutive render passes so we
    // can report real frame-time percentiles, not just an averaged fps counter.
    // Only active in --developer mode. Remove before any product change.
    static PerformanceTimer s_perfFrameTimer;
    static std::vector<double> s_perfIntervalsMs;
    static bool s_perfStarted = false;
    const bool perfEnabled = CmdlineArgs::Instance().getDeveloper();
    if (perfEnabled) {
        if (!s_perfStarted) {
            s_perfFrameTimer.start();
            s_perfStarted = true;
        } else {
            s_perfIntervalsMs.push_back(
                    static_cast<double>(s_perfFrameTimer.restart().toIntegerMicros()) / 1000.0);
        }
    }

    if (!m_skipRender) {
        if (m_type) {   // no regular updates for an empty waveform
            // next rendered frame is displayed after next buffer swap and than after VSync
            QVarLengthArray<bool, 10> shouldRenderWaveforms(
                    static_cast<int>(m_waveformWidgetHolders.size()));
            for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                    i < m_waveformWidgetHolders.size();
                    i++) {
                WaveformWidgetAbstract* pWaveformWidget = m_waveformWidgetHolders[i].m_waveformWidget;
                // Don't bother doing the pre-render work if we aren't going to
                // render this widget.
                bool shouldRender = shouldRenderWaveform(pWaveformWidget);
                shouldRenderWaveforms[static_cast<int>(i)] = shouldRender;
                if (!shouldRender) {
                    continue;
                }
                // Calculate play position for the new Frame in following run
                PerformanceTimer tPre;
                tPre.start();
                pWaveformWidget->preRender(m_vsyncThread);
                g_perfPreRenderMs +=
                        static_cast<double>(tPre.elapsed().toIntegerMicros()) / 1000.0;
            }
            //qDebug() << "prerender" << m_vsyncThread->elapsed();

            // It may happen that there is an artificially delayed due to
            // anti tearing driver settings
            // all render commands are delayed until the swap from the previous run is executed
            for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                    i < m_waveformWidgetHolders.size();
                    i++) {
                WaveformWidgetAbstract* pWaveformWidget = m_waveformWidgetHolders[i].m_waveformWidget;
                if (!shouldRenderWaveforms[static_cast<int>(i)]) {
                    continue;
                }
                PerformanceTimer tRen;
                tRen.start();
                ++g_perfRenderedCount;
                pWaveformWidget->render();
                g_perfRenderMs +=
                        static_cast<double>(tRen.elapsed().toIntegerMicros()) / 1000.0;
                //qDebug() << "render" << i << m_vsyncThread->elapsed();
            }

            // TEMPORARY BENCHMARK HOOK: see benchProbePixels() above. Runs
            // after every waveform of this frame has been rendered and before
            // the buffers are swapped, so it reads exactly the frame that is
            // about to be shown, for every visible deck.
            static const int probeSetting =
                    qEnvironmentVariableIntValue("MIXXX_BENCH_PIXELPROBE");
            if (probeSetting > 0) {
                const int probeEvery = probeSetting > 1
                        ? probeSetting
                        : std::max(1, static_cast<int>(m_frameRate));
                static int s_probeCountdown = 0;
                static bool s_probeAnnounced = false;
                if (!s_probeAnnounced) {
                    s_probeAnnounced = true;
                    qDebug().nospace() << "BENCHHIT MIXXX_BENCH_PIXELPROBE=" << probeSetting
                                       << " probingEvery=" << probeEvery << " frames"
                                       << " (frame times of this run are NOT comparable)";
                }
                if (--s_probeCountdown <= 0) {
                    s_probeCountdown = probeEvery;
                    for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                            i < m_waveformWidgetHolders.size();
                            i++) {
                        if (shouldRenderWaveforms[static_cast<int>(i)]) {
                            benchProbePixels(m_waveformWidgetHolders[i].m_waveformWidget);
                        }
                    }
                }
            }
        }

        // WSpinnys are also double-buffered WGLWidgets, like all the waveform
        // renderers. Render all the WSpinny widgets now.
        PerformanceTimer tExtra;
        tExtra.start();
        emit renderSpinnies(m_vsyncThread);
        // Same for WVuMeterGL. Note that we are either using WVuMeter or WVuMeterGL.
        // If we are using WVuMeter, this does nothing
        emit renderVuMeters(m_vsyncThread);
        g_perfExtrasMs += static_cast<double>(tExtra.elapsed().toIntegerMicros()) / 1000.0;

        // Notify all other waveform-like widgets (e.g. WSpinny's) that they should
        // update.
        PerformanceTimer tTick;
        tTick.start();
        emit waveformUpdateTick();
        g_perfTickMs += static_cast<double>(tTick.elapsed().toIntegerMicros()) / 1000.0;
        //qDebug() << "emit" << m_vsyncThread->elapsed() - t1;

        m_frameCnt += 1.0f;
        mixxx::Duration timeCnt = m_time.elapsed();
        if (timeCnt > mixxx::Duration::fromSeconds(1)) {
            m_time.start();
            const int perfFrames = static_cast<int>(m_frameCnt);
            m_frameCnt = m_frameCnt * 1000 / timeCnt.toIntegerMillis(); // latency correction
            const double perfFps = m_frameCnt;
            emit waveformMeasured(m_frameCnt, m_vsyncThread->droppedFrames());
            m_frameCnt = 0.0;

            // --- TEMPORARY PERF INSTRUMENTATION (WAVEPERF) ---
            if (perfEnabled) {
                static int s_perfLastDrops = 0;
                const int drops = m_vsyncThread->droppedFrames();
                const VSyncThread::PhaseErrorStats phaseErr =
                        m_vsyncThread->takePhaseErrorStats();
                auto& v = s_perfIntervalsMs;
                double mean = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, maxMs = 0.0;
                if (!v.empty()) {
                    for (double x : v) {
                        mean += x;
                    }
                    mean /= static_cast<double>(v.size());
                    std::sort(v.begin(), v.end());
                    const auto at = [&v](double q) {
                        size_t i = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
                        return v[i];
                    };
                    p50 = at(0.50);
                    p95 = at(0.95);
                    p99 = at(0.99);
                    maxMs = v.back();
                }
                // Widget geometry: frame cost must be normalised by the area
                // actually rasterised, otherwise runs are not comparable and we
                // cannot separate GPU fill rate from CPU/draw-call overhead.
                int wfW = 0;
                int wfH = 0;
                double wfDpr = 0.0;
                double totalDevicePx = 0.0;
                for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
                    const WaveformWidgetAbstract* pW = holder.m_waveformWidget;
                    if (!pW) {
                        continue;
                    }
                    const double dpr = static_cast<double>(pW->getDevicePixelRatio());
                    totalDevicePx += static_cast<double>(pW->getWidth()) *
                            static_cast<double>(pW->getHeight()) * dpr * dpr;
                    wfW = pW->getWidth();
                    wfH = pW->getHeight();
                    wfDpr = dpr;
                }

                qDebug().nospace()
                        << "WAVEPERF"
                        << " frames=" << perfFrames
                        << " fps=" << QString::number(perfFps, 'f', 1)
                        << " samples=" << static_cast<int>(v.size())
                        << " dropsTotal=" << drops
                        << " dropsDelta=" << (drops - s_perfLastDrops)
                        << " meanMs=" << QString::number(mean, 'f', 2)
                        << " p50Ms=" << QString::number(p50, 'f', 2)
                        << " p95Ms=" << QString::number(p95, 'f', 2)
                        << " p99Ms=" << QString::number(p99, 'f', 2)
                        << " maxMs=" << QString::number(maxMs, 'f', 2)
                        << " widgets=" << static_cast<int>(m_waveformWidgetHolders.size())
                        << " type=" << static_cast<int>(m_type)
                        << " options=" << g_perfEffectiveOptions
                        << " targetFps=" << m_frameRate
                        << " wLogical=" << wfW
                        << " hLogical=" << wfH
                        << " dpr=" << QString::number(wfDpr, 'f', 2)
                        << " totalDevicePx=" << static_cast<qint64>(totalDevicePx)
                        // per-frame mean cost of each phase of the frame, in ms
                        << " preRenderMs=" << QString::number(perfFrames > 0
                                           ? g_perfPreRenderMs / perfFrames : 0.0, 'f', 3)
                        << " renderMs=" << QString::number(perfFrames > 0
                                           ? g_perfRenderMs / perfFrames : 0.0, 'f', 3)
                        << " extrasMs=" << QString::number(perfFrames > 0
                                           ? g_perfExtrasMs / perfFrames : 0.0, 'f', 3)
                        << " swapMs=" << QString::number(perfFrames > 0
                                           ? g_perfSwapMs / perfFrames : 0.0, 'f', 3)
                        << " dispatchMs=" << QString::number(perfFrames > 0
                                           ? g_perfDispatchMs / perfFrames : 0.0, 'f', 3)
                        // surfaces actually touched per frame (visible decks),
                        // as opposed to the number merely registered
                        << " renderedPerFrame=" << QString::number(perfFrames > 0
                                           ? static_cast<double>(g_perfRenderedCount) / perfFrames
                                           : 0.0, 'f', 2)
                        << " swappedPerFrame=" << QString::number(perfFrames > 0
                                           ? static_cast<double>(g_perfSwappedCount) / perfFrames
                                           : 0.0, 'f', 2)
                        << " tickMs=" << QString::number(perfFrames > 0
                                           ? g_perfTickMs / perfFrames : 0.0, 'f', 3)
                        << " visualsMs=" << QString::number(perfFrames > 0
                                           ? g_perfVisualsMs / perfFrames : 0.0, 'f', 3)
                        << " updateMs=" << QString::number(perfFrames > 0
                                           ? g_perfUpdateMs / perfFrames : 0.0, 'f', 3)
                        // Renamed from the misleading "pllDeltaUs": this is
                        // the sync interval that was ASKED for (1e6/FrameRate),
                        // not the period the PLL settled on. The latter is
                        // pllPeriodUs below.
                        << " syncIntervalUs=" << static_cast<int>(
                                   m_vsyncThread->getSyncInterval().count())
                        << " pllPeriodUs=" << QString::number(
                                   m_vsyncThread->pllPeriodMicros(), 'f', 1)
                        // Phase error is the metric that separates "the cause
                        // is gone" from "the symptom is held down": a period
                        // clamped to the display can look perfect while the
                        // loop still fights it every frame. Per second, not
                        // per ten seconds - a 90 s run would otherwise carry
                        // nine samples.
                        << " phaseErrN=" << phaseErr.count
                        << " phaseErrMeanUs=" << QString::number(phaseErr.meanUs, 'f', 1)
                        << " phaseErrSdUs=" << QString::number(phaseErr.sdUs, 'f', 1)
                        << " phaseErrWorstUs=" << QString::number(phaseErr.worstUs, 'f', 1);
                g_perfTickMs = 0.0;
                g_perfVisualsMs = 0.0;
                g_perfUpdateMs = 0.0;
                g_perfDispatchMs = 0.0;
                g_perfRenderedCount = 0;
                g_perfSwappedCount = 0;
                g_perfPreRenderMs = 0.0;
                g_perfRenderMs = 0.0;
                g_perfExtrasMs = 0.0;
                g_perfSwapMs = 0.0;
                s_perfLastDrops = drops;
                v.clear();
            }
        }
    }

    PerformanceTimer tVis;
    tVis.start();
    m_pVisualsManager->process(m_endOfTrackWarningTime);
    m_pGuiTick->process();
    g_perfVisualsMs += static_cast<double>(tVis.elapsed().toIntegerMicros()) / 1000.0;

    //qDebug() << "refresh end" << m_vsyncThread->elapsed();
}

void WaveformWidgetFactory::render() {
    renderSelf();
    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::swapSelf() {
    ScopedTimer t(QStringLiteral("WaveformWidgetFactory::swap() %1waveforms"),
            static_cast<int>(m_waveformWidgetHolders.size()));

    // Do this in an extra slot to be sure to hit the desired interval
    if (!m_skipRender) {
        if (m_type) {   // no regular updates for an empty waveform
            // Show rendered buffer from last render() run
            //qDebug() << "swap() start" << m_vsyncThread->elapsed();
            for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
                WaveformWidgetAbstract* pWaveformWidget = holder.m_waveformWidget;

                // Don't swap invalid / invisible widgets or widgets with an
                // unexposed window. Prevents continuous log spew of
                // "QOpenGLContext::swapBuffers() called with non-exposed
                // window, behavior is undefined" on Qt5. See issue #9360.
                if (!shouldRenderWaveform(pWaveformWidget)) {
                    continue;
                }
                WGLWidget* pGlw = pWaveformWidget->getGLWidget();
                if (pGlw != nullptr) {
                    PerformanceTimer tSwap;
                    tSwap.start();
                    ++g_perfSwappedCount;
                    pGlw->makeCurrentIfNeeded();
                    pGlw->swapBuffers();
                    // TEMPORARY BENCHMARK HOOK: releasing the context after
                    // every surface forces the next one to re-acquire it, and
                    // each acquisition runs Apple's GL->Metal resource sync.
                    static const bool skipDoneCurrent =
                            !qEnvironmentVariableIsEmpty("MIXXX_BENCH_NO_DONECURRENT");
                    if (!skipDoneCurrent) {
                        pGlw->doneCurrent();
                    }
                    g_perfSwapMs +=
                            static_cast<double>(tSwap.elapsed().toIntegerMicros()) / 1000.0;
                }
                //qDebug() << "swap x" << m_vsyncThread->elapsed();
            }
        }
        // WSpinnys are also double-buffered QGLWidgets, like all the waveform
        // renderers. Swap all the WSpinny widgets now.
        emit swapSpinnies();
        // Same for WVuMeterGL. Note that we are either using WVuMeter or WVuMeterGL
        // If we are using WVuMeter, this does nothing
        emit swapVuMeters();
    }
}

void WaveformWidgetFactory::swap() {
    swapSelf();
    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::swapAndRender() {
    // TEMPORARY INSTRUMENTATION: how long did this frame's signal wait in the
    // GUI thread's event queue? Large values mean the main thread was busy with
    // something else and the frame missed its vsync slot.
    g_perfDispatchMs +=
            static_cast<double>(m_vsyncThread->m_emitTimer.elapsed().toIntegerMicros()) / 1000.0;

    // used for PLL
    PerformanceTimer tUpd;
    tUpd.start();
    WGLWidget* widget = SharedGLContext::getWidget();
    widget->getOpenGLWindow()->update();
    g_perfUpdateMs += static_cast<double>(tUpd.elapsed().toIntegerMicros()) / 1000.0;

    swapSelf();
    renderSelf();

    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::tryReattachDisplayLink(int attemptsLeft) {
#ifdef MIXXX_USE_QOPENGL
    if (!m_pDisplayLinkFrameDriver) {
        return;
    }
    // The driver is created on the 3x3 shared-context helper window. In a
    // normal window the compositor never serves update requests for it, so no
    // frame is ever driven and the waveforms stay blank; only in full screen
    // did it happen to work. Move the driver onto a real, exposed waveform
    // window as soon as one exists.
    //
    // This must NOT be done from the frame callback: without a frame there is
    // nothing to move it from, and without moving it there is no frame.
    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        WaveformWidgetAbstract* pWidget = holder.m_waveformWidget;
        if (!pWidget) {
            continue;
        }
        WGLWidget* pGlw = pWidget->getGLWidget();
        if (pGlw && pGlw->getOpenGLWindow() && pGlw->shouldRender()) {
            m_pDisplayLinkFrameDriver->reattachTo(pGlw->getOpenGLWindow());
            return;
        }
    }
    if (attemptsLeft > 0) {
        // Windows are realised lazily in their show event; try again shortly.
        QTimer::singleShot(100, this, [this, attemptsLeft]() {
            tryReattachDisplayLink(attemptsLeft - 1);
        });
    } else {
        qWarning() << "DisplayLinkFrameDriver: no exposed waveform window found,"
                   << "frames stay on the shared-context window";
    }
#else
    Q_UNUSED(attemptsLeft);
#endif
}

void WaveformWidgetFactory::slotDisplayLinkFrame() {
    // Same work as swapAndRender(), but without the cross thread semaphore
    // handshake and without triggering a repaint of the shared GL window:
    // the frame pacing comes from the platform display link
    // (QWindow::requestUpdate()), which the DisplayLinkFrameDriver has already
    // re-armed for the next frame.
    swapSelf();
    renderSelf();
}

void WaveformWidgetFactory::slotFrameSwapped() {
#ifdef MIXXX_USE_QOPENGL
    if (m_vsyncThread->pllInitializing()) {
        // continuously trigger redraws during PLL init
        WGLWidget* widget = SharedGLContext::getWidget();
        widget->getOpenGLWindow()->update();
    }
    // update the phase-locked-loop
    m_vsyncThread->updatePLL();
#endif
}

void WaveformWidgetFactory::addHandle(
        QHash<WaveformWidgetType::Type, QList<WaveformWidgetBackend>>&
                collectedHandles,
        WaveformWidgetType::Type type,
        const WaveformWidgetVars& vars) const {
    WaveformWidgetBackend backend = WaveformWidgetBackend::None;
    bool active = true;
    if (isOpenGlAvailable()) {
        if (vars.m_useGLES && !vars.m_useGL) {
            active = false;
        } else if (vars.m_useGLSL && !isOpenGlShaderAvailable()) {
            active = false;
        }
    } else if (isOpenGlesAvailable()) {
        if (vars.m_useGL && !vars.m_useGLES) {
            active = false;
        } else if (vars.m_useGLSL && !isOpenGlShaderAvailable()) {
            active = false;
        }
    } else {
        // No sufficient GL support
        if (vars.m_useGLES || vars.m_useGL || vars.m_useGLSL) {
            active = false;
        }
    }

    if (vars.m_category == WaveformWidgetCategory::DeveloperOnly &&
            !CmdlineArgs::Instance().getDeveloper()) {
        active = false;
    }
#ifdef MIXXX_USE_QOPENGL
    else if (vars.m_category == WaveformWidgetCategory::AllShader) {
        backend = WaveformWidgetBackend::AllShader;
    }
#endif
    else if (vars.m_category == WaveformWidgetCategory::Legacy && vars.m_useGLSL) {
        backend = WaveformWidgetBackend::GLSL;
    } else if (vars.m_category == WaveformWidgetCategory::Legacy) {
        backend = WaveformWidgetBackend::GL;
    }

    if (active) {
        if (collectedHandles.contains(type)) {
            collectedHandles[type].push_back(backend);
        } else {
            collectedHandles.insert(type,
                    QList<WaveformWidgetBackend>{backend});
        }
    }
}

namespace {
template<typename WaveformT>
WaveformWidgetVars waveformWidgetVars() {
    WaveformWidgetVars result;
    result.m_useGL = WaveformT::useOpenGl();
    result.m_useGLES = WaveformT::useOpenGles();
    result.m_useGLSL = WaveformT::useOpenGLShaders();
    result.m_category = WaveformT::category();

    return result;
}
} // namespace

void WaveformWidgetFactory::evaluateWidgets() {
    m_waveformWidgetHandles.clear();
    QHash<WaveformWidgetType::Type, QList<WaveformWidgetBackend>> collectedHandles;
    QHash<WaveformWidgetType::Type,
            WaveformRendererSignalBase::Options>
            supportedOptions;
    bool useGles = isOpenGlesAvailable(); // we can make use of GLES waveforms
    for (WaveformWidgetType::Type type : WaveformWidgetType::kValues) {
        switch (type) {
        case WaveformWidgetType::Empty:
            addHandle(collectedHandles, type, waveformWidgetVars<EmptyWaveformWidget>());
            break;
        case WaveformWidgetType::Simple:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<SimpleSignalWaveformWidget>());
            break;
        case WaveformWidgetType::Filtered:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<SoftwareWaveformWidget>());
            break;
        case WaveformWidgetType::VSyncTest:
#if defined(MIXXX_USE_QOPENGL) && !defined(QT_OPENGL_ES_2)
            addHandle(collectedHandles, type, waveformWidgetVars<GLVSyncTestWidget>());
#endif
            break;
        case WaveformWidgetType::RGB:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<RGBWaveformWidget>());
            break;
        case WaveformWidgetType::HSV:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<HSVWaveformWidget>());
            break;
        case WaveformWidgetType::Stacked:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            break;
        default:
            DEBUG_ASSERT(!"Unexpected WaveformWidgetType");
            continue;
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    for (auto [type, backends] : collectedHandles.asKeyValueRange()) {
#else
    QHashIterator<WaveformWidgetType::Type,
            QList<WaveformWidgetBackend>>
            handleIter(collectedHandles);
    while (handleIter.hasNext()) {
        handleIter.next();
        const auto& type = handleIter.key();
        const auto& backends = handleIter.value();
#endif
        m_waveformWidgetHandles.push_back(WaveformWidgetAbstractHandle(type, backends
#ifdef MIXXX_USE_QOPENGL
                ,
                supportedOptions.value(type, WaveformRendererSignalBase::Option::None)
#endif
                        ));
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createAllshaderWaveformWidget(
        WaveformWidgetType::Type type,
        WWaveformViewer* viewer,
        WaveformRendererSignalBase::Options options) {
    return new allshader::WaveformWidget(viewer, type, viewer->getGroup(), options);
}

WaveformWidgetAbstract* WaveformWidgetFactory::createFilteredWaveformWidget(
        WWaveformViewer* viewer, WaveformRendererSignalBase::Options options) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader: {
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::Filtered, viewer, options);
    }
#endif
    default:
        return new SoftwareWaveformWidget(viewer->getGroup(), viewer, options);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createHSVWaveformWidget(
        WWaveformViewer* viewer, WaveformRendererSignalBase::Options options) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::HSV, viewer, options);
#endif
    default:
        return new HSVWaveformWidget(viewer->getGroup(), viewer, options);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createRGBWaveformWidget(
        WWaveformViewer* viewer, WaveformRendererSignalBase::Options options) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::RGB, viewer, options);
#endif
    default:
        return new RGBWaveformWidget(viewer->getGroup(), viewer, options);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createStackedWaveformWidget(
        WWaveformViewer* viewer, WaveformRendererSignalBase::Options options) {
#ifdef MIXXX_USE_QOPENGL
    WaveformWidgetBackend backend = getBackendFromConfig();
    switch (backend) {
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::Stacked, viewer, options);
#endif
    default:
        return new EmptyWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createSimpleWaveformWidget(
        WWaveformViewer* viewer, WaveformRendererSignalBase::Options options) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::Simple, viewer, options);
#endif
    default:
        return new SimpleSignalWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createVSyncTestWaveformWidget(
        WWaveformViewer* pViewer) {
#ifdef MIXXX_USE_QOPENGL
    return new GLVSyncTestWidget(pViewer->getGroup(), pViewer);
#else
    return new EmptyWaveformWidget(pViewer->getGroup(), pViewer);
#endif
}

WaveformWidgetAbstract* WaveformWidgetFactory::createWaveformWidget(
        WaveformWidgetType::Type type, WWaveformViewer* pViewer) {
    WaveformWidgetAbstract* pWidget = nullptr;
    if (pViewer) {
        if (CmdlineArgs::Instance().getSafeMode()) {
            type = WaveformWidgetType::Empty;
        }

        WaveformRendererSignalBase::Options options =
                m_config->getValue(ConfigKey("[Waveform]", "waveform_options"),
                        WaveformRendererSignalBase::Option::None);
        // TEMPORARY PERF INSTRUMENTATION: which renderer was really built. The
        // option decides between the textured and the geometric signal
        // renderer, i.e. between two different pieces of code, so a run whose
        // options silently differ from the requested ones measures the wrong
        // thing. Reported as options= in WAVEPERF.
        g_perfEffectiveOptions = static_cast<int>(options);

        switch (type) {
        case WaveformWidgetType::Simple:
            pWidget = createSimpleWaveformWidget(pViewer, options);
            break;
        case WaveformWidgetType::Filtered:
            pWidget = createFilteredWaveformWidget(pViewer, options);
            break;
        case WaveformWidgetType::HSV:
            pWidget = createHSVWaveformWidget(pViewer, options);
            break;
        case WaveformWidgetType::VSyncTest:
            pWidget = createVSyncTestWaveformWidget(pViewer);
            break;
        case WaveformWidgetType::RGB:
            pWidget = createRGBWaveformWidget(pViewer, options);
            break;
        case WaveformWidgetType::Stacked:
            pWidget = createStackedWaveformWidget(pViewer, options);
            break;
        default:
            pWidget = new EmptyWaveformWidget(pViewer->getGroup(), pViewer);
            break;
        }
        pWidget->castToQWidget();
        if (!pWidget->isValid()) {
            qWarning() << "failed to init WaveformWidget" << type << "fall back to \"Empty\"";
            delete pWidget;
            pWidget = new EmptyWaveformWidget(pViewer->getGroup(), pViewer);
            pWidget->castToQWidget();
            if (!pWidget->isValid()) {
                qWarning() << "failed to init EmptyWaveformWidget";
                delete pWidget;
                pWidget = nullptr;
            }
        }
    }
    return pWidget;
}

int WaveformWidgetFactory::findIndexOf(WWaveformViewer* viewer) const {
    for (int i = 0; i < (int)m_waveformWidgetHolders.size(); i++) {
        if (m_waveformWidgetHolders[i].m_waveformViewer == viewer) {
            return i;
        }
    }
    return -1;
}

void WaveformWidgetFactory::startVSync(
        GuiTick* pGuiTick, VisualsManager* pVisualsManager, bool useQML) {
    auto vSyncMode = useQML
            ? VSyncThread::ST_TIMER
            : static_cast<VSyncThread::VSyncMode>(m_config->getValue(kVSyncKey, 0));

#ifndef MIXXX_USE_QOPENGL
    if (vSyncMode == VSyncThread::ST_DISPLAY_LINK) {
        qWarning() << "VSync mode ST_DISPLAY_LINK requires the QOpenGLWindow "
                      "based widgets, falling back to the default mode";
        vSyncMode = VSyncThread::ST_DEFAULT;
    }
#else
    OpenGLWindow* pDisplayLinkWindow = nullptr;
    if (vSyncMode == VSyncThread::ST_DISPLAY_LINK) {
        WGLWidget* pWidget = SharedGLContext::getWidget();
        // Note: the OpenGLWindow of the shared GL widget is created lazily in
        // its show event, which has already happened at this point (the
        // initialization continues from WInitialGLWidget::onInitialized).
        pDisplayLinkWindow = pWidget
                ? qobject_cast<OpenGLWindow*>(pWidget->getOpenGLWindow())
                : nullptr;
        if (!pDisplayLinkWindow) {
            qWarning() << "VSync mode ST_DISPLAY_LINK requires the shared GL "
                          "window, falling back to the default mode";
            vSyncMode = VSyncThread::ST_DEFAULT;
        }
    }
#endif

    m_pGuiTick = pGuiTick;
    m_pVisualsManager = pVisualsManager;
    m_vsyncThread = new VSyncThread(this, vSyncMode);
    m_vsyncThread->setObjectName(QStringLiteral("VSync"));
    m_vsyncThread->setSyncIntervalTimeMicros(static_cast<int>(1e6 / m_frameRate));

#ifdef MIXXX_USE_QOPENGL
    if (m_vsyncThread->vsyncMode() == VSyncThread::ST_DISPLAY_LINK) {
        DEBUG_ASSERT(pDisplayLinkWindow);
        WGLWidget* pWidget = SharedGLContext::getWidget();
        // The window must be visible, otherwise the platform does not deliver
        // update requests for it.
        pWidget->show();
        m_pDisplayLinkFrameDriver = new DisplayLinkFrameDriver(
                pDisplayLinkWindow, m_vsyncThread, this);
        m_pDisplayLinkFrameDriver->setFrameRate(m_frameRate);
        connect(m_pDisplayLinkFrameDriver,
                &DisplayLinkFrameDriver::frameDue,
                this,
                &WaveformWidgetFactory::slotDisplayLinkFrame);
        m_pDisplayLinkFrameDriver->start();
        // No VSyncThread is started in this mode: it only serves as the
        // VSyncTimeProvider, fed by the driver.
        return;
    }

    if (m_vsyncThread->vsyncMode() == VSyncThread::ST_PLL) {
        WGLWidget* widget = SharedGLContext::getWidget();
        if (widget) {
            QOpenGLWindow* pWindow = widget->getOpenGLWindow();
            connect(pWindow,
                    &QOpenGLWindow::frameSwapped,
                    this,
                    &WaveformWidgetFactory::slotFrameSwapped,
                    Qt::DirectConnection);
            widget->show();
            // The PLL must sanity-check itself against the screen this window
            // is really on, not against the primary screen: with a 120 Hz
            // panel next to a 60 Hz primary monitor, every true interval of
            // the other screen looks implausible. Windows also move between
            // monitors while Mixxx runs, so follow that too.
            auto reportScreen = [this, pWindow]() {
                const QScreen* pScreen = pWindow->screen();
                m_vsyncThread->setDisplayRefreshRate(pScreen ? pScreen->refreshRate() : 0.0);
            };
            connect(pWindow, &QWindow::screenChanged, this, reportScreen);
            reportScreen();
        }
    }
#endif

    connect(m_vsyncThread,
            &VSyncThread::vsyncRender,
            this,
            &WaveformWidgetFactory::render);
    connect(m_vsyncThread,
            &VSyncThread::vsyncSwap,
            this,
            &WaveformWidgetFactory::swap);
    connect(m_vsyncThread,
            &VSyncThread::vsyncSwapAndRender,
            this,
            &WaveformWidgetFactory::swapAndRender);

    m_vsyncThread->start(QThread::NormalPriority);
}

void WaveformWidgetFactory::getAvailableVSyncTypes(QList<QPair<int, QString>>* pList) {
    m_vsyncThread->getAvailableVSyncTypes(pList);
}

WaveformWidgetType::Type WaveformWidgetFactory::findTypeFromHandleIndex(int index) {
    WaveformWidgetType::Type type = WaveformWidgetType::Invalid;
    if (index >= 0 && index < m_waveformWidgetHandles.size()) {
        type = m_waveformWidgetHandles[index].m_type;
    }
    return type;
}

int WaveformWidgetFactory::findHandleIndexFromType(WaveformWidgetType::Type type) {
    for (int i = 0; i < m_waveformWidgetHandles.size(); i++) {
        const WaveformWidgetAbstractHandle& handle = m_waveformWidgetHandles[i];
        if (handle.m_type == type) {
            return i;
        }
    }
    return -1;
}

bool WaveformWidgetFactory::widgetTypeSupportsAcceleration(WaveformWidgetType::Type type) {
    for (const auto& handle : std::as_const(m_waveformWidgetHandles)) {
        if (handle.m_type == type) {
            return handle.supportAcceleration();
        }
    }
    return false;
}

bool WaveformWidgetFactory::widgetTypeSupportsSoftware(WaveformWidgetType::Type type) {
    for (const auto& handle : std::as_const(m_waveformWidgetHandles)) {
        if (handle.m_type == type) {
            return handle.supportSoftware();
        }
    }
    return false;
}

WaveformWidgetBackend WaveformWidgetFactory::getBackendFromConfig() const {
    // On the UI, hardware acceleration is a boolean (0 => software rendering, 1
    // => hardware acceleration), but in the setting, we keep the granularity so
    // in case of issue when we release, we can communicate workaround on
    // editing the INI file to target a specific rendering backend. If no
    // complains come back, we can convert this safely to a backend eventually.
    return m_config->getValue(
            ConfigKey(QStringLiteral("[Waveform]"), QStringLiteral("use_hardware_acceleration")),
            preferredBackend());
}

WaveformWidgetBackend WaveformWidgetFactory::preferredBackend() const {
#ifdef MIXXX_USE_QOPENGL
    if (m_openGlAvailable || m_openGlesAvailable) {
        return WaveformWidgetBackend::AllShader;
    }
#endif
    if (m_openGlAvailable && m_openGLShaderAvailable) {
        return WaveformWidgetBackend::GLSL;
    } else if (m_openGlAvailable) {
        return WaveformWidgetBackend::GL;
    }
    return WaveformWidgetBackend::None;
}

void WaveformWidgetFactory::setDefaultBackend() {
    m_config->setValue(kHardwareAccelerationKey, preferredBackend());
}

QString WaveformWidgetAbstractHandle::getDisplayName() const {
    return getDisplayName(m_type);
}

// Static
QString WaveformWidgetAbstractHandle::getDisplayName(WaveformWidgetType::Type type) {
    switch (type) {
    case WaveformWidgetType::Empty:
        return QObject::tr("Empty");
    case WaveformWidgetType::Simple:
        return QObject::tr("Simple");
    case WaveformWidgetType::Filtered:
        return QObject::tr("Filtered");
    case WaveformWidgetType::HSV:
        return QObject::tr("HSV");
    case WaveformWidgetType::VSyncTest:
        return QObject::tr("VSyncTest");
    case WaveformWidgetType::RGB:
        return QObject::tr("RGB");
    case WaveformWidgetType::Stacked:
        return QObject::tr("Stacked");
    default:
        return QObject::tr("Unknown");
    }
}

// static
QSurfaceFormat WaveformWidgetFactory::getSurfaceFormat(UserSettingsPointer pConfig) {
    // The first call should pass the config to set the vsync mode. Subsequent
    // calls will use the value as set on the first call.
    static const VSyncThread::VSyncMode vsyncMode = pConfig
            ? pConfig->getValue(kVSyncKey, VSyncThread::ST_DEFAULT)
            : VSyncThread::ST_DEFAULT;

    QSurfaceFormat format;
    // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0, default is 2.0
    // format.setVersion(2, 1);
    // Core and Compatibility contexts have been introduced in openGL 3.2
    // From 3.0 to 3.1 we have implicit the Core profile and Before 3.0 we have the
    // Compatibility profile
    // format.setProfile(QSurfaceFormat::CoreProfile);

    // setSwapInterval sets the application preferred swap interval
    // in minimum number of video frames that are displayed before a buffer swap occurs
    // - 0 will turn the vertical refresh syncing off
    // - 1 (default) means swapping after drawig a video frame to the buffer
    // - n means swapping after drawing n video frames to the buffer
    //
    // The vertical sync setting requested by the OpenGL application, can be overwritten
    // if a user changes the "Wait for vertical refresh" setting in AMD graphic drivers
    // for Windows.

#if defined(__APPLE__)
    // On OS X, syncing to vsync has good performance FPS-wise and
    // eliminates tearing. (This is an comment from pre QOpenGLWindow times)
    //
    // TEMPORARY BENCHMARK HOOK: MIXXX_BENCH_SWAP_INTERVAL=0 lifts the forced
    // vsync so the raw throughput ceiling of the renderer can be measured.
    // Without this, even VSyncThread::ST_FREE still blocks in swapBuffers.
    const QByteArray benchSwapInterval = qgetenv("MIXXX_BENCH_SWAP_INTERVAL");
    format.setSwapInterval(benchSwapInterval.isEmpty() ? 1 : benchSwapInterval.toInt());
    (void)vsyncMode;
#else
    // It seems that on Windows (at least for some AMD drivers), the setting 1 is not
    // not properly handled. We saw frame rates divided by exact integers, like it should
    // be with values >1 (see https://github.com/mixxxdj/mixxx/issues/11617)
    // Reported as https://bugreports.qt.io/browse/QTBUG-114882
    // On Linux, horrible FPS were seen with "VSync off" before switching to QOpenGLWindow too
    // Note: ST_DISPLAY_LINK is handled like ST_PLL here. On macOS (where the
    // swap interval is 1 anyway, see above) the Qt Cocoa plugin only serves
    // QWindow::requestUpdate() from its CVDisplayLink if the surface format
    // has a swap interval > 0 (QCocoaWindow::updatesWithDisplayLink()).
    format.setSwapInterval(vsyncMode == VSyncThread::ST_PLL ||
                            vsyncMode == VSyncThread::ST_DISPLAY_LINK
                    ? 1
                    : 0);
#endif

    // TEMPORARY BENCHMARK HOOK: MIXXX_BENCH_SWAP_BEHAVIOR=triple|double.
    // Triple buffering lets the GPU keep working while a finished frame waits
    // to be presented, which can hide a late frame instead of dropping it.
    const QByteArray benchSwapBehavior = qgetenv("MIXXX_BENCH_SWAP_BEHAVIOR");
    if (benchSwapBehavior == "triple") {
        format.setSwapBehavior(QSurfaceFormat::TripleBuffer);
    } else if (benchSwapBehavior == "double") {
        format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    }

    // TEMPORARY BENCHMARK HOOK: request a 4.1 Core context. Without this macOS
    // hands out a legacy 2.1 NoProfile context, which is the slowest path
    // through Apple's GL-on-Metal translation layer.
    if (!qEnvironmentVariableIsEmpty("MIXXX_BENCH_CORE_PROFILE")) {
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setVersion(4, 1);
    }

#ifdef FORCE_GLES
    // Define FORCE_GLES to test GLES waveforms on a GLSL Hardware
    qDebug() << "QOpenGLContext::openGLModuleType()" << QOpenGLContext::openGLModuleType();

    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
#endif

    return format;
}

void WaveformWidgetFactory::setUntilMarkShowBeats(bool value) {
    m_untilMarkShowBeats = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowBeats")),
                m_untilMarkShowBeats);
    }
    emit untilMarkShowBeatsChanged(value);
}

void WaveformWidgetFactory::setUntilMarkShowTime(bool value) {
    m_untilMarkShowTime = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowTime")),
                m_untilMarkShowTime);
    }
    emit untilMarkShowTimeChanged(value);
}

void WaveformWidgetFactory::setUntilMarkAlign(Qt::Alignment align) {
    m_untilMarkAlign = align;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkAlign")),
                toUntilMarkAlignIndex(m_untilMarkAlign));
    }
    emit untilMarkAlignChanged(align);
}

void WaveformWidgetFactory::setUntilMarkTextPointSize(int value) {
    m_untilMarkTextPointSize = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextPointSize")),
                m_untilMarkTextPointSize);
    }
    emit untilMarkTextPointSizeChanged(value);
}

void WaveformWidgetFactory::setUntilMarkTextHeightLimit(float value) {
    m_untilMarkTextHeightLimit = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextHeightLimit")),
                toUntilMarkTextHeightLimitIndex(m_untilMarkTextHeightLimit));
    }
    emit untilMarkTextHeightLimitChanged(value);
}

void WaveformWidgetFactory::setStemReorderOnChange(bool value) {
    m_stemReorderOnChange = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_reorder_on_change")),
                value);
    }
    emit stemReorderOnChangeChanged(value);
}

void WaveformWidgetFactory::setStemOutlineOpacity(float value) {
    m_stemOutlineOpacity = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_outline_opacity")),
                static_cast<double>(value));
    }
    emit stemOutlineOpacityChanged(value);
}

void WaveformWidgetFactory::setStemOpacity(float value) {
    m_stemOpacity = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_opacity")),
                static_cast<double>(value));
    }
    emit stemOpacityChanged(value);
}

void WaveformWidgetFactory::setStemSplitTracks(bool value) {
    m_stemSplitTracks = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_split_tracks")),
                value);
    }
    m_pStemSplitTracksControl->set(value ? 1.0 : 0.0);
    emit stemSplitTracksChanged(value);
}

// static
Qt::Alignment WaveformWidgetFactory::toUntilMarkAlign(int index) {
    switch (index) {
    case 0:
        return Qt::AlignTop;
    case 1:
        return Qt::AlignVCenter;
    case 2:
        return Qt::AlignBottom;
    }
    DEBUG_ASSERT(!"unsupported align");
    return Qt::AlignVCenter;
}
// static
int WaveformWidgetFactory::toUntilMarkAlignIndex(Qt::Alignment align) {
    switch (align) {
    case Qt::AlignTop:
        return 0;
    case Qt::AlignVCenter:
        return 1;
    case Qt::AlignBottom:
        return 2;
    default:
        break;
    }
    DEBUG_ASSERT(!"unsupported align index");
    return 1;
}
// static
float WaveformWidgetFactory::toUntilMarkTextHeightLimit(int index) {
    switch (index) {
    case 0:
        return 0.333f;
    case 1:
        return 1.f;
    }
    DEBUG_ASSERT(!"unsupported height limit");
    return 0.33f;
}
// static
int WaveformWidgetFactory::toUntilMarkTextHeightLimitIndex(float value) {
    if (value == 0.333f) {
        return 0;
    }
    if (value == 1.f) {
        return 1;
    }
    DEBUG_ASSERT(!"unsupported height limit");
    return 0;
}
