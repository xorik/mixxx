#pragma once

#include <cmath>
#include <limits>

#include "analyzer/analyzer.h"
#include "library/dao/analysisdao.h"
#include "util/performancetimer.h"
#include "util/sample.h"
#include "waveform/waveform.h"

//NOTS vrince some test to segment sound, to apply color in the waveform
//#define TEST_HEAT_MAP
#ifdef TEST_HEAT_MAP
class QImage;
#endif

class EngineFilterIIRBase;
class QSqlDatabase;

struct WaveformStride {
    WaveformStride(double samples, double averageSamples, int stemCount, int sampleRate = 0)
            : m_position(0),
              m_stemCount(stemCount),
              m_length(samples),
              m_averageLength(averageSamples),
              m_averagePosition(0),
              m_averageDivisor(0),
              m_bandFrameCount(0),
              m_bandMeanSquareDivisor(0),
              m_postScaleConversion(static_cast<float>(
                      std::numeric_limits<unsigned char>::max())) {
        m_sampleRate = sampleRate;
        reset();
    }

    inline void reset() {
        m_position = 0;
        m_averageDivisor = 0;
        m_bandFrameCount = 0;
        m_bandMeanSquareDivisor = 0;
        for (int i = 0; i < ChannelCount; ++i) {
            m_overallData[i] = 0.0f;
            m_averageOverallData[i] = 0.0f;
            for (int f = 0; f < BandCount; ++f) {
                m_bandEnvelope[i][f] = 0.0f;
            }
            SampleUtil::clear(m_filteredData[i], BandCount);
            SampleUtil::clear(m_averageFilteredData[i], BandCount);
            SampleUtil::clear(m_stemData[i], m_stemCount);
        }
    }

    /// Scale a linear magnitude into the single byte the waveform format
    /// stores. The same scale is used for every band on purpose: normalising
    /// bands individually pushes quiet bands into the bottom of the range and
    /// costs a lot of colour accuracy.
    ///
    /// WARNING, this is load bearing beyond colour accuracy: the renderer of
    /// the Spectrum waveform divides the stored peak (all) by the stored band
    /// magnitudes to get the crest factor of the bin, and shades the column
    /// vertically with it. That division is only meaningful while peak and
    /// bands share one scale. Normalising bands on their own would not break
    /// anything visibly here - it would silently turn that shading into
    /// nonsense. See verticalProfile() in res/shaders/spectrumsignal.frag.
    inline unsigned char toByte(float value) const {
        return static_cast<unsigned char>(std::min(255.0,
                static_cast<double>(m_postScaleConversion) *
                                static_cast<double>(value) +
                        0.5));
    }

    /// Headroom on the height, and on the height only.
    ///
    /// The height is the peak sample of the block, and a byte holds 0..255 for
    /// a signal of 0..1 - so anything above full scale is cut. Modern masters
    /// go well above it: measured over eight tracks of this library, block
    /// peaks reach 1.620, and on one of them 65.9% of all blocks sit at or above
    /// 0.999. Cutting them does not just shorten a column, it flattens the
    /// shape: a single snare hit becomes a plateau three columns wide because
    /// its neighbours are cut to the same value.
    ///
    /// 1.75 is the smallest divisor that leaves nothing cut on that material:
    ///
    ///     1.50  ->  0.298% of blocks at the ceiling, a quiet track at 67% height
    ///     1.75  ->  0.000%                          57%
    ///     2.00  ->  0.000%                          50%
    ///
    /// Two buys no less cutting and costs another seven percent of height, so
    /// it is not worth paying for.
    ///
    /// WHAT THIS DOES NOT SOLVE, and deliberately: a quiet track is drawn
    /// shorter than a loud one, and this makes it shorter still. That is not a
    /// bug to be fixed by normalising each track to its own peak - Traktor does
    /// not do that either. Measured on its stripes, the height 64 of 64 is
    /// never reached and the per-track maximum wanders between 214 and 255,
    /// which is what a fixed scale with headroom looks like; per-track
    /// normalisation would pin every track to the ceiling exactly. Two tracks
    /// of different loudness are supposed to look different, so that a DJ can
    /// see it before hearing it.
    static constexpr float kHeightHeadroom = 1.75f;

    /// Common scale for the three band values, and for them only.
    ///
    /// The band filters now carry the measured balance, and the measured
    /// balance is not normalised to anything: a band value is what the filter
    /// puts out, which for real music is a small fraction of full scale. On a
    /// loud track the largest band byte came out at 43 of 255 - the colour was
    /// there, but squeezed into a sixth of the range, and with two and a half
    /// bits gone the quiet parts fall under the level floor of the renderer and
    /// the waveform goes dark.
    ///
    /// So the three are scaled together. Together is the whole point: a factor
    /// common to the bands cannot change their ratios, and the ratio is the
    /// colour. Applying it per band would move the hue, which is the one thing
    /// this must not do.
    ///
    /// Four is measured rather than derived. For a full scale signal the high
    /// band takes 0.76 of white noise and 0.36 of something pink and music
    /// shaped, so nothing realistic comes near clipping; on the loudest real
    /// track to hand the largest bin lands at 172 of 255, which leaves about
    /// half the range as headroom for material louder than anything we have.
    static constexpr float kBandScale = 4.0f;

