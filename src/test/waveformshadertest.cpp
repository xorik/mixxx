// Renders the real fragment shader of the Spectrum waveform off screen and
// checks properties of the result. There is no window and no Mixxx: a hidden
// surface, a frame buffer, and the shader straight out of the resources.
//
// It runs the actual res/shaders/spectrumsignal.frag rather than a copy of its
// arithmetic in C++, and it reads the parameters from spectrumparams.h, which
// the renderer uses as well. Both are on purpose: a test with its own copy of
// either would keep passing while the copy and the original drifted apart.
//
// Every check here corresponds to a mistake that was actually made and that
// cost a round of "the user looks at it and says it is wrong":
//   * the colours of a column       - the band balance regressing silently;
//   * the soft edge at two sizes    - carrying over an absolute number of
//                                     pixels where the measurement was a
//                                     fraction of the height;
//   * alpha never reaching zero     - the axis line showing through the body
//                                     as a white stripe;
//   * tonal against percussive      - a vertical shading that is present in
//                                     the arithmetic and invisible on screen;
//   * silence draws nothing         - artefacts where there is no signal;
//   * height follows the amplitude  - the amplitude floor or the gains eating
//                                     the dynamics.
//
// On a machine without an OpenGL context the tests skip loudly rather than
// silently, so that a suite which stopped checking anything cannot look green.
// The offscreen Qt platform has no GL, so run them as:
//
//   QT_QPA_PLATFORM=cocoa QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1 \
//       mixxx-test --gtest_filter='WaveformShader*'

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QOffscreenSurface>
#include <QPainter>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <vector>

#include "waveform/renderers/allshader/spectrumparams.h"

using namespace mixxx::spectrumwaveform;

namespace {

/// One visual bin as the analyzer stores it: the peak of the bin and the RMS of
/// the three bands, each in one byte.
struct Bin {
    int all;
    int low;
    int mid;
    int high;
};

constexpr int kTextureStride = 64; // the texture is square, stride by stride
constexpr int kColumns = 128;      // visual bins we fill, two texels each

class WaveformShaderTest : public testing::Test {
  protected:
    void SetUp() override {
        QSurfaceFormat format;
        format.setVersion(2, 1);
        format.setProfile(QSurfaceFormat::NoProfile);
        format.setRenderableType(QSurfaceFormat::OpenGL);

        m_pSurface = std::make_unique<QOffscreenSurface>();
        m_pSurface->setFormat(format);
        m_pSurface->create();
        if (!m_pSurface->isValid()) {
            GTEST_SKIP() << "SKIPPING THE SHADER TESTS: no offscreen surface. "
                            "The shader was NOT checked. Run with "
                            "QT_QPA_PLATFORM=cocoa to check it.";
        }

        m_pContext = std::make_unique<QOpenGLContext>();
        m_pContext->setFormat(format);
        if (!m_pContext->create() || !m_pContext->makeCurrent(m_pSurface.get())) {
            GTEST_SKIP() << "SKIPPING THE SHADER TESTS: no OpenGL context. "
                            "The shader was NOT checked. Run with "
                            "QT_QPA_PLATFORM=cocoa to check it.";
        }

        m_pProgram = std::make_unique<QOpenGLShaderProgram>();
        ASSERT_TRUE(m_pProgram->addShaderFromSourceFile(
                QOpenGLShader::Vertex, QStringLiteral(":/shaders/passthrough.vert")))
                << m_pProgram->log().toStdString();
        ASSERT_TRUE(m_pProgram->addShaderFromSourceFile(QOpenGLShader::Fragment,
                QStringLiteral(":/shaders/spectrumsignal.frag")))
                << m_pProgram->log().toStdString();
        ASSERT_TRUE(m_pProgram->link()) << m_pProgram->log().toStdString();
    }

    void TearDown() override {
        m_pProgram.reset();
        if (m_pContext) {
            m_pContext->doneCurrent();
        }
        m_pContext.reset();
        m_pSurface.reset();
    }

