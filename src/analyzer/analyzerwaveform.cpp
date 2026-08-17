#include "analyzer/analyzerwaveform.h"

#include <memory>
#include <vector>

#include "analyzer/analyzertrack.h"
#include "analyzer/constants.h"
#include "engine/filters/enginefilterwaveform.h"
#include "track/track.h"
#include "util/logger.h"
#include "waveform/waveform.h"
#include "waveform/waveformfactory.h"

namespace {

mixxx::Logger kLogger("AnalyzerWaveform");

} // namespace

AnalyzerWaveform::AnalyzerWaveform(
        UserSettingsPointer pConfig,
        const QSqlDatabase& dbConnection)
        : m_analysisDao(pConfig),
          m_waveformData(nullptr),
          m_waveformSummaryData(nullptr),
          m_stride(0, 0, 0),
          m_currentStride(0),
          m_currentSummaryStride(0) {
    m_analysisDao.initialize(dbConnection);
}

AnalyzerWaveform::~AnalyzerWaveform() {
    kLogger.debug() << "~AnalyzerWaveform():";
    destroyFilters();
}

bool AnalyzerWaveform::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        mixxx::audio::ChannelCount channelCount,
        SINT frameLength) {
    if (frameLength <= 0) {
        qWarning() << "AnalyzerWaveform::initialize - no waveform/waveform summary";
        return false;
    }

    // If we don't need to calculate the waveform/wavesummary, skip.
    if (!shouldAnalyze(track.getTrack())) {
        return false;
    }

    m_timer.start();

    // Now actually initialize the AnalyzerWaveform:
    destroyFilters();
    createFilters(sampleRate);

    //TODO (vrince) Do we want to expose this as settings or whatever ?
    // Values per second stored for the deck waveform, per channel.
    //
    // 441 was not enough for the shape of a column to come out of the data. The
    // renderer works out the alpha of a pixel from how many of its sub columns
    // reach a given height, and at zoom 3 on a retina screen 441 values per
    // second put 1.5 of them in a pixel: eight sub columns then read two
    // distinct values, the transition happens in one step, and a drum hit is
    // drawn as a block. At 1764 there are six values in a pixel and the fall
    // from the tip of the column appears by itself, from the data, without any
    // curve applied on top.
    //
    // The colour does not need this - measured, the density contributes 0.2
    // degrees of hue against a model error of 16 - but the four channels share
    // one packed record, and separating them would mean a second texture and a
    // second grid through a hundred call sites. Four times the data on all four
    // channels costs 1.8 MB more per seven-minute track, which is the cheaper
    // side of that trade by a wide margin.
    //
    // NOTE: the user visible zoom scale is multiplied by the same factor, in
    // WaveformWidgetRenderer, so that a given zoom number keeps showing the
    // same stretch of time. The two constants belong together.
    constexpr int mainWaveformSampleRate = 1764;
    // two visual sample per pixel in full width overview in full hd
    constexpr int summaryWaveformSamples = 2 * 1920;

    int stemCount = channelCount == mixxx::kAnalysisChannels
            ? 0
            : channelCount / mixxx::kAnalysisChannels;
    m_waveform = WaveformPointer(new Waveform(
            sampleRate, frameLength, mainWaveformSampleRate, -1, stemCount));
    m_waveformSummary = WaveformPointer(new Waveform(
            sampleRate, frameLength, mainWaveformSampleRate, summaryWaveformSamples, stemCount));

    // Now, that the Waveform memory is initialized, we can set set them to
    // the track. Be aware that other threads of Mixxx can touch them from
    // now.
    track.getTrack()->setWaveform(m_waveform);
    track.getTrack()->setWaveformSummary(m_waveformSummary);

    m_waveformData = m_waveform->data();
    m_waveformSummaryData = m_waveformSummary->data();

    m_stride = WaveformStride(m_waveform->getAudioVisualRatio(),
            m_waveformSummary->getAudioVisualRatio(),
            stemCount,
            sampleRate);

    m_currentStride = 0;
    m_currentSummaryStride = 0;
    m_channelCount = channelCount;

    //debug
    //m_waveform->dump();
    //m_waveformSummary->dump();

#ifdef TEST_HEAT_MAP
    test_heatMap = new QImage(256, 256, QImage::Format_RGB32);
    test_heatMap->fill(0xFFFFFFFF);
#endif
    return true;
}