    /// Per band ballistics, directly observed in the values Traktor stores:
    /// the low band follows the music with a time constant of about 100 ms, the
    /// mid 25 and the high 20. Different constants per band change the RATIO
    /// between the bands over time, and the ratio is the colour, so this is a
    /// hue effect rather than a smoothing one.
    ///
    /// The envelope is applied to the band value before it is stored, i.e.
    /// before anything turns it into a colour, and it decays with real time,
    /// exp(-dt/tau), rather than with a per stride coefficient: a stride is
    /// 1024 samples at one rate and something else at another, and a constant
    /// per stride would make the ballistics depend on the sample rate.
    ///
    /// There is no attack: the value follows a rise immediately and only the
    /// fall is slowed. That is what "envelope" means here.
    static constexpr float kBandTauSeconds[BandCount] = {0.0f, 0.100f, 0.025f, 0.020f};

    inline float applyBallistics(int channel, int band, float value, float dtSeconds) {
        if (band == AllBand || kBandTauSeconds[band] <= 0.0f || dtSeconds <= 0.0f) {
            return value;
        }
        const float decay = std::exp(-dtSeconds / kBandTauSeconds[band]);
        float& held = m_bandEnvelope[channel][band];
        held = std::max(value, held * decay);
        return held;
    }

    /// m_filteredData holds the sum of squares of the filtered signal over the
    /// current stride, so the stored band magnitude is its RMS.
    inline float bandRms(int channel, int band) const {
        if (m_bandFrameCount <= 0) {
            return 0.0f;
        }
        return std::sqrt(m_filteredData[channel][band] /
                static_cast<float>(m_bandFrameCount));
    }

    /// Largest band byte written so far. A track whose loudest band never
    /// reaches half the range is not quiet, it is mis-scaled: the colour is
    /// then carried by the bottom bits and the quiet parts of it fall under the
    /// level floor of the renderer. Worth saying out loud rather than writing
    /// dim data in silence, which is how this was found - by a user seeing an
    /// empty waveform.
    inline unsigned char loudestBandByte() const {
        return m_loudestBandByte;
    }

    /// Largest height byte written so far. With the headroom above, a normal
    /// track lands somewhere in the upper half of the range; a track that never
    /// reaches a quarter of it is either genuinely very quiet or is being
    /// scaled wrongly, and the two are worth telling apart by a number rather
    /// than by eye.
    inline unsigned char loudestHeightByte() const {
        return m_loudestHeightByte;
    }

    inline void store(WaveformData* data) {
        for (int i = 0; i < ChannelCount; ++i) {
            WaveformData& datum = *(data + i);
            datum.filtered.all = toByte(m_overallData[i] / kHeightHeadroom);
            m_loudestHeightByte = std::max(m_loudestHeightByte, datum.filtered.all);
            const float dt = m_bandFrameCount > 0 && m_sampleRate > 0
                    ? static_cast<float>(m_bandFrameCount) / static_cast<float>(m_sampleRate)
                    : 0.0f;
            datum.filtered.low = toByte(kBandScale * applyBallistics(i, Low, bandRms(i, Low), dt));
            datum.filtered.mid = toByte(kBandScale * applyBallistics(i, Mid, bandRms(i, Mid), dt));
            datum.filtered.high =
                    toByte(kBandScale * applyBallistics(i, High, bandRms(i, High), dt));
            m_loudestBandByte = std::max({m_loudestBandByte,
                    datum.filtered.low,
                    datum.filtered.mid,
                    datum.filtered.high});
            for (int stemIdx = 0; stemIdx < m_stemCount; stemIdx++) {
                datum.stems[stemIdx] = toByte(m_stemData[i][stemIdx]);
            }
        }
        m_averageDivisor++;
        // Reset the stride counters
        for (int i = 0; i < ChannelCount; ++i) {
            m_averageOverallData[i] += m_overallData[i];
            m_overallData[i] = 0.0f;
            for (int f = 0; f < BandCount; ++f) {
                // Accumulate mean squares, not magnitudes: RMS over a group of
                // strides is the root of the mean of their mean squares.
                // Averaging or maxing the magnitudes instead measurably shifts
                // the colour (up to 20 degrees of hue on spectrally lopsided
                // material).
                if (m_bandFrameCount > 0) {
                    m_averageFilteredData[i][f] += m_filteredData[i][f] /
                            static_cast<float>(m_bandFrameCount);
                }
                m_filteredData[i][f] = 0.0f;
            }
            for (int stemIdx = 0; stemIdx < m_stemCount; ++stemIdx) {
                m_stemData[i][stemIdx] = 0.0f;
            }
        }
        if (m_bandFrameCount > 0) {
            m_bandMeanSquareDivisor++;
        }
        m_bandFrameCount = 0;
    }

