#include "library/similar/similartracktablemodel.h"

#include <QSqlQuery>
#include <QSqlRecord>
#include <algorithm>
#include <cmath>

#include "library/dao/trackschema.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_similartracktablemodel.cpp"
#include "track/keyutils.h"
#include "util/db/sqltransaction.h"

namespace {

constexpr char kModelSettingsNamespace[] = "mixxx.db.model.similar";

// Distinct names, so that neither an upstream view nor another feature can
// shadow them in the same connection.
const QString kScoreTableName = QStringLiteral("mixxx_similar_scores");
const QString kWeightTableName = QStringLiteral("mixxx_similar_weights");
const QString kViewName = QStringLiteral("mixxx_similar_view");

// Internal columns of the score table, not exposed to the view.
const QString kRawScore = QStringLiteral("raw_score");
// The strictest slider position at which each candidate is still shown.
// Computed once per answer: it depends on the seed and the candidate, not on
// where the sliders stand.
const QString kBpmLevelReached = QStringLiteral("bpm_visible_from");
const QString kKeyLevelReached = QStringLiteral("key_visible_from");
const QString kNoKey = QStringLiteral("no_key");

constexpr int kRankColumnCount = 4;

/// Tempo tolerance of each slider position, in per cent, left to right. The
/// last position is "any" and filters nothing.
constexpr double kBpmTolerancePercent[] = {1.5, 3.0, 4.5, 6.0};
constexpr int kBpmAnyLevel = static_cast<int>(std::size(kBpmTolerancePercent));

} // anonymous namespace

SimilarTrackTableModel::SimilarTrackTableModel(
        QObject* parent, TrackCollectionManager* pTrackCollectionManager)
        : BaseSqlTableModel(parent, pTrackCollectionManager, kModelSettingsNamespace),
          m_bpmLevel(BpmLevel::Any),
          m_keyLevel(KeyLevel::All) {
    createTables();
    writeLevels();

    QStringList columns;
    columns << LIBRARYTABLE_ID
            << PLAYLISTTRACKSTABLE_POSITION
            << SIMILARTABLE_SCORE
            << SIMILARTABLE_RANK1
            << SIMILARTABLE_RANK2
            << SIMILARTABLE_RANK3
            << SIMILARTABLE_RANK4
            << LIBRARYTABLE_PREVIEW
            << LIBRARYTABLE_COVERART;
    setTable(kViewName,
            LIBRARYTABLE_ID,
            columns,
            pTrackCollectionManager->internalCollection()->getTrackSource());
    setSearch(QString());
    // Always the order the stand sent: the sliders only take rows away, they
    // never re-rank. That also keeps the list identical to the dashboard, which
    // ranks by the centred cosine while the column shows the plain one.
    setDefaultSort(positionColumn(), Qt::AscendingOrder);
    setSort(defaultSortColumn(), defaultSortOrder());
}

int SimilarTrackTableModel::scoreColumn() const {
    return fieldIndex(ColumnCache::COLUMN_SIMILARTABLE_SCORE);
}

int SimilarTrackTableModel::positionColumn() const {
    return fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
}

