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
        m_pProgram->setUniformValue("axesColor", QVector4D(1.0f, 1.0f, 1.0f, 1.0f));

        m_pProgram->setUniformValue("colorSmoothBins", kColorSmoothBins);
        m_pProgram->setUniformValue("softEdgeFraction", kSoftEdgeFraction);
        m_pProgram->setUniformValue("softEdgePixels", kSoftEdgePixels);
        m_pProgram->setUniformValue("amplitudeFloor", kAmplitudeFloor);
        m_pProgram->setUniformValue("colorGamma", kColorGamma);
        m_pProgram->setUniformValue("colorLevelFloor", kColorLevelFloor);
        m_pProgram->setUniformValue("bandColorGain",
                QVector3D(kBandColorGainLow, kBandColorGainMid, kBandColorGainHigh));
        m_pProgram->setUniformValue("verticalStrength", kVerticalStrength);
        m_pProgram->setUniformValue("dipCenterTonal", kDipCenterTonal);
        m_pProgram->setUniformValue("dipCenterImpulsive", kDipCenterImpulsive);
        m_pProgram->setUniformValue("dipWidth", kDipWidth);
        m_pProgram->setUniformValue("crestNeutral", kCrestNeutral);
        m_pProgram->setUniformValue("crestScale", kCrestScale);
        m_pProgram->setUniformValue("crestLevelFloor", kCrestLevelFloor);
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
    EXPECT_LT(hueDistance(color.hueF() * 360.0, 243.4), 2.0)
            << "equal bands gave hue " << color.hueF() * 360.0;
    EXPECT_GT(color.saturationF(), 0.5) << "equal bands came out unsaturated";
}

TEST_F(WaveformShaderTest, SoftEdgeKeepsItsProportionAtEverySize) {
    // The width of the fade is a fraction of the half height, so a taller
    // widget gets a proportionally wider fade. Carrying over the absolute
    // number of pixels instead is exactly how the edge came out looking hard
    // on a large deck while the small frames of the test bench looked right.
    //
    // The fade is measured as the distance over which the alpha climbs from a
    // tenth to nine tenths of the value just inside the column. Counting every
    // partly transparent row instead would measure the vertical shading too,
    // which thins most of the body on purpose.
    const auto bins = uniformBins(Bin{120, 90, 20, 5});
    std::vector<int> fades;
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
        ASSERT_GT(firstLit, 0) << "the column reached the top of the image at height " << height;

        const double expected = kSoftEdgeFraction * (height / 2.0);
        const int inside = std::min(centre - 1, firstLit + static_cast<int>(3 * expected) + 3);
        const double reference = image.pixelColor(64, inside).alphaF();
        ASSERT_GT(reference, 0.3) << "no solid part found below the edge at height " << height;

        int low = -1;
        int high = -1;
        for (int y = firstLit; y <= inside; ++y) {
            const double alpha = image.pixelColor(64, y).alphaF();
            if (low < 0 && alpha >= 0.1 * reference) {
                low = y;
            }
            if (high < 0 && alpha >= 0.9 * reference) {
                high = y;
                break;
            }
        }
        ASSERT_GE(low, 0);
        ASSERT_GE(high, low);
        const int fade = high - low + 1;
        fades.push_back(fade);
        EXPECT_GE(fade, std::max(1, static_cast<int>(expected * 0.5)))
                << "at height " << height << " the fade is " << fade
                << " rows, expected about " << expected;
        EXPECT_LE(fade, static_cast<int>(expected * 2.5) + 2)
                << "at height " << height << " the fade is " << fade
                << " rows, expected about " << expected;
    }
    // And the proportion itself: a widget three times taller must fade over
    // roughly three times as many rows. This is the part that a fade of a
    // fixed number of pixels cannot satisfy, whatever the tolerances above
    // happen to allow.
    ASSERT_EQ(fades.size(), 2u);
    EXPECT_GE(fades[1], 2 * fades[0])
            << "the fade did not grow with the widget: " << fades[0] << " rows at 120 and "
            << fades[1] << " at 400, so it is a fixed number of pixels rather than a fraction";
}

