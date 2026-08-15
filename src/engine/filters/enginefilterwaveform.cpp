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

/// Solve |1 - p| / |1 - p*e^-jw1| = 1/k for the pole p. This is a plain
/// quadratic whose two roots multiply to one, so exactly one of them is stable.
///
/// Both sections of the high band need this. Their corner frequencies lie above
/// Nyquist, so the usual prewarped bilinear transform is not available at all
/// (fidlib rejects such a spec outright), and a pole placed by impulse
/// invariance collapses to nothing useful. Instead the pole is placed so that
/// the digital response passes through the analogue prototype's value at one
/// reference frequency, which pins the whole in band shape because a first
/// order section has only one degree of freedom left.
///
/// The point of doing this rather than reaching for a plain sample difference
/// is that the result is the same at every sample rate: tracks analysed at 44.1
/// and 96 kHz get the same colours.
double solvePoleForGainRatio(double k, double w1) {
    const double kk = k * k;
    const double a = kk - 1.0;
    const double b = -2.0 * (kk - std::cos(w1));
    const double disc = b * b - 4.0 * a * a;
    VERIFY_OR_DEBUG_ASSERT(std::abs(a) > 1e-9 && disc >= 0.0) {
        // No bend at all, which is the correct limit for k -> 1.
        return 0.0;
    }
    const double root = (-b - std::sqrt(disc)) / (2.0 * a);
    return std::abs(root) < 1.0 ? root : (-b + std::sqrt(disc)) / (2.0 * a);
}

/// Magnitude of the analogue first order prototype at `f`, relative to its flat
/// end: 1/sqrt(1 + (f/fc)^2) for a lowpass, and the same expression for the
/// ratio of a highpass to its 6 dB/octave asymptote.
double analogFirstOrderRatio(double fc, double f) {
    return fc / std::hypot(f, fc);
}

/// Pole of a differentiator, i.e. a highpass whose corner is above Nyquist. The
/// zero on DC contributes a sinc factor that has to be divided out before the
/// remaining bend is matched.
double designDifferentiatorPole(double fc, double fRef, double sampleRate) {
    const double w1 = 2.0 * M_PI * fRef / sampleRate;
    return solvePoleForGainRatio(
            2.0 * std::sin(w1 / 2.0) / (w1 * analogFirstOrderRatio(fc, fRef)), w1);
}

/// Pole of a lowpass whose corner is above Nyquist. There is no zero, so the
/// wanted ratio is used directly.
double designRolloffPole(double fc, double fRef, double sampleRate) {
    const double w1 = 2.0 * M_PI * fRef / sampleRate;
    return solvePoleForGainRatio(1.0 / analogFirstOrderRatio(fc, fRef), w1);
}

/// Highest frequency we can still measure the response at for a given sample
/// rate. Above 40.9 kHz this is kHighNormalizationHz, i.e. every normal file.
double normalizationFrequency(double sampleRate) {
    return math_min(kHighNormalizationHz, 0.49 * sampleRate);
}

/// Magnitude of the whole analogue high band prototype at `f`, normalised so
/// that it is 1.0 at kHighNormalizationHz. Only needed for sample rates too low
/// to reach 20 kHz, where the band has to be anchored somewhere else and the
/// level then corrected back so that it keeps its weight against low and mid.
double analogHighBandMagnitude(double f) {
    const auto shape = [](double frequency) {
        return (frequency / std::hypot(frequency, kHighCornerHz)) *
                analogFirstOrderRatio(kHighRolloffHz, frequency);
    };
    return shape(f) / shape(kHighNormalizationHz);
}

/// Points used to locate the peak of the mid band cascade. The peak is a broad
/// maximum around 800 Hz, so a coarse logarithmic scan is plenty: 256 points
/// place it to better than 0.01 dB.
constexpr int kPeakScanPoints = 256;

} // namespace

template<enum IIRPass PASS>
EngineFilterOnePoleRaw<PASS>::EngineFilterOnePoleRaw(double gain,
        double poleCoef,
        double sampleRate)
        : m_sampleRate(sampleRate) {
    std::memcpy(this->m_oldCoef, this->m_coef, sizeof(this->m_coef));
    this->m_coef[0] = gain;
    this->m_coef[1] = poleCoef;
    this->initBuffers();
}

template<enum IIRPass PASS>
void EngineFilterOnePoleRaw<PASS>::setGain(double gain) {
    this->m_coef[0] = gain;
}

template<enum IIRPass PASS>
void EngineFilterOnePoleRaw<PASS>::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    for (std::size_t i = 0; i < bufferSize; i += 2) {
        pOutput[i] = static_cast<CSAMPLE>(
                this->processSample(this->m_coef, this->m_buf1, pIn[i]));
        pOutput[i + 1] = static_cast<CSAMPLE>(
                this->processSample(this->m_coef, this->m_buf2, pIn[i + 1]));
    }
}

template<enum IIRPass PASS>
double EngineFilterOnePoleRaw<PASS>::magnitudeAt(double frequencyHz) const {
    const std::complex<double> zInv =
            std::exp(std::complex<double>(0.0, -2.0 * M_PI * frequencyHz / m_sampleRate));
    const std::complex<double> numerator = PASS == IIR_P1 ? std::complex<double>(1.0, 0.0)
                                                          : 1.0 - zInv;
    return std::abs(this->m_coef[0] * numerator / (1.0 + this->m_coef[1] * zInv));
}

template class EngineFilterOnePoleRaw<IIR_HPMO>;
template class EngineFilterOnePoleRaw<IIR_P1>;

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

namespace {
/// Both sections store the denominator as (1 + c1 * z^-1), so the pole sits at
/// -c1. Written out because getting this sign wrong is silent and expensive.
double poleToCoefficient(double pole) {
    return -pole;
}
} // namespace

EngineFilterWaveformHigh::EngineFilterWaveformHigh(mixxx::audio::SampleRate sampleRate)
        : m_differentiator(1.0,
                  poleToCoefficient(designDifferentiatorPole(kHighCornerHz,
                          normalizationFrequency(sampleRate),
                          sampleRate)),
                  sampleRate),
          m_rolloff(1.0,
                  poleToCoefficient(designRolloffPole(kHighRolloffHz,
                          normalizationFrequency(sampleRate),
                          sampleRate)),
                  sampleRate) {
    // Anchor the cascade at 20 kHz rather than at its peak: it keeps rising
    // past that point, and its peak therefore sits on Nyquist, which moves with
    // the sample rate. For sample rates that cannot reach 20 kHz the anchor
    // moves down and the level is corrected back, so the band keeps its weight
    // against low and mid.
    const double fRef = normalizationFrequency(sampleRate);
    const double atRef = m_differentiator.magnitudeAt(fRef) * m_rolloff.magnitudeAt(fRef);
    VERIFY_OR_DEBUG_ASSERT(atRef > 0.0) {
        return;
    }
    m_differentiator.setGain(analogHighBandMagnitude(fRef) / atRef);
}

void EngineFilterWaveformHigh::process(const CSAMPLE* pIn,
        CSAMPLE* pOutput,
        std::size_t bufferSize) {
    m_differentiator.process(pIn, pOutput, bufferSize);
    // In place, which the sections support by construction.
    m_rolloff.process(pOutput, pOutput, bufferSize);
}

void EngineFilterWaveformHigh::assumeSettled() {
    m_differentiator.assumeSettled();
    m_rolloff.assumeSettled();
}