void SimilarTrackTableModel::createTables() {
    QSqlQuery query(m_database);

    // TEMPORARY: these belong to this connection and disappear with it, so
    // nothing of ours is ever written to mixxxdb.sqlite.
    query.prepare(QStringLiteral(
            "CREATE TEMPORARY TABLE IF NOT EXISTS %1 ("
            "  %2 INTEGER PRIMARY KEY,"
            "  %3 INTEGER,"
            "  %4 REAL,"
            "  %5 INTEGER,"
            "  %6 INTEGER,"
            "  %7 INTEGER,"
            "  %8 INTEGER, %9 INTEGER, %10 INTEGER, %11 INTEGER)")
                          .arg(kScoreTableName,
                                  LIBRARYTABLE_ID,
                                  PLAYLISTTRACKSTABLE_POSITION,
                                  kRawScore,
                                  kBpmLevelReached,
                                  kKeyLevelReached,
                                  kNoKey,
                                  SIMILARTABLE_RANK1,
                                  SIMILARTABLE_RANK2,
                                  SIMILARTABLE_RANK3,
                                  SIMILARTABLE_RANK4));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }

    // One row, joined by the view: moving a slider is an UPDATE, not a rebuild.
    query.prepare(QStringLiteral(
            "CREATE TEMPORARY TABLE IF NOT EXISTS %1 (bpm_level INTEGER, key_level INTEGER)")
                          .arg(kWeightTableName));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }
    query.prepare(QStringLiteral("INSERT INTO %1 (bpm_level, key_level) "
                                 "SELECT 0, 0 WHERE NOT EXISTS (SELECT 1 FROM %1)")
                          .arg(kWeightTableName));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }

    // The sliders are thresholds, not weights: a candidate carries the
    // strictest level it still passes, and the view keeps the rows that reach
    // the level the slider is on. The score is left exactly as the stand sent
    // it, so nothing on screen is a number we invented.
    query.prepare(QStringLiteral(
            "CREATE TEMPORARY VIEW IF NOT EXISTS %1 AS SELECT "
            "  scores.%2 AS %2,"
            "  scores.%3 AS %3,"
            "  scores.%4 AS %7,"
            "  scores.%8 AS %8, scores.%9 AS %9, scores.%10 AS %10, scores.%11 AS %11,"
            "  '' AS %12,"
            // The cover art column sorts by the digest, as everywhere else.
            "  library.%13 AS %14 "
            "FROM %15 AS scores "
            "CROSS JOIN %16 AS w "
            "INNER JOIN library ON library.id = scores.%2 "
            "INNER JOIN track_locations ON library.location = track_locations.id "
            "WHERE library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0 "
            "  AND scores.%5 <= w.bpm_level "
            "  AND scores.%6 <= w.key_level")
                          .arg(kViewName,
                                  LIBRARYTABLE_ID,
                                  PLAYLISTTRACKSTABLE_POSITION,
                                  kRawScore,
                                  kBpmLevelReached,
                                  kKeyLevelReached,
                                  SIMILARTABLE_SCORE,
                                  SIMILARTABLE_RANK1,
                                  SIMILARTABLE_RANK2,
                                  SIMILARTABLE_RANK3,
                                  SIMILARTABLE_RANK4,
                                  LIBRARYTABLE_PREVIEW,
                                  LIBRARYTABLE_COVERART_DIGEST,
                                  LIBRARYTABLE_COVERART,
                                  kScoreTableName,
                                  kWeightTableName));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

void SimilarTrackTableModel::writeLevels() {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE %1 SET bpm_level = :bpm, key_level = :key")
                          .arg(kWeightTableName));
    query.bindValue(QStringLiteral(":bpm"), static_cast<int>(m_bpmLevel));
    query.bindValue(QStringLiteral(":key"), static_cast<int>(m_keyLevel));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

int SimilarTrackTableModel::bpmVisibleFrom(double seedBpm, double candidateBpm) const {
    if (seedBpm <= 0.0) {
        // Nothing to compare against: show it everywhere rather than emptying
        // the list because the seed was never analysed.
        return 0;
    }
    if (candidateBpm <= 0.0) {
        // An unanalysed tempo only survives "any".
        return kBpmAnyLevel;
    }
    // No half/double folding on purpose: this library keeps drum & bass at half
    // tempo throughout, so 85 and 170 are different tempos here, not the same one.
    const double differencePercent =
            std::abs(candidateBpm - seedBpm) / seedBpm * 100.0;
    for (int level = 0; level < kBpmAnyLevel; ++level) {
        if (differencePercent <= kBpmTolerancePercent[level]) {
            return level;
        }
    }
    return kBpmAnyLevel;
}

int SimilarTrackTableModel::keyVisibleFrom(int seedKeyId, int candidateKeyId) const {
    const auto seedKey = KeyUtils::keyFromNumericValue(seedKeyId);
    const auto candidateKey = KeyUtils::keyFromNumericValue(candidateKeyId);
    if (seedKey == mixxx::track::io::key::INVALID) {
        // The seed has no key, so nothing can be judged against it: do not
        // silently empty the list.
        return static_cast<int>(KeyLevel::Harmonic);
    }
    if (candidateKey == mixxx::track::io::key::INVALID) {
        return static_cast<int>(KeyLevel::All);
    }
    if (seedKey == candidateKey ||
            KeyUtils::getCompatibleKeys(seedKey).contains(candidateKey)) {
        return static_cast<int>(KeyLevel::Harmonic);
    }
    // Steps around the Camelot wheel, counting a major/minor change as a step.
    const int seedNumber = KeyUtils::keyToOpenKeyNumber(seedKey);
    const int candidateNumber = KeyUtils::keyToOpenKeyNumber(candidateKey);
    int steps = std::abs(seedNumber - candidateNumber);
    steps = std::min(steps, 12 - steps);
    if (KeyUtils::keyIsMajor(seedKey) != KeyUtils::keyIsMajor(candidateKey)) {
        steps += 1;
    }
    return steps <= 2 ? static_cast<int>(KeyLevel::Wide)
                      : static_cast<int>(KeyLevel::All);
}