    /// Renders the given bins into an image of the given size. Every bin is
    /// written to both stereo channels, so the picture is symmetric.
    QImage render(const std::vector<Bin>& bins, int width, int height) {
        auto* gl = m_pContext->functions();

        std::vector<unsigned char> texels(kTextureStride * kTextureStride * 4, 0);
        for (int i = 0; i < static_cast<int>(bins.size()) && 2 * i + 1 < kTextureStride * kTextureStride;
                ++i) {
            for (int channel = 0; channel < 2; ++channel) {
                unsigned char* p = texels.data() + (2 * i + channel) * 4;
                p[0] = static_cast<unsigned char>(bins[i].low);
                p[1] = static_cast<unsigned char>(bins[i].mid);
                p[2] = static_cast<unsigned char>(bins[i].high);
                p[3] = static_cast<unsigned char>(bins[i].all);
            }
        }

        GLuint texture = 0;
        gl->glGenTextures(1, &texture);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexImage2D(GL_TEXTURE_2D,
                0,
                GL_RGBA,
                kTextureStride,
                kTextureStride,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                texels.data());

        QOpenGLFramebufferObject fbo(width, height);
        fbo.bind();
        gl->glViewport(0, 0, width, height);
        gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        gl->glDisable(GL_BLEND);

        m_pProgram->bind();
        setUniforms(static_cast<int>(bins.size()), width, height);
        m_pProgram->setUniformValue("waveformDataTexture", 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, texture);

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(bins.size()), -1.0, 1.0, -10.0, 10.0);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f);
        glVertex3f(0.0f, -1.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f);
        glVertex3f(static_cast<float>(bins.size()), -1.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f);
        glVertex3f(static_cast<float>(bins.size()), 1.0f, 0.0f);
        glTexCoord2f(0.0f, 1.0f);
        glVertex3f(0.0f, 1.0f, 0.0f);
        glEnd();

        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();

        m_pProgram->release();
        // toImage() hands back the bytes labelled as premultiplied, but the
        // shader writes straight colour and alpha. Reinterpreting keeps the
        // bytes and fixes the label; converting instead would divide the
        // colour by the alpha and overflow the channel that sits at maximum.
        QImage image = fbo.toImage();
        image.reinterpretAsFormat(QImage::Format_ARGB32);
        fbo.release();
        gl->glDeleteTextures(1, &texture);
        return image;
    }

    /// Values that the comparison sheet overrides to show what the waveform
    /// looked like before a change. Empty means "use the shipped constants".
    struct Overrides {
        float softEdgeFraction = kSoftEdgeFraction;
        float softEdgePixels = kSoftEdgePixels;
        float amplitudeFloor = kAmplitudeFloor;
        // One sub column per screen pixel would be no supersampling at all;
        // the frame buffer of the test is not oversampled, so the count here is
        // the count per screen pixel.
        float subColumnSamples = 8.0f;
        // The axis line is part of the skin, not of the waveform, and it sits
        // in the middle of every image. Tests that compare the mask itself turn
        // it off rather than work around it.
        bool axis = true;
    };
    Overrides m_overrides;

    void setUniforms(int columns, int width, int height) {
        m_pProgram->setUniformValue("framebufferSize", QVector2D(width, height));
        m_pProgram->setUniformValue("waveformLength", 2 * columns);
        m_pProgram->setUniformValue("textureSize", kTextureStride * kTextureStride);
        m_pProgram->setUniformValue("textureStride", kTextureStride);
        m_pProgram->setUniformValue("firstVisualIndex", 0.0f);
        m_pProgram->setUniformValue("lastVisualIndex", static_cast<float>(columns));
        m_pProgram->setUniformValue("allGain", 1.0f);
        m_pProgram->setUniformValue("lowGain", 1.0f);
        m_pProgram->setUniformValue("midGain", 1.0f);
        m_pProgram->setUniformValue("highGain", 1.0f);
        m_pProgram->setUniformValue("splitStereoSignal", false);
        // Pure red, green and blue, which is what the colour model is defined
        // against, and an axis colour that is easy to recognize if it ever
        // shows through the waveform: white, as in the skin of the user.
        m_pProgram->setUniformValue("lowColor", QVector4D(1.0f, 0.0f, 0.0f, 1.0f));
        m_pProgram->setUniformValue("midColor", QVector4D(0.0f, 1.0f, 0.0f, 1.0f));
        m_pProgram->setUniformValue("highColor", QVector4D(0.0f, 0.0f, 1.0f, 1.0f));
        m_pProgram->setUniformValue(
                "axesColor", QVector4D(1.0f, 1.0f, 1.0f, m_overrides.axis ? 1.0f : 0.0f));

        m_pProgram->setUniformValue("colorSmoothBins", kColorSmoothBins);
        m_pProgram->setUniformValue("softEdgeFraction", m_overrides.softEdgeFraction);
        m_pProgram->setUniformValue("softEdgePixels", m_overrides.softEdgePixels);
        m_pProgram->setUniformValue("amplitudeFloor", m_overrides.amplitudeFloor);
        m_pProgram->setUniformValue("subColumnSamples", m_overrides.subColumnSamples);
        m_pProgram->setUniformValue("colorGamma", kColorGamma);
        m_pProgram->setUniformValue("colorLevelFloor", kColorLevelFloor);
        m_pProgram->setUniformValue("bandColorGain",
                QVector3D(kBandColorGainLow, kBandColorGainMid, kBandColorGainHigh));
    }

    /// A block of identical bins wide enough that the colour smoothing, which
    /// averages over neighbouring bins, sees nothing but this value.
    static std::vector<Bin> uniformBins(const Bin& bin, int columns = kColumns) {
        return std::vector<Bin>(columns, bin);
    }

    std::unique_ptr<QOffscreenSurface> m_pSurface;
    std::unique_ptr<QOpenGLContext> m_pContext;
    std::unique_ptr<QOpenGLShaderProgram> m_pProgram;
};

