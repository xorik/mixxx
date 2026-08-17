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
const QString kBpmPenalty = QStringLiteral("bpm_penalty");
const QString kKeyPenalty = QStringLiteral("key_penalty");
const QString kNoKey = QStringLiteral("no_key");

constexpr double kDefaultBpmRangePercent = 6.0;
constexpr int kRankColumnCount = 4;

/// The penalty a full mismatch adds to a mean rank. Adding instead of dividing
/// keeps the arithmetic away from a near-zero denominator, and one library
/// length is enough to push a fully penalised track behind everything else.
constexpr double kRankPenaltyScale = 100000.0;

} // anonymous namespace

SimilarTrackTableModel::SimilarTrackTableModel(
        QObject* parent, TrackCollectionManager* pTrackCollectionManager)
        : BaseSqlTableModel(parent, pTrackCollectionManager, kModelSettingsNamespace),
          m_bpmWeight(0.0),
          m_keyWeight(0.0),
          m_bpmRangePercent(kDefaultBpmRangePercent),
          m_keyMatchMode(KeyMatchMode::CamelotDistance) {
    createTables();
    writeWeights();

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
    // Rank order while no weight is active: the stand ranks by the centred
    // cosine and we print the plain one, so sorting by the printed number would
    // quietly disagree with the dashboard. As soon as a weight is on, the
    // weighted score takes over (see setWeights).
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
            "  %5 REAL,"
            "  %6 REAL,"
            "  %7 INTEGER,"
            "  %8 INTEGER, %9 INTEGER, %10 INTEGER, %11 INTEGER)")
                          .arg(kScoreTableName,
                                  LIBRARYTABLE_ID,
                                  PLAYLISTTRACKSTABLE_POSITION,
                                  kRawScore,
                                  kBpmPenalty,
                                  kKeyPenalty,
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
            "CREATE TEMPORARY TABLE IF NOT EXISTS %1 ("
            "  w_bpm REAL, w_key REAL, is_rank INTEGER, rank_scale REAL)")
                          .arg(kWeightTableName));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }
    query.prepare(QStringLiteral("INSERT INTO %1 (w_bpm, w_key, is_rank, rank_scale) "
                                 "SELECT 0.0, 0.0, 0, %2 WHERE NOT EXISTS (SELECT 1 FROM %1)")
                          .arg(kWeightTableName, QString::number(kRankPenaltyScale)));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }

    // The weighted score:
    //   cosine providers  score x (1 - w_bpm*p_bpm) x (1 - w_key*p_key)
    //   the ensemble      mean rank + scale x (w_bpm*p_bpm + w_key*p_key)
    // The ensemble counts upwards (lower is closer), so a penalty has to make
    // the number bigger; adding avoids dividing by a near-zero factor.
    // A weight of 1 against a full penalty removes the row altogether, which is
    // the "hard filter" end of the slider.
    query.prepare(QStringLiteral(
            "CREATE TEMPORARY VIEW IF NOT EXISTS %1 AS SELECT "
            "  scores.%2 AS %2,"
            "  scores.%3 AS %3,"
            "  CASE WHEN w.is_rank = 1"
            "    THEN scores.%4 + w.rank_scale *"
            "         (w.w_bpm * scores.%5 + w.w_key * scores.%6)"
            "    ELSE scores.%4 * (1.0 - w.w_bpm * scores.%5)"
            "                   * (1.0 - w.w_key * scores.%6)"
            "  END AS %7,"
            "  scores.%8 AS %8, scores.%9 AS %9, scores.%10 AS %10, scores.%11 AS %11,"
            "  '' AS %12,"
            // The cover art column sorts by the digest, as everywhere else.
            "  library.%13 AS %14 "
            "FROM %15 AS scores "
            "CROSS JOIN %16 AS w "
            "INNER JOIN library ON library.id = scores.%2 "
            "INNER JOIN track_locations ON library.location = track_locations.id "
            "WHERE library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0 "
            "  AND w.w_bpm * scores.%5 < 1.0 "
            "  AND w.w_key * scores.%6 < 1.0")
                          .arg(kViewName,
                                  LIBRARYTABLE_ID,
                                  PLAYLISTTRACKSTABLE_POSITION,
                                  kRawScore,
                                  kBpmPenalty,
                                  kKeyPenalty,
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

void SimilarTrackTableModel::writeWeights() {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE %1 SET w_bpm = :wbpm, w_key = :wkey, "
                                 "is_rank = :isrank")
                          .arg(kWeightTableName));
    query.bindValue(QStringLiteral(":wbpm"), m_bpmWeight);
    query.bindValue(QStringLiteral(":wkey"), m_keyWeight);
    query.bindValue(QStringLiteral(":isrank"), m_result.scoreIsMeanRank ? 1 : 0);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

double SimilarTrackTableModel::bpmPenalty(double seedBpm, double candidateBpm) const {
    if (seedBpm <= 0.0) {
        // Nothing to compare against: do not punish anybody.
        return 0.0;
    }
    if (candidateBpm <= 0.0) {
        // Unanalysed tempo is as far away as it gets.
        return 1.0;
    }
    // No half/double folding on purpose: this library keeps drum & bass at half
    // tempo throughout, so 85 and 170 are different tempos here, not the same one.
    const double tolerance = seedBpm * m_bpmRangePercent / 100.0;
    if (tolerance <= 0.0) {
        return candidateBpm == seedBpm ? 0.0 : 1.0;
    }
    return std::min(1.0, std::abs(candidateBpm - seedBpm) / tolerance);
}

double SimilarTrackTableModel::keyPenalty(int seedKeyId, int candidateKeyId) const {
    const auto seedKey = KeyUtils::keyFromNumericValue(seedKeyId);
    const auto candidateKey = KeyUtils::keyFromNumericValue(candidateKeyId);
    if (seedKey == mixxx::track::io::key::INVALID) {
        // The seed has no key, so nothing can be judged against it.
        return 0.0;
    }
    if (candidateKey == mixxx::track::io::key::INVALID) {
        return 1.0;
    }
    switch (m_keyMatchMode) {
    case KeyMatchMode::Exact:
        return seedKey == candidateKey ? 0.0 : 1.0;
    case KeyMatchMode::Harmonic:
        // The set Mixxx itself calls harmonic: the relative key and the wheel
        // neighbours of both modes (also what the '~key:' search uses).
        return KeyUtils::getCompatibleKeys(seedKey).contains(candidateKey) ? 0.0 : 1.0;
    case KeyMatchMode::CamelotDistance:
    default: {
        const int seedNumber = KeyUtils::keyToOpenKeyNumber(seedKey);
        const int candidateNumber = KeyUtils::keyToOpenKeyNumber(candidateKey);
        int steps = std::abs(seedNumber - candidateNumber);
        steps = std::min(steps, 12 - steps); // 0..6 around the wheel
        if (KeyUtils::keyIsMajor(seedKey) != KeyUtils::keyIsMajor(candidateKey)) {
            // Relative major/minor is one step, as on the Camelot wheel.
            steps += 1;
        }
        return std::min(1.0, steps / 6.0);
    }
    }
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
            "VALUES (:id, :position, :score, :bpmpen, :keypen, :nokey, "
            ":rank1, :rank2, :rank3, :rank4)")
                                .arg(kScoreTableName,
                                        LIBRARYTABLE_ID,
                                        PLAYLISTTRACKSTABLE_POSITION,
                                        kRawScore,
                                        kBpmPenalty,
                                        kKeyPenalty,
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
        insertQuery.bindValue(QStringLiteral(":bpmpen"),
                bpmPenalty(seedValues.first, values.first));
        insertQuery.bindValue(QStringLiteral(":keypen"),
                keyPenalty(seedValues.second, values.second));
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
    writeWeights(); // is_rank follows the provider
    updateHeaders();
    refill();
    select();
}

void SimilarTrackTableModel::clearResult() {
    m_result = mixxx::SimilarityResult();
    refill();
    select();
}

void SimilarTrackTableModel::setWeights(double bpmWeight, double keyWeight) {
    m_bpmWeight = std::clamp(bpmWeight, 0.0, 1.0);
    m_keyWeight = std::clamp(keyWeight, 0.0, 1.0);
    writeWeights();
    select();
}

void SimilarTrackTableModel::setBpmRangePercent(double percent) {
    if (percent <= 0.0 || qFuzzyCompare(percent, m_bpmRangePercent)) {
        return;
    }
    m_bpmRangePercent = percent;
    // The penalty itself changes, so the rows have to be written again.
    refill();
    select();
}

void SimilarTrackTableModel::setKeyMatchMode(KeyMatchMode mode) {
    if (mode == m_keyMatchMode) {
        return;
    }
    m_keyMatchMode = mode;
    refill();
    select();
}

SimilarTrackTableModel::HiddenCounts SimilarTrackTableModel::hiddenCounts() const {
    HiddenCounts counts;
    QSqlQuery query(m_database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT "
            "  SUM(CASE WHEN :wkey * %2 >= 1.0 AND %4 = 1 THEN 1 ELSE 0 END),"
            "  SUM(CASE WHEN :wkey * %2 >= 1.0 AND %4 = 0 THEN 1 ELSE 0 END),"
            "  SUM(CASE WHEN :wbpm * %3 >= 1.0 THEN 1 ELSE 0 END) "
            "FROM %1")
                          .arg(kScoreTableName, kKeyPenalty, kBpmPenalty, kNoKey));
    query.bindValue(QStringLiteral(":wkey"), m_keyWeight);
    query.bindValue(QStringLiteral(":wbpm"), m_bpmWeight);
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
