#include "engine/filters/enginefilterwaveform.h"

#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include "util/math.h"

namespace {

using namespace mixxx::waveformfilter;

struct BandDefinition {
    bool firstHighpass;
    double firstHz;
    bool secondHighpass;
    double secondHz;
    double gain;
};

constexpr BandDefinition kBands[3] = {
        {false, kLowLp1Hz, false, kLowLp2Hz, kLowGain},
        {true, kMidHpHz, false, kMidLpHz, kMidGain},
        {true, kHighHp1Hz, true, kHighHp2Hz, kHighGain},
};

/// Magnitude of one smoother with coefficient `alpha` at `f`.
double sectionMagnitude(bool highpass, double alpha, double f, double sampleRate) {
    const std::complex<double> zInv =
            std::exp(std::complex<double>(0.0, -2.0 * M_PI * f / sampleRate));
    const std::complex<double> lowpass = alpha / (1.0 - (1.0 - alpha) * zInv);
    return std::abs(highpass ? 1.0 - lowpass : lowpass);
}

/// The coefficient the model is defined at. Everything was measured on 44.1 kHz
/// material, so that is the one rate where we know the truth.
constexpr double kModelSampleRate = 44100.0;

double modelAlpha(double cornerHz) {
    return 1.0 - std::exp(-2.0 * M_PI * cornerHz / kModelSampleRate);
}

/// Coefficient for a section running at `sampleRate` whose magnitude follows
/// the 44.1 kHz model as closely as one coefficient can.
///
/// Taking 1 - exp(-2*pi*fc/sampleRate) directly, as the model does at its own
/// rate, does NOT give the same response at another rate: the shape is pinned
/// at the bottom and free near Nyquist, and Nyquist is somewhere else. Measured
/// on the magnitudes, the high band moves by 11 % at 48 kHz and by 91 % at
/// 96 kHz; in hue that is 5.6 degrees at worst for 48 kHz and 31.5 for 96 kHz,
/// on a pure tone at the frequency where it hurts most.
///
/// Thirty degrees is a different colour. A track sounds the same at either
/// rate, so it has to look the same, and that is worth a search at construction
/// time: the response at 44.1 kHz becomes the target and the coefficient is
/// fitted to it. At 44.1 kHz itself the search returns the model coefficient,
/// so nothing is approximated where nothing has to be.
double fitAlpha(bool highpass, double cornerHz, double sampleRate) {
    if (std::abs(sampleRate - kModelSampleRate) < 1.0) {
        return modelAlpha(cornerHz);
    }
    constexpr int kPoints = 160;
    constexpr double kLowHz = 20.0;
    const double topHz = math_min(kScanHighHz, 0.45 * math_min(sampleRate, kModelSampleRate));
    std::vector<double> grid;
    std::vector<double> target;
    grid.reserve(kPoints);
    target.reserve(kPoints);
    const double reference = modelAlpha(cornerHz);
    for (int i = 0; i < kPoints; ++i) {
        const double f = kLowHz *
                std::pow(topHz / kLowHz, static_cast<double>(i) / (kPoints - 1));
        grid.push_back(f);
        target.push_back(sectionMagnitude(highpass, reference, f, kModelSampleRate));
    }

    double low = 1e-6;
    double high = 1.0;
    double best = reference;
    for (int pass = 0; pass < 4; ++pass) {
        constexpr int kSteps = 80;
        double bestError = std::numeric_limits<double>::max();
        for (int i = 0; i <= kSteps; ++i) {
            const double alpha = low + (high - low) * i / kSteps;
            double error = 0.0;
            int counted = 0;
            for (std::size_t j = 0; j < grid.size(); ++j) {
                const double value = sectionMagnitude(highpass, alpha, grid[j], sampleRate);
                if (value <= 0.0 || target[j] <= 0.0) {
                    continue;
                }
                const double difference = std::log(value) - std::log(target[j]);
                error += difference * difference;
                counted++;
            }
            // A candidate that is silent everywhere skips every point and would
            // otherwise score a perfect zero on an empty set - which is what
            // alpha = 1 does to a highpass, since 1 - lowpass is then nothing at
            // all. Scoring is only meaningful where there is something to score.
            if (counted < static_cast<int>(grid.size()) / 2) {
                continue;
            }
            error /= counted;
            if (error < bestError) {
                bestError = error;
                best = alpha;
            }
        }
        const double span = (high - low) / 8.0;
        low = math_max(1e-6, best - span);
        high = math_min(1.0, best + span);
    }
    return best;
}

} // namespace

