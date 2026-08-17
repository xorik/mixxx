#include "library/similar/similartracktablemodel.h"

#include <QSqlQuery>

#include "library/dao/trackschema.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_similartracktablemodel.cpp"
#include "util/db/sqltransaction.h"

namespace {

constexpr char kModelSettingsNamespace[] = "mixxx.db.model.similar";

// Distinct names, so that neither an upstream view nor another feature can
// shadow them in the same connection.
const QString kScoreTableName = QStringLiteral("mixxx_similar_scores");
const QString kViewName = QStringLiteral("mixxx_similar_view");

} // anonymous namespace

SimilarTrackTableModel::SimilarTrackTableModel(
        QObject* parent, TrackCollectionManager* pTrackCollectionManager)
        : BaseSqlTableModel(parent,
                  pTrackCollectionManager,
                  kModelSettingsNamespace) {
    createTables();

    QStringList columns;
    columns << LIBRARYTABLE_ID
            << PLAYLISTTRACKSTABLE_POSITION
            << SIMILARTABLE_SCORE
            << LIBRARYTABLE_PREVIEW
            << LIBRARYTABLE_COVERART;
    setTable(kViewName,
            LIBRARYTABLE_ID,
            columns,
            pTrackCollectionManager->internalCollection()->getTrackSource());
    setSearch(QString());
    // Rank order, always: it is the one column that means the same thing for a
    // cosine provider and for the rank-averaging ensemble.
    setDefaultSort(fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION),
            Qt::AscendingOrder);
    setSort(defaultSortColumn(), defaultSortOrder());
}

void SimilarTrackTableModel::createTables() {
    QSqlQuery query(m_database);

    // TEMPORARY: the table belongs to this connection and disappears with it,
    // so nothing of ours is ever written to mixxxdb.sqlite.
    query.prepare(QStringLiteral(
            "CREATE TEMPORARY TABLE IF NOT EXISTS %1 ("
            "  %2 INTEGER PRIMARY KEY,"
            "  %3 INTEGER,"
            "  %4 REAL)")
                          .arg(kScoreTableName,
                                  LIBRARYTABLE_ID,
                                  PLAYLISTTRACKSTABLE_POSITION,
                                  SIMILARTABLE_SCORE));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }

    query.prepare(QStringLiteral(
            "CREATE TEMPORARY VIEW IF NOT EXISTS %1 AS SELECT "
            "  scores.%2 AS %2,"
            "  scores.%3 AS %3,"
            "  scores.%4 AS %4,"
            "  '' AS %5,"
            // The cover art column sorts by the digest, as everywhere else.
            "  library.%6 AS %7 "
            "FROM %8 AS scores "
            "INNER JOIN library ON library.id = scores.%2 "
            "INNER JOIN track_locations ON library.location = track_locations.id "
            "WHERE library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0")
                          .arg(kViewName,
                                  LIBRARYTABLE_ID,
                                  PLAYLISTTRACKSTABLE_POSITION,
                                  SIMILARTABLE_SCORE,
                                  LIBRARYTABLE_PREVIEW,
                                  LIBRARYTABLE_COVERART_DIGEST,
                                  LIBRARYTABLE_COVERART,
                                  kScoreTableName));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

void SimilarTrackTableModel::writeRows(const QList<mixxx::SimilarTrack>& tracks) {
    SqlTransaction transaction(m_database);

    QSqlQuery deleteQuery(m_database);
    deleteQuery.prepare(QStringLiteral("DELETE FROM %1").arg(kScoreTableName));
    if (!deleteQuery.exec()) {
        LOG_FAILED_QUERY(deleteQuery);
        return;
    }

    if (!tracks.isEmpty()) {
        QSqlQuery insertQuery(m_database);
        insertQuery.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO %1 (%2, %3, %4) VALUES (:id, :position, :score)")
                                    .arg(kScoreTableName,
                                            LIBRARYTABLE_ID,
                                            PLAYLISTTRACKSTABLE_POSITION,
                                            SIMILARTABLE_SCORE));
        for (const auto& track : tracks) {
            insertQuery.bindValue(QStringLiteral(":id"), track.trackId.toVariant());
            insertQuery.bindValue(QStringLiteral(":position"), track.position);
            insertQuery.bindValue(QStringLiteral(":score"), track.score);
            if (!insertQuery.exec()) {
                LOG_FAILED_QUERY(insertQuery);
                return;
            }
        }
    }

    transaction.commit();
}

void SimilarTrackTableModel::setResult(const mixxx::SimilarityResult& result) {
    // The same column holds a cosine per cent for the embedding providers and a
    // mean rank for the ensemble - opposite directions, so the header has to
    // say which one is on screen.
    const int scoreColumn = fieldIndex(ColumnCache::COLUMN_SIMILARTABLE_SCORE);
    if (scoreColumn >= 0) {
        setHeaderData(scoreColumn,
                Qt::Horizontal,
                result.scoreIsMeanRank ? tr("Mean rank") : tr("Similarity, %"),
                Qt::DisplayRole);
        setHeaderData(scoreColumn,
                Qt::Horizontal,
                result.scoreIsMeanRank
                        ? tr("Average rank over all providers, lower is closer")
                        : tr("Cosine similarity in per cent, the same number the "
                             "stand shows. The order comes from the centred cosine, "
                             "so this column is not always monotonic."),
                Qt::ToolTipRole);
    }
    writeRows(result.tracks);
    select();
}

void SimilarTrackTableModel::clearResult() {
    writeRows({});
    select();
}

void SimilarTrackTableModel::initSortColumnMapping() {
    // Same shape as PlaylistTableModel: the base class maps library columns
    // only, and WTrackTableView resolves its default sort through SortColumnId.
    // Without Position mapped here the view finds no valid sort id, falls back
    // to the first sortable column and quietly replaces the ranking with an
    // alphabetical list.
    BaseSqlTableModel::initSortColumnMapping();

    m_columnIndexBySortColumnId[static_cast<int>(TrackModel::SortColumnId::Position)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(TrackModel::SortColumnId::Similarity)] =
            fieldIndex(ColumnCache::COLUMN_SIMILARTABLE_SCORE);

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        const auto sortColumn = static_cast<TrackModel::SortColumnId>(i);
        m_sortColumnIdByColumnIndex.insert(
                m_columnIndexBySortColumnId[static_cast<int>(sortColumn)],
                sortColumn);
    }
}

bool SimilarTrackTableModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_URL) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CUEPOINT) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_MIXXXDELETED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_HEADERPARSED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BEATS_VERSION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_DIRECTORY) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_SOURCE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_TYPE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_LOCATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_COLOR) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_DIGEST) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_HASH);
}

TrackModel::Capabilities SimilarTrackTableModel::getCapabilities() const {
    // No Hide, no RemoveFromDisk, no EditMetadata: this list is a view on an
    // answer, not a place to manage the library from.
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::LoadToDeck |
            Capability::LoadToSampler |
            Capability::LoadToPreviewDeck |
            Capability::Analyze |
            Capability::Properties |
            Capability::Sorting;
}