/// Distance between two hues in degrees, the short way round.
double hueDistance(double a, double b) {
    double d = std::fabs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

// The antialiasing reference the user approved: the mask of the waveform
// sampled eight times across and eight times down every pixel, averaged.
// kGoldenPeaks are the peaks of 64 sub columns, eight per pixel over eight
// pixels, as bytes; kGoldenAlpha is what that mask gives for the sixteen pixel
// rows of the upper half, row 0 being the one next to the centre line.
//
// THESE NUMBERS COME FROM OUTSIDE THE SHADER. Two tests use them: one drives
// the shader directly, the other goes through the oversampled buffer the
// renderer really uses.
constexpr int kGoldenPeaks[64] = {
        52, 81, 85, 60, 70, 72, 98, 93, 114, 74, 141, 117, 137, 129, 131, 152,
        164, 152, 159, 180, 159, 154, 194, 181, 165, 191, 203, 196, 197, 231, 252, 238,
        5, 73, 139, 170, 220, 246, 222, 191, 174, 120, 74, 5, 65, 121, 152, 217,
        238, 211, 225, 174, 117, 50, 41, 64, 156, 201, 187, 236, 216, 208, 143, 94,
};
constexpr double kGoldenAlpha[16][8] = {
        {1.0000, 1.0000, 1.0000, 1.0000, 0.9219, 0.9219, 1.0000, 1.0000},
        {1.0000, 1.0000, 1.0000, 1.0000, 0.8750, 0.8750, 1.0000, 1.0000},
        {1.0000, 1.0000, 1.0000, 1.0000, 0.8750, 0.8750, 0.9531, 1.0000},
        {0.8750, 1.0000, 1.0000, 1.0000, 0.8750, 0.8750, 0.7656, 1.0000},
        {0.6094, 0.9531, 1.0000, 1.0000, 0.8125, 0.7188, 0.6250, 1.0000},
        {0.2969, 0.8750, 1.0000, 1.0000, 0.7500, 0.6250, 0.6250, 0.9844},
        {0.0156, 0.8750, 1.0000, 1.0000, 0.7500, 0.6250, 0.6250, 0.8750},
        {0.0000, 0.6875, 1.0000, 1.0000, 0.7500, 0.5156, 0.5312, 0.8750},
        {0.0000, 0.3594, 1.0000, 1.0000, 0.7188, 0.3750, 0.5000, 0.8750},
        {0.0000, 0.0625, 0.8906, 1.0000, 0.6250, 0.3125, 0.5000, 0.7188},
        {0.0000, 0.0000, 0.4219, 0.9219, 0.5781, 0.2344, 0.4844, 0.6250},
        {0.0000, 0.0000, 0.2188, 0.8750, 0.5000, 0.1250, 0.3750, 0.5938},
        {0.0000, 0.0000, 0.0156, 0.5625, 0.3750, 0.1250, 0.3750, 0.4531},
        {0.0000, 0.0000, 0.0000, 0.3750, 0.3594, 0.0781, 0.2812, 0.2031},
        {0.0000, 0.0000, 0.0000, 0.3125, 0.1250, 0.0000, 0.1406, 0.0938},
        {0.0000, 0.0000, 0.0000, 0.1094, 0.0625, 0.0000, 0.0000, 0.0000},
};

} // namespace

