#pragma once

#include "audio/types.h"
#include "engine/filters/enginefilteriir.h"

/// Band splitting filters used by AnalyzerWaveform to colour the waveform.
///
/// These are not crossover filters. They reproduce the three overlapping
/// frequency responses measured from Traktor Pro 4.1.1 against synthetic
/// probes: the measurement is research/traktor/band_response_linear_u1.json,
/// the model research/stand/IMPLEMENTATION_SPEC.md.
///
/// Each band is a cascade of two first order sections, and each section is a
/// plain exponential smoother rather than a filter designed from an analogue
/// prototype:
///
///     a  = 1 - exp(-2*pi*fc / sampleRate)
///     LP: y[n] = y[n-1] + a * (x[n] - y[n-1])
///     HP: x[n] - LP(x)[n]
///
/// That is what Traktor appears to use, and matching the form rather than
/// approximating it removes most of the residual: fitted against the measured
/// sweep, as rms of the log magnitude with one gain per band and measured
/// points below 3e-4 dropped as noise floor, this scores 0.018 / 0.008 / 0.020
/// per band and 0.016 overall, against 0.032 for the closest analogue model.
/// The mid band improves by a factor of five, and the limit we thought we had
/// hit was the prototype, not the measurement.
///
/// A lowpass of this form has NO zero at Nyquist, which is the property that
/// gave the analogue approximation its trouble; a highpass, being one minus a
/// lowpass, has an exact zero at DC, which is what keeps the subsonic content
/// of a track out of the high band.
///
/// Note what this buys and what it does not. It buys a filter that is right and
/// short. It does NOT move the colour: measured over nine tracks, the hue error
/// against Traktor is 21.0 degrees for either form. The fit is already finer
/// than the hue metric can resolve, and what is left sits in the alignment and
/// in the fold into a pixel, not in the filters.
namespace mixxx {
namespace waveformfilter {

/// The three cascades, as corner frequencies of the smoothers.
constexpr double kLowLp1Hz = 178.2;
constexpr double kLowLp2Hz = 130.4;
constexpr double kMidHpHz = 517.5;
constexpr double kMidLpHz = 978.9;
constexpr double kHighHp1Hz = 1790.4;
constexpr double kHighHp2Hz = 11843.0;

/// Balance between the bands, applied to the output of each cascade. These are
/// the measured gains; only their ratios matter, because the renderer
/// normalises a column to its largest channel.
///
/// Do not read them as loudness. A cascade does not reach 1.0 inside itself -
/// the high band peaks at 0.273, because a highpass built as one minus a
/// lowpass never quite gets there - so the gains look larger than they are.
/// The quantity that is physical, and the one to check an implementation
/// against, is the peak of gain times cascade: 1.000 : 0.807 : 3.783.
constexpr double kLowGain = 0.2637;
constexpr double kMidGain = 0.3372;
constexpr double kHighGain = 3.6545;

/// Range over which a band is scanned when its peak is needed.
constexpr double kScanLowHz = 20.0;
constexpr double kScanHighHz = 20000.0;

} // namespace waveformfilter
} // namespace mixxx

/// One exponential smoother, as a lowpass or as its complement.
class EngineFilterWaveformSection {
  public:
    EngineFilterWaveformSection(bool highpass,
            double cornerHz,
            mixxx::audio::SampleRate sampleRate,
            double outputGain);

    void process(const CSAMPLE* pIn, CSAMPLE* pOutput, std::size_t bufferSize);
    void assumeSettled();
    double magnitudeAt(double frequencyHz) const;

  private:
    bool m_highpass;
    double m_alpha;
    double m_gain;
    double m_sampleRate;
    double m_state[2];
};

/// A band: two sections in cascade, with the measured gain of the band on the
/// first of them. In a linear cascade it makes no difference where a gain is
/// applied, and one place is easier to find than two.
class EngineFilterWaveformBand : public EngineFilterIIRBase {
  public:
    EngineFilterWaveformBand(bool firstHighpass,
            double firstHz,
            bool secondHighpass,
            double secondHz,
            double gain,
            mixxx::audio::SampleRate sampleRate);

    void process(const CSAMPLE* pIn, CSAMPLE* pOutput, std::size_t bufferSize) override;
    void assumeSettled() override;

    /// Magnitude of the whole band, its gain included.
    double magnitudeAt(double frequencyHz) const;
    /// Largest magnitude over the audible range.
    double peakMagnitude() const;

  private:
    EngineFilterWaveformSection m_first;
    EngineFilterWaveformSection m_second;
};

/// Low band: lowpasses at 178 and 130 Hz.
class EngineFilterWaveformLow : public EngineFilterWaveformBand {
  public:
    explicit EngineFilterWaveformLow(mixxx::audio::SampleRate sampleRate);
};

/// Mid band: a highpass at 518 Hz and a lowpass at 979 Hz.
class EngineFilterWaveformMid : public EngineFilterWaveformBand {
  public:
    explicit EngineFilterWaveformMid(mixxx::audio::SampleRate sampleRate);
};

/// High band: highpasses at 1790 and 11843 Hz.
class EngineFilterWaveformHigh : public EngineFilterWaveformBand {
  public:
    explicit EngineFilterWaveformHigh(mixxx::audio::SampleRate sampleRate);
};
