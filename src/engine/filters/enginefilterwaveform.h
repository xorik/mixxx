#pragma once

#include "audio/types.h"
#include "engine/filters/enginefilteriir.h"

/// Band splitting filters used by AnalyzerWaveform to colour the waveform.
///
/// These are not crossover filters: they reproduce the three overlapping
/// frequency responses that were measured from Traktor Pro 4 and stored in
/// mixxx-research/traktor/color_afr_final.json. They are built from first order
/// shelves, which is what the measured slopes (~6 dB/octave, no floors) call
/// for. Each band is normalised so that its peak magnitude is 1.0, and the
/// balance between bands lives in the renderer instead (bandColorGain,
/// low 1.068 / mid 1.000 / high 7.111).
///
/// Fit error against the measured target, 100 Hz .. 20 kHz, in dB RMS:
/// low 0.37, mid 0.38, high 0.99. For comparison, the Bessel crossovers that
/// were used before missed the same target by 28-29 dB.
namespace mixxx {
namespace waveformfilter {

/// Low band: one high frequency shelf, corner 115 Hz, shelf -37 dB.
constexpr double kLowCornerHz = 115.0;
constexpr double kLowShelfDb = -37.0;

/// Mid band: a low frequency shelf plus a gentle high frequency one. The second
/// section is not decoration: the measured mid band droops by 2.4-2.7 dB above
/// 5 kHz and a single one pole shelf is flat there. Since mid is usually the
/// largest of the three bands, and the colour is formed by normalising to the
/// largest, an error in it moves both of the other ratios. Measured on 13
/// tracks, adding this section cuts the hue error contributed by the filter
/// shapes from a median of 9.5 to 5.7 degrees.
constexpr double kMidLowShelfCornerHz = 225.0;
constexpr double kMidLowShelfDb = -15.0;
constexpr double kMidHighShelfCornerHz = 1750.0;
constexpr double kMidHighShelfDb = -3.0;

/// High band: first order highpass with the corner far above the audible band,
/// i.e. a differentiator. The corner has to stay above 20 kHz: with any shape
/// that has a finite low frequency shelf the whole subsonic content of a track
/// leaks into the high band and inflates it tenfold (median hue error 52
/// degrees instead of 17). A first order highpass has an exact zero at DC, so
/// that failure mode is impossible by construction.
constexpr double kHighCornerHz = 50000.0;
/// Frequency at which the high band is normalised to unit gain. Matches the top
/// of the measured 1/3 octave grid, which is where the measured high band peaks.
constexpr double kHighNormalizationHz = 20000.0;

} // namespace waveformfilter
} // namespace mixxx

/// A first order shelf: the dry signal mixed with a one pole section. The dry
/// path is what makes the shelf finite; a bare one pole section would keep
/// falling forever.
///
/// These sections override process() with a single pass that is safe to run in
/// place, and they deliberately skip the crossfade that EngineFilterIIR does
/// when coefficients change. Nothing here ever changes coefficients: the
/// analyzer builds the filters once per track and settles them immediately.
template<enum IIRPass PASS>
class EngineFilterOnePoleShelf : public EngineFilterIIR<1, PASS> {
  public:
    /// `cornerHz` and `shelfDb` describe the shelf; `outputGain` scales the
    /// whole section and exists so a cascade can be normalised without an extra
    /// pass over the buffer.
    EngineFilterOnePoleShelf(double cornerHz,
            double shelfDb,
            mixxx::audio::SampleRate sampleRate,
            double outputGain = 1.0);

    void process(const CSAMPLE* pIn, CSAMPLE* pOutput, std::size_t bufferSize) override;

    /// Replaces the output gain while keeping the shelf itself unchanged.
    void setOutputGain(double outputGain);

    /// Magnitude of this section at `frequencyHz`, used to normalise cascades.
    double magnitudeAt(double frequencyHz) const;

  private:
    double m_sampleRate;
    double m_dryGain;
    double m_wetGain;
};

/// Flat below the corner, `shelfDb` down above it.
using EngineFilterHighShelf1 = EngineFilterOnePoleShelf<IIR_LPMO>;
/// `shelfDb` down below the corner, flat above it.
using EngineFilterLowShelf1 = EngineFilterOnePoleShelf<IIR_HPMO>;

/// Low band. Peaks at exactly 1.0 at DC, where the dry and the filtered path
/// are in phase and add up to the full signal.
class EngineFilterWaveformLow : public EngineFilterHighShelf1 {
  public:
    explicit EngineFilterWaveformLow(mixxx::audio::SampleRate sampleRate);
};

/// Mid band: two shelves in cascade, normalised to unit peak.
class EngineFilterWaveformMid : public EngineFilterIIRBase {
  public:
    explicit EngineFilterWaveformMid(mixxx::audio::SampleRate sampleRate);

    void process(const CSAMPLE* pIn, CSAMPLE* pOutput, std::size_t bufferSize) override;
    void assumeSettled() override;

  private:
    EngineFilterLowShelf1 m_lowShelf;
    EngineFilterHighShelf1 m_highShelf;
};

/// High band: first order highpass with a corner far above Nyquist.
class EngineFilterWaveformHigh : public EngineFilterIIR<1, IIR_HPMO> {
  public:
    explicit EngineFilterWaveformHigh(mixxx::audio::SampleRate sampleRate);
};