TEST_F(WaveformShaderTest, ColorOfAColumnFollowsTheModel) {
    // Synthetic band triples with the hue the colour model gives for them:
    // the bands are multiplied by the band gain, normalized to the brightest
    // channel and converted to HSV. They cover the corners and the edges of the
    // cube plus a few ordinary mixtures.
    struct Case {
        int low;
        int mid;
        int high;
        double hue;
        double tolerance;
    };
    // Recomputed from the model that is actually shipped: bands times the band
    // gain, normalized to the brightest channel, quantized to bytes, converted
    // to HSV. The reference table this test started from had been computed with
    // a mid gain of 1.000 while the shipped value is 0.700, and this test found
    // that on its first run: seven of the sixteen hues were off by 3 to 55
    // degrees, in a pattern that pointed straight at the mid band.
    constexpr Case kCases[] = {
            {255, 0, 0, 0.0, 2.0},
            {0, 255, 0, 120.0, 2.0},
            {0, 0, 255, 240.0, 2.0},
            {255, 255, 0, 39.3, 2.0},
            {255, 0, 255, 248.9, 2.0},
            {0, 255, 255, 234.1, 2.0},
            {255, 255, 255, 243.4, 2.0},
            {200, 100, 20, 329.8, 2.0},
            {60, 180, 40, 223.0, 2.0},
            {40, 60, 200, 240.0, 2.0},
            {120, 120, 120, 243.4, 2.0},
            {255, 40, 10, 349.3, 2.0},
            {10, 40, 255, 239.5, 2.0},
            {180, 90, 30, 291.7, 2.0},
            {30, 90, 180, 238.3, 2.0},
            {90, 255, 90, 230.9, 2.0},
    };
    for (const Case& c : kCases) {
        // A tall column so that the sample sits well inside the body, away
        // from the soft edge and from the vertical shading.
        const QImage image = render(uniformBins(Bin{255, c.low, c.mid, c.high}), 256, 200);
        const QColor color = image.pixelColor(128, 100 - 40);
        ASSERT_GT(color.value(), 0) << "nothing was drawn for bands " << c.low << ", " << c.mid
                                    << ", " << c.high;
        EXPECT_LT(hueDistance(color.hueF() * 360.0, c.hue), c.tolerance)
                << "bands " << c.low << ", " << c.mid << ", " << c.high << " gave hue "
                << color.hueF() * 360.0 << " instead of " << c.hue;
    }
}

TEST_F(WaveformShaderTest, EqualBandsAreBlueNotGrey) {
    // A consequence of raising the high band by 17 dB, and an intentional one:
    // equal band magnitudes do not mean equal energy per band. Guarded here
    // because it looks like a bug to anyone who has not measured it, and
    // "fixing" it would quietly undo the colour model.
    const QImage image = render(uniformBins(Bin{255, 120, 120, 120}), 256, 200);
    const QColor color = image.pixelColor(128, 60);
    ASSERT_GT(color.alphaF(), 0.5) << "nothing was drawn, there is no colour to judge";
    EXPECT_LT(hueDistance(color.hueF() * 360.0, 243.4), 2.0)
            << "equal bands gave hue " << color.hueF() * 360.0;
    EXPECT_GT(color.saturationF(), 0.5) << "equal bands came out unsaturated";
}

TEST_F(WaveformShaderTest, TheEdgeIsAntialiasedAndNotABlur) {
    // The edge has to be soft enough not to stair-step and hard enough to still
    // read as an edge. Both halves of that matter and the second one was got
    // wrong: a fade of four percent of the half height is over two pixels on a
    // small deck and eight on a large one, which is a gradient painted on top
    // of the antialiasing rather than an edge.
    //
    // What softens it now is the coverage itself, worked out from the sub
    // columns inside each pixel, so the fade should stay about a pixel wide
    // whatever the deck is scaled to.
    const auto bins = uniformBins(Bin{120, 90, 20, 5});
    for (const int height : {120, 400}) {
        const QImage image = render(bins, 128, height);
        const int centre = height / 2;
        int firstLit = -1;
        for (int y = 0; y < centre; ++y) {
            if (image.pixelColor(64, y).alphaF() > 0.02f) {
                firstLit = y;
                break;
            }
        }
        ASSERT_GT(firstLit, 0) << "nothing was drawn at height " << height;

        int partial = 0;
        for (int y = firstLit; y < centre; ++y) {
            const double alpha = image.pixelColor(64, y).alphaF();
            if (alpha > 0.02 && alpha < 0.98) {
                partial++;
            }
        }
        EXPECT_GE(partial, 1) << "at height " << height
                              << " the edge is a hard step, it will stair-step as it scrolls";
        EXPECT_LE(partial, 3) << "at height " << height << " the edge fades over " << partial
                              << " rows, which is a gradient rather than an edge";
    }
}

