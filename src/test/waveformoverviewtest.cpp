// The overview is drawn with QPainter rather than a shader, so it needs no
// OpenGL and can be checked anywhere.
//
// Two things are worth guarding here. The Spectrum overview must not have the
// chopped edge that a whole-pixel line gives, because it sits next to a deck
// whose edge fades; and the RGB overview must keep exactly the edge it always
// had, because it belongs to whoever chose it rather than to us.

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <memory>

#include "waveform/renderers/waveformoverviewrenderer.h"
#include "waveform/renderers/waveformsignalcolors.h"
#include "skin/legacy/skincontext.h"
#include "waveform/waveform.h"

#include <QDomDocument>

namespace {

/// A waveform of identical bins, loud enough that its column reaches well into
/// the image and leaves room above it to look at.
WaveformPointer makeWaveform(int bins, unsigned char all, int low = 120, int mid = 60, int high = 20) {
    auto pWaveform = WaveformPointer(new Waveform(44100, 44100 * bins, 441, 2 * bins, 0));
    WaveformData* pData = pWaveform->data();
    for (int i = 0; i < 2 * bins; ++i) {
        pData[i].filtered.all = all;
        pData[i].filtered.low = static_cast<unsigned char>(low);
        pData[i].filtered.mid = static_cast<unsigned char>(mid);
        pData[i].filtered.high = static_cast<unsigned char>(high);
    }
    pWaveform->setCompletion(2 * bins);
    return pWaveform;
}

/// Number of rows at the top of a column whose alpha is neither nothing nor
/// full, i.e. the width of the fade.
int countPartialRows(const QImage& image, int x, int centre, int direction) {
    int partial = 0;
    for (int step = 1; step < centre; ++step) {
        const int alpha = image.pixelColor(x, centre + direction * step).alpha();
        if (alpha > 4 && alpha < 250) {
            partial++;
        }
    }
    return partial;
}

} // namespace

class WaveformOverviewTest : public testing::Test {
  protected:
    QImage draw(mixxx::OverviewType type) {
        return drawBands(type, 120, 60, 20);
    }

    QImage drawBands(mixxx::OverviewType type, int low, int mid, int high) {
        constexpr int kBins = 64;
        constexpr unsigned char kAmplitude = 150;
        WaveformPointer pWaveform = makeWaveform(kBins, kAmplitude, low, mid, high);

        // The colours come from the skin, and without them every column is
        // black and nothing is drawn - which would leave both tests passing
        // while checking nothing at all. Hence the minimal skin node below and
        // the assertion further down that something was actually painted.
        QDomDocument document;
        QDomElement node = document.createElement(QStringLiteral("Waveform"));
        const auto addColor = [&](const char* name, const char* value) {
            QDomElement element = document.createElement(QString::fromLatin1(name));
            element.appendChild(document.createTextNode(QString::fromLatin1(value)));
            node.appendChild(element);
        };
        addColor("SignalRGBLowColor", "#ff0000");
        addColor("SignalRGBMidColor", "#00ff00");
        addColor("SignalRGBHighColor", "#0000ff");
        addColor("SignalColor", "#ffffff");
        addColor("SignalLowColor", "#ff0000");
        addColor("SignalMidColor", "#00ff00");
        addColor("SignalHighColor", "#0000ff");
        document.appendChild(node);

        SkinContext context(nullptr, QString());
        WaveformSignalColors colors;
        colors.setup(node, context);

        QImage image(kBins, 2 * 255, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(0, 0, 0, 0));
        QPainter painter(&image);
        painter.translate(0.0, static_cast<double>(image.height()) / 2.0);
        int start = 0;
        if (type == mixxx::OverviewType::Spectrum) {
            waveformOverviewRenderer::drawWaveformPartSpectrum(
                    &painter, pWaveform, &start, 2 * kBins, colors, false);
        } else {
            waveformOverviewRenderer::drawWaveformPartRGB(
                    &painter, pWaveform, &start, 2 * kBins, colors, false);
        }
        painter.end();
        return image;
    }
};

