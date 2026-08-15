#pragma once

#include "audio/types.h"
#include "engine/filters/enginefilteriir.h"

/// Band splitting filters used by AnalyzerWaveform to colour the waveform.
///
/// These are not crossover filters: they reproduce the three overlapping
/// frequency responses that were measured from Traktor Pro 4 and stored in
/// mixxx-research/traktor/color_afr_final.json. All three are first order,
/// which is what the measured slopes (~6 dB/octave, no floors) call for; each
/// one is normalised so that its peak magnitude over the audible band is 1.0,
/// and the inter-band balance lives in the renderer instead (bandColorGain,
/// low 1.068 / mid 1.000 / high 7.111).
///
/// Fit error against the measured target, 100 Hz .. 20 kHz, in dB RMS:
/// low 0.37, mid 0.95, high 0.99. For comparison, the Bessel filters that were
/// used before missed the same target by 28-29 dB.
namespace mixxx {
namespace waveformfilter {

/// Low band: first order high frequency shelf, corner 115 Hz, shelf -37 dB.
constexpr double kLowCornerHz = 115.0;
constexpr double kLowShelfDb = -37.0;

/// Mid band: first order low frequency shelf, corner 145 Hz, shelf -20 dB.
constexpr double kMidCornerHz = 145.0;
constexpr double kMidShelfDb = -20.0;

/// High band: first order highpass with the corner far above the audible band,
/// i.e. a differentiator. The corner has to stay above 20 kHz: with any shape
/// that has a finite low frequency shelf the whole subsonic content of a track
/// leaks into the high band and inflates it tenfold (median hue error 52
/// degrees instead of 17). A first order highpass has an exact zero at DC, so
/// that failure mode is impossible by construction.
constexpr double kHighCornerHz = 50000.0;
/// Frequency at which the high band is normalised to unit gain. Matches the
/// top of the measured 1/3 octave grid, which is where the measured high band
/// response peaks.
constexpr double kHighNormalizationHz = 20000.0;

} // namespace waveformfilter
} // namespace mixxx

/// First order high frequency shelf: dry + one pole lowpass.
class EngineFilterWaveformLow : public EngineFilterIIR<1, IIR_LPMO> {
  public:
    explicit EngineFilterWaveformLow(mixxx::audio::SampleRate sampleRate);

    void process(const CSAMPLE* pIn, CSAMPLE* pOutput, std::size_t bufferSize) override;

  private:
    double m_dryGain;
    double m_wetGain;
};

/// First order low frequency shelf: dry + one pole highpass.
class EngineFilterWaveformMid : public EngineFilterIIR<1, IIR_HPMO> {
  public:
    explicit EngineFilterWaveformMid(mixxx::audio::SampleRate sampleRate);

    void process(const CSAMPLE* pIn, CSAMPLE* pOutput, std::size_t bufferSize) override;

  private:
    double m_dryGain;
    double m_wetGain;
};

/// First order highpass with a corner far above Nyquist (a differentiator).
class EngineFilterWaveformHigh : public EngineFilterIIR<1, IIR_HPMO> {
  public:
    explicit EngineFilterWaveformHigh(mixxx::audio::SampleRate sampleRate);
};
