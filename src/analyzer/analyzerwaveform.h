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
    WaveformStride(double samples, double averageSamples, int stemCount)
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
            SampleUtil::clear(m_filteredData[i], BandCount);
            SampleUtil::clear(m_averageFilteredData[i], BandCount);
            SampleUtil::clear(m_stemData[i], m_stemCount);
        }
    }

    /// Scale a linear magnitude into the single byte the waveform format
    /// stores. The same scale is used for every band on purpose: normalising
    /// bands individually pushes quiet bands into the bottom of the range and
    /// costs a lot of colour accuracy.
    inline unsigned char toByte(float value) const {
        return static_cast<unsigned char>(std::min(255.0,
                static_cast<double>(m_postScaleConversion) *
                                static_cast<double>(value) +
                        0.5));
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

    inline void store(WaveformData* data) {
        for (int i = 0; i < ChannelCount; ++i) {
            WaveformData& datum = *(data + i);
            datum.filtered.all = toByte(m_overallData[i]);
            datum.filtered.low = toByte(bandRms(i, Low));
            datum.filtered.mid = toByte(bandRms(i, Mid));
            datum.filtered.high = toByte(bandRms(i, High));
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