TEST_F(WaveformShaderTest, TheCentreOfAColumnIsOpaque) {
    // The axis line is drawn underneath the waveform. When the shading took the
    // alpha near the centre below one, the axis showed through the middle of
    // every column as a white stripe.
    //
    // With the amplitude distribution this is a property of the model rather
    // than something arranged: the share of samples reaching zero is one, for
    // every kind of material. The outer part of the column is meant to be
    // translucent, so only the centre is checked here.
    for (const Bin& bin : {Bin{255, 200, 40, 10}, Bin{255, 60, 60, 60}, Bin{120, 100, 90, 80}}) {
        const QImage image = render(uniformBins(bin), 128, 200);
        const int centre = 100;
        // On the centre line itself the distribution is one for every kind of
        // material: every sample reaches level zero. This is the property that
        // makes a hole in the middle impossible.
        EXPECT_GT(image.pixelColor(64, centre).alphaF(), 0.98)
                << "the centre line of the column is not opaque";
        // Away from it the alpha falls smoothly, as it should, so over the rest
        // of the four pixels the axis occupies the question is only whether it
        // can be read through the fill. Under a fifth of white showing through
        // is not a stripe; the companion test checks the colour directly.
        double overTheAxis = 1.0;
        for (int y = centre - 4; y <= centre + 4; ++y) {
            overTheAxis = std::min(overTheAxis, static_cast<double>(image.pixelColor(64, y).alphaF()));
        }
        EXPECT_GT(overTheAxis, 0.85) << "over the axis line the body dropped to alpha "
                                     << overTheAxis << ", the axis would show through";
    }
}

TEST_F(WaveformShaderTest, TheAxisDoesNotShowThroughTheWaveform) {
    // The same thing seen from the other side: with a white axis colour, no
    // pixel in the middle of a column may come out white.
    const QImage image = render(uniformBins(Bin{255, 200, 40, 10}), 128, 200);
    ASSERT_GT(image.pixelColor(64, 100).alphaF(), 0.5)
            << "the column was not drawn, so nothing being white proves nothing";
    for (int y = 96; y <= 104; ++y) {
        const QColor color = image.pixelColor(64, y);
        EXPECT_FALSE(color.saturationF() < 0.2 && color.valueF() > 0.9)
                << "row " << y << " is white, the axis is showing through the body";
    }
}

TEST_F(WaveformShaderTest, NoPartOfAColumnComesOutWhite) {
    // "and do not turn the middle white". The axis line under the waveform is
    // white in the skin of the user, and anything that makes the body of a
    // column translucent lets it through; so does dividing the colour by an
    // alpha smaller than itself, which drives all three channels into clipping.
    // Both have happened here. The check is on the finished pixel, over the
    // whole height of the column, because that is where the complaint was.
    // Peaks below full scale, so the tip of each column is inside the image.
    for (const Bin& bin : {Bin{200, 160, 30, 8},
                 Bin{200, 50, 50, 50},
                 Bin{160, 120, 60, 15},
                 Bin{120, 100, 90, 80}}) {
        const QImage image = render(uniformBins(bin), 128, 200);
        const int centre = 100;
        int firstLit = -1;
        for (int y = 0; y < centre; ++y) {
            if (image.pixelColor(64, y).alphaF() > 0.5f) {
                firstLit = y;
                break;
            }
        }
        ASSERT_GT(firstLit, 0) << "nothing was drawn, so the absence of white proves nothing";
        for (int y = firstLit + 1; y <= centre; ++y) {
            const QColor color = image.pixelColor(64, y);
            EXPECT_GT(color.saturationF(), 0.25)
                    << "row " << y << " of the column is nearly white (saturation "
                    << color.saturationF() << ")";
        }
    }
}

TEST_F(WaveformShaderTest, SilenceDrawsNothing) {
    // A bin with no signal at all must stay empty: the amplitude floor lifts
    // quiet material, not silence.
    // This is the one check here that an empty picture satisfies by itself, so
    // it starts by showing that the same setup does draw when there is
    // something to draw. Otherwise a shader that renders nothing at all would
    // look like a shader that handles silence correctly.
    const QImage loud = render(uniformBins(Bin{200, 150, 80, 20}), 128, 200);
    ASSERT_GT(loud.pixelColor(64, 60).alphaF(), 0.5)
            << "nothing is drawn even for a loud bin, so silence proves nothing";

    const QImage image = render(uniformBins(Bin{0, 0, 0, 0}), 128, 200);
    for (int y = 0; y < 200; ++y) {
        const QColor color = image.pixelColor(64, y);
        if (y >= 96 && y <= 104) {
            continue; // the axis line is allowed to be there
        }
        EXPECT_LT(color.alphaF(), 0.02) << "row " << y << " was painted on silence";
    }
}

