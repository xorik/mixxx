#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>
#include <cmath>

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

    // Which beat of the bar each line is.
    //
    // COUNTED FROM THE ANCHOR OF THE GRID, not from the first beat, and the
    // difference is the whole feature. For a constant tempo track the beats are
    // anchor + k * interval for every integer k, so moving the anchor by a
    // whole number of beats leaves every beat exactly where it was and changes
    // only which of them is number zero. That is how the user marks where a bar
    // begins - adjust_beatgrid puts the anchor on the beat nearest the playhead
    // - without a single line on the waveform moving.
    //
    // Counting from the first beat cannot do this: the first beat is whichever
    // beat lands at or after the start of the track, and it does not move when
    // the anchor does. Nor is there anywhere else to keep the mark - the anchor
    // is the only thing in the grid that can carry it, and it already persists.
    //
    // A track of varying tempo has no single anchor, so it falls back to the
    // first beat and the mark cannot be moved. The user does not use those.
    constexpr int kBeatsPerBar = 4;
    const bool constantTempo = trackBeats->hasConstantTempo();
    const mixxx::audio::FramePos anchorPosition =
            constantTempo ? trackBeats->getLastMarkerPosition() : trackBeats->firstBeat();
    const double beatLength = constantTempo ? trackBeats->anchorBeatLengthFrames() : 0.0;

    int beatIndex = 0;
    if (constantTempo && beatLength > 0.0) {
        // Arithmetic rather than counting: the anchor can be thousands of beats
        // away from what is on screen, and walking there every frame would cost
        // the same as drawing it.
        const double beatsFromAnchor =
                (startPosition - anchorPosition) / beatLength;
        const int firstIndex = static_cast<int>(std::ceil(beatsFromAnchor - 1e-6));
        beatIndex = ((firstIndex % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar;
    } else if (anchorPosition.isValid()) {
        // Varying tempo: no interval to divide by, so the beats are counted.
        // The count has to start on the beat the drawing starts on, or the two
        // walk apart as the window moves - see the note in the history of this
        // file.
        int offset = 0;
        auto counter = trackBeats->iteratorFrom(startPosition);
        if (counter != trackBeats->cend() && *counter < anchorPosition) {
            for (; counter != trackBeats->cend() && *counter < anchorPosition; ++counter) {
                offset--;
            }
        } else {
            for (auto forward = trackBeats->iteratorFrom(anchorPosition);
                    forward != trackBeats->cend() && *forward < startPosition;
                    ++forward) {
                offset++;
            }
        }
        beatIndex = ((offset % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar;
    }

    // NOTE for whoever reads this next: the anchor starts life where the
    // ANALYSER put it, which is not the musical downbeat. Until the user marks
    // one with adjust_beatgrid, every accent in the track may sit on the same
    // wrong beat of the bar. That is not something the renderer can work out -
    // Mixxx stores where the beats are, not where a bar begins.

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
