#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "engine/filters/enginefilterwaveform.h"
#include "util/sample.h"

namespace {

constexpr double kTwoPi = 2.0 * M_PI;

/// Measured Traktor band responses, mixxx-research/traktor/color_afr_final.json,
/// in dB relative to the peak of each band, interpolated on a log frequency axis
/// onto a handful of round frequencies. Only a subset of the 1/3 octave grid is
/// repeated here; it is enough to pin down the shape of all three bands.
struct ResponsePoint {
    double frequencyHz;
    double lowDb;
    double midDb;
    double highDb;
};

constexpr ResponsePoint kTarget[] = {
        {20.0, -1.16, -14.53, -59.08},
        {50.0, -0.76, -10.21, -53.55},
        {100.0, -1.98, -6.76, -47.04},
        {200.0, -4.94, -3.64, -40.00},
        {500.0, -11.86, -0.48, -30.94},
        {1000.0, -18.00, -0.13, -24.08},
        {2000.0, -24.23, -1.20, -17.84},
        {5000.0, -30.57, -2.40, -9.79},
        {10000.0, -34.18, -2.71, -4.45},
        {16000.0, -35.57, -2.26, -1.22},
};

/// The fitted shapes stay within about 1 dB RMS of the measured target, with
/// single point deviations up to 2.2 dB (the worst is the high band below
/// 100 Hz). For scale: the Bessel crossovers used before missed the same target
/// by 28-29 dB.
constexpr double kToleranceDb = 2.5;

/// Feeds a sine at `frequencyHz` through `filter` and returns the RMS gain,
/// discarding the first half of the buffer so the filter state has settled.
double measureGain(EngineFilterIIRBase* pFilter, double frequencyHz, int sampleRate) {
    constexpr int kFrames = 1 << 16;
    std::vector<CSAMPLE> in(kFrames * 2);
    std::vector<CSAMPLE> out(kFrames * 2);
    for (int i = 0; i < kFrames; ++i) {
        const auto value = static_cast<CSAMPLE>(
                std::sin(kTwoPi * frequencyHz * i / sampleRate));
        in[i * 2] = value;
        in[i * 2 + 1] = value;
    }
    pFilter->assumeSettled();
    pFilter->process(in.data(), out.data(), in.size());

    double sumIn = 0.0;
    double sumOut = 0.0;
    for (int i = kFrames; i < kFrames * 2; i += 2) {
        sumIn += static_cast<double>(in[i]) * in[i];
        sumOut += static_cast<double>(out[i]) * out[i];
    }
    return std::sqrt(sumOut / sumIn);
}

double toDb(double gain) {
    return 20.0 * std::log10(gain);
}

class EngineFilterWaveformTest : public testing::Test {};

TEST_F(EngineFilterWaveformTest, lowBandMatchesMeasuredResponse) {
    for (const auto& point : kTarget) {
        EngineFilterWaveformLow filter{mixxx::audio::SampleRate(44100)};
        const double db = toDb(measureGain(&filter, point.frequencyHz, 44100));
        EXPECT_NEAR(db, point.lowDb, kToleranceDb) << "at " << point.frequencyHz << " Hz";
    }
}

TEST_F(EngineFilterWaveformTest, midBandMatchesMeasuredResponse) {
    for (const auto& point : kTarget) {
        EngineFilterWaveformMid filter{mixxx::audio::SampleRate(44100)};
        const double db = toDb(measureGain(&filter, point.frequencyHz, 44100));
        EXPECT_NEAR(db, point.midDb, kToleranceDb) << "at " << point.frequencyHz << " Hz";
    }
}

TEST_F(EngineFilterWaveformTest, highBandMatchesMeasuredResponse) {
    for (const auto& point : kTarget) {
        EngineFilterWaveformHigh filter{mixxx::audio::SampleRate(44100)};
        const double db = toDb(measureGain(&filter, point.frequencyHz, 44100));
        EXPECT_NEAR(db, point.highDb, kToleranceDb) << "at " << point.frequencyHz << " Hz";
    }
}

/// Every band is normalised to unit peak gain so a single byte scale can be
/// shared by all three; the balance between bands lives in the renderer.
/// Low peaks at DC and high at 20 kHz, both by construction. Mid is a cascade
/// and peaks in the middle, near 780 Hz, which the constructor has to find.
TEST_F(EngineFilterWaveformTest, bandsPeakAtUnitGain) {
    EngineFilterWaveformLow low{mixxx::audio::SampleRate(44100)};
    EXPECT_NEAR(measureGain(&low, 20.0, 44100), 0.985, 0.01);
    EngineFilterWaveformMid mid{mixxx::audio::SampleRate(44100)};
    EXPECT_NEAR(measureGain(&mid, 780.0, 44100), 1.0, 0.01);
    EngineFilterWaveformHigh high{mixxx::audio::SampleRate(44100)};
    EXPECT_NEAR(measureGain(&high, 20000.0, 44100), 1.0, 0.01);

    // Nothing may exceed the peak inside the measured range, or the byte scale
    // shared by the three bands would clip one of them.
    for (double f = 20.0; f <= mixxx::waveformfilter::kHighNormalizationHz; f *= 1.1) {
        EngineFilterWaveformLow l{mixxx::audio::SampleRate(44100)};
        EngineFilterWaveformMid m{mixxx::audio::SampleRate(44100)};
        EngineFilterWaveformHigh h{mixxx::audio::SampleRate(44100)};
        EXPECT_LE(measureGain(&l, f, 44100), 1.001) << "low at " << f << " Hz";
        EXPECT_LE(measureGain(&m, f, 44100), 1.001) << "mid at " << f << " Hz";
        EXPECT_LE(measureGain(&h, f, 44100), 1.001) << "high at " << f << " Hz";
    }
}

/// Between 20 kHz and Nyquist the high band keeps rising, because it is
/// anchored at 20 kHz rather than at Nyquist. Anchoring at Nyquist instead
/// would make the response depend on the sample rate, which is a far worse
/// trade; the overshoot is bounded and tiny, and this test says by how much.
/// Only a full scale tone above 20 kHz could reach the top of the byte range
/// through it, which no real material contains.
TEST_F(EngineFilterWaveformTest, highBandOvershootAboveTheMeasuredRangeIsNegligible) {
    EngineFilterWaveformHigh high{mixxx::audio::SampleRate(44100)};
    const double atNyquist = measureGain(&high, 22000.0, 44100);
    EXPECT_GT(atNyquist, 1.0);
    EXPECT_LT(toDb(atNyquist), 0.2);
}

/// The high band must have an exact zero at DC. Any shape with a finite low
/// frequency shelf lets the subsonic content of a track leak into the high band
/// and inflates it tenfold (median hue error 52 degrees instead of 17).
TEST_F(EngineFilterWaveformTest, highBandRejectsDc) {
    EngineFilterWaveformHigh filter{mixxx::audio::SampleRate(44100)};
    constexpr int kFrames = 4096;
    std::vector<CSAMPLE> in(kFrames * 2, 1.0f);
    std::vector<CSAMPLE> out(kFrames * 2, 0.0f);
    filter.assumeSettled();
    filter.process(in.data(), out.data(), in.size());
    // Only the initial step survives, the steady state is exactly zero.
    for (int i = kFrames; i < kFrames * 2; ++i) {
        EXPECT_NEAR(out[i], 0.0f, 1e-6f);
    }
}

/// The response has to be the same at every sample rate, otherwise the same
/// track would get different colours depending on the file it was decoded from.
/// This is the reason the high band pole is placed by matching the analogue
/// prototype rather than by simply differencing consecutive samples.
TEST_F(EngineFilterWaveformTest, responseIsSampleRateIndependent) {
    for (const auto& point : kTarget) {
        EngineFilterWaveformLow low44{mixxx::audio::SampleRate(44100)};
        EngineFilterWaveformLow low96{mixxx::audio::SampleRate(96000)};
        // The low band tolerance is the wider one because the zero that the
        // bilinear transform puts on Nyquist sits at a different audio
        // frequency for each sample rate. It only matters at 16 kHz, where the
        // band is 35 dB down anyway.
        EXPECT_NEAR(toDb(measureGain(&low44, point.frequencyHz, 44100)),
                toDb(measureGain(&low96, point.frequencyHz, 96000)),
                1.0)
                << "low at " << point.frequencyHz << " Hz";

        EngineFilterWaveformMid mid44{mixxx::audio::SampleRate(44100)};
        EngineFilterWaveformMid mid96{mixxx::audio::SampleRate(96000)};
        EXPECT_NEAR(toDb(measureGain(&mid44, point.frequencyHz, 44100)),
                toDb(measureGain(&mid96, point.frequencyHz, 96000)),
                0.5)
                << "mid at " << point.frequencyHz << " Hz";

        EngineFilterWaveformHigh high44{mixxx::audio::SampleRate(44100)};
        EngineFilterWaveformHigh high96{mixxx::audio::SampleRate(96000)};
        EXPECT_NEAR(toDb(measureGain(&high44, point.frequencyHz, 44100)),
                toDb(measureGain(&high96, point.frequencyHz, 96000)),
                0.5)
                << "high at " << point.frequencyHz << " Hz";
    }
}

} // namespace