bool AnalyzerWaveform::shouldAnalyze(TrackPointer pTrack) const {
    ConstWaveformPointer pTrackWaveform = pTrack->getWaveform();
    ConstWaveformPointer pTrackWaveformSummary = pTrack->getWaveformSummary();
    ConstWaveformPointer pLoadedTrackWaveform;
    ConstWaveformPointer pLoadedTrackWaveformSummary;
#ifdef __STEM__
    bool isStemTrack = !pTrack->getStemInfo().isEmpty();
#endif

    TrackId trackId = pTrack->getId();
    bool missingWaveform = pTrackWaveform.isNull();
    bool missingWavesummary = pTrackWaveformSummary.isNull();

    if (trackId.isValid() && (missingWaveform || missingWavesummary)) {
        QList<AnalysisDao::AnalysisInfo> analyses =
                m_analysisDao.getAnalysesForTrack(trackId);

        QListIterator<AnalysisDao::AnalysisInfo> it(analyses);
        while (it.hasNext()) {
            const AnalysisDao::AnalysisInfo& analysis = it.next();
            WaveformFactory::VersionClass vc;

            if (analysis.type == AnalysisDao::TYPE_WAVEFORM) {
                vc = WaveformFactory::waveformVersionToVersionClass(analysis.version);
                if (missingWaveform && vc == WaveformFactory::VC_USE) {
                    pLoadedTrackWaveform = ConstWaveformPointer(
                            WaveformFactory::loadWaveformFromAnalysis(analysis));
                    missingWaveform = false;
                } else if (vc != WaveformFactory::VC_KEEP) {
                    // remove all other Analysis except that one we should keep
                    m_analysisDao.deleteAnalysis(analysis.analysisId);
                }
            }
            if (analysis.type == AnalysisDao::TYPE_WAVESUMMARY) {
                vc = WaveformFactory::waveformSummaryVersionToVersionClass(analysis.version);
                if (missingWavesummary && vc == WaveformFactory::VC_USE) {
                    pLoadedTrackWaveformSummary = ConstWaveformPointer(
                            WaveformFactory::loadWaveformFromAnalysis(analysis));
                    missingWavesummary = false;
                } else if (vc != WaveformFactory::VC_KEEP) {
                    // remove all other Analysis except that one we should keep
                    m_analysisDao.deleteAnalysis(analysis.analysisId);
                }
            }
        }
    }

#ifdef __STEM__
    // If the waveform was generated without stem information but the track has
    // some, we need to regenerate the waveform.
    const bool waveformHasStemData = (!pTrackWaveform.isNull() &&
                                             pTrackWaveform->hasStem()) ||
            (!pLoadedTrackWaveform.isNull() &&
                    pLoadedTrackWaveform->hasStem());
    if (!missingWaveform && !waveformHasStemData && isStemTrack) {
        missingWaveform = true;
    }
#endif

    // If we don't need to calculate the waveform/wavesummary, skip.
    if (!missingWaveform && !missingWavesummary) {
        kLogger.debug() << "loadStored - Stored waveform loaded";
        if (pLoadedTrackWaveform) {
            pTrack->setWaveform(pLoadedTrackWaveform);
        }
        if (pLoadedTrackWaveformSummary) {
            pTrack->setWaveformSummary(pLoadedTrackWaveformSummary);
        }
        return false;
    }
    return true;
}

void AnalyzerWaveform::createFilters(mixxx::audio::SampleRate sampleRate) {
    // These are deliberately not crossover filters: they are three heavily
    // overlapping first order responses measured from Traktor, see
    // engine/filters/enginefilterwaveform.h. Each one peaks at unit gain, the
    // relative weighting of the bands happens in the renderer.
    m_filters = {
            std::make_unique<EngineFilterWaveformLow>(sampleRate),
            std::make_unique<EngineFilterWaveformMid>(sampleRate),
            std::make_unique<EngineFilterWaveformHigh>(sampleRate)};

    // settle filters for silence in preroll to avoids ramping (Issue #7776)
    m_filters.low->assumeSettled();
    m_filters.mid->assumeSettled();
    m_filters.high->assumeSettled();
}

void AnalyzerWaveform::destroyFilters() {
    m_filters = {};
}