void SimilarTrackTableModel::refill() {
    SqlTransaction transaction(m_database);

    QSqlQuery deleteQuery(m_database);
    deleteQuery.prepare(QStringLiteral("DELETE FROM %1").arg(kScoreTableName));
    if (!deleteQuery.exec()) {
        LOG_FAILED_QUERY(deleteQuery);
        return;
    }

    if (m_result.tracks.isEmpty()) {
        transaction.commit();
        return;
    }

    // Tempo and key come from this Mixxx library, not from the stand: these are
    // the values the DJ sees in the BPM and Key columns, and they stay correct
    // when a track is re-analysed after the stand was built.
    QStringList idStrings;
    idStrings.reserve(m_result.tracks.size() + 1);
    for (const auto& track : m_result.tracks) {
        idStrings << track.trackId.toString();
    }
    if (m_result.seedTrackId.isValid()) {
        idStrings << m_result.seedTrackId.toString();
    }
    QHash<TrackId, QPair<double, int>> bpmAndKey;
    bpmAndKey.reserve(idStrings.size());
    {
        QSqlQuery query(m_database);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral("SELECT id, bpm, key_id FROM library WHERE id IN (%1)")
                              .arg(idStrings.join(QChar(','))));
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            return;
        }
        while (query.next()) {
            bpmAndKey.insert(TrackId(query.value(0)),
                    qMakePair(query.value(1).toDouble(), query.value(2).toInt()));
        }
    }
    const auto seedValues = bpmAndKey.value(m_result.seedTrackId, qMakePair(0.0, 0));

    QSqlQuery insertQuery(m_database);
    insertQuery.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO %1 (%2, %3, %4, %5, %6, %7, %8, %9, %10, %11) "
            "VALUES (:id, :position, :score, :bpmlevel, :keylevel, :nokey, "
            ":rank1, :rank2, :rank3, :rank4)")
                                .arg(kScoreTableName,
                                        LIBRARYTABLE_ID,
                                        PLAYLISTTRACKSTABLE_POSITION,
                                        kRawScore,
                                        kBpmLevelReached,
                                        kKeyLevelReached,
                                        kNoKey,
                                        SIMILARTABLE_RANK1,
                                        SIMILARTABLE_RANK2,
                                        SIMILARTABLE_RANK3,
                                        SIMILARTABLE_RANK4));
    const QString rankBindNames[kRankColumnCount] = {
            QStringLiteral(":rank1"),
            QStringLiteral(":rank2"),
            QStringLiteral(":rank3"),
            QStringLiteral(":rank4")};
    for (const auto& track : m_result.tracks) {
        const auto values = bpmAndKey.value(track.trackId, qMakePair(0.0, 0));
        insertQuery.bindValue(QStringLiteral(":id"), track.trackId.toVariant());
        insertQuery.bindValue(QStringLiteral(":position"), track.position);
        insertQuery.bindValue(QStringLiteral(":score"), track.score);
        insertQuery.bindValue(QStringLiteral(":bpmlevel"),
                bpmVisibleFrom(seedValues.first, values.first));
        insertQuery.bindValue(QStringLiteral(":keylevel"),
                keyVisibleFrom(seedValues.second, values.second));
        insertQuery.bindValue(QStringLiteral(":nokey"), values.second <= 0 ? 1 : 0);
        for (int i = 0; i < kRankColumnCount; ++i) {
            const QString toolKey = m_rankToolKeys.value(i);
            const auto it = track.ranks.constFind(toolKey);
            if (toolKey.isEmpty() || it == track.ranks.constEnd()) {
                insertQuery.bindValue(rankBindNames[i], QVariant());
            } else {
                insertQuery.bindValue(rankBindNames[i], it.value());
            }
        }
        if (!insertQuery.exec()) {
            LOG_FAILED_QUERY(insertQuery);
            return;
        }
    }

    transaction.commit();
}