    /// Root of the mean of the mean squares accumulated by store().
    inline float averageBandRms(int channel, int band) const {
        if (m_bandMeanSquareDivisor <= 0) {
            return 0.0f;
        }
        return std::sqrt(m_averageFilteredData[channel][band] /
                static_cast<float>(m_bandMeanSquareDivisor));
    }

    inline void averageStore(WaveformData* data) {
        if (m_averageDivisor) {
            for (int i = 0; i < ChannelCount; ++i) {
                WaveformData& datum = *(data + i);
                datum.filtered.all = toByte(
                        m_averageOverallData[i] / static_cast<float>(m_averageDivisor));
                datum.filtered.low = toByte(averageBandRms(i, Low));
                datum.filtered.mid = toByte(averageBandRms(i, Mid));
                datum.filtered.high = toByte(averageBandRms(i, High));
            }
        } else {
            // This is the case if The Overview Waveform has more samples than the detailed waveform
            for (int i = 0; i < ChannelCount; ++i) {
                WaveformData& datum = *(data + i);
                datum.filtered.all = toByte(m_overallData[i]);
                datum.filtered.low = toByte(bandRms(i, Low));
                datum.filtered.mid = toByte(bandRms(i, Mid));
                datum.filtered.high = toByte(bandRms(i, High));
            }
        }

        m_averageDivisor = 0;
        m_bandMeanSquareDivisor = 0;
        for (int i = 0; i < ChannelCount; ++i) {
            m_averageOverallData[i] = 0.0f;
            for (int f = 0; f < BandCount; ++f) {
                m_averageFilteredData[i][f] = 0.0f;
            }
        }
    }

    int m_position;
    int m_stemCount;
    double m_length;
    double m_averageLength;
    int m_averagePosition;
    int m_averageDivisor;
    /// Frames accumulated into m_filteredData since the last store().
    int m_bandFrameCount;
    /// Strides accumulated into m_averageFilteredData since averageStore().
    int m_bandMeanSquareDivisor;

    float m_overallData[ChannelCount];
    /// Sum of squares of the filtered signal, not a magnitude. See store().
    float m_filteredData[ChannelCount][BandCount];
    float m_stemData[ChannelCount][mixxx::kMaxSupportedStems];

    float m_averageOverallData[ChannelCount];
    /// Sum of per-stride mean squares. See store()/averageStore().
    float m_averageFilteredData[ChannelCount][BandCount];

    float m_postScaleConversion;
    /// Largest band byte written for this track, see loudestBandByte().
    unsigned char m_loudestBandByte = 0;
    /// Largest height byte written for this track, see loudestHeightByte().
    unsigned char m_loudestHeightByte = 0;
    /// Held value of the per band envelope, see applyBallistics().
    float m_bandEnvelope[ChannelCount][BandCount];
    /// Sample rate the ballistics are measured against, in Hz.
    int m_sampleRate = 0;
};

class AnalyzerWaveform : public Analyzer {
  public:
    AnalyzerWaveform(
            UserSettingsPointer pConfig,
            const QSqlDatabase& dbConnection);
    ~AnalyzerWaveform() override;

    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* buffer, SINT count) override;
    void storeResults(TrackPointer tio) override;
    void cleanup() override;

  private:
    bool shouldAnalyze(TrackPointer tio) const;

    void storeCurrentStridePower();
    void resetCurrentStride();

    void createFilters(mixxx::audio::SampleRate sampleRate);
    void destroyFilters();
    void storeIfGreater(float* pDest, float source);

    mutable AnalysisDao m_analysisDao;

    WaveformPointer m_waveform;
    WaveformPointer m_waveformSummary;
    WaveformData* m_waveformData;
    WaveformData* m_waveformSummaryData;

    WaveformStride m_stride;

    int m_currentStride;
    int m_currentSummaryStride;
    mixxx::audio::ChannelCount m_channelCount;

    struct Filters {
        std::unique_ptr<EngineFilterIIRBase> low;
        std::unique_ptr<EngineFilterIIRBase> mid;
        std::unique_ptr<EngineFilterIIRBase> high;
    };

    Filters m_filters;

    struct Buffers {
        std::vector<float> low;
        std::vector<float> mid;
        std::vector<float> high;

        SINT size;

        Buffers()
                : low(),
                  mid(),
                  high(),
                  size(0) {
        }
    };

    Buffers m_buffers;

    PerformanceTimer m_timer;

#ifdef TEST_HEAT_MAP
    QImage* test_heatMap;
#endif
};
