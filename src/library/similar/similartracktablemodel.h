#pragma once

#include "library/basesqltablemodel.h"
#include "library/similar/similarityclient.h"

/// Track table over the ranking the annotation stand returned for one seed.
///
/// The rows live in a temporary table that is refilled on every answer, and a
/// temporary view joins them to the library. That is the same shape
/// PlaylistTableModel uses for playlist entries, and it buys the two things
/// that matter here: an extra column that is not a library column (the score),
/// and an order that survives BaseTrackCache - sorting by a table column keeps
/// the SQL order instead of handing it to the track source.
class SimilarTrackTableModel : public BaseSqlTableModel {
    Q_OBJECT

  public:
    SimilarTrackTableModel(
            QObject* parent, TrackCollectionManager* pTrackCollectionManager);
    ~SimilarTrackTableModel() override = default;

    /// Replace the contents with one answer from the stand. Tracks the local
    /// library does not have (purged in the meantime) drop out silently.
    void setResult(const mixxx::SimilarityResult& result);
    void clearResult();

    bool isColumnInternal(int column) final;
    TrackModel::Capabilities getCapabilities() const final;

  protected:
    void initSortColumnMapping() override;

  private:
    void createTables();
    void writeRows(const QList<mixxx::SimilarTrack>& tracks);
};