TEST_F(WaveformShaderTest, TheBodyNeverBecomesTransparent) {
    // The axis line is drawn underneath the waveform. When the vertical shading
    // took the alpha of the body to zero, the axis showed through the middle of
    // every tonal column as a white stripe. Nothing inside the body may be
    // transparent enough to let it out.
    for (const Bin& bin : {Bin{255, 200, 40, 10}, Bin{255, 60, 60, 60}, Bin{120, 100, 90, 80}}) {
        const QImage image = render(uniformBins(bin), 128, 200);
        const int centre = 100;
        // Over the axis line itself the body has to stay nearly opaque, or the
        // white line underneath shows through as a stripe. That is the shape
        // this test was written for.
        double overTheAxis = 1.0;
        for (int y = centre - 5; y <= centre + 5; ++y) {
            overTheAxis = std::min(overTheAxis, static_cast<double>(image.pixelColor(64, y).alphaF()));
        }
        EXPECT_GT(overTheAxis, 0.8) << "over the axis line the body dropped to alpha "
                                    << overTheAxis << ", the axis would show through";
        // And nowhere in the body may the fill become see-through enough for
        // the background to read through it.
        // And nowhere inside the body may the fill become see-through enough
        // for the background to read through it. The range is the part of the
        // column that is safely inside it, away from the fading rim.
        int firstLit = 0;
        for (int y = 0; y < centre; ++y) {
            if (image.pixelColor(64, y).alphaF() > 0.5f) {
                firstLit = y;
                break;
            }
        }
        double anywhere = 1.0;
        for (int y = firstLit + 4; y <= centre; ++y) {
            anywhere = std::min(anywhere, static_cast<double>(image.pixelColor(64, y).alphaF()));
        }
        EXPECT_GT(anywhere, 0.3) << "the body dropped to alpha " << anywhere;
    }
}

TEST_F(WaveformShaderTest, TheAxisDoesNotShowThroughTheWaveform) {
    // The same thing seen from the other side: with a white axis colour, no
    // pixel in the middle of a column may come out white.
    const QImage image = render(uniformBins(Bin{255, 200, 40, 10}), 128, 200);
    for (int y = 96; y <= 104; ++y) {
        const QColor color = image.pixelColor(64, y);
        EXPECT_FALSE(color.saturationF() < 0.2 && color.valueF() > 0.9)
                << "row " << y << " is white, the axis is showing through the body";
    }
}

TEST_F(WaveformShaderTest, TonalAndPercussiveColumnsLookDifferent) {
    // The vertical shading only reads as texture if columns of different
    // character are thinned in different places. It was once present in the
    // arithmetic and invisible on screen, because the profile shaded every
    // column the same way.
    //
    // Same peak, different band magnitudes: a low crest factor is a tone, a
    // high one is a hit.
    const QImage tonal = render(uniformBins(Bin{255, 150, 100, 20}), 128, 200);
    const QImage percussive = render(uniformBins(Bin{255, 40, 25, 5}), 128, 200);

    double largestDifference = 0.0;
    for (int y = 20; y < 100; ++y) {
        largestDifference = std::max(largestDifference,
                std::fabs(static_cast<double>(tonal.pixelColor(64, y).alphaF()) -
                        static_cast<double>(percussive.pixelColor(64, y).alphaF())));
    }
    EXPECT_GT(largestDifference, 0.15)
            << "a tonal and a percussive column differ by at most " << largestDifference
            << " in alpha, which is not visible";
}

TEST_F(WaveformShaderTest, SilenceDrawsNothing) {
    // A bin with no signal at all must stay empty: the amplitude floor lifts
    // quiet material, not silence.
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
    // And the floor really is a floor: the quietest bin still has a body.
    const QImage quiet = render(uniformBins(Bin{4, 3, 2, 1}), 128, 200);
    int lit = 0;
    for (int y = 0; y < 100; ++y) {
        if (quiet.pixelColor(64, y).alphaF() > 0.5) {
            lit++;
        }
    }
    // Deliberately not written in terms of kAmplitudeFloor: an expectation
    // computed from the value under test moves together with it and cannot
    // fail. Twelve rows out of a hundred is well below the 19 the floor gives
    // and well above the two or three a bin of this level would get without it.
    EXPECT_GE(lit, 12) << "a barely audible bin was drawn " << lit
                       << " pixels tall, the floor did not apply";
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
