#include "waveform/renderers/allshader/waveformrenderertextured.h"

#ifndef QT_OPENGL_ES_2

#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QStringList>
#include <QVector3D>
#include <algorithm>

#include "control/controlproxy.h"
#include "moc_waveformrenderertextured.cpp"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

namespace {
const QString kPassthroughShaderPath = QStringLiteral(":/shaders/passthrough.vert");

// We render into a frame buffer that is this much larger than the renderer
// itself to "oversample" the texture relative to the surface we're drawing on.
constexpr int kOversamplingFactor = 4;

float tunable(const char* name, float defaultValue) {
    bool ok = false;
    const float value = qEnvironmentVariable(name).toFloat(&ok);
    return ok ? value : defaultValue;
}

// Radius of the window the color of a column is averaged over, in visual bins
// (441 bins per second). The amplitude is not affected, it keeps the full
// detail. Four bins is about 9 ms, which is the grid Traktor appears to use.
//
// A wider window scores better on every number we could think of - the colour
// jitter between neighbouring columns and the sharpness of the transitions
// both keep improving up to about 12 bins - and looks worse: averaging over
// 27 ms mixes neighbouring columns into each other, which invents orange
// between red and green and washes the saturation out. The numbers measured
// how fast the colour changes, not which colours appear, so they never saw it.
// The eye did.
float colorSmoothBins() {
    static const float value =
            std::clamp(tunable("MIXXX_WF_COLOR_SMOOTH_BINS", 4.0f), 0.0f, 20.0f);
    return value;
}

// Width of the soft edge of the waveform, in device pixels. Traktor fades out
// over 3-4 device pixels.
float softEdgePixels() {
    static const float value = std::max(tunable("MIXXX_WF_SOFT_EDGE_PX", 3.0f), 0.0f);
    return value;
}

// Minimum visible half-height of bins that carry any signal, as a fraction of
// the half-height of the widget. Measured in Traktor: quiet columns stop
// following the amplitude and sit on a plateau of 0.214 of the half-height,
// whatever their loudness.
float amplitudeFloor() {
    static const float value =
            std::clamp(tunable("MIXXX_WF_AMP_FLOOR", 0.19f), 0.0f, 0.9f);
    return value;
}

// Compression applied to the band values before they become a color. Traktor
// stores the square root of the band magnitude, Mixxx stores it as is, so 0.5
// imitates Traktor on the data we have today.
float colorGamma() {
    static const float value = std::clamp(tunable("MIXXX_WF_COLOR_GAMMA", 1.0f), 0.05f, 4.0f);
    return value;
}

// Level below which the color of a column is no longer normalized to full
// brightness, in band units. Keeps quiet passages dark instead of letting the
// normalization turn their noise into a fully saturated color.
float colorLevelFloor() {
    static const float value =
            std::clamp(tunable("MIXXX_WF_COLOR_LEVEL_FLOOR", 0.01f), 0.0f, 1.0f);
    return value;
}

// How strongly a column is shaded from its centre to its edge. 0 draws the
// flat fill of the other types; the default is deliberately mild, enough to
// give the waveform a texture without emptying its middle.
float verticalStrength() {
    static const float value =
            std::clamp(tunable("MIXXX_WF_VERT_STRENGTH", 0.35f), 0.0f, 1.0f);
    return value;
}

// Shape of that shading.
float verticalSharpness() {
    static const float value =
            std::clamp(tunable("MIXXX_WF_VERT_SHARPNESS", 1.5f), 0.1f, 8.0f);
    return value;
}

// The crest factor at which a column is drawn flat, and how fast the shading
// follows it away from there. A sine sits at 1.41 and percussive material goes
// well above it.
float crestNeutral() {
    static const float value = std::clamp(tunable("MIXXX_WF_CREST_NEUTRAL", 1.6f), 0.5f, 8.0f);
    return value;
}

float crestScale() {
    static const float value = std::clamp(tunable("MIXXX_WF_CREST_SCALE", 0.6f), 0.0f, 5.0f);
    return value;
}

// Below this band level the crest factor is noise (one byte per band), so the
// column is drawn flat instead.
float crestLevelFloor() {
    static const float value =
            std::clamp(tunable("MIXXX_WF_CREST_LEVEL_FLOOR", 0.02f), 0.0f, 1.0f);
    return value;
}

// Whether the EQ knobs of the deck are allowed to change what the Spectrum
// waveform draws. They are not, by default: the waveform shows what is in the
// file, as it does in Traktor, and not the current position of the knobs.
// MIXXX_WF_EQ_AFFECTS=1 restores the behaviour of the other waveform types.
bool eqAffectsDrawing() {
    static const bool value = qEnvironmentVariableIntValue("MIXXX_WF_EQ_AFFECTS") > 0;
    return value;
}

// Whether the height follows the ReplayGain of the track. It does by default:
// ReplayGain is not a knob but a property of the file, constant for the whole
// track, and with it the waveform answers "how loud will this sound" instead
// of "what is in the file". MIXXX_WF_TRACK_GAIN=0 makes the height depend on
// the file alone.
bool replayGainAffectsHeight() {
    static const bool value = qEnvironmentVariable("MIXXX_WF_TRACK_GAIN") != QStringLiteral("0");
    return value;
}

// Balance between the three bands, applied to the color only. The high band is
// raised by 17 dB, which is what the measurement of Traktor says and what the
// band magnitudes of the analyzer need. The mid band is held back to 0.7:
// measured on the same material, that takes the share of yellow-green columns
// from 6.1% down to 1.2% and the columns where green dominates from 11% to
// 2.7%, against 3.0% and 7.5% in Traktor. Overridable as
// MIXXX_WF_BAND_GAIN="low,mid,high", so 1.068,1,7.111 restores the plain
// measured balance for comparison.
QVector3D bandColorGain() {
    static const QVector3D value = []() {
        const QStringList parts =
                qEnvironmentVariable("MIXXX_WF_BAND_GAIN").split(QChar(','));
        if (parts.size() == 3) {
            bool okLow = false, okMid = false, okHigh = false;
            const float low = parts.at(0).toFloat(&okLow);
            const float mid = parts.at(1).toFloat(&okMid);
            const float high = parts.at(2).toFloat(&okHigh);
            if (okLow && okMid && okHigh) {
                return QVector3D(low, mid, high);
            }
        }
        return QVector3D(1.068f, 0.7f, 7.111f);
    }();
    return value;
}
} // namespace

