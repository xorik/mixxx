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
#include <memory>

#include "waveform/renderers/waveformoverviewrenderer.h"
#include "waveform/renderers/waveformsignalcolors.h"
#include "skin/legacy/skincontext.h"
#include "waveform/waveform.h"

#include <QDomDocument>

namespace {

/// A waveform of identical bins, loud enough that its column reaches well into
/// the image and leaves room above it to look at.
WaveformPointer makeWaveform(int bins, unsigned char all) {
    auto pWaveform = WaveformPointer(new Waveform(44100, 44100 * bins, 441, 2 * bins, 0));
    WaveformData* pData = pWaveform->data();
    for (int i = 0; i < 2 * bins; ++i) {
        pData[i].filtered.all = all;
        pData[i].filtered.low = 120;
        pData[i].filtered.mid = 60;
        pData[i].filtered.high = 20;
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
        constexpr int kBins = 64;
        constexpr unsigned char kAmplitude = 150;
        WaveformPointer pWaveform = makeWaveform(kBins, kAmplitude);

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
