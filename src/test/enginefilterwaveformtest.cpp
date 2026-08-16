#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <cstddef>
#include <vector>

#include "engine/filters/enginefilterwaveform.h"
#include "util/sample.h"

namespace {

/// One point of the measured sweep: the frequency and the three band values
/// Traktor drew there.
struct MeasuredPoint {
    double frequencyHz;
    double raw[3];
};

/// Reads the measured sweep. Kept as data rather than as a table in the source
/// because it is 973 points and because the same file is the reference for the
/// stand.
std::vector<MeasuredPoint> loadMeasuredSweep() {
    QFile file(QStringLiteral(WAVEFORM_GOLDEN_DIR "/band_response_linear_u1.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    std::vector<MeasuredPoint> points;
    points.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const QJsonArray raw = object.value(QStringLiteral("raw")).toArray();
        if (raw.size() != 3) {
            continue;
        }
        points.push_back({object.value(QStringLiteral("hz")).toDouble(),
                {raw.at(0).toDouble(), raw.at(1).toDouble(), raw.at(2).toDouble()}});
    }
    return points;
}

/// Rms of log(model) - log(measured) for one band, with the best common gain
/// divided out, over the points where the measurement is above the noise floor.
///
/// THE SELECTION RULE IS PART OF THE CRITERION, not a detail. With every point
/// included the same model scores 0.082 and would fail; the difference is
/// entirely the far skirts, where a 24 bit probe reads about 1e-5 and the
/// logarithm turns its noise into a large error. The threshold was established
/// on this rule and is flat across two decades of it: 1e-4 gives 0.035,
/// 3e-4 gives 0.032, 1e-3 gives 0.030.
constexpr double kNoiseFloor = 3e-4;

double logRmsAgainstMeasured(const std::vector<MeasuredPoint>& points,
        int band,
        const std::function<double(double)>& model) {
    double sum = 0.0;
    int count = 0;
    for (const MeasuredPoint& point : points) {
        if (point.raw[band] <= kNoiseFloor) {
            continue;
        }
        const double value = model(point.frequencyHz);
        if (value <= 0.0) {
            continue;
        }
        sum += std::log(point.raw[band]) - std::log(value);
        count++;
    }
    EXPECT_GT(count, 100) << "band " << band << " kept too few points to judge";
    if (count == 0) {
        return std::numeric_limits<double>::max();
    }
    const double gain = std::exp(sum / count);
    double error = 0.0;
    for (const MeasuredPoint& point : points) {
        if (point.raw[band] <= kNoiseFloor) {
            continue;
        }
        const double value = gain * model(point.frequencyHz);
        if (value <= 0.0) {
            continue;
        }
        const double difference = std::log(value) - std::log(point.raw[band]);
        error += difference * difference;
    }
    return std::sqrt(error / count);
}

class EngineFilterWaveformTest : public testing::Test {};

TEST_F(EngineFilterWaveformTest, theBandsMatchTheMeasuredSweep) {
    // The acceptance criterion is two sided on purpose. A single overall number
    // can hide a bad band behind two good ones, and the mid band is the one at
    // risk: its points are never dropped by the noise floor, so its 0.047 is
    // the honest remainder of the model rather than an artefact of selection.
    // Tightened when the sections became exponential smoothers rather than
    // approximations of an analogue prototype: the old limits, 0.05 and 0.06,
    // were set by how well an analogue model could be made to fit and would now
    // let that weaker model back in.
    constexpr double kOverallLimit = 0.035;
    constexpr double kBandLimit = 0.045;

    const std::vector<MeasuredPoint> points = loadMeasuredSweep();
    ASSERT_FALSE(points.empty()) << "the measured sweep is missing";

    EngineFilterWaveformLow low{mixxx::audio::SampleRate(44100)};
    EngineFilterWaveformMid mid{mixxx::audio::SampleRate(44100)};
    EngineFilterWaveformHigh high{mixxx::audio::SampleRate(44100)};
    const std::array<EngineFilterWaveformBand*, 3> bands{&low, &mid, &high};
    const std::array<const char*, 3> names{"low", "mid", "high"};

    double sumOfSquares = 0.0;
    int total = 0;
    for (int band = 0; band < 3; ++band) {
        const double rms = logRmsAgainstMeasured(points, band, [&](double f) {
            return bands[band]->magnitudeAt(f);
        });
        printf("%-5s digital rms %.4f\n", names[band], rms);
        EXPECT_LE(rms, kBandLimit) << names[band] << " band is " << rms
                                   << " from the measured response";
        sumOfSquares += rms * rms;
        total++;
    }
    const double overall = std::sqrt(sumOfSquares / total);
    printf("overall %.4f (limit %.2f)\n", overall, kOverallLimit);
    EXPECT_LE(overall, kOverallLimit);
}

TEST_F(EngineFilterWaveformTest, theBandBalanceIsTheMeasuredOne) {
    // The renderer no longer holds a per band gain, so the balance has to be
    // here and nowhere else. Doubling it in both places is exactly how this
    // would go wrong silently, so the ratios are checked against the measured
    // numbers rather than against themselves.
    EngineFilterWaveformLow low{mixxx::audio::SampleRate(44100)};
    EngineFilterWaveformMid mid{mixxx::audio::SampleRate(44100)};
    EngineFilterWaveformHigh high{mixxx::audio::SampleRate(44100)};

    const double lowPeak = low.peakMagnitude();
    const double midPeak = mid.peakMagnitude();
    const double highPeak = high.peakMagnitude();

    // The physical quantity, and the one to check against: the peak of gain
    // times cascade, relative to the low band. Measured 1.000 : 0.807 : 3.783.
    // The gains themselves are not comparable across bands - a highpass built
    // as one minus a lowpass peaks at 0.273 - so reading them directly is how
    // an implementation ends up looking wrong while being right.
    EXPECT_NEAR(midPeak / lowPeak, 0.807, 0.03);
    EXPECT_NEAR(highPeak / lowPeak, 3.783, 0.15);

    // And the loudest band fills the byte it is stored in without going
    // through it: the balance leaves the high band peaking just under one, so
    // no extra headroom factor is needed.
    EXPECT_NEAR(highPeak, 1.0, 0.05) << "the high band does not fill the byte it is stored in";
}

TEST_F(EngineFilterWaveformTest, theHighBandRejectsDc) {
    // A first order highpass has an exact zero at DC, which is what keeps the
    // subsonic content of a track out of the high band. With any shape that has
    // a finite low frequency shelf it leaks in and inflates the band tenfold.
    EngineFilterWaveformHigh high{mixxx::audio::SampleRate(44100)};
    EXPECT_LT(high.magnitudeAt(0.0), 1e-9);
    EXPECT_LT(high.magnitudeAt(1.0), 1e-4);
}

TEST_F(EngineFilterWaveformTest, theResponseBarelyDependsOnTheSampleRate) {
    // Two copies of the same music analysed at different sample rates should
    // come out the same colour. With an exponential smoother that is nearly,
    // but not exactly, true: the coefficient is exp(-2*pi*fc/sampleRate), which
    // fixes the shape at low frequencies and lets it differ near Nyquist, where
    // the two rates are simply not the same distance away.
    //
    // So the coefficient is not taken from the formula at the running rate: the
    // 44.1 kHz response is the target and the coefficient is fitted to it, 44.1
    // being the only rate the model was measured at. Without that fit the high
    // band moves by 91 % at 96 kHz and the hue of a pure tone by 31.5 degrees,
    // which is a different colour for the same music.
    //
    // What is left is measured here rather than assumed, and it is not zero:
    // one coefficient cannot reproduce at 96 kHz a shape recorded at 44.1.
    EngineFilterWaveformLow referenceLow{mixxx::audio::SampleRate(44100)};
    EngineFilterWaveformMid referenceMid{mixxx::audio::SampleRate(44100)};
    EngineFilterWaveformHigh referenceHigh{mixxx::audio::SampleRate(44100)};

    for (const int sampleRate : {48000, 96000}) {
        EngineFilterWaveformLow low{mixxx::audio::SampleRate(sampleRate)};
        EngineFilterWaveformMid mid{mixxx::audio::SampleRate(sampleRate)};
        EngineFilterWaveformHigh high{mixxx::audio::SampleRate(sampleRate)};

        double worstLowMid = 0.0;
        double worstHigh = 0.0;
        for (double f = 40.0; f <= 16000.0; f *= 1.1) {
            const auto relative = [f](const EngineFilterWaveformBand& a,
                                          const EngineFilterWaveformBand& b) {
                const double reference = b.magnitudeAt(f);
                return reference > 1e-6 ? std::abs(a.magnitudeAt(f) - reference) / reference : 0.0;
            };
            worstLowMid = std::max({worstLowMid, relative(low, referenceLow), relative(mid, referenceMid)});
            worstHigh = std::max(worstHigh, relative(high, referenceHigh));
        }
        // The limits are what the fit achieves with one coefficient per
        // section, and they are stated in magnitude because that is what this
        // test can see. What they cost in COLOUR, which is the thing that
        // matters, was measured separately on a pure tone against the 44.1 kHz
        // response: 0.6 degrees of hue at 48 kHz and 3.1 at 96 kHz, worst case,
        // against 5.5 and 31.5 without the fit. Three degrees is below what the
        // eye separates on a waveform, so the remaining magnitude error is
        // accepted rather than chased with a second coefficient.
        EXPECT_LT(worstLowMid, 0.25) << "low or mid band moved by " << worstLowMid
                                     << " between 44100 and " << sampleRate;
        EXPECT_LT(worstHigh, 0.35) << "high band moved by " << worstHigh << " between 44100 and "
                                   << sampleRate;
        printf("sample rate %d: low/mid within %.3f, high within %.3f of 44100\n",
                sampleRate,
                worstLowMid,
                worstHigh);
    }
}

} // namespace
