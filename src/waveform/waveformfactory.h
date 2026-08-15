#pragma once

#include "library/dao/analysisdao.h"

class Waveform;

#define WAVEFORM_2_VERSION "Waveform-2.0"
#define WAVEFORMSUMMARY_2_VERSION "WaveformSummary-2.0"
#define WAVEFORM_2_DESCRIPTION "Waveform 2.0"
#define WAVEFORMSUMMARY_2_DESCRIPTION "WaveformSummary 2.0"

// Used in Mixxx 1.11 beta
#define WAVEFORM_3_VERSION "Waveform-3.0"
#define WAVEFORMSUMMARY_3_VERSION "WaveformSummary-3.0"
#define WAVEFORM_3_DESCRIPTION "Waveform 3.0"
#define WAVEFORMSUMMARY_3_DESCRIPTION "WaveformSummary 3.0"

// Used from Mixxx 1.11 pre
#define WAVEFORM_4_VERSION "Waveform-4.0"
#define WAVEFORMSUMMARY_4_VERSION "WaveformSummary-4.0"
#define WAVEFORM_4_DESCRIPTION "Waveform 4.0"
#define WAVEFORMSUMMARY_4_DESCRIPTION "WaveformSummary 4.0"

// Used from Mixxx 1.12 alpha
#define WAVEFORM_5_VERSION "Waveform-5.0"
#define WAVEFORMSUMMARY_5_VERSION "WaveformSummary-5.0"
#define WAVEFORM_5_DESCRIPTION "Waveform 5.0"
#define WAVEFORMSUMMARY_5_DESCRIPTION "WaveformSummary 5.0"

#ifdef __STEM__
// Used from Mixxx 2.6-pre-alpha with Stem data (6.0) and without
// analyzer/analyzerwaveform.h:scaleSignal (6.1)
#define WAVEFORM_6_0_VERSION "Waveform-6.0" // Superseded by 6.1
#define WAVEFORM_6_VERSION "Waveform-6.1"
#define WAVEFORMSUMMARY_6_VERSION "WaveformSummary-6.1"
#define WAVEFORM_6_DESCRIPTION "Waveform 6.1"
#define WAVEFORMSUMMARY_6_DESCRIPTION "WaveformSummary 6.1"

#endif

// Used from Mixxx 2.6: the three colour bands are no longer Bessel crossovers
// but the first order responses measured from Traktor, and the stored band
// value is the RMS of the filtered signal inside the bin instead of its peak.
// The byte layout is unchanged but the meaning of the bytes is not, so every
// track has to be analysed again. 7.1 is the variant that carries stem data.
// Both the detailed waveform and the overview change, hence both versions are
// bumped: they are versioned separately and bumping only one of them would
// leave the library overview showing the old colours forever.
#define WAVEFORM_7_VERSION "Waveform-7.0"
#define WAVEFORM_7_DESCRIPTION "Waveform 7.0"
#define WAVEFORM_7_STEM_VERSION "Waveform-7.1"
#define WAVEFORM_7_STEM_DESCRIPTION "Waveform 7.1"
#define WAVEFORMSUMMARY_7_VERSION "WaveformSummary-7.0"
#define WAVEFORMSUMMARY_7_DESCRIPTION "WaveformSummary 7.0"

#ifdef __STEM__
#define WAVEFORM_CURRENT_VERSION WAVEFORM_7_STEM_VERSION
#define WAVEFORM_CURRENT_DESCRIPTION WAVEFORM_7_STEM_DESCRIPTION
#else
#define WAVEFORM_CURRENT_VERSION WAVEFORM_7_VERSION
#define WAVEFORM_CURRENT_DESCRIPTION WAVEFORM_7_DESCRIPTION
#endif
#define WAVEFORMSUMMARY_CURRENT_VERSION WAVEFORMSUMMARY_7_VERSION
#define WAVEFORMSUMMARY_CURRENT_DESCRIPTION WAVEFORMSUMMARY_7_DESCRIPTION

class WaveformFactory {
  public:
    enum VersionClass {
        VC_USE,
        VC_KEEP,
        VC_REMOVE
    };

    static Waveform* loadWaveformFromAnalysis(
            const AnalysisDao::AnalysisInfo& analysis);
    static VersionClass waveformVersionToVersionClass(const QString& version);
    static VersionClass waveformSummaryVersionToVersionClass(const QString& version);
    static QString currentWaveformVersion();
    static QString currentWaveformDescription();
    static QString currentWaveformSummaryVersion();
    static QString currentWaveformSummaryDescription();
};
