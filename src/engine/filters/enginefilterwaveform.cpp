#include "engine/filters/enginefilterwaveform.h"

#include <cmath>
#include <cstring>

#include "util/math.h"

namespace {

using namespace mixxx::waveformfilter;

/// Bilinear transform of a one pole lowpass with corner frequency `fc`, in the
/// coefficient layout of EngineFilterIIR<1, IIR_LPMO>:
///     H(z) = c0 * (1 + z^-1) / (1 + c1 * z^-1)
void designOnePoleLowpass(double fc, double sampleRate, double* pC0, double* pC1) {
    const double k = std::tan(M_PI * fc / sampleRate);
    *pC0 = k / (1.0 + k);
    *pC1 = (k - 1.0) / (k + 1.0);
}

/// Bilinear transform of a one pole highpass with corner frequency `fc`, in the
/// coefficient layout of EngineFilterIIR<1, IIR_HPMO>:
///     H(z) = c0 * (1 - z^-1) / (1 + c1 * z^-1)
void designOnePoleHighpass(double fc, double sampleRate, double* pC0, double* pC1) {
    const double k = std::tan(M_PI * fc / sampleRate);
    *pC0 = 1.0 / (1.0 + k);
    *pC1 = (k - 1.0) / (k + 1.0);
}

/// Magnitude of c0 * (1 - z^-1) / (1 - p * z^-1) at `f`, for c0 = 1.
double onePoleHighpassMagnitude(double p, double f, double sampleRate) {
    const double w = 2.0 * M_PI * f / sampleRate;
    return 2.0 * std::sin(w / 2.0) /
            std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p);
}

/// Place the pole of a one pole highpass so that its digital magnitude response
/// follows the analogue prototype s / (s + 2*pi*fc) across the audible band.
///
/// The corner is above Nyquist, so the usual prewarped bilinear transform is not
/// available (fidlib rejects such a spec outright). Instead the pole is chosen
/// so that the droop of the digital differentiator towards Nyquist matches the
/// roll-off of the analogue prototype at `fRef`. Writing K for the ratio of the
/// wanted response at `fRef` to the low frequency asymptote, the condition
///     |1 - p| / |1 - p*e^-jw| = 1/K
/// is a plain quadratic in p with one root inside the unit circle.
///
/// The point of doing this rather than using a plain sample difference is that
/// the resulting response is the same at every sample rate: tracks analysed at
/// 44.1 and 96 kHz get the same colours.
double designDifferentiatorPole(double fc, double fRef, double sampleRate) {
    const double w1 = 2.0 * M_PI * fRef / sampleRate;
    const double analogRatio = fc / std::hypot(fRef, fc);
    const double k = 2.0 * std::sin(w1 / 2.0) / (w1 * analogRatio);
    const double kk = k * k;
    const double a = kk - 1.0;
    const double b = -2.0 * (kk - std::cos(w1));
    const double disc = b * b - 4.0 * a * a;
    VERIFY_OR_DEBUG_ASSERT(std::abs(a) > 1e-9 && disc >= 0.0) {
        // Degenerates into a plain sample difference, which is the correct
        // limit for very high sample rates anyway.
        return 0.0;
    }
    // Both roots multiply to a/a = 1, so exactly one of them is stable.
    const double root = (-b - std::sqrt(disc)) / (2.0 * a);
    return std::abs(root) < 1.0 ? root : (-b + std::sqrt(disc)) / (2.0 * a);
}

/// Highest frequency we can still measure the response at for a given sample
/// rate. Above 40.9 kHz this is kHighNormalizationHz, i.e. every normal file.
double normalizationFrequency(double sampleRate) {
    return math_min(kHighNormalizationHz, 0.49 * sampleRate);
}

/// Magnitude of the analogue prototype s / (s + 2*pi*fc) at `f`, normalised so
/// that it is 1.0 at kHighNormalizationHz. Only needed for sample rates too low
/// to reach 20 kHz, where the filter has to be anchored somewhere else.
double analogHighpassMagnitude(double fc, double f) {
    return (f / std::hypot(f, fc)) /
            (kHighNormalizationHz / std::hypot(kHighNormalizationHz, fc));
}

} // namespace

EngineFilterWaveformLow::EngineFilterWaveformLow(mixxx::audio::SampleRate sampleRate)
        : m_dryGain(std::pow(10.0, kLowShelfDb / 20.0)),
          m_wetGain(1.0 - std::pow(10.0, kLowShelfDb / 20.0)) {
    // low = dry * x + wet * lowpass(x, 115 Hz); at DC both parts are in phase
    // and add up to exactly 1.0, which is where this band peaks.
    double c0, c1;
    designOnePoleLowpass(mixxx::waveformfilter::kLowCornerHz, sampleRate, &c0, &c1);
    std::memcpy(m_oldCoef, m_coef, sizeof(m_coef));
    m_coef[0] = c0;
    m_coef[1] = c1;
    initBuffers();
}

void EngineFilterWaveformLow::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    EngineFilterIIR<1, IIR_LPMO>::process(pIn, pOutput, bufferSize);
    for (std::size_t i = 0; i < bufferSize; ++i) {
        pOutput[i] = static_cast<CSAMPLE>(
                m_dryGain * pIn[i] + m_wetGain * pOutput[i]);
    }
}

EngineFilterWaveformMid::EngineFilterWaveformMid(mixxx::audio::SampleRate sampleRate)
        : m_dryGain(std::pow(10.0, kMidShelfDb / 20.0)),
          m_wetGain(1.0 - std::pow(10.0, kMidShelfDb / 20.0)) {
    // mid = dry * x + wet * highpass(x, 145 Hz); well above the corner both
    // parts are in phase and add up to exactly 1.0, the peak of this band.
    double c0, c1;
    designOnePoleHighpass(mixxx::waveformfilter::kMidCornerHz, sampleRate, &c0, &c1);
    std::memcpy(m_oldCoef, m_coef, sizeof(m_coef));
    m_coef[0] = c0;
    m_coef[1] = c1;
    initBuffers();
}

void EngineFilterWaveformMid::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    EngineFilterIIR<1, IIR_HPMO>::process(pIn, pOutput, bufferSize);
    for (std::size_t i = 0; i < bufferSize; ++i) {
        pOutput[i] = static_cast<CSAMPLE>(
                m_dryGain * pIn[i] + m_wetGain * pOutput[i]);
    }
}

EngineFilterWaveformHigh::EngineFilterWaveformHigh(mixxx::audio::SampleRate sampleRate) {
    const double fRef = normalizationFrequency(sampleRate);
    const double pole = designDifferentiatorPole(
            mixxx::waveformfilter::kHighCornerHz, fRef, sampleRate);
    // Normalise to unit gain at the top of the measured grid. For sample rates
    // that cannot reach 20 kHz the anchor moves down but the level is corrected
    // back, so the band keeps the same weight relative to low and mid.
    const double gain = analogHighpassMagnitude(mixxx::waveformfilter::kHighCornerHz, fRef) /
            onePoleHighpassMagnitude(pole, fRef, sampleRate);
    std::memcpy(m_oldCoef, m_coef, sizeof(m_coef));
    m_coef[0] = gain;
    // IIR_HPMO stores the denominator as (1 + c1 * z^-1), the pole is at -c1.
    m_coef[1] = -pole;
    initBuffers();
}
