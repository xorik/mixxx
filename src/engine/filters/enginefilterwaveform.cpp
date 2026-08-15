#include "engine/filters/enginefilterwaveform.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

#include "util/math.h"

namespace {

using namespace mixxx::waveformfilter;

/// Bilinear transform of a one pole section with corner frequency `fc`, in the
/// coefficient layout used by EngineFilterIIR<1, IIR_LPMO / IIR_HPMO>:
///     lowpass   H(z) = c0 * (1 + z^-1) / (1 + c1 * z^-1)
///     highpass  H(z) = c0 * (1 - z^-1) / (1 + c1 * z^-1)
/// Both share the same pole, only the numerator differs.
void designOnePole(bool highpass, double fc, double sampleRate, double* pC0, double* pC1) {
    const double k = std::tan(M_PI * fc / sampleRate);
    *pC0 = highpass ? 1.0 / (1.0 + k) : k / (1.0 + k);
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

/// Points used to locate the peak of the mid band cascade. The peak is a broad
/// maximum around 800 Hz, so a coarse logarithmic scan is plenty: 256 points
/// place it to better than 0.01 dB.
constexpr int kPeakScanPoints = 256;

} // namespace

template<enum IIRPass PASS>
EngineFilterOnePoleShelf<PASS>::EngineFilterOnePoleShelf(double cornerHz,
        double shelfDb,
        mixxx::audio::SampleRate sampleRate,
        double outputGain)
        : m_sampleRate(sampleRate),
          m_dryGain(outputGain * std::pow(10.0, shelfDb / 20.0)),
          m_wetGain(outputGain * (1.0 - std::pow(10.0, shelfDb / 20.0))) {
    double c0, c1;
    designOnePole(PASS == IIR_HPMO, cornerHz, sampleRate, &c0, &c1);
    std::memcpy(this->m_oldCoef, this->m_coef, sizeof(this->m_coef));
    this->m_coef[0] = c0;
    this->m_coef[1] = c1;
    this->initBuffers();
}

template<enum IIRPass PASS>
void EngineFilterOnePoleShelf<PASS>::setOutputGain(double outputGain) {
    const double shelf = m_dryGain / (m_dryGain + m_wetGain);
    m_dryGain = outputGain * shelf;
    m_wetGain = outputGain * (1.0 - shelf);
}

template<enum IIRPass PASS>
void EngineFilterOnePoleShelf<PASS>::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    // Each sample is read before the matching output is written, so it is safe
    // to pass the same buffer for input and output.
    for (std::size_t i = 0; i < bufferSize; i += 2) {
        const double left = pIn[i];
        const double right = pIn[i + 1];
        pOutput[i] = static_cast<CSAMPLE>(m_dryGain * left +
                m_wetGain * this->processSample(this->m_coef, this->m_buf1, left));
        pOutput[i + 1] = static_cast<CSAMPLE>(m_dryGain * right +
                m_wetGain * this->processSample(this->m_coef, this->m_buf2, right));
    }
}

template<enum IIRPass PASS>
double EngineFilterOnePoleShelf<PASS>::magnitudeAt(double frequencyHz) const {
    const std::complex<double> zInv =
            std::exp(std::complex<double>(0.0, -2.0 * M_PI * frequencyHz / m_sampleRate));
    const std::complex<double> numerator =
            PASS == IIR_HPMO ? 1.0 - zInv : 1.0 + zInv;
    const std::complex<double> onePole =
            this->m_coef[0] * numerator / (1.0 + this->m_coef[1] * zInv);
    return std::abs(m_dryGain + m_wetGain * onePole);
}

template class EngineFilterOnePoleShelf<IIR_LPMO>;
template class EngineFilterOnePoleShelf<IIR_HPMO>;

EngineFilterWaveformLow::EngineFilterWaveformLow(mixxx::audio::SampleRate sampleRate)
        : EngineFilterHighShelf1(kLowCornerHz, kLowShelfDb, sampleRate) {
    // No normalisation needed: at DC the dry and the filtered path are in phase
    // and add up to exactly 1.0, which is where this band peaks.
}

EngineFilterWaveformMid::EngineFilterWaveformMid(mixxx::audio::SampleRate sampleRate)
        : m_lowShelf(kMidLowShelfCornerHz, kMidLowShelfDb, sampleRate),
          m_highShelf(kMidHighShelfCornerHz, kMidHighShelfDb, sampleRate) {
    // Unlike the other two bands the cascade does not peak at either end, so
    // find the maximum (around 800 Hz) and fold its inverse into the second
    // section. The measured mid band peaks at 684 Hz, for comparison.
    const double top = normalizationFrequency(sampleRate);
    double peak = 0.0;
    for (int i = 0; i <= kPeakScanPoints; ++i) {
        const double frequency = 20.0 *
                std::pow(top / 20.0, static_cast<double>(i) / kPeakScanPoints);
        peak = std::max(peak,
                m_lowShelf.magnitudeAt(frequency) * m_highShelf.magnitudeAt(frequency));
    }
    VERIFY_OR_DEBUG_ASSERT(peak > 0.0) {
        return;
    }
    m_highShelf.setOutputGain(1.0 / peak);
}

void EngineFilterWaveformMid::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    m_lowShelf.process(pIn, pOutput, bufferSize);
    // In place, which the sections support by construction.
    m_highShelf.process(pOutput, pOutput, bufferSize);
}

void EngineFilterWaveformMid::assumeSettled() {
    m_lowShelf.assumeSettled();
    m_highShelf.assumeSettled();
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
