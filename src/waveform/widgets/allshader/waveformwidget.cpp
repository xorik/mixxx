
#include "waveform/widgets/allshader/waveformwidget.h"

#include <QApplication>
#include <QImage>
#include <QWheelEvent>
#include <algorithm>

#include "control/controlproxy.h"

#include "rendergraph/engine.h"
#include "rendergraph/opacitynode.h"
#include "waveform/renderers/allshader/waveformrenderbackground.h"
#include "waveform/renderers/allshader/waveformrenderbeat.h"
#include "waveform/renderers/allshader/waveformrendererendoftrack.h"
#include "waveform/renderers/allshader/waveformrendererfiltered.h"
#include "waveform/renderers/allshader/waveformrendererhsv.h"
#include "waveform/renderers/allshader/waveformrendererpreroll.h"
#include "waveform/renderers/allshader/waveformrendererrgb.h"
#include "waveform/renderers/allshader/waveformrenderersimple.h"
#include "waveform/renderers/allshader/waveformrendererslipmode.h"
#include "waveform/renderers/allshader/waveformrendererstem.h"
#include "waveform/renderers/allshader/waveformrenderertextured.h"
#include "waveform/renderers/allshader/waveformrendermark.h"
#include "waveform/renderers/allshader/waveformrendermarkrange.h"
#include "waveform/waveformwidgetfactory.h"
#include "waveform/widgets/allshader/moc_waveformwidget.cpp"