void SimilarTrackTableModel::updateHeaders() {
    const int scoreCol = scoreColumn();
    if (scoreCol >= 0) {
        // The same column holds a cosine per cent for the embedding providers
        // and a mean rank for the ensemble - opposite directions, so the header
        // has to say which one is on screen.
        setHeaderData(scoreCol,
                Qt::Horizontal,
                m_result.scoreIsMeanRank ? tr("Mean rank") : tr("Similarity, %"),
                Qt::DisplayRole);
        setHeaderData(scoreCol,
                Qt::Horizontal,
                m_result.scoreIsMeanRank
                        ? tr("Average rank over all providers, lower is closer. "
                             "Weights push a penalised track further down.")
                        : tr("Cosine similarity in per cent, the same number the "
                             "stand shows, scaled by the weights. The unweighted "
                             "order comes from the centred cosine, so this column "
                             "is not always monotonic."),
                Qt::ToolTipRole);
    }
    const ColumnCache::Column rankColumns[kRankColumnCount] = {
            ColumnCache::COLUMN_SIMILARTABLE_RANK1,
            ColumnCache::COLUMN_SIMILARTABLE_RANK2,
            ColumnCache::COLUMN_SIMILARTABLE_RANK3,
            ColumnCache::COLUMN_SIMILARTABLE_RANK4};
    for (int i = 0; i < kRankColumnCount; ++i) {
        const int column = fieldIndex(rankColumns[i]);
        if (column < 0) {
            continue;
        }
        const QString name = m_rankToolNames.value(i);
        setHeaderData(column, Qt::Horizontal, name, Qt::DisplayRole);
        setHeaderData(column,
                Qt::Horizontal,
                name.isEmpty() ? QString()
                               : tr("Rank of this track in %1").arg(name),
                Qt::ToolTipRole);
    }
}

void SimilarTrackTableModel::setRankTools(
        const QStringList& toolKeys, const QStringList& toolNames) {
    m_rankToolKeys = toolKeys.mid(0, kRankColumnCount);
    m_rankToolNames = toolNames.mid(0, kRankColumnCount);
    updateHeaders();
}

void SimilarTrackTableModel::setResult(const mixxx::SimilarityResult& result) {
    m_result = result;
    writeLevels();
    updateHeaders();
    refill();
    select();
}

void SimilarTrackTableModel::clearResult() {
    m_result = mixxx::SimilarityResult();
    refill();
    select();
}

void SimilarTrackTableModel::setLevels(BpmLevel bpmLevel, KeyLevel keyLevel) {
    if (bpmLevel == m_bpmLevel && keyLevel == m_keyLevel) {
        return;
    }
    m_bpmLevel = bpmLevel;
    m_keyLevel = keyLevel;
    // The distances are already in the rows; only the thresholds move.
    writeLevels();
    select();
}

SimilarTrackTableModel::HiddenCounts SimilarTrackTableModel::hiddenCounts() const {
    HiddenCounts counts;
    QSqlQuery query(m_database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT "
            "  SUM(CASE WHEN %2 > :key AND %4 = 1 THEN 1 ELSE 0 END),"
            "  SUM(CASE WHEN %2 > :key AND %4 = 0 THEN 1 ELSE 0 END),"
            "  SUM(CASE WHEN %3 > :bpm THEN 1 ELSE 0 END) "
            "FROM %1")
                          .arg(kScoreTableName, kKeyLevelReached, kBpmLevelReached, kNoKey));
    query.bindValue(QStringLiteral(":key"), static_cast<int>(m_keyLevel));
    query.bindValue(QStringLiteral(":bpm"), static_cast<int>(m_bpmLevel));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return counts;
    }
    if (query.next()) {
        counts.withoutKey = query.value(0).toInt();
        counts.keyTooFar = query.value(1).toInt();
        counts.outsideBpmRange = query.value(2).toInt();
    }
    return counts;
}

void SimilarTrackTableModel::initSortColumnMapping() {
    // Same shape as PlaylistTableModel: the base class maps library columns
    // only, and WTrackTableView resolves its default sort through SortColumnId.
    // Without Position mapped here the view finds no valid sort id, falls back
    // to the first sortable column and quietly replaces the ranking with an
    // alphabetical list.
    BaseSqlTableModel::initSortColumnMapping();

    m_columnIndexBySortColumnId[static_cast<int>(TrackModel::SortColumnId::Position)] =
            positionColumn();
    m_columnIndexBySortColumnId[static_cast<int>(TrackModel::SortColumnId::Similarity)] =
            scoreColumn();

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