TEST_F(WaveformShaderTest, HeightFollowsTheAmplitude) {
    // Louder bins must be drawn taller, with the floor as the lower bound.
    // Breaks if the floor, the gains or the normalization eat the dynamics.
    int previous = -1;
    for (const int all : {30, 80, 150, 255}) {
        const QImage image = render(uniformBins(Bin{all, 100, 80, 20}), 128, 200);
        int lit = 0;
        for (int y = 0; y < 100; ++y) {
            if (image.pixelColor(64, y).alphaF() > 0.5) {
                lit++;
            }
        }
        EXPECT_GT(lit, previous) << "amplitude " << all << " was not drawn taller than the one below";
        previous = lit;
    }
    // A barely audible bin is drawn barely at all: there is no floor lifting
    // quiet material, by the choice of the user, who was shown both and picked
    // this. It still has to be drawn rather than dropped, so the check is that
    // it is there and small.
    const QImage quiet = render(uniformBins(Bin{4, 3, 2, 1}), 128, 200);
    int lit = 0;
    for (int y = 0; y < 100; ++y) {
        if (quiet.pixelColor(64, y).alphaF() > 0.5) {
            lit++;
        }
    }
    EXPECT_GT(lit, 0) << "a quiet bin was not drawn at all";
    EXPECT_LT(lit, 12) << "a barely audible bin was drawn " << lit
                       << " pixels tall, something is lifting quiet material";
}

TEST_F(WaveformShaderTest, HeightDoesNotDependOnTheColourBalance) {
    // The height comes from the peak of the bin and nothing else. The band
    // gains are a colour matter; if they ever leak into the height, loud bass
    // and loud treble would be drawn at different sizes.
    const auto measureHeight = [this](const Bin& bin) {
        const QImage image = render(uniformBins(bin), 128, 200);
        int lit = 0;
        for (int y = 0; y < 100; ++y) {
            if (image.pixelColor(64, y).alphaF() > 0.5) {
                lit++;
            }
        }
        return lit;
    };
    const int bass = measureHeight(Bin{200, 250, 10, 5});
    const int treble = measureHeight(Bin{200, 5, 10, 250});
    ASSERT_GT(bass, 10) << "no column was drawn, so two equal heights prove nothing";
    EXPECT_LE(std::abs(bass - treble), 2)
            << "the same peak was drawn " << bass << " pixels tall for bass and " << treble
            << " for treble";
}

TEST_F(WaveformShaderTest, QuietColumnsAreDimRatherThanSaturated) {
    // Without the level floor the colour of a column is divided by its own
    // brightest channel, so a column carrying almost nothing comes out at full
    // saturation with a hue decided by the last bits of the data. That is how
    // a silent intro once turned into a solid bright green stripe.
    const QImage quiet = render(uniformBins(Bin{60, 2, 1, 1}), 128, 200);
    const QColor quietColor = quiet.pixelColor(64, 90);
    ASSERT_GT(quietColor.alphaF(), 0.5) << "the quiet column was not drawn at all";
    EXPECT_LT(quietColor.value(), 120)
            << "a column with almost no signal was drawn at brightness " << quietColor.value()
            << ", i.e. normalized up to full colour";

    // A loud column of the same shape is normalized as usual, so the test
    // fails if the floor is applied to everything instead.
    const QImage loud = render(uniformBins(Bin{200, 200, 100, 100}), 128, 200);
    EXPECT_GT(loud.pixelColor(64, 90).value(), 200)
            << "a loud column came out dim, the level floor is applying too widely";
}

namespace {

/// Perceived brightness of a pixel once it has been composited over the
/// background of the skin, on the 0..255 scale.
///
/// The shader writes colour and alpha; what the user sees is that blended over
/// whatever is behind the waveform. Judging the shading by its alpha alone
/// would be judging a value nobody looks at: the same drop in alpha is obvious
/// over a light background and invisible over a dark one.
double brightnessOverBackground(const QColor& pixel, double background) {
    const double alpha = pixel.alphaF();
    const double luminance = 0.2126 * pixel.redF() + 0.7152 * pixel.greenF() +
            0.0722 * pixel.blueF();
    return 255.0 * (alpha * luminance + (1.0 - alpha) * background);
}

/// The waveform of the skins we care about sits on a nearly black background;
/// LateNight uses #1a1a1a, which is this.
constexpr double kSkinBackground = 0.1;

/// How much the brightness has to change across a column before we are willing
/// to call the vertical shading visible, on the 0..255 scale.
///
/// This number is CHOSEN, not measured. There is no measurement of Traktor
/// behind it and no experiment on human vision: it is the value that the build
/// the user accepted clears with a good margin (it produces about 40) while
/// half the shading strength does not. It exists to catch the failure we hit
/// twice - an effect that is present in the arithmetic, passes every structural
/// test and is invisible on screen - and not to define what is visible.
///
/// It has to be revisited if the background of the skin changes materially, or
/// if the colour model changes the brightness of the fill, because both move
/// the contrast without touching the shading at all.
constexpr double kVisibleBrightnessStep = 18.0;

} // namespace