namespace allshader {

WaveformWidget::WaveformWidget(QWidget* parent,
        WaveformWidgetType::Type type,
        const QString& group,
        ::WaveformRendererSignalBase::Options options)
        : WGLWidget(parent),
          WaveformWidgetAbstract(group),
          m_type(type),
          m_pWaveformRenderMarkSlip(nullptr),
          m_pWaveformRendererSignal(nullptr) {
    auto pTopNode = std::make_unique<rendergraph::Node>();
    auto pOpacityNode = std::make_unique<rendergraph::OpacityNode>();

    pTopNode->appendChildNode(addRendererNode<WaveformRenderBackground>());
    auto pEndOfTrackRenderer = addRendererNode<WaveformRendererEndOfTrack>();
    pEndOfTrackRenderer->setEndOfTrackWarningTime(
            WaveformWidgetFactory::instance()->getEndOfTrackWarningTime());
    pOpacityNode->appendChildNode(std::move(pEndOfTrackRenderer));
    pOpacityNode->appendChildNode(addRendererNode<WaveformRendererPreroll>());
    m_pWaveformRenderMarkRange = pOpacityNode->appendChildNode(
            addRendererNode<WaveformRenderMarkRange>());

#ifdef __STEM__
    // The following two renderers work in tandem: if the rendered waveform is
    // for a stem track, WaveformRendererSignalBase will skip rendering and let
    // WaveformRendererStem do the rendering, and vice-versa.
    pOpacityNode->appendChildNode(addRendererNode<WaveformRendererStem>());
#endif
    std::unique_ptr<WaveformRendererSignalBase> pWaveformRendererSignal = addWaveformSignalRenderer(
            type, options, ::WaveformRendererAbstract::Play);
    m_pWaveformRendererSignal = pWaveformRendererSignal.get();
    if (pWaveformRendererSignal) {
        auto* pNode = dynamic_cast<rendergraph::BaseNode*>(pWaveformRendererSignal.release());
        DEBUG_ASSERT(pNode);
        pOpacityNode->appendChildNode(std::unique_ptr<rendergraph::BaseNode>(pNode));
    }
    pOpacityNode->appendChildNode(addRendererNode<WaveformRenderBeat>());
    m_pWaveformRenderMark = pOpacityNode->appendChildNode(addRendererNode<WaveformRenderMark>());

    // if the added signal renderer supports slip, we add it again, now for
    // slip, together with the other slip renderers
    if (m_pWaveformRendererSignal && m_pWaveformRendererSignal->supportsSlip()) {
        // The following renderer will add an overlay waveform if a slip is in progress
        pOpacityNode->appendChildNode(addRendererNode<WaveformRendererSlipMode>());
        pOpacityNode->appendChildNode(
                addRendererNode<WaveformRendererPreroll>(
                        ::WaveformRendererAbstract::Slip));
#ifdef __STEM__
        pOpacityNode->appendChildNode(
                addRendererNode<WaveformRendererStem>(
                        ::WaveformRendererAbstract::Slip));
#endif
        std::unique_ptr<WaveformRendererSignalBase> pSlipNode = addWaveformSignalRenderer(
                type, options, ::WaveformRendererAbstract::Slip);
        auto* pNode = dynamic_cast<rendergraph::BaseNode*>(pSlipNode.release());
        DEBUG_ASSERT(pNode);
        pOpacityNode->appendChildNode(std::unique_ptr<rendergraph::BaseNode>(pNode));
        pOpacityNode->appendChildNode(
                addRendererNode<WaveformRenderBeat>(
                        ::WaveformRendererAbstract::Slip));
        m_pWaveformRenderMarkSlip = pOpacityNode->appendChildNode(
                addRendererNode<WaveformRenderMark>(
                        ::WaveformRendererAbstract::Slip));
    }

    m_initSuccess = init();

    m_pOpacityNode = pTopNode->appendChildNode(std::move(pOpacityNode));

    m_pEngine = std::make_unique<rendergraph::Engine>(std::move(pTopNode));
}

WaveformWidget::~WaveformWidget() {
    makeCurrentIfNeeded();
    m_rendererStack.clear();
    // destruction of nodes needs to happen within the opengl context
    m_pEngine.reset();
    doneCurrent();
}

std::unique_ptr<WaveformRendererSignalBase>
WaveformWidget::addWaveformSignalRenderer(WaveformWidgetType::Type type,
        ::WaveformRendererSignalBase::Options options,
        ::WaveformRendererAbstract::PositionSource positionSource) {
#ifndef QT_OPENGL_ES_2
    if (options & ::WaveformRendererSignalBase::Option::HighDetail) {
        switch (type) {
        case ::WaveformWidgetType::RGB:
        case ::WaveformWidgetType::Traktor:
        case ::WaveformWidgetType::Filtered:
        case ::WaveformWidgetType::Stacked:
            return addWaveformSignalRenderer<WaveformRendererTextured>(
                    type, positionSource, options);
        default:
            break;
        }
    }
#endif

    switch (type) {
    case ::WaveformWidgetType::Simple:
        return addWaveformSignalRenderer<WaveformRendererSimple>(options);
    case ::WaveformWidgetType::RGB:
    case ::WaveformWidgetType::Traktor:
        // Without the HighDetail option there is no textured renderer, so the
        // Traktor style falls back to the geometry based RGB waveform.
        return addWaveformSignalRenderer<WaveformRendererRGB>(positionSource, options);
    case ::WaveformWidgetType::HSV:
        return addWaveformSignalRenderer<WaveformRendererHSV>(options);
    case ::WaveformWidgetType::Filtered:
        return addWaveformSignalRenderer<WaveformRendererFiltered>(false, options);
    case ::WaveformWidgetType::Stacked:
        return addWaveformSignalRenderer<WaveformRendererFiltered>(
                true, options); // true for RGB Stacked
    default:
        break;
    }
    return nullptr;
}

mixxx::Duration WaveformWidget::render() {
    makeCurrentIfNeeded();
    paintGL();
    doneCurrent();
    // In the legacy widgets, this is used to "return timer for painter setup"
    // which is not relevant here. Also note that the return value is not used
    // at all, so it might be better to remove it everywhere. In the meantime.
    // we need to return something for API compatibility.
    return mixxx::Duration();
}

void WaveformWidget::paintGL() {
    // opacity of 0.f effectively skips the subtree rendering
    m_pOpacityNode->setOpacity(shouldOnlyDrawBackground() ? 0.f : 1.f);

    m_pWaveformRenderMark->update();
    m_pWaveformRenderMarkRange->update();
    if (m_pWaveformRenderMarkSlip) {
        m_pWaveformRenderMarkSlip->update();
    }

    m_pEngine->preprocess();
    m_pEngine->render();

    grabFrameIfRequested();
}

// Writes the content of the waveform widget to a PNG file, once, after
// MIXXX_WF_GRAB_AFTER frames. This reads back the frame buffer that was just
// drawn, so what lands in the file is exactly what the shader produced,
// unscaled and independent of what covers the window on the screen.
// Enabled by setting MIXXX_WF_GRAB to a path prefix, e.g.
//   MIXXX_WF_GRAB=/tmp/shot build/mixxx ...
// writes /tmp/shot-[Channel1].png.
void WaveformWidget::grabFrameIfRequested() {
    if (m_grabDone) {
        return;
    }
    static const QString prefix = qEnvironmentVariable("MIXXX_WF_GRAB");
    if (prefix.isEmpty()) {
        m_grabDone = true;
        return;
    }
    static const int grabAfterFrames =
            qEnvironmentVariableIntValue("MIXXX_WF_GRAB_AFTER") > 0
            ? qEnvironmentVariableIntValue("MIXXX_WF_GRAB_AFTER")
            : 300;
    // Decks load paused at the cue point, where there is usually no music yet.
    // MIXXX_WF_SEEK moves them to a given second of the track a little before
    // the grab, without ever starting playback.
    ++m_framesRendered;
    if (m_framesRendered == std::max(grabAfterFrames - 120, 1)) {
        const double seconds = qEnvironmentVariable("MIXXX_WF_SEEK").toDouble();
        if (seconds > 0.0) {
            ControlProxy duration(getGroup(), QStringLiteral("duration"));
            const double trackSeconds = duration.get();
            if (trackSeconds > 0.0) {
                ControlProxy playPosition(getGroup(), QStringLiteral("playposition"));
                playPosition.set(seconds / trackSeconds);
                qDebug() << "WaveformWidget - seeking" << getGroup() << "to" << seconds << "s of"
                         << trackSeconds << "s";
            }
        }
    }
    if (m_framesRendered < grabAfterFrames) {
        return;
    }
    m_grabDone = true;

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0) {
        return;
    }
    QImage image(width, height, QImage::Format_RGBA8888);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(viewport[0], viewport[1], width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    const QString path = prefix + QStringLiteral("-") + getGroup() + QStringLiteral(".png");
    if (image.mirrored(false, true).save(path)) {
        qDebug() << "WaveformWidget - wrote" << path << width << "x" << height;
    } else {
        qWarning() << "WaveformWidget - could not write" << path;
    }
}

void WaveformWidget::castToQWidget() {
    m_widget = this;
}

void WaveformWidget::initializeGL() {
}

void WaveformWidget::resizeRenderer(int, int, float) {
    // This is called when the widget is resized, but as this is a WGLWidget, we
    // also get the resizeGL call and use that instead, as it has the opengl
    // context set.
}

void WaveformWidget::resizeGL(int w, int h) {
    w = static_cast<int>(std::lround(static_cast<qreal>(w) / devicePixelRatioF()));
    h = static_cast<int>(std::lround(static_cast<qreal>(h) / devicePixelRatioF()));

    // Many allshader components relies on WaveformWidgetRenderer::getWidth and
    // WaveformWidgetRenderer::getHeight to update their rendering stack, so we
    // must resize the renderer first, before updating the rendergraph
    WaveformWidgetRenderer::resizeRenderer(w, h, static_cast<float>(devicePixelRatio()));
    m_pEngine->resize(w, h);
}

void WaveformWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
}

void WaveformWidget::wheelEvent(QWheelEvent* pEvent) {
    QApplication::sendEvent(parentWidget(), pEvent);
    pEvent->accept();
}

void WaveformWidget::leaveEvent(QEvent* pEvent) {
    QApplication::sendEvent(parentWidget(), pEvent);
    pEvent->accept();
}

/* static */
WaveformWidgetVars WaveformWidget::vars() {
    WaveformWidgetVars result;
    result.m_useGL = true;
    result.m_useGLES = true;
    result.m_useGLSL = true;
    result.m_category = WaveformWidgetCategory::AllShader;
    return result;
}

} // namespace allshader
