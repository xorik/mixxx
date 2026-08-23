#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>

#include "moc_waveformrenderbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace allshader {

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRendererAbstract(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
    // RGBA rather than one colour for the whole grid: the first beat of a bar
    // is drawn at the full alpha of the beat grid and the other three at half
    // of it, so the alpha has to travel per vertex. One geometry and one draw
    // call either way.
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_color = QColor(skinContext.selectString(node, QStringLiteral("BeatColor")));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderBeat::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
    }
}

bool WaveformRenderBeat::preprocessInner() {
    const TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return false;
    }

#ifndef __SCENEGRAPH__
    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return false;
    }
    m_color.setAlphaF(alpha / 100.0f);
#endif

    if (!m_color.alpha()) {
        // Don't render the beatgrid lines is there are fully transparent
        return true;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return false;
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6; // 2 triangles

    // Count the number of beats in the range to reserve space in the m_vertices vector.
    // Note that we could also use
    //   int numBearsInRange = trackBeats->numBeatsInRange(startPosition, endPosition);
    // for this, but there have been reports of that method failing with a DEBUG_ASSERT.
    int numBeatsInRange = 0;
    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it) {
        numBeatsInRange++;
    }

    const int reserved = numBeatsInRange * numVerticesPerLine;
    geometry().allocate(reserved);

    RGBAVertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};

    // Which beat of the bar each line is. Counted from the first beat of the
    // track in fours, because that is all the information there is: Mixxx knows
    // where the beats are and not where a bar starts.
    //
    // NOTE, and it matters on real tracks: firstBeat is the first beat the
    // ANALYSER found, not the musical downbeat. Where the analyser latched onto
    // an off-beat - a shaker before the first kick, a pickup bar - the accent
    // lands on the wrong beat of the bar, consistently, for the whole track.
    // Moving the first beat in the beat editor fixes it; nothing here can,
    // because the information is not in the file.
    constexpr int kBeatsPerBar = 4;
    const mixxx::audio::FramePos firstBeatPosition = trackBeats->firstBeat();
    int beatIndex = 0;
    if (firstBeatPosition.isValid()) {
        int countedFromFirst = 0;
        for (auto counter = trackBeats->iteratorFrom(firstBeatPosition);
                counter != trackBeats->cend() && *counter < startPosition;
                ++counter) {
            countedFromFirst++;
        }
        beatIndex = countedFromFirst % kBeatsPerBar;
    }

    const float red = static_cast<float>(m_color.redF());
    const float green = static_cast<float>(m_color.greenF());
    const float blue = static_cast<float>(m_color.blueF());
    const float fullAlpha = static_cast<float>(m_color.alphaF());
    // Half the alpha of the grid, not a fixed number: the user sets how visible
    // the grid is in the preferences, and the accent has to keep following it.
    const float weakAlpha = fullAlpha * 0.5f;

    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it, beatIndex = (beatIndex + 1) % kBeatsPerBar) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const float x1 = static_cast<float>(xBeatPoint);
        const float x2 = x1 + 1.f;

        const float alpha = beatIndex == 0 ? fullAlpha : weakAlpha;
        vertexUpdater.addRectangle({x1, 0.f},
                {x2, m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth},
                {red, green, blue, alpha});
    }
    markDirtyGeometry();

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    return true;
}

} // namespace allshader
