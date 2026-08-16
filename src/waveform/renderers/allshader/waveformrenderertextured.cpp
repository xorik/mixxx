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

// Everything below was chosen by comparing frames against Traktor, on the test
// files where the comparison goes by segment and on music. The reasoning is
// kept next to each number, because it is the only thing that explains why a
// different value would be worse.

// Radius of the window the colour of a column is averaged over, in visual bins
// (441 bins per second), so four bins is about 9 ms - the grid Traktor appears
// to use. Wider windows score better on every number we could measure (colour
// jitter between neighbouring columns keeps falling up to about 12 bins, the
// sharpness of transitions keeps improving) and look worse: averaging over
// 27 ms mixes neighbouring columns together, invents orange between red and
// green and washes out the saturation. The numbers measured how fast the
// colour changes, not which colours appear.
constexpr float kColorSmoothBins = 4.0f;

// Width of the soft edge. Traktor fades out over 3-4 device pixels, but that
// was measured on a waveform 174 pixels tall, i.e. about 4% of its half-height,
// and the proportion is what has to be carried over rather than the pixels: on
// a deck twice as tall the same three pixels look like a hard cut. The floor in
// device pixels keeps the fade from disappearing on a small deck.
constexpr float kSoftEdgeFraction = 0.04f;
constexpr float kSoftEdgePixels = 2.0f;

// Minimum visible half-height of a column that carries any signal. In Traktor
// quiet columns stop following the amplitude and sit on a plateau: measured
// over three loudness buckets below 20% it is 0.214 of the half-height. This
// value reproduces it - the median height of our own quiet columns comes out
// at 0.209.
constexpr float kAmplitudeFloor = 0.19f;

// Compression of the band values before they become a colour. Traktor stores
// the square root of the band magnitude, so 0.5 would imitate it, but the
// error against the reference turns out to be flat in this parameter: over
// nine tracks the spread between 0.40 and 1.00 is 0.2 to 2.8 degrees of hue
// against a median error of 16, i.e. noise. One operation less.
constexpr float kColorGamma = 1.0f;

// Level below which the colour of a column is no longer normalized to full
// brightness. Without it a column that carries almost nothing is divided by
// its own maximum and comes out fully saturated with a hue decided by noise -
// a silent intro turned into a solid bright green stripe. This value keeps the
// brightness of quiet columns relative to loud ones at 1.26 against the 1.23
// measured in Traktor; the next value we tried, 0.08, gave 0.48, i.e. quiet
// material twice as dark as loud where it should be slightly brighter.
constexpr float kColorLevelFloor = 0.01f;

// Balance between the three bands, applied to the colour only, never to the
// height. The high band is raised by 17 dB, which is what the measurement of
// Traktor says and what the RMS band magnitudes of the analyzer need: without
// it the share of blue columns on our material is 0.4%. The mid band is held
// back to 0.7, which takes the share of yellow-green columns from 6.1% to
// 1.2% and the columns where green dominates from 11% to 2.7%, against 3.0%
// and 7.5% measured in Traktor.
constexpr float kBandColorGainLow = 1.068f;
constexpr float kBandColorGainMid = 0.7f;
constexpr float kBandColorGainHigh = 7.111f;

// Vertical shading of a column, see verticalProfile() in the shader. The
// thinning is a band inside the body: at these positions, measured from the
// centre of the column in units of its own half height, and this wide. Tonal
// columns are thinned closer to the centre, percussive ones closer to the rim.
//
// The band deliberately reaches neither end of the column. A monotonic profile
// was tried first and failed twice over: at the centre it opened a hole
// through which the axis line showed as a white stripe, and at the rim it fell
// where the soft edge already fades, so it was invisible.
constexpr float kVerticalStrength = 0.6f;
constexpr float kDipCenterTonal = 0.35f;
constexpr float kDipCenterImpulsive = 0.75f;
constexpr float kDipWidth = 0.25f;

// The crest factor at which a column is drawn flat, and how fast the shading
// follows it away from there.
//
// These two are calibrated on data, not on theory, and the difference matters.
// A sine has a crest factor of 1.41, so that looked like the natural neutral
// point - but the crest factor we compute divides the stored peak by the
// stored bands, and the bands are shaped by the frequency responses of the
// analyzer, which makes the ratio systematically larger than the acoustic one.
// Measured over the whole of seven analysed tracks (between 53 000 and 351 000
// bins each): the median is 2.11 to 2.51 with a mean of 2.24, the fifth
// percentile 1.28 to 1.56 and the ninety fifth 2.94 to 3.25. Stable enough to
// take 2.3 as the point where a column is drawn flat, and 1.1 as the scale, so
// that the extremes of that distribution reach the full profile.
//
// If the shading ever looks one sided on other material, this is the number to
// recompute, and the way to do it is to read the analysis files rather than to
// reason about waveforms.
constexpr float kCrestNeutral = 2.3f;
constexpr float kCrestScale = 1.1f;