namespace allshader {

// static
QString WaveformRendererTextured::fragShaderForType(::WaveformWidgetType::Type t) {
    switch (t) {
    case ::WaveformWidgetType::Filtered:
        return QStringLiteral(":/shaders/filteredsignal.frag");
    case ::WaveformWidgetType::RGB:
        return QStringLiteral(":/shaders/rgbsignal.frag");
    case ::WaveformWidgetType::Spectrum:
        return QStringLiteral(":/shaders/spectrumsignal.frag");
    case ::WaveformWidgetType::Stacked:
        return QStringLiteral(":/shaders/stackedsignal.frag");
    default:
        break;
    }
    DEBUG_ASSERT(!"unsupported WaveformWidgetType");
    return QString();
}

WaveformRendererTextured::WaveformRendererTextured(
        WaveformWidgetRenderer* waveformWidget,
        ::WaveformWidgetType::Type t,
        ::WaveformRendererAbstract::PositionSource type,
        ::WaveformRendererSignalBase::Options options)
        : WaveformRendererSignalBase(waveformWidget, options),
          m_unitQuadListId(-1),
          m_textureId(0),
          m_textureRenderedWaveformCompletion(0),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip),
          m_options(options),
          m_shadersValid(false),
          m_type(t),
          m_fragShader(fragShaderForType(t)) {
}

WaveformRendererTextured::~WaveformRendererTextured() {
    if (m_textureId) {
        glDeleteTextures(1, &m_textureId);
    }

    if (m_frameShaderProgram) {
        m_frameShaderProgram->removeAllShaders();
    }
}