bool AnalyzerWaveform::processSamples(const CSAMPLE* pIn, SINT count) {
    VERIFY_OR_DEBUG_ASSERT(m_waveform) {
        return false;
    }
    VERIFY_OR_DEBUG_ASSERT(m_waveformSummary) {
        return false;
    }

    SINT numFrames = count / m_channelCount;
    count = numFrames * mixxx::audio::ChannelCount::stereo();
    int stemCount = 0;

    const CSAMPLE* pWaveformInput = pIn;
    CSAMPLE* pMixedChannel = nullptr;

    if (m_channelCount > mixxx::audio::ChannelCount::stereo()) {
        DEBUG_ASSERT(0 == m_channelCount % mixxx::audio::ChannelCount::stereo());

        pMixedChannel = SampleUtil::alloc(count);
        VERIFY_OR_DEBUG_ASSERT(pMixedChannel) {
            return false;
        }
        SampleUtil::mixMultichannelToStereo(pMixedChannel, pIn, numFrames, m_channelCount);
        stemCount = m_channelCount / mixxx::audio::ChannelCount::stereo();
        pWaveformInput = pMixedChannel;
    }

    // This should only append once if count is constant
    if (count > m_buffers.size) {
        m_buffers.low.resize(count);
        m_buffers.mid.resize(count);
        m_buffers.high.resize(count);
        m_buffers.size = count;
    }

    m_filters.low->process(pWaveformInput, &m_buffers.low[0], count);
    m_filters.mid->process(pWaveformInput, &m_buffers.mid[0], count);
    m_filters.high->process(pWaveformInput, &m_buffers.high[0], count);

    m_waveform->setSaveState(Waveform::SaveState::NotSaved);
    m_waveformSummary->setSaveState(Waveform::SaveState::NotSaved);

    for (SINT i = 0; i < count; i += 2) {
        // The height of a column is the span of the signal inside it, so both
        // ends of that span are tracked, with their sign. Taking the magnitude
        // here instead would fold the two together and lose the asymmetry that
        // makes a drum hit look like a hit.
        for (int channel = 0; channel < 2; ++channel) {
            const CSAMPLE sample = pWaveformInput[i + channel];
            m_stride.m_minData[channel] = math_min(m_stride.m_minData[channel], sample);
            m_stride.m_maxData[channel] = math_max(m_stride.m_maxData[channel], sample);
        }

        // The band magnitudes that drive the colour are the RMS of the
        // filtered signal inside the bin, so accumulate energy here and take
        // the root in WaveformStride::store(). This is a different quantity
        // from the envelope above on purpose: Traktor's height is linear in the
        // peak while its colour comes from band energy.
        const CSAMPLE flow[2] = {m_buffers.low[i], m_buffers.low[i + 1]};
        const CSAMPLE fmid[2] = {m_buffers.mid[i], m_buffers.mid[i + 1]};
        const CSAMPLE fhigh[2] = {m_buffers.high[i], m_buffers.high[i + 1]};
        m_stride.m_filteredData[Left][Low] += flow[Left] * flow[Left];
        m_stride.m_filteredData[Right][Low] += flow[Right] * flow[Right];
        m_stride.m_filteredData[Left][Mid] += fmid[Left] * fmid[Left];
        m_stride.m_filteredData[Right][Mid] += fmid[Right] * fmid[Right];
        m_stride.m_filteredData[Left][High] += fhigh[Left] * fhigh[Left];
        m_stride.m_filteredData[Right][High] += fhigh[Right] * fhigh[Right];
        m_stride.m_bandFrameCount++;

        for (int s = 0; s < stemCount; s++) {
            CSAMPLE cstem[2] = {
                    fabs(pIn[i * stemCount + s * mixxx::kAnalysisChannels]),
                    fabs(pIn[i * stemCount + s * mixxx::kAnalysisChannels +
                            1])};
            storeIfGreater(&m_stride.m_stemData[Left][s], cstem[Left]);
            storeIfGreater(&m_stride.m_stemData[Right][s], cstem[Right]);
        }

        m_stride.m_position++;

        if (fmod(m_stride.m_position, m_stride.m_length) < 1) {
            VERIFY_OR_DEBUG_ASSERT(m_currentStride + ChannelCount <= m_waveform->getDataSize()) {
                qWarning() << "AnalyzerWaveform::process - currentStride > waveform size";
                return false;
            }
            m_stride.store(m_waveformData + m_currentStride);
            m_currentStride += ChannelCount;
            m_waveform->setCompletion(m_currentStride);
        }

        if (fmod(m_stride.m_position, m_stride.m_averageLength) < 1) {
            VERIFY_OR_DEBUG_ASSERT(m_currentSummaryStride + ChannelCount <= m_waveformSummary->getDataSize()) {
                qWarning() << "AnalyzerWaveform::process - current summary stride > waveform summary size";
                return false;
            }
            m_stride.averageStore(m_waveformSummaryData + m_currentSummaryStride);
            m_currentSummaryStride += ChannelCount;
            m_waveformSummary->setCompletion(m_currentSummaryStride);

#ifdef TEST_HEAT_MAP
            QPointF point(m_stride.m_filteredData[Right][High],
                    m_stride.m_filteredData[Right][Mid]);

            float norm = sqrt(point.x() * point.x() + point.y() * point.y());
            point /= norm;

            point *= m_stride.m_filteredData[Right][Low];
            test_heatMap->setPixel(point.toPoint(), 0xFF0000FF);
#endif
        }
    }

    //kLogger.debug() << "process - m_waveform->getCompletion()" << m_waveform->getCompletion() << "off" << m_waveform->getDataSize();
    //kLogger.debug() << "process - m_waveformSummary->getCompletion()" << m_waveformSummary->getCompletion() << "off" << m_waveformSummary->getDataSize();
    if (pMixedChannel) {
        SampleUtil::free(pMixedChannel);
    }
    return true;
}