TEST_F(WaveformShaderTest, TheCoverageMatchesAnEightBySupersampledMask) {
    // THE REFERENCE HERE COMES FROM OUTSIDE THE SHADER. It is the antialiasing
    // the user approved: the mask of the waveform sampled eight times across
    // and eight times down every pixel, averaged. A screen pixel spans several
    // bins - about five at the usual zoom - and the signal rises and falls
    // inside it, so what the pixel shows is the share of its area the column
    // covers rather than a yes or no about its centre.
    //
    // kGoldenPeaks are the peaks of 64 sub columns, eight per pixel over eight
    // pixels, as bytes. kGoldenAlpha is what the 8 by 8 supersampled mask gives
    // for the sixteen pixel rows of the upper half.
    //
    // The shader does not supersample vertically: it computes the vertical
    // coverage of each sub column in closed form, which is the same quantity
    // without the sixty four samples. Against this reference that costs at most
    // 0.021 of alpha and 0.002 on average.
    constexpr double kTolerance = 0.05;

    // The shading and the amplitude floor are switched off, and the soft edge
    // is set to exactly one pixel row: what is left is the coverage itself,
    // which is what this reference describes.
    m_overrides.amplitudeFloor = 0.0f;
    m_overrides.softEdgeFraction = 0.0f;
    m_overrides.softEdgePixels = 1.0f;
    m_overrides.subColumnSamples = 8.0f;
    // Without this the axis line, which is drawn under the waveform within four
    // pixels of the centre, shows through wherever the coverage is partial and
    // lifts the alpha of the four rows nearest the middle. It is not part of
    // what this reference describes.
    m_overrides.axis = false;

    std::vector<Bin> bins;
    bins.reserve(64);
    for (const int peak : kGoldenPeaks) {
        // The bands only decide the colour here; the height comes from the peak.
        bins.push_back(Bin{peak, 100, 40, 10});
    }
    const QImage image = render(bins, 8, 32);
    m_overrides = Overrides{};

    for (int row = 0; row < 16; ++row) {
        for (int column = 0; column < 8; ++column) {
            // Row 0 of the reference is the row next to the centre line.
            const double alpha = image.pixelColor(column, 15 - row).alphaF();
            EXPECT_NEAR(alpha, kGoldenAlpha[row][column], kTolerance)
                    << "pixel row " << row << ", column " << column << ": the shader covers "
                    << alpha << " where the supersampled mask covers "
                    << kGoldenAlpha[row][column];
        }
    }
}

TEST_F(WaveformShaderTest, TheShippedSubColumnCountReachesTheReference) {
    // The check above overrides the number of sub columns, so it says nothing
    // about the value actually shipped. This one uses it, and renders the way
    // the renderer does: into a frame buffer four times the size of the widget,
    // which is then averaged down. Two sub columns per frame buffer pixel times
    // four frame buffer pixels per screen pixel is the eight the reference was
    // measured at, and the whole path has to land on the reference.
    constexpr int kOversampling = 4;
    constexpr double kTolerance = 0.05;

    m_overrides.amplitudeFloor = 0.0f;
    m_overrides.softEdgeFraction = 0.0f;
    // One row of the finished picture, in the units of the oversampled buffer.
    m_overrides.softEdgePixels = kOversampling;
    m_overrides.subColumnSamples = kSubColumnSamples;
    m_overrides.axis = false;

    std::vector<Bin> bins;
    bins.reserve(64);
    for (const int peak : kGoldenPeaks) {
        bins.push_back(Bin{peak, 100, 40, 10});
    }
    const QImage oversampled = render(bins, 8 * kOversampling, 32 * kOversampling);
    m_overrides = Overrides{};

    for (int row = 0; row < 16; ++row) {
        for (int column = 0; column < 8; ++column) {
            // Average the block of frame buffer pixels that becomes one pixel
            // of the picture, which is what the blit does.
            double sum = 0.0;
            for (int dy = 0; dy < kOversampling; ++dy) {
                for (int dx = 0; dx < kOversampling; ++dx) {
                    sum += oversampled
                                   .pixelColor(column * kOversampling + dx,
                                           (15 - row) * kOversampling + dy)
                                   .alphaF();
                }
            }
            const double alpha = sum / (kOversampling * kOversampling);
            EXPECT_NEAR(alpha, kGoldenAlpha[row][column], kTolerance)
                    << "pixel row " << row << ", column " << column << ": the shipped path covers "
                    << alpha << " where the supersampled mask covers "
                    << kGoldenAlpha[row][column];
        }
    }
}

