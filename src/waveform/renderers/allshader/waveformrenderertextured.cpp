#include "waveform/renderers/allshader/waveformrenderertextured.h"

#ifndef QT_OPENGL_ES_2

#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QStringList>
#include <QVector3D>
#include <algorithm>

#include "control/controlproxy.h"
#include "waveform/renderers/allshader/spectrumparams.h"
#include "moc_waveformrenderertextured.cpp"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

namespace {
const QString kPassthroughShaderPath = QStringLiteral(":/shaders/passthrough.vert");

// We render into a frame buffer that is this much larger than the renderer
// itself to "oversample" the texture relative to the surface we're drawing on.
constexpr int kOversamplingFactor = 4;

// The numbers that define how this waveform looks live in spectrumparams.h,
// so that the shader test can check the very values used here.
using namespace mixxx::spectrumwaveform;

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
                QStringLiteral("BENCHHIT"),
                QStringLiteral("type=") + QString::number(static_cast<int>(m_type)),
                QStringLiteral("options=") +
                        QString::number(static_cast<int>(
                                static_cast<::WaveformRendererSignalBase::Options::Int>(
                                        m_options))),
                QStringLiteral("shader=") + m_fragShader,
                QStringLiteral("subColumnSamples=") + QString::number(kSubColumnSamples),
                QStringLiteral("softEdgePixels=") + QString::number(kSoftEdgePixels),
                QStringLiteral("amplitudeFloor=") + QString::number(kAmplitudeFloor),
                QStringLiteral("colorLevelFloor=") +
                        QString::number(kColorLevelFloor),
                QStringLiteral("colorGamma=") + QString::number(kColorGamma),
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
            // The shader works in frame buffer pixels, the tunable is in
            // device pixels.
            m_frameShaderProgram->setUniformValue("subColumnSamples", kSubColumnSamples);
            m_frameShaderProgram->setUniformValue("pixelsPerScreenPixel",
                    static_cast<float>(kOversamplingFactor));
            m_frameShaderProgram->setUniformValue("softEdgeFraction", kSoftEdgeFraction);
            m_frameShaderProgram->setUniformValue("softEdgePixels",
                    kSoftEdgePixels * static_cast<float>(kOversamplingFactor));
            m_frameShaderProgram->setUniformValue("amplitudeFloor", kAmplitudeFloor);
            m_frameShaderProgram->setUniformValue("bandColorGain",
                    QVector3D(kBandColorGainLow, kBandColorGainMid, kBandColorGainHigh));
            m_frameShaderProgram->setUniformValue("colorGamma", kColorGamma);
            m_frameShaderProgram->setUniformValue("colorLevelFloor", kColorLevelFloor);
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