void AnalyzerWaveform::cleanup() {
    m_waveform.clear();
    m_waveformData = nullptr;
    m_waveformSummary.clear();
    m_waveformSummaryData = nullptr;
}

void AnalyzerWaveform::storeResults(TrackPointer pTrack) {
    // Same idea for the height: with the headroom a normal track lands in the
    // upper half of the range, so a quarter of it means either very quiet
    // material or a scale that no longer matches.
    constexpr unsigned char kExpectedLoudestHeight = 64;
    if (m_stride.loudestHeightByte() < kExpectedLoudestHeight) {
        qWarning() << "AnalyzerWaveform: the tallest column of" << pTrack->getLocation()
                   << "reached only" << static_cast<int>(m_stride.loudestHeightByte())
                   << "of 255. Either the track is very quiet, or the height headroom no longer "
                      "matches the material.";
    }

    // A track whose loudest band never reaches half the byte it is stored in is
    // not a quiet track, it is a mis-scaled one: the colour then lives in the
    // bottom bits and its quiet half disappears under the level floor of the
    // renderer. That is how an empty looking waveform is produced without a
    // single error anywhere, so it is said out loud.
    constexpr unsigned char kExpectedLoudestBand = 128;
    if (m_stride.loudestBandByte() < kExpectedLoudestBand) {
        qWarning() << "AnalyzerWaveform: the loudest colour band of"
                   << pTrack->getLocation() << "reached only"
                   << static_cast<int>(m_stride.loudestBandByte())
                   << "of 255. The colour is being stored in the bottom bits, which usually "
                      "means the band scale no longer matches the band filters.";
    }

    // Force completion to waveform size
    if (m_waveform) {
        m_waveform->setSaveState(Waveform::SaveState::SavePending);
        m_waveform->setCompletion(m_waveform->getDataSize());
        m_waveform->setVersion(WaveformFactory::currentWaveformVersion());
        m_waveform->setDescription(WaveformFactory::currentWaveformDescription());
    }

    // Force completion to waveform size
    if (m_waveformSummary) {
        m_waveformSummary->setSaveState(Waveform::SaveState::SavePending);
        m_waveformSummary->setCompletion(m_waveformSummary->getDataSize());
        m_waveformSummary->setVersion(WaveformFactory::currentWaveformSummaryVersion());
        m_waveformSummary->setDescription(WaveformFactory::currentWaveformSummaryDescription());
    }

#ifdef TEST_HEAT_MAP
    test_heatMap->save("heatMap.png");
#endif
    // Ensure that the analyses get saved. This is also called from
    // TrackDAO.updateTrack(), but it can happen that we analyze only the
    // waveforms (i.e. if the config setting was disabled in a previous scan)
    // and then it is not called. The other analyzers have signals which control
    // the update of their data.
    m_analysisDao.saveTrackAnalyses(
            pTrack->getId(),
            m_waveform,
            m_waveformSummary);

    // Set waveforms on track AFTER they'been written to disk in order to have
    // a consistency when OverviewCache asks AnalysisDAO for a waveform summary.
    pTrack->setWaveform(m_waveform);
    pTrack->setWaveformSummary(m_waveformSummary);

    kLogger.debug() << "Waveform generation for track" << pTrack->getId() << "done"
                    << m_timer.elapsed().debugSecondsWithUnit();
}

void AnalyzerWaveform::storeIfGreater(float* pDest, float source) {
    if (*pDest < source) {
        *pDest = source;
    }
}