EngineFilterWaveformSection::EngineFilterWaveformSection(bool highpass,
        double cornerHz,
        mixxx::audio::SampleRate sampleRate,
        double outputGain)
        : m_highpass(highpass),
          m_alpha(fitAlpha(highpass, cornerHz, sampleRate)),
          m_gain(outputGain),
          m_sampleRate(sampleRate),
          m_state{0.0, 0.0} {
}

void EngineFilterWaveformSection::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    // Reads each sample before writing the matching output, so it is safe to
    // pass the same buffer for input and output.
    for (std::size_t i = 0; i < bufferSize; i += 2) {
        for (int channel = 0; channel < 2; ++channel) {
            const double x = pIn[i + channel];
            m_state[channel] += m_alpha * (x - m_state[channel]);
            const double y = m_highpass ? x - m_state[channel] : m_state[channel];
            pOutput[i + channel] = static_cast<CSAMPLE>(m_gain * y);
        }
    }
}

void EngineFilterWaveformSection::assumeSettled() {
    m_state[0] = 0.0;
    m_state[1] = 0.0;
}

double EngineFilterWaveformSection::magnitudeAt(double frequencyHz) const {
    const std::complex<double> zInv =
            std::exp(std::complex<double>(0.0, -2.0 * M_PI * frequencyHz / m_sampleRate));
    const std::complex<double> lowpass = m_alpha / (1.0 - (1.0 - m_alpha) * zInv);
    return std::abs(m_gain * (m_highpass ? 1.0 - lowpass : lowpass));
}

EngineFilterWaveformBand::EngineFilterWaveformBand(bool firstHighpass,
        double firstHz,
        bool secondHighpass,
        double secondHz,
        double gain,
        mixxx::audio::SampleRate sampleRate)
        : m_first(firstHighpass, firstHz, sampleRate, gain),
          m_second(secondHighpass, secondHz, sampleRate, 1.0) {
}

void EngineFilterWaveformBand::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    m_first.process(pIn, pOutput, bufferSize);
    m_second.process(pOutput, pOutput, bufferSize);
}

void EngineFilterWaveformBand::assumeSettled() {
    m_first.assumeSettled();
    m_second.assumeSettled();
}

double EngineFilterWaveformBand::magnitudeAt(double frequencyHz) const {
    return m_first.magnitudeAt(frequencyHz) * m_second.magnitudeAt(frequencyHz);
}

double EngineFilterWaveformBand::peakMagnitude() const {
    // A logarithmic scan: the bands are broad and smooth, so a fine grid is not
    // needed to place a peak to a fraction of a percent.
    constexpr int kPoints = 2048;
    double peak = 0.0;
    for (int i = 0; i <= kPoints; ++i) {
        const double frequency = kScanLowHz *
                std::pow(kScanHighHz / kScanLowHz, static_cast<double>(i) / kPoints);
        peak = math_max(peak, magnitudeAt(frequency));
    }
    return peak;
}

EngineFilterWaveformLow::EngineFilterWaveformLow(mixxx::audio::SampleRate sampleRate)
        : EngineFilterWaveformBand(kBands[0].firstHighpass,
                  kBands[0].firstHz,
                  kBands[0].secondHighpass,
                  kBands[0].secondHz,
                  kBands[0].gain,
                  sampleRate) {
}

EngineFilterWaveformMid::EngineFilterWaveformMid(mixxx::audio::SampleRate sampleRate)
        : EngineFilterWaveformBand(kBands[1].firstHighpass,
                  kBands[1].firstHz,
                  kBands[1].secondHighpass,
                  kBands[1].secondHz,
                  kBands[1].gain,
                  sampleRate) {
}

EngineFilterWaveformHigh::EngineFilterWaveformHigh(mixxx::audio::SampleRate sampleRate)
        : EngineFilterWaveformBand(kBands[2].firstHighpass,
                  kBands[2].firstHz,
                  kBands[2].secondHighpass,
                  kBands[2].secondHz,
                  kBands[2].gain,
                  sampleRate) {
}