bool WaveformRendererTextured::loadShaders() {
    qDebug() << "WaveformRendererTextured::loadShaders" << m_fragShader << "type"
             << static_cast<int>(m_type);
    m_shadersValid = false;

    if (m_frameShaderProgram->isLinked()) {
        m_frameShaderProgram->release();
    }

    m_frameShaderProgram->removeAllShaders();

    if (!m_frameShaderProgram->addShaderFromSourceFile(
                QOpenGLShader::Vertex,
                kPassthroughShaderPath)) {
        qWarning()
                << "WaveformRendererTextured::loadShaders - compilation failed:"
                << kPassthroughShaderPath;
        qDebug() << m_frameShaderProgram->log();
        return false;
    }

    if (!m_frameShaderProgram->addShaderFromSourceFile(
                QOpenGLShader::Fragment,
                m_fragShader)) {
        qWarning() << "WaveformRendererTextured::loadShaders - compilation failed:" << m_fragShader;
        return false;
    }

    if (!m_frameShaderProgram->link()) {
        qDebug() << "WaveformRendererTextured::loadShaders - linking failed."
                 << m_frameShaderProgram->log();
        return false;
    }

    if (!m_frameShaderProgram->bind()) {
        qDebug() << "WaveformRendererTextured::loadShaders - binding failed";
        return false;
    }

    m_shadersValid = true;
    return true;
}

bool WaveformRendererTextured::loadTexture() {
    int dataSize = 0;
    const WaveformData* data = nullptr;

    ConstWaveformPointer pWaveform = m_waveformRenderer->getWaveform();
    if (pWaveform) {
        dataSize = pWaveform->getDataSize();
        if (dataSize > 1) {
            data = pWaveform->data();
        }
    }

    glEnable(GL_TEXTURE_2D);

    if (m_textureId == 0) {
        glGenTextures(1, &m_textureId);

        int error = glGetError();
        if (error) {
            qDebug() << "WaveformRendererTextured::loadTexture - m_textureId"
                     << m_textureId << "error" << error;
        }
    }

    glBindTexture(GL_TEXTURE_2D, m_textureId);

    int error = glGetError();
    if (error) {
        qDebug() << "WaveformRendererTextured::loadTexture - bind error" << error;
    }

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    if (pWaveform != nullptr && data != nullptr) {
        // Make a copy of the waveform data, stripping the stems portion. Note that the datasize is
        // different from the texture size -- we want the full texture size so the upload works. See
        // m_data in waveform/waveform.h.
        if (m_data.size() == 0 || static_cast<int>(m_data.size()) != pWaveform->getTextureSize()) {
            m_data.resize(pWaveform->getTextureSize());
        }
        for (int i = 0; i < pWaveform->getDataSize(); i++) {
            m_data[i] = data[i].filtered;
        }
        // Waveform ensures that getTextureSize is a multiple of
        // getTextureStride so there is no rounding here.
        int textureWidth = pWaveform->getTextureStride();
        int textureHeight = pWaveform->getTextureSize() / pWaveform->getTextureStride();

        glTexImage2D(GL_TEXTURE_2D,
                0,
                GL_RGBA,
                textureWidth,
                textureHeight,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                m_data.data());
        int error = glGetError();
        VERIFY_OR_DEBUG_ASSERT(!error) {
            qWarning() << "WaveformRendererTextured::loadTexture - glTexImage2D error" << error;
        }
    } else {
        glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
    }

    glDisable(GL_TEXTURE_2D);

    return true;
}

void WaveformRendererTextured::createGeometry() {
    if (m_unitQuadListId != -1) {
        return;
    }

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -10.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    m_unitQuadListId = glGenLists(1);
    glNewList(m_unitQuadListId, GL_COMPILE);
    {
        glBegin(GL_QUADS);
        {
            glTexCoord2f(0.0, 0.0);
            glVertex3f(-1.0f, -1.0f, 0.0f);

            glTexCoord2f(1.0, 0.0);
            glVertex3f(1.0f, -1.0f, 0.0f);

            glTexCoord2f(1.0, 1.0);
            glVertex3f(1.0f, 1.0f, 0.0f);

            glTexCoord2f(0.0, 1.0);
            glVertex3f(-1.0f, 1.0f, 0.0f);
        }
        glEnd();
    }
    glEndList();
}