/// Rows of a column that are fully painted. If this is zero the picture is
/// empty and every other measurement on it is meaningless.
int countSolidRows(const QImage& image, int x, int centre, int direction) {
    int solid = 0;
    for (int step = 1; step < centre; ++step) {
        if (image.pixelColor(x, centre + direction * step).alpha() >= 250) {
            solid++;
        }
    }
    return solid;
}

TEST_F(WaveformOverviewTest, TheSpectrumOverviewHasASoftEdge) {
    const QImage image = draw(mixxx::OverviewType::Spectrum);
    const int centre = image.height() / 2;
    ASSERT_GT(countSolidRows(image, 32, centre, -1), 20)
            << "nothing was painted, so this test would pass without checking anything";
    // Four percent of the 255 pixels one unit of amplitude occupies is ten, so
    // anything from a few rows upwards means the edge is a ramp rather than a
    // step. Both halves, because they are drawn by separate branches.
    EXPECT_GE(countPartialRows(image, 32, centre, -1), 4) << "the upper edge is a hard step";
    EXPECT_GE(countPartialRows(image, 32, centre, 1), 4) << "the lower edge is a hard step";
}

TEST_F(WaveformOverviewTest, TheOverviewAndTheDeckAgreeOnColour) {
    // The two are drawn by different code - the deck by a shader, the overview
    // by QPainter - and they have already drifted apart once: the measured band
    // balance moved into the analyser, the deck was updated and this file was
    // not, so the preview applied the old balance a second time on data that
    // already carried it. The same track was one colour in the deck and another
    // in the list.
    //
    // Rather than compare pictures, which differ in every other way, this
    // compares the arithmetic they share: given the same three band values, the
    // hue has to come out the same. That is what a viewer notices and it is
    // what a stray gain breaks.
    constexpr double kToleranceDegrees = 3.0;

    struct Case {
        int low;
        int mid;
        int high;
    };
    constexpr Case kCases[] = {
            {200, 40, 10}, {40, 200, 20}, {20, 40, 200}, {120, 120, 120}, {180, 90, 30}};

    for (const Case& c : kCases) {
        // The overview, through the code the library and the deck overview use.
        const QImage overview = drawBands(mixxx::OverviewType::Spectrum, c.low, c.mid, c.high);
        const int centre = overview.height() / 2;
        const QColor drawn = overview.pixelColor(32, centre - 20);
        ASSERT_GT(drawn.alpha(), 200) << "nothing was drawn for " << c.low << ", " << c.mid << ", "
                                      << c.high;

        // The deck: the same formula the shader applies, with the same colours.
        const double bands[3] = {c.low / 255.0, c.mid / 255.0, c.high / 255.0};
        double rgb[3] = {bands[0], bands[1], bands[2]};
        const double largest = std::max({rgb[0], rgb[1], rgb[2]});
        ASSERT_GT(largest, 0.0);
        QColor expected;
        expected.setRgbF(static_cast<float>(rgb[0] / largest),
                static_cast<float>(rgb[1] / largest),
                static_cast<float>(rgb[2] / largest));

        double difference = std::abs(drawn.hueF() * 360.0 - expected.hueF() * 360.0);
        if (difference > 180.0) {
            difference = 360.0 - difference;
        }
        EXPECT_LT(difference, kToleranceDegrees)
                << "bands " << c.low << ", " << c.mid << ", " << c.high << ": the overview draws "
                << drawn.hueF() * 360.0 << " degrees where the deck draws "
                << expected.hueF() * 360.0;
    }
}

TEST_F(WaveformOverviewTest, TheRgbOverviewIsUnchanged) {
    const QImage image = draw(mixxx::OverviewType::RGB);
    const int centre = image.height() / 2;
    ASSERT_GT(countSolidRows(image, 32, centre, -1), 20)
            << "nothing was painted, so this test would pass without checking anything";
    EXPECT_EQ(countPartialRows(image, 32, centre, -1), 0)
            << "the RGB overview grew a soft edge, which is not ours to change";
    EXPECT_EQ(countPartialRows(image, 32, centre, 1), 0)
            << "the RGB overview grew a soft edge, which is not ours to change";
}
