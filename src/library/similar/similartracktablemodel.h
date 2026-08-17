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
    /// How the distance between two keys is turned into a penalty.
    enum class KeyMatchMode {
        /// Steps around the Camelot wheel, plus one for a major/minor change,
        /// scaled so that the opposite side of the wheel is a full penalty.
        CamelotDistance,
        /// Free inside KeyUtils::getCompatibleKeys() (the wheel neighbours and
        /// the relative key), full penalty outside.
        Harmonic,
        /// Free for the same key only.
        Exact,
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

    /// Replace the contents with one answer from the stand. Tracks the local
    /// library no longer has (purged in the meantime) drop out silently.
    void setResult(const mixxx::SimilarityResult& result);
    void clearResult();

    /// 0 ignores the factor, 1 turns it into a hard filter, in between the
    /// score is scaled down. Cheap: no request, no penalty recomputation.
    void setWeights(double bpmWeight, double keyWeight);
    /// Tempo tolerance in per cent, the range over which the tempo penalty
    /// grows from none to full.
    void setBpmRangePercent(double percent);
    void setKeyMatchMode(KeyMatchMode mode);

    /// Tool keys behind the four rank columns, in column order, plus the names
    /// to put in their headers.
    void setRankTools(const QStringList& toolKeys, const QStringList& toolNames);

    HiddenCounts hiddenCounts() const;
    bool scoreIsMeanRank() const {
        return m_result.scoreIsMeanRank;
    }
    /// True while at least one weight is active, which is when the list is
    /// ordered by the weighted score instead of the order the stand sent.
    bool isWeighted() const {
        return m_bpmWeight > 0.0 || m_keyWeight > 0.0;
    }
    int scoreColumn() const;
    int positionColumn() const;

    bool isColumnInternal(int column) final;
    TrackModel::Capabilities getCapabilities() const final;

  protected:
    void initSortColumnMapping() override;

  private:
    void createTables();
    void writeWeights();
    void refill();
    void updateHeaders();
    double bpmPenalty(double seedBpm, double candidateBpm) const;
    double keyPenalty(int seedKeyId, int candidateKeyId) const;

    mixxx::SimilarityResult m_result;
    double m_bpmWeight;
    double m_keyWeight;
    double m_bpmRangePercent;
    KeyMatchMode m_keyMatchMode;
    QStringList m_rankToolKeys;
    QStringList m_rankToolNames;
};