void WaveformRendererTextured::createFrameBuffers() {
    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    constexpr int oversamplingFactor = kOversamplingFactor;
    const auto bufferWidth = oversamplingFactor *
            static_cast<int>(m_waveformRenderer->getWidth() * devicePixelRatio);
    const auto bufferHeight = oversamplingFactor *
            static_cast<int>(
                    m_waveformRenderer->getHeight() * devicePixelRatio);

    m_framebuffer = std::make_unique<QOpenGLFramebufferObject>(bufferWidth,
            bufferHeight);

    if (!m_framebuffer->isValid()) {
        qWarning() << "WaveformRendererTextured::createFrameBuffer - frame buffer not valid";
    }
}

void WaveformRendererTextured::initializeGL() {
    m_textureRenderedWaveformCompletion = 0;

    if (!m_frameShaderProgram) {
        m_frameShaderProgram = std::make_unique<QOpenGLShaderProgram>();
    }

    if (!loadShaders()) {
        return;
    }
    createFrameBuffers();
    createGeometry();
    if (!loadTexture()) {
        return;
    }
}

void WaveformRendererTextured::onSetup(const QDomNode&) {
}

void WaveformRendererTextured::onSetTrack() {
    if (m_loadedTrack) {
        disconnect(m_loadedTrack.get(),
                &Track::waveformUpdated,
                this,
                &WaveformRendererTextured::slotWaveformUpdated);
    }

    slotWaveformUpdated();

    const TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    if (!pTrack) {
        return;
    }

    // When the track's waveform has been changed (or cleared), it is necessary
    // to update (or delete) the texture containing the waveform which was
    // uploaded to GPU. Otherwise, previous waveform will be shown.
    connect(pTrack.get(),
            &Track::waveformUpdated,
            this,
            &WaveformRendererTextured::slotWaveformUpdated);

    m_loadedTrack = pTrack;
}

void WaveformRendererTextured::resizeGL(int, int) {
    createFrameBuffers();
}

void WaveformRendererTextured::slotWaveformUpdated() {
    m_textureRenderedWaveformCompletion = 0;
    // initializeGL not called yet
    if (!m_frameShaderProgram) {
        return;
    }
    loadTexture();
}

