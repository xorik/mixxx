#include "waveformoverviewrenderer.h"

#include <QPainter>
#include <QStringList>
#include <algorithm>

#include "util/colorcomponents.h"
#include "util/math.h"
#include "util/timer.h"
#include "waveform/renderers/waveformsignalcolors.h"

namespace {

/// Balance between the three bands. ONE for both overview types now, and it is
/// neutral: the measured balance moved into the band filters of the analyser,
/// where it is a property of how a band is measured rather than of how it is
/// drawn.
///
/// It used to sit here as 1.068 / 0.700 / 7.111, and when it moved the deck was
/// updated and this file was not - so the library preview coloured data that
/// the filters had already balanced with the old balance applied a second time,
/// and the same track was one colour in the deck and another in the list. If a
/// band ever looks mis-weighted, the place to look is enginefilterwaveform.h,
/// and there must be exactly one such place.
struct BandColorGain {
    float low;
    float mid;
    float high;
};

constexpr BandColorGain kSpectrumBandColorGain{1.0f, 1.0f, 1.0f};
constexpr BandColorGain kNeutralBandColorGain{1.0f, 1.0f, 1.0f};

/// Width of the soft edge of a column, as a fraction of the distance that one
/// unit of amplitude occupies in this image. The same 4% the deck uses.
///
/// WHAT IT IS A FRACTION OF, because we have already got this wrong twice by
/// carrying over a number without its denominator: not of the height of the
/// image, and not of the height of the column, but of the distance a full scale
/// signal reaches from the centre line. In the stereo overview that is half the
/// image, in the mono one the whole of it, since mono draws from the bottom.
/// The image is scaled to the widget afterwards, so the proportion survives -
/// except that the overview also normalizes each track to its own peak, which
/// stretches the picture by a different amount per track. The fade therefore
/// does not come out identical to the deck; it only stops the edge from being
/// a hard step, which is the point.
constexpr float kSoftEdgeFraction = 0.04f;
/// However short the column, the fade never shrinks below this many pixels of
/// the image, or it would disappear on quiet material before the scaling even
/// happens.
constexpr float kSoftEdgeMinimumPixels = 2.0f;

/// Draws one column from the centre line outwards, fading over its last pixels.
/// `direction` is +1 for downwards in the coordinates of the painter and -1 for
/// upwards; `unitPixels` is what one unit of amplitude measures here.
void drawColumn(QPainter* pPainter,
        int x,
        float height,
        int direction,
        const QColor& color,
        float unitPixels) {
    const int tip = static_cast<int>(height);
    if (tip <= 0) {
        pPainter->setPen(color);
        pPainter->drawPoint(x, 0);
        return;
    }
    const float fade = std::max(kSoftEdgeFraction * unitPixels, kSoftEdgeMinimumPixels);
    const int solid = static_cast<int>(std::max(0.0f, height - fade));

    pPainter->setPen(color);
    if (solid > 0) {
        pPainter->drawLine(x, 0, x, direction * (solid - 1));
    }
    QColor faded = color;
    for (int y = solid; y <= tip; ++y) {
        const float remaining = (height - static_cast<float>(y)) / fade;
        faded.setAlphaF(std::clamp(remaining, 0.0f, 1.0f));
        pPainter->setPen(faded);
        pPainter->drawPoint(x, direction * y);
    }
}

} // namespace

namespace waveformOverviewRenderer {

QImage render(ConstWaveformPointer pWaveform,
        mixxx::OverviewType type,
        const WaveformSignalColors& signalColors,
        bool mono) {
    const int dataSize = pWaveform->getDataSize();
    if (dataSize <= 0) {
        return QImage();
    }

    QImage image(dataSize / 2, 2 * 255, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0, 0, 0, 0).value());

    QPainter painter(&image);
    painter.translate(0.0, static_cast<double>(image.height()) / 2.0);