namespace {

// The antialiasing reference the user approved: the mask of the waveform
// sampled eight times across and eight times down every pixel, averaged.
// kGoldenPeaks are the peaks of 64 sub columns, eight per pixel over eight
// pixels, as bytes; kGoldenAlpha is what that mask gives for the sixteen pixel
// rows of the upper half, row 0 being the one next to the centre line.
//
// THESE NUMBERS COME FROM OUTSIDE THE SHADER and are checked by two tests: one
// drives the shader directly, the other goes through the oversampled buffer the
// renderer really uses.

/// A stretch of bins that alternates between tonal and percussive character,
/// which is what makes the vertical shading readable: it shows up as a
/// difference between neighbours rather than as a gradient inside one column.
std::vector<Bin> mixedCharacterBins(int columns) {
    std::vector<Bin> bins;
    bins.reserve(columns);
    for (int i = 0; i < columns; ++i) {
        // Slow swell of the amplitude, so the sheet also shows the envelope.
        const double envelope = 0.45 + 0.55 * std::sin(i * 0.055);
        const int peak = std::max(20, static_cast<int>(230 * envelope));
        // Alternate the character in blocks of eight columns.
        const bool tonal = ((i / 8) % 2) == 0;
        const double bandScale = tonal ? 0.62 : 0.17;
        const int low = static_cast<int>(peak * bandScale * 1.0);
        const int mid = static_cast<int>(peak * bandScale * 0.65);
        const int high = static_cast<int>(peak * bandScale * (tonal ? 0.12 : 0.30));
        bins.push_back(Bin{peak, low, mid, high});
    }
    return bins;
}

} // namespace

/// Not a check: renders the sheet that shows what the waveform looks like now
/// against what it looked like before the soft edge became a fraction of the
/// height and before the vertical shading existed. Disabled, so it only runs
/// when asked for:
///
///   mixxx-test --gtest_also_run_disabled_tests \
///       --gtest_filter='*ComparisonSheet*'
///
/// The file goes to MIXXX_WF_SHEET or to /tmp/wfsheet.png.
TEST_F(WaveformShaderTest, DISABLED_ComparisonSheet) {
    constexpr int kWidth = 880;
    constexpr int kSmall = 120;
    // The larger panel is the height of a deck on the machine of the user, in
    // device pixels, which is where the difference in the soft edge lives: at
    // 120 pixels four percent of the half height is 2.4 pixels, i.e. LESS than
    // the three fixed pixels it replaced.
    constexpr int kLarge = 400;
    constexpr int kGap = 10;

    const auto bins = mixedCharacterBins(220);

    // "Before": one sample per pixel, i.e. the edge decided by the single bin
    // under the centre of the pixel, which is the hard mask the user compared
    // against.
    m_overrides.subColumnSamples = 1.0f;
    m_overrides.softEdgeFraction = 0.0f;
    m_overrides.softEdgePixels = 0.0f;
    const QImage beforeSmall = render(bins, kWidth, kSmall);
    const QImage beforeLarge = render(bins, kWidth, kLarge);
    m_overrides = Overrides{};
    const QImage afterSmall = render(bins, kWidth, kSmall);
    const QImage afterLarge = render(bins, kWidth, kLarge);

    const int height = 2 * kSmall + 2 * kLarge + 5 * kGap;
    QImage sheet(kWidth, height, QImage::Format_ARGB32);
    // The background of the skin, so the sheet shows the contrast the user
    // sees rather than the raw alpha.
    sheet.fill(QColor(26, 26, 26));

    QPainter painter(&sheet);
    int y = kGap;
    const auto place = [&](const QImage& image, const QString& label) {
        painter.drawImage(0, y, image);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(6, y + 14, label);
        y += image.height() + kGap;
    };
    place(beforeSmall, QStringLiteral("hard mask, 120 px"));
    place(afterSmall, QStringLiteral("supersampled, 120 px"));
    place(beforeLarge, QStringLiteral("hard mask, 400 px"));
    place(afterLarge, QStringLiteral("supersampled, 400 px"));
    painter.end();

    const QString path = qEnvironmentVariable("MIXXX_WF_SHEET", QStringLiteral("/tmp/wfsheet.png"));
    ASSERT_TRUE(sheet.save(path)) << "could not write " << path.toStdString();
    printf("comparison sheet written to %s (%dx%d)\n", path.toStdString().c_str(), kWidth, height);
}