void WaveformRendererTextured::paintGL() {
    TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    if (!pTrack || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return;
    }

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    ConstWaveformPointer pWaveform = m_waveformRenderer->getWaveform();
    if (pWaveform.isNull()) {
        return;
    }

    const double audioVisualRatio = pWaveform->getAudioVisualRatio();
    if (audioVisualRatio <= 0) {
        return;
    }

    int dataSize = pWaveform->getDataSize();
    if (dataSize <= 1) {
        return;
    }

    if (pWaveform->data() == nullptr) {
        return;
    }
#ifdef __STEM__
    auto stemInfo = pTrack->getStemInfo();
    // If this track is a stem track, skip the rendering
    if (!stemInfo.isEmpty() && pWaveform->hasStem()) {
        return;
    }
#endif

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        return;
    }

    // NOTE(vRince): completion can change during loadTexture
    // do not remove currentCompletion temp variable !
    const int currentCompletion = pWaveform->getCompletion();
    if (m_textureRenderedWaveformCompletion < currentCompletion) {
        loadTexture();
        m_textureRenderedWaveformCompletion = currentCompletion;
    }

    float lowGain(1.0), midGain(1.0), highGain(1.0), allGain(1.0);
    if (m_type == ::WaveformWidgetType::Spectrum && !eqAffectsDrawing()) {
        // The Spectrum waveform draws the file, not the mixer. None of the
        // knobs of the deck reach it: neither the three EQ knobs and their
        // kill switches, nor the gain knob. What is left is the ReplayGain of
        // the track, which is a property of the file rather than a knob.
        //
        // NOTE for whoever reads this next: this also means the per band
        // visual gains of Preferences -> Waveforms (low, mid, high) do nothing
        // for this type. That is deliberate, not a bug: the balance between
        // the bands here is the measured one and lives in bandColorGain above,
        // and two controls for the same thing only confuse. The overall visual
        // gain still applies.
        allGain = m_allChannelVisualGain;
        if (replayGainAffectsHeight()) {
            if (!m_pReplayGain) {
                m_pReplayGain = std::make_unique<ControlProxy>(
                        m_waveformRenderer->getGroup(), QStringLiteral("replaygain"));
            }
            const double replayGain = m_pReplayGain->get();
            if (replayGain > 0.0) {
                allGain *= static_cast<float>(replayGain);
            }
        }
    } else {
        // Per-band gain from the EQ knobs.
        getGains(&allGain, &lowGain, &midGain, &highGain);
    }

    const auto firstVisualIndex = static_cast<GLfloat>(
            m_waveformRenderer->getFirstDisplayedPosition(positionType) * trackSamples /
            audioVisualRatio / 2.0);
    const auto lastVisualIndex = static_cast<GLfloat>(
            m_waveformRenderer->getLastDisplayedPosition(positionType) *
            trackSamples / audioVisualRatio / 2.0);

    // const int firstIndex = int(firstVisualIndex+0.5);
    // firstVisualIndex = firstIndex - firstIndex%2;

    // const int lastIndex = int(lastVisualIndex+0.5);
    // lastVisualIndex = lastIndex + lastIndex%2;

    // qDebug() << "GAIN" << allGain << lowGain << midGain << highGain;

    if (!m_paintLogged) {
        m_paintLogged = true;
        // Reported in the BENCHHIT convention of the performance harness: what
        // is logged is what the shader actually received, not what was asked
        // for. A knob that silently did not arrive, or a waveform type that was
        // silently replaced, is the mistake that has cost this project the most
        // time so far.
        const QVector3D gain = bandColorGain();
        const QStringList applied = {
                QStringLiteral("BENCHHIT MIXXX_WF_COLOR_SMOOTH_BINS=") +
                        QString::number(colorSmoothBins()),
                QStringLiteral("type=") + QString::number(static_cast<int>(m_type)),
                QStringLiteral("options=") +
                        QString::number(static_cast<int>(
                                static_cast<::WaveformRendererSignalBase::Options::Int>(
                                        m_options))),
                QStringLiteral("shader=") + m_fragShader,
                QStringLiteral("MIXXX_WF_SOFT_EDGE_PX=") + QString::number(softEdgePixels()),
                QStringLiteral("MIXXX_WF_AMP_FLOOR=") + QString::number(amplitudeFloor()),
                QStringLiteral("MIXXX_WF_COLOR_LEVEL_FLOOR=") +
                        QString::number(colorLevelFloor()),
                QStringLiteral("MIXXX_WF_COLOR_GAMMA=") + QString::number(colorGamma()),
                QStringLiteral("MIXXX_WF_VERT_STRENGTH=") + QString::number(verticalStrength()),
                QStringLiteral("MIXXX_WF_VERT_SHARPNESS=") + QString::number(verticalSharpness()),
                QStringLiteral("MIXXX_WF_CREST_NEUTRAL=") + QString::number(crestNeutral()),
                QStringLiteral("MIXXX_WF_CREST_SCALE=") + QString::number(crestScale()),
                QStringLiteral("MIXXX_WF_BAND_GAIN=") + QString::number(gain.x()) +
                        QChar(',') + QString::number(gain.y()) + QChar(',') +
                        QString::number(gain.z()),
        };
        qDebug().noquote() << applied.join(QChar(' '));
    }

    // paint into frame buffer
    {
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        if (m_orientation == Qt::Vertical) {
            glRotatef(90.0f, 0.0f, 0.0f, 1.0f);
            glScalef(-1.0f, 1.0f, 1.0f);
        }
        glOrtho(firstVisualIndex, lastVisualIndex, -1.0, 1.0, -10.0, 10.0);

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glTranslatef(.0f, .0f, .0f);

        m_frameShaderProgram->bind();

        glViewport(0, 0, m_framebuffer->width(), m_framebuffer->height());

        m_frameShaderProgram->setUniformValue("framebufferSize",
                QVector2D(m_framebuffer->width(), m_framebuffer->height()));
        m_frameShaderProgram->setUniformValue("waveformLength", dataSize);
        m_frameShaderProgram->setUniformValue("textureSize", pWaveform->getTextureSize());
        m_frameShaderProgram->setUniformValue("textureStride", pWaveform->getTextureStride());

        m_frameShaderProgram->setUniformValue("firstVisualIndex", firstVisualIndex);
        m_frameShaderProgram->setUniformValue("lastVisualIndex", lastVisualIndex);

        m_frameShaderProgram->setUniformValue("allGain", allGain);
        m_frameShaderProgram->setUniformValue("lowGain", lowGain);
        m_frameShaderProgram->setUniformValue("midGain", midGain);
        m_frameShaderProgram->setUniformValue("highGain", highGain);

        if (m_type == ::WaveformWidgetType::RGB || m_type == ::WaveformWidgetType::Spectrum) {
            m_frameShaderProgram->setUniformValue("splitStereoSignal",
                    m_options & ::WaveformRendererSignalBase::Option::SplitStereoSignal);
        }

        if (m_type == ::WaveformWidgetType::Spectrum) {
            m_frameShaderProgram->setUniformValue("colorSmoothBins", colorSmoothBins());
            // The shader works in frame buffer pixels, the tunable is in
            // device pixels.
            m_frameShaderProgram->setUniformValue("softEdgePixels",
                    softEdgePixels() * static_cast<float>(kOversamplingFactor));
            m_frameShaderProgram->setUniformValue("amplitudeFloor", amplitudeFloor());
            m_frameShaderProgram->setUniformValue("bandColorGain", bandColorGain());
            m_frameShaderProgram->setUniformValue("colorGamma", colorGamma());
            m_frameShaderProgram->setUniformValue("colorLevelFloor", colorLevelFloor());
            m_frameShaderProgram->setUniformValue("verticalStrength", verticalStrength());
            m_frameShaderProgram->setUniformValue("verticalSharpness", verticalSharpness());
            m_frameShaderProgram->setUniformValue("crestNeutral", crestNeutral());
            m_frameShaderProgram->setUniformValue("crestScale", crestScale());
            m_frameShaderProgram->setUniformValue("crestLevelFloor", crestLevelFloor());
        }

        m_frameShaderProgram->setUniformValue("axesColor",
                QVector4D(static_cast<GLfloat>(m_axesColor_r),
                        static_cast<GLfloat>(m_axesColor_g),
                        static_cast<GLfloat>(m_axesColor_b),
                        static_cast<GLfloat>(m_axesColor_a)));

        if (m_type == ::WaveformWidgetType::Stacked) {
            m_frameShaderProgram->setUniformValue("lowFilteredColor",
                    QVector4D(static_cast<GLfloat>(m_rgbLowFilteredColor_r),
                            static_cast<GLfloat>(m_rgbLowFilteredColor_g),
                            static_cast<GLfloat>(m_rgbLowFilteredColor_b),
                            1.0));
            m_frameShaderProgram->setUniformValue("midFilteredColor",
                    QVector4D(static_cast<GLfloat>(m_rgbMidFilteredColor_r),
                            static_cast<GLfloat>(m_rgbMidFilteredColor_g),
                            static_cast<GLfloat>(m_rgbMidFilteredColor_b),
                            1.0));
            m_frameShaderProgram->setUniformValue("highFilteredColor",
                    QVector4D(static_cast<GLfloat>(m_rgbHighFilteredColor_r),
                            static_cast<GLfloat>(m_rgbHighFilteredColor_g),
                            static_cast<GLfloat>(m_rgbHighFilteredColor_b),
                            1.0));
        }
        if (m_type == ::WaveformWidgetType::RGB || m_type == ::WaveformWidgetType::Stacked ||
                m_type == ::WaveformWidgetType::Spectrum) {
            m_frameShaderProgram->setUniformValue("lowColor",
                    QVector4D(static_cast<GLfloat>(m_rgbLowColor_r),
                            static_cast<GLfloat>(m_rgbLowColor_g),
                            static_cast<GLfloat>(m_rgbLowColor_b),
                            1.0));
            m_frameShaderProgram->setUniformValue("midColor",
                    QVector4D(static_cast<GLfloat>(m_rgbMidColor_r),
                            static_cast<GLfloat>(m_rgbMidColor_g),
                            static_cast<GLfloat>(m_rgbMidColor_b),
                            1.0));
            m_frameShaderProgram->setUniformValue("highColor",
                    QVector4D(static_cast<GLfloat>(m_rgbHighColor_r),
                            static_cast<GLfloat>(m_rgbHighColor_g),
                            static_cast<GLfloat>(m_rgbHighColor_b),
                            1.0));
        } else {
            m_frameShaderProgram->setUniformValue("lowColor",
                    QVector4D(static_cast<GLfloat>(m_lowColor_r),
                            static_cast<GLfloat>(m_lowColor_g),
                            static_cast<GLfloat>(m_lowColor_b),
                            1.0));
            m_frameShaderProgram->setUniformValue("midColor",
                    QVector4D(static_cast<GLfloat>(m_midColor_r),
                            static_cast<GLfloat>(m_midColor_g),
                            static_cast<GLfloat>(m_midColor_b),
                            1.0));
            m_frameShaderProgram->setUniformValue("highColor",
                    QVector4D(static_cast<GLfloat>(m_highColor_r),
                            static_cast<GLfloat>(m_highColor_g),
                            static_cast<GLfloat>(m_highColor_b),
                            1.0));
        }

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, m_textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        m_framebuffer->bind();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // glCallList(m_unitQuadListId);

        glBegin(GL_QUADS);
        {
            if (m_isSlipRenderer && m_waveformRenderer->isSlipActive()) {
                glTexCoord2f(0.0, 0.5);
                glVertex3f(firstVisualIndex, 0.0f, 0.0f);

                glTexCoord2f(1.0, 0.5);
                glVertex3f(lastVisualIndex, 0.0f, 0.0f);
            } else {
                glTexCoord2f(0.0, 0.0);
                glVertex3f(firstVisualIndex, -1.0f, 0.0f);

                glTexCoord2f(1.0, 0.0);
                glVertex3f(lastVisualIndex, -1.0f, 0.0f);
            }

            glTexCoord2f(1.0, 1.0);
            glVertex3f(lastVisualIndex, 1.0f, 0.0f);

            glTexCoord2f(0.0, 1.0);
            glVertex3f(firstVisualIndex, 1.0f, 0.0f);
        }
        glEnd();

        m_framebuffer->release();

        m_frameShaderProgram->release();

        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
    }

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -10.0, 10.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glTranslatef(0.0, 0.0, 0.0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);

    // paint buffer into viewport
    {
        // OpenGL pixels are real screen pixels, not device independent
        // pixels like QPainter provides. We scale the viewport by the
        // devicePixelRatio to render the texture to the surface.
        const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
        glViewport(0,
                0,
                static_cast<GLsizei>(
                        devicePixelRatio * m_waveformRenderer->getWidth()),
                static_cast<GLsizei>(
                        devicePixelRatio * m_waveformRenderer->getHeight()));
        glBindTexture(GL_TEXTURE_2D, m_framebuffer->texture());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glBegin(GL_QUADS);
        {
            glTexCoord2f(0.0, 0.0);
            glVertex3f(-1.0f, -1.0f, 0.0f);

            glTexCoord2f(1.0, 0.0);
            glVertex3f(1.0f, -1.0f, 0.0f);

            glTexCoord2f(1.0, 1.0);
            glVertex3f(1.0f, 1.0f, 0.0f);

            glTexCoord2f(0.0, 1.0);
            glVertex3f(-1.0f, 1.0f, 0.0f);
        }
        glEnd();
    }

    glDisable(GL_TEXTURE_2D);

    // DEBUG
    /*
    glBegin(GL_LINE_LOOP);
    {
        glColor4f(0.5,1.0,0.5,0.75);
        glVertex3f(-1.0f,-1.0f, 0.0f);
        glVertex3f(1.0f, 1.0f, 0.0f);
        glVertex3f(1.0f,-1.0f, 0.0f);
        glVertex3f(-1.0f, 1.0f, 0.0f);
    }
    glEnd();
    */

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
}

} // namespace allshader

#endif // QT_OPENGL_ES_2
