#pragma once

#include <QStringList>

#include "library/basesqltablemodel.h"
#include "library/similar/similarityclient.h"

/// Track table over the ranking the annotation stand returned for one seed,
/// re-weighted locally by tempo and key.
///
/// The rows live in a temporary table that is refilled on every answer, and a
/// temporary view joins them to the library and applies the weights. That is
/// the shape PlaylistTableModel uses for playlist entries, and it buys the two
/// things that matter here: extra columns that are not library columns (the
/// score and the per-tool ranks), and an order that survives BaseTrackCache -
/// sorting by a table column keeps the SQL order instead of handing it to the
/// track source.
///
/// The weights live in a one-row temporary table the view joins, so moving a
/// slider is one UPDATE plus a select() - no HTTP request, no recomputation of
/// the penalties, which do not depend on the weights.
class SimilarTrackTableModel : public BaseSqlTableModel {
    Q_OBJECT

  public:
    /// Positions of the key slider, left to right, exactly as the user named
    /// them. Each position is a threshold, not a weight: a candidate either
    /// survives it or is not shown. Moving right widens the list, so the
    /// rightmost position is the default and nothing is hidden until asked.
    enum class KeyLevel {
        /// The set Mixxx already calls harmonic: KeyUtils::getCompatibleKeys(),
        /// the relative key and the wheel neighbours - the same set '~key:' uses.
        Harmonic = 0,
        /// One step wider: up to two steps around the Camelot wheel, counting a
        /// major/minor change as a step.
        Wide = 1,
        /// No key filter at all.
        All = 2,
    };

    /// Positions of the tempo slider, left to right. The labels carry the
    /// numbers, so no separate tolerance box is needed.
    enum class BpmLevel {
        OnePointFivePercent = 0,
        ThreePercent = 1,
        FourPointFivePercent = 2,
        SixPercent = 3,
        Any = 4,
    };

    /// What the weights dropped from the list, for the line under the table.
    class HiddenCounts final {
      public:
        int withoutKey = 0;
        int keyTooFar = 0;
        int outsideBpmRange = 0;
        bool isEmpty() const {
            return withoutKey == 0 && keyTooFar == 0 && outsideBpmRange == 0;
        }
    };

    SimilarTrackTableModel(
            QObject* parent, TrackCollectionManager* pTrackCollectionManager);
    ~SimilarTrackTableModel() override = default;

    /// Tempo and key of the seed, taken from the loaded track rather than from
    /// its database row: a track analysed in this session may not have been
    /// written back yet, and a seed that looks unanalysed silently disables
    /// both filters.
    void setSeedAttributes(double bpm, int keyId);

    /// Replace the contents with one answer from the stand. Tracks the local
    /// library no longer has (purged in the meantime) drop out silently.
    void setResult(const mixxx::SimilarityResult& result);
    void clearResult();

    /// Move a slider. Cheap by construction: how far each candidate is from the
    /// seed does not depend on the sliders, so it is worked out once per answer
    /// and only the thresholds change here - one UPDATE and a re-select, no
    /// request and no recomputation.
    void setLevels(BpmLevel bpmLevel, KeyLevel keyLevel);

    /// Tool keys behind the four rank columns, in column order, plus the names
    /// to put in their headers.
    void setRankTools(const QStringList& toolKeys, const QStringList& toolNames);

    HiddenCounts hiddenCounts() const;
    /// False when the seed itself has no key: nothing can be measured against
    /// it, so the key filter is inert and the pane has to say so instead of
    /// looking broken.
    bool seedHasKey() const {
        return m_seedHasKey;
    }
    /// Same for tempo.
    bool seedHasBpm() const {
        return m_seedHasBpm;
    }
    bool scoreIsMeanRank() const {
        return m_result.scoreIsMeanRank;
    }
    int scoreColumn() const;
    int positionColumn() const;

    bool isColumnInternal(int column) final;
    TrackModel::Capabilities getCapabilities() const final;

  protected:
    void initSortColumnMapping() override;

  private:
    void createTables();
    void writeLevels();
    void refill();
    void updateHeaders();
    /// Leftmost (strictest) slider position at which this candidate is still
    /// shown. A row visible at one position stays visible at every looser one,
    /// so the view only has to compare this with where the slider stands.
    int bpmVisibleFrom(double seedBpm, double candidateBpm) const;
    int keyVisibleFrom(int seedKeyId, int candidateKeyId) const;

    mixxx::SimilarityResult m_result;
    double m_seedBpm;
    int m_seedKeyId;
    bool m_seedHasKey;
    bool m_seedHasBpm;
    BpmLevel m_bpmLevel;
    KeyLevel m_keyLevel;
    QStringList m_rankToolKeys;
    QStringList m_rankToolNames;
};