    if (type == mixxx::OverviewType::HSV) {
        drawWaveformPartHSV(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else if (type == mixxx::OverviewType::Spectrum) {
        drawWaveformPartSpectrum(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else if (type == mixxx::OverviewType::Filtered) {
        drawWaveformPartLMH(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else {
        drawWaveformPartRGB(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    }

    // Evaluate waveform ratio peak
    float peak = 1;
    for (int i = 0; i < dataSize; i += 2) {
        peak = math_max3(
                peak,
                static_cast<float>(pWaveform->getAll(i)),
                static_cast<float>(pWaveform->getAll(i + 1)));
    }
    // Normalize
    float diffGain = 0;
    if (peak > 1) {
        diffGain = 255 - peak - 1;
    }

    const int topLeft = static_cast<int>(mono ? diffGain * 2 : diffGain);
    const QRect sourceRect(0,
            topLeft,
            image.width(),
            image.height() -
                    2 * static_cast<int>(diffGain));
    QImage croppedImage = image.copy(sourceRect);
    // Copy image, otherwise QPainter crashes when we alter it.
    QImage normImage = croppedImage.scaled(image.size(),
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);

    return normImage;
}

/// Shared by the RGB and the Spectrum overview: the same drawing with a
/// different balance between the bands. RGB passes a neutral gain and is
/// therefore exactly what it always was.
void drawWaveformPartRGBWithGain(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono,
        const BandColorGain& gain,
        bool softEdge) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartRGB"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    const QColor lowColor = signalColors.getRgbLowColor();
    const QColor midColor = signalColors.getRgbMidColor();
    const QColor highColor = signalColors.getRgbHighColor();
    QColor color;

    float lowColor_r = 0, lowColor_g = 0, lowColor_b = 0,
          midColor_r = 0, midColor_g = 0, midColor_b = 0,
          highColor_r = 0, highColor_g = 0, highColor_b = 0,
          all = 0, low = 0, mid = 0, high = 0,
          red = 0, green = 0, blue = 0, max = 0;

    getRgbF(lowColor, &lowColor_r, &lowColor_g, &lowColor_b);
    getRgbF(midColor, &midColor_r, &midColor_g, &midColor_b);
    getRgbF(highColor, &highColor_r, &highColor_g, &highColor_b);

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            // Left
            all = pWaveform->getAll(i) + pWaveform->getAll(i + 1);
            low = pWaveform->getLow(i) + pWaveform->getLow(i + 1);
            mid = pWaveform->getMid(i) + pWaveform->getMid(i + 1);
            high = pWaveform->getHigh(i) + pWaveform->getHigh(i + 1);

            low *= gain.low;
            mid *= gain.mid;
            high *= gain.high;

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;
            // Normalize
            max = math_max3(red, green, blue);
            // Draw
            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                if (softEdge) {
                    // Mono draws from the bottom, so a full scale signal
                    // reaches the whole height of the image.
                    drawColumn(pPainter, x, all, 1, color, 2.0f * 255.0f);
                } else {
                    pPainter->setPen(color);
                    pPainter->drawLine(x, static_cast<int>(all), x, 0);
                }
            }
        }
    } else { // stereo
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            // Left
            all = pWaveform->getAll(i);
            low = pWaveform->getLow(i);
            mid = pWaveform->getMid(i);
            high = pWaveform->getHigh(i);

            low *= gain.low;
            mid *= gain.mid;
            high *= gain.high;

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;
            // Normalize
            max = math_max3(red, green, blue);
            // Draw
            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                if (softEdge) {
                    drawColumn(pPainter, x, all, -1, color, 255.0f);
                } else {
                    pPainter->setPen(color);
                    pPainter->drawLine(x, static_cast<int>(-all), x, 0);
                }
            }

            // Right
            all = pWaveform->getAll(i + 1);
            low = pWaveform->getLow(i + 1);
            mid = pWaveform->getMid(i + 1);
            high = pWaveform->getHigh(i + 1);

            low *= gain.low;
            mid *= gain.mid;
            high *= gain.high;

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;

            max = math_max3(red, green, blue);

            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                if (softEdge) {
                    drawColumn(pPainter, x, all, 1, color, 255.0f);
                } else {
                    pPainter->setPen(color);
                    pPainter->drawLine(x, 0, x, static_cast<int>(all));
                }
            }
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartRGB(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    drawWaveformPartRGBWithGain(
            pPainter, pWaveform, start, end, signalColors, mono, kNeutralBandColorGain, false);
}

void drawWaveformPartSpectrum(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    drawWaveformPartRGBWithGain(
            pPainter, pWaveform, start, end, signalColors, mono, kSpectrumBandColorGain, true);
}

void drawWaveformPartLMH(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartLMH"));
    const QColor lowColor = signalColors.getLowColor();
    const QColor midColor = signalColors.getMidColor();
    const QColor highColor = signalColors.getHighColor();
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);

        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            x = i / 2;
            pPainter->setPen(lowColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getLow(i) + pWaveform->getLow(i + 1)));

            pPainter->setPen(midColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getMid(i) + pWaveform->getMid(i + 1)));

            pPainter->setPen(highColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getHigh(i) + pWaveform->getHigh(i + 1)));
        }
    } else { // stereo
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            x = i / 2;
            pPainter->setPen(lowColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getLow(i)),
                    QPoint(x, pWaveform->getLow(i + 1)));

            pPainter->setPen(midColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getMid(i)),
                    QPoint(x, pWaveform->getMid(i + 1)));

            pPainter->setPen(highColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getHigh(i)),
                    QPoint(x, pWaveform->getHigh(i + 1)));
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartHSV(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartHSV"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    float h = 0, s = 0, v = 0, lo = 0, hi = 0, total = 0;
    // Get HSV of low color.
    const QColor lowColor = signalColors.getLowColor();
    getHsvF(lowColor, &h, &s, &v);
    QColor color;

    unsigned char low[2] = {0, 0};
    unsigned char high[2] = {0, 0};
    unsigned char mid[2] = {0, 0};
    unsigned char all[2] = {0, 0};

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);
    }

    for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
        x = i / 2;
        all[0] = pWaveform->getAll(i);
        all[1] = pWaveform->getAll(i + 1);

        if (!all[0] && !all[1]) {
            continue;
        }

        low[0] = pWaveform->getLow(i);
        low[1] = pWaveform->getLow(i + 1);
        mid[0] = pWaveform->getMid(i);
        mid[1] = pWaveform->getMid(i + 1);
        high[0] = pWaveform->getHigh(i);
        high[1] = pWaveform->getHigh(i + 1);

        total = (low[0] + low[1] + mid[0] + mid[1] +
                        high[0] + high[1]) *
                1.2f;

        // Prevent division by zero
        if (total > 0) {
            // Normalize low and high
            // (mid not need, because it not change the color)
            lo = (low[0] + low[1]) / total;
            hi = (high[0] + high[1]) / total;
        } else {
            lo = hi = 0.0;
        }

        // Set color
        color.setHsvF(h, 1.0f - hi, 1.0f - lo);

        if (mono) {
            pPainter->setPen(color);
            pPainter->drawLine(QPoint(i / 2, 0),
                    QPoint(i / 2, all[0] + all[1]));
        } else {
            pPainter->setPen(color);
            pPainter->drawLine(QPoint(i / 2, -all[0]),
                    QPoint(i / 2, all[1]));
        }
    }

    if (start) {
        *start = end;
    }
}

} // namespace waveformOverviewRenderer
