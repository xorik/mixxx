#include "library/similar/similarityclient.h"

#include <QJsonArray>
#include <QJsonObject>

#include "moc_similarityclient.cpp"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("SimilarityClient");

constexpr int kHealthTimeoutMillis = 2000;
constexpr int kSimilarTimeoutMillis = 5000;

const QString kHealthPath = QStringLiteral("/api/health");
const QString kSimilarPath = QStringLiteral("/api/similar");

/// The stand reports the ensemble under this key; it is the only provider whose
/// score is a mean rank rather than a cosine.
const QString kEnsembleKey = QStringLiteral("ensemble");

} // anonymous namespace

namespace mixxx {

SimilarityWebTask::SimilarityWebTask(
        QNetworkAccessManager* pNetworkAccessManager,
        const QUrl& baseUrl,
        const QString& path,
        QUrlQuery&& query,
        QObject* parent)
        : network::JsonWebTask(
                  pNetworkAccessManager,
                  baseUrl,
                  network::JsonWebRequest{
                          network::HttpRequestMethod::Get,
                          path,
                          std::move(query),
                          QJsonDocument{}},
                  parent) {
}

void SimilarityWebTask::onFinished(
        const network::JsonWebResponse& response) {
    if (!response.isStatusCodeSuccess()) {
        emitFailed(response);
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(response.content().isObject()) {
        emitFailed(response);
        return;
    }
    const QJsonDocument content = response.content();
    // The base class deletes the task after this call returns, so the signal is
    // emitted first and the receiver must not keep a pointer to us.
    emit succeeded(content);
    deleteLater();
}

SimilarityClient::SimilarityClient(QObject* parent)
        : QObject(parent) {
}

SimilarityWebTask* SimilarityClient::startTask(
        const QString& path, QUrlQuery&& query, int timeoutMillis) {
    auto* pTask = new SimilarityWebTask(
            &m_network, m_baseUrl, path, std::move(query), this);
    connect(pTask,
            &network::JsonWebTask::failed,
            this,
            [this](const network::JsonWebResponse& response) {
                emit requestFailed(
                        tr("The stand answered with HTTP status %1")
                                .arg(response.statusCode()),
                        false);
            });
    connect(pTask,
            &network::WebTask::networkError,
            this,
            [this](QNetworkReply::NetworkError errorCode,
                    const QString& errorString,
                    const network::WebResponseWithContent& /*response*/) {
                Q_UNUSED(errorCode);
                emit requestFailed(errorString, true);
            });
    pTask->slotStart(timeoutMillis);
    return pTask;
}

void SimilarityClient::requestProviders() {
    auto* pTask = startTask(kHealthPath, QUrlQuery{}, kHealthTimeoutMillis);
    connect(pTask,
            &SimilarityWebTask::succeeded,
            this,
            &SimilarityClient::onProvidersReceived);
}

void SimilarityClient::requestSimilar(
        TrackId seedTrackId, const QString& tool, int k) {
    VERIFY_OR_DEBUG_ASSERT(seedTrackId.isValid()) {
        return;
    }
    abortPending();

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), seedTrackId.toString());
    if (!tool.isEmpty()) {
        query.addQueryItem(QStringLiteral("tool"), tool);
    }
    query.addQueryItem(QStringLiteral("k"), QString::number(k));

    m_pendingSeedTrackId = seedTrackId;
    m_pPendingSimilarTask = startTask(
            kSimilarPath, std::move(query), kSimilarTimeoutMillis);
    connect(m_pPendingSimilarTask,
            &SimilarityWebTask::succeeded,
            this,
            &SimilarityClient::onSimilarReceived);
}

void SimilarityClient::abortPending() {
    if (m_pPendingSimilarTask) {
        m_pPendingSimilarTask->slotAbort();
        m_pPendingSimilarTask.clear();
    }
}

void SimilarityClient::onProvidersReceived(const QJsonDocument& content) {
    const QJsonObject root = content.object();
    QList<SimilarityProvider> providers;
    const QJsonArray tools = root.value(QLatin1String("tools")).toArray();
    for (const auto& tool : tools) {
        const QJsonObject toolObject = tool.toObject();
        SimilarityProvider provider;
        provider.key = toolObject.value(QLatin1String("key")).toString();
        if (provider.key.isEmpty()) {
            continue;
        }
        provider.shortName = toolObject.value(QLatin1String("short")).toString();
        if (provider.shortName.isEmpty()) {
            provider.shortName = provider.key;
        }
        provider.title = toolObject.value(QLatin1String("title")).toString();
        provider.covered = toolObject.value(QLatin1String("covered")).toInt();
        provider.scoreIsMeanRank = provider.key == kEnsembleKey;
        providers.append(provider);
    }
    const int trackCount = root.value(QLatin1String("tracks")).toInt();
    kLogger.debug() << "Stand reports" << providers.size() << "providers and"
                    << trackCount << "tracks";
    emit providersReady(providers, trackCount);
}

void SimilarityClient::onSimilarReceived(const QJsonDocument& content) {
    const QJsonObject root = content.object();

    SimilarityResult result;
    result.tool = root.value(QLatin1String("tool")).toString();
    result.scoreIsMeanRank = result.tool == kEnsembleKey;
    result.toolHasTrack = root.value(QLatin1String("tool_has_track")).toBool();
    result.total = root.value(QLatin1String("total")).toInt();
    const int seedId = root.value(QLatin1String("seed"))
                               .toObject()
                               .value(QLatin1String("id"))
                               .toInt();
    result.seedTrackId = seedId > 0 ? TrackId(QVariant(seedId)) : m_pendingSeedTrackId;

    const QJsonArray rows = root.value(QLatin1String("similar")).toArray();
    result.tracks.reserve(rows.size());
    int position = root.value(QLatin1String("offset")).toInt() + 1;
    for (const auto& row : rows) {
        const QJsonObject rowObject = row.toObject();
        const int id = rowObject.value(QLatin1String("id")).toInt();
        if (id <= 0) {
            continue;
        }
        SimilarTrack track;
        track.trackId = TrackId(QVariant(id));
        track.position = position++;
        if (result.scoreIsMeanRank) {
            track.score = rowObject.value(QLatin1String("mrank")).toDouble();
        } else {
            // The plain cosine as a per cent - the number the dashboard prints,
            // deliberately the same here. The ranking is built from the centred
            // cosine instead, so this column can step back up here and there;
            // that is how the stand has always looked and the user reads it
            // that way.
            track.score = rowObject.value(QLatin1String("raw")).toDouble() * 100.0;
        }
        const QJsonObject ranks = rowObject.value(QLatin1String("ranks")).toObject();
        for (auto it = ranks.constBegin(); it != ranks.constEnd(); ++it) {
            if (it.value().isDouble()) {
                track.ranks.insert(it.key(), it.value().toInt());
            }
        }
        result.tracks.append(track);
    }

    emit similarReady(result);
}

} // namespace mixxx