// Below this band level the crest factor is a ratio of a few units of one byte
// each, i.e. noise, and the column is drawn flat instead.
constexpr float kCrestLevelFloor = 0.02f;

// The knobs of the mixer do not reach this waveform: it draws the file, the
// way Traktor does, and not the current position of the EQ knobs, their kill
// switches or the gain knob. The ReplayGain of the track does reach it,
// because it is a property of the file rather than a knob, and with it the
// height answers "how loud will this sound" instead of "what is in the file".
constexpr bool kEqAffectsDrawing = false;
constexpr bool kReplayGainAffectsHeight = true;

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
    if (m_type == ::WaveformWidgetType::Spectrum && !kEqAffectsDrawing) {
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
        if (kReplayGainAffectsHeight) {
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
        // What is logged is what the shader actually received, not what the
        // code above says it should be. A value that silently did not arrive,
        // or a waveform type that was silently replaced by another, is the
        // mistake that has cost this project the most time so far, and this
        // line is how both were caught.
        const QVector3D gain(kBandColorGainLow, kBandColorGainMid, kBandColorGainHigh);
        const QStringList applied = {
                QStringLiteral("BENCHHIT colorSmoothBins=") +
                        QString::number(kColorSmoothBins),
                QStringLiteral("type=") + QString::number(static_cast<int>(m_type)),
                QStringLiteral("options=") +
                        QString::number(static_cast<int>(
                                static_cast<::WaveformRendererSignalBase::Options::Int>(
                                        m_options))),
                QStringLiteral("shader=") + m_fragShader,
                QStringLiteral("softEdgePixels=") + QString::number(kSoftEdgePixels),
                QStringLiteral("amplitudeFloor=") + QString::number(kAmplitudeFloor),
                QStringLiteral("colorLevelFloor=") +
                        QString::number(kColorLevelFloor),
                QStringLiteral("colorGamma=") + QString::number(kColorGamma),
                QStringLiteral("verticalStrength=") + QString::number(kVerticalStrength),
                QStringLiteral("dipCenterTonal=") + QString::number(kDipCenterTonal),
                QStringLiteral("dipCenterImpulsive=") + QString::number(kDipCenterImpulsive),
                QStringLiteral("crestNeutral=") + QString::number(kCrestNeutral),
                QStringLiteral("crestScale=") + QString::number(kCrestScale),
                QStringLiteral("bandColorGain=") + QString::number(gain.x()) +
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
            m_frameShaderProgram->setUniformValue("colorSmoothBins", kColorSmoothBins);
            // The shader works in frame buffer pixels, the tunable is in
            // device pixels.
            m_frameShaderProgram->setUniformValue("softEdgeFraction", kSoftEdgeFraction);
            m_frameShaderProgram->setUniformValue("softEdgePixels",
                    kSoftEdgePixels * static_cast<float>(kOversamplingFactor));
            m_frameShaderProgram->setUniformValue("amplitudeFloor", kAmplitudeFloor);
            m_frameShaderProgram->setUniformValue("bandColorGain",
                    QVector3D(kBandColorGainLow, kBandColorGainMid, kBandColorGainHigh));
            m_frameShaderProgram->setUniformValue("colorGamma", kColorGamma);
            m_frameShaderProgram->setUniformValue("colorLevelFloor", kColorLevelFloor);
            m_frameShaderProgram->setUniformValue("verticalStrength", kVerticalStrength);
            m_frameShaderProgram->setUniformValue("dipCenterTonal", kDipCenterTonal);
            m_frameShaderProgram->setUniformValue("dipCenterImpulsive", kDipCenterImpulsive);
            m_frameShaderProgram->setUniformValue("dipWidth", kDipWidth);
            m_frameShaderProgram->setUniformValue("crestNeutral", kCrestNeutral);
            m_frameShaderProgram->setUniformValue("crestScale", kCrestScale);
            m_frameShaderProgram->setUniformValue("crestLevelFloor", kCrestLevelFloor);
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
