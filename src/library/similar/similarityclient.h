#pragma once

#include <QJsonDocument>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include "network/jsonwebtask.h"
#include "track/trackid.h"

namespace mixxx {

/// One neighbour of the seed track, as ranked by the annotation stand.
class SimilarTrack final {
  public:
    TrackId trackId;
    /// Rank in the answer, 1 = closest. The list arrives already ordered and
    /// this is what the table sorts by: it is the one number that means the
    /// same thing for a cosine tool and for the rank-averaging ensemble.
    int position = 0;
    /// The number the stand prints in its score column: cosine per cent for the
    /// embedding tools, mean rank for the ensemble. Display only.
    double score = 0.0;
    /// Rank of this candidate in each embedding tool ("clap" -> 3). Empty when
    /// the server reports none.
    QMap<QString, int> ranks;
};

/// One similarity provider offered by the stand.
class SimilarityProvider final {
  public:
    QString key;
    QString shortName;
    QString title;
    int covered = 0;
    /// The ensemble scores by mean rank instead of cosine, so its column reads
    /// the other way round (lower is better).
    bool scoreIsMeanRank = false;
};

class SimilarityResult final {
  public:
    TrackId seedTrackId;
    QString tool;
    /// False when the stand knows the track but this provider has no vector for
    /// it - a different situation from "the stand does not know this track",
    /// which arrives as an error instead.
    bool toolHasTrack = false;
    bool scoreIsMeanRank = false;
    /// Length of the full ranking on the server, not of this answer.
    int total = 0;
    QList<SimilarTrack> tracks;
};

/// A single GET against the stand that hands the parsed JSON back.
///
/// JsonWebTask deletes itself once the response has been handled, so a task is
/// created per request and never stored beyond that.
class SimilarityWebTask : public network::JsonWebTask {
    Q_OBJECT

  public:
    SimilarityWebTask(
            QNetworkAccessManager* pNetworkAccessManager,
            const QUrl& baseUrl,
            const QString& path,
            QUrlQuery&& query,
            QObject* parent = nullptr);
    ~SimilarityWebTask() override = default;

  signals:
    void succeeded(const QJsonDocument& content);

  private:
    void onFinished(const network::JsonWebResponse& response) override;
};

/// Talks to the annotation stand (tagging/server.py) over loopback HTTP.
///
/// Mixxx computes no similarity of its own: the stand holds the embeddings and
/// the rankings, and every track is addressed by its Mixxx library id, which is
/// the id the stand was built with. See research/mixxx-similar-integration.md.
class SimilarityClient : public QObject {
    Q_OBJECT

  public:
    explicit SimilarityClient(QObject* parent = nullptr);
    ~SimilarityClient() override = default;

    void setBaseUrl(const QUrl& baseUrl) {
        m_baseUrl = baseUrl;
    }
    const QUrl& baseUrl() const {
        return m_baseUrl;
    }

    /// Liveness probe that also carries the provider list, so the pane never
    /// has to pull the whole track table just to fill a combo box.
    void requestProviders();

    /// Ask for the ranking of one seed under one provider. Any request still in
    /// flight is aborted first: while the user walks down the library with the
    /// arrow keys only the last answer is of any interest.
    void requestSimilar(TrackId seedTrackId, const QString& tool, int k);

    void abortPending();

  signals:
    void providersReady(const QList<mixxx::SimilarityProvider>& providers, int trackCount);
    void similarReady(const mixxx::SimilarityResult& result);
    /// unreachable is true when the stand did not answer at all, which is the
    /// common case (it is simply not running) and needs different wording than
    /// a server-side error.
    void requestFailed(const QString& message, bool unreachable);

  private:
    SimilarityWebTask* startTask(const QString& path, QUrlQuery&& query, int timeoutMillis);
    void onProvidersReceived(const QJsonDocument& content);
    void onSimilarReceived(const QJsonDocument& content);

    QNetworkAccessManager m_network;
    QUrl m_baseUrl;
    QPointer<SimilarityWebTask> m_pPendingSimilarTask;
    TrackId m_pendingSeedTrackId;
};

} // namespace mixxx
