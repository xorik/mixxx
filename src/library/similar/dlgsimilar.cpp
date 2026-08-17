#include "library/similar/dlgsimilar.h"

#include <QApplication>
#include <QBoxLayout>
#include <QTimer>
#include <algorithm>

#include "library/library.h"
#include "library/similar/similartracktablemodel.h"
#include "library/trackcollectionmanager.h"
#include "moc_dlgsimilar.cpp"
#include "track/track.h"
#include "util/assert.h"
#include "widget/wlibrary.h"
#include "widget/wtracktableview.h"

namespace {

const char* kPreferenceGroup = "[Similar]";
const char* kServerUrlConfigKey = "ServerUrl";
const char* kProviderConfigKey = "Provider";

const QString kDefaultServerUrl = QStringLiteral("http://127.0.0.1:8731");

/// How many candidates to ask for. The stand answers the whole ranking in about
/// ten milliseconds on loopback (measured: 1608 rows, 404 KB, 8-10 ms), so
/// there is nothing to gain from paging - and the filters of phase 2 need a
/// generous list to survive.
constexpr int kRequestedCandidates = 5000;

} // anonymous namespace

DlgSimilar::DlgSimilar(WLibrary* parent, UserSettingsPointer pConfig, Library* pLibrary)
        : QWidget(parent),
          m_pConfig(pConfig),
          m_pTrackTableView(nullptr),
          m_pTrackTableModel(nullptr),
          m_pClient(nullptr),
          m_providersLoaded(false),
          m_relayingOwnSelection(false) {
    setupUi(this);

    m_pTrackTableView = new WTrackTableView(this,
            pConfig,
            pLibrary,
            parent->getTrackTableBackgroundColorOpacity());
    connect(m_pTrackTableView,
            &WTrackTableView::loadTrack,
            this,
            &DlgSimilar::loadTrack);
    connect(m_pTrackTableView,
            &WTrackTableView::loadTrackToPlayer,
            this,
            [this](TrackPointer pTrack, const QString& group) {
                emit loadTrackToPlayer(pTrack, group);
            });
    connect(m_pTrackTableView,
            &WTrackTableView::trackSelected,
            this,
            [this](TrackPointer pTrack) {
                m_relayingOwnSelection = true;
                emit trackSelected(pTrack);
                m_relayingOwnSelection = false;
            });

    QBoxLayout* pBox = qobject_cast<QBoxLayout*>(layout());
    VERIFY_OR_DEBUG_ASSERT(pBox) { // Assumes the form layout is a QVBox/QHBoxLayout!
    }
    else {
        pBox->removeWidget(m_pTrackTablePlaceholder);
        m_pTrackTablePlaceholder->hide();
        pBox->insertWidget(1, m_pTrackTableView);
    }

    m_pTrackTableModel = new SimilarTrackTableModel(
            this, pLibrary->trackCollectionManager());
    m_pTrackTableView->loadTrackModel(m_pTrackTableModel);

    m_pClient = new mixxx::SimilarityClient(this);
    m_pClient->setBaseUrl(QUrl(m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, kServerUrlConfigKey), kDefaultServerUrl)));
    connect(m_pClient,
            &mixxx::SimilarityClient::providersReady,
            this,
            &DlgSimilar::slotProvidersReady);
    connect(m_pClient,
            &mixxx::SimilarityClient::similarReady,
            this,
            &DlgSimilar::slotSimilarReady);
    connect(m_pClient,
            &mixxx::SimilarityClient::requestFailed,
            this,
            &DlgSimilar::slotRequestFailed);

    connect(comboBoxProvider,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &DlgSimilar::slotProviderChanged);
    connect(pushButtonReload,
            &QPushButton::clicked,
            this,
            &DlgSimilar::slotReloadClicked);

    connect(pLibrary,
            &Library::setTrackTableFont,
            m_pTrackTableView,
            &WTrackTableView::setTrackTableFont);
    connect(pLibrary,
            &Library::setTrackTableRowHeight,
            m_pTrackTableView,
            &WTrackTableView::setTrackTableRowHeight);
    connect(pLibrary,
            &Library::setSelectedClick,
            m_pTrackTableView,
            &WTrackTableView::setSelectedClick);

    labelStatus->setText(tr("Asking the stand..."));
    m_pClient->requestProviders();

    maybeArmScreenshot(pLibrary);
}

void DlgSimilar::maybeArmScreenshot(Library* pLibrary) {
    // Debug hook, off unless MIXXX_SIMILAR_SHOT names a file:
    //   MIXXX_SIMILAR_SHOT=/tmp/pane.png MIXXX_SIMILAR_SEED=1434,220,777
    // Seeds the pane with each track id in turn and writes one PNG per seed
    // (the id is appended to the file name when more than one is given), then
    // quits. One launch per picture set, and the process ends by itself -
    // driving this by hand once left seven copies of Mixxx running.
    //
    // Grabbing inside the process is what the waveform harness settled on: a
    // screenshot is silently black without the Screen Recording permission, and
    // whatever covers the window ends up in the picture.
    const QString shotPath = qEnvironmentVariable("MIXXX_SIMILAR_SHOT");
    if (shotPath.isEmpty()) {
        return;
    }
    const QStringList seedIds = qEnvironmentVariable("MIXXX_SIMILAR_SEED")
                                        .split(QChar(','), Qt::SkipEmptyParts);
    if (seedIds.isEmpty()) {
        return;
    }
    // Time for the skin, the first request and the table to settle.
    const int settleMillis = qEnvironmentVariableIsSet("MIXXX_SIMILAR_SHOT_DELAY")
            ? qEnvironmentVariableIntValue("MIXXX_SIMILAR_SHOT_DELAY")
            : 14000;
    constexpr int kPerSeedMillis = 5000;

    for (int i = 0; i < seedIds.size(); ++i) {
        bool ok = false;
        const int seedId = seedIds.at(i).trimmed().toInt(&ok);
        if (!ok || seedId <= 0) {
            continue;
        }
        const bool isLast = i == seedIds.size() - 1;
        const int grabAt = settleMillis + i * kPerSeedMillis;
        const int seedAt = std::max(500, grabAt - 2500);

        QTimer::singleShot(seedAt, this, [this, pLibrary, seedId]() {
            const TrackPointer pTrack =
                    pLibrary->trackCollectionManager()->getTrackById(
                            TrackId(QVariant(seedId)));
            if (pTrack) {
                setSeed(pTrack);
            } else {
                qWarning() << "No track with id" << seedId;
            }
        });

        QString path = shotPath;
        if (seedIds.size() > 1) {
            const int dot = path.lastIndexOf(QChar('.'));
            const QString suffix = QStringLiteral("-%1").arg(seedId);
            if (dot > 0) {
                path = path.left(dot) + suffix + path.mid(dot);
            } else {
                path += suffix;
            }
        }
        QTimer::singleShot(grabAt, this, [this, path, isLast]() {
            if (width() < 200 || height() < 200) {
                resize(1200, 700);
            }
            if (grab().save(path)) {
                qInfo() << "Similar pane written to" << path;
            } else {
                qWarning() << "Could not write" << path;
            }
            if (isLast) {
                // Leave no process behind, whatever happens to the caller.
                QTimer::singleShot(500, qApp, &QCoreApplication::quit);
            }
        });
    }
}

DlgSimilar::~DlgSimilar() {
    // Delete the view before the model: the view saves its header state through
    // the model in its destructor.
    delete m_pTrackTableView;
}

void DlgSimilar::onShow() {
    if (!m_providersLoaded) {
        m_pClient->requestProviders();
    }
}

void DlgSimilar::onSearch(const QString& text) {
    m_pTrackTableModel->search(text);
}

const QString DlgSimilar::currentSearch() {
    return m_pTrackTableModel->currentSearch();
}

bool DlgSimilar::hasFocus() const {
    return m_pTrackTableView->hasFocus();
}

void DlgSimilar::setFocus() {
    m_pTrackTableView->setFocus();
}

void DlgSimilar::saveCurrentViewState() {
    m_pTrackTableView->slotSaveCurrentViewState();
}

bool DlgSimilar::restoreCurrentViewState() {
    return m_pTrackTableView->slotRestoreCurrentViewState();
}

void DlgSimilar::installEventFilter(QObject* pFilter) {
    QWidget::installEventFilter(pFilter);
    m_pTrackTableView->installEventFilter(pFilter);
}

QString DlgSimilar::selectedProviderKey() const {
    return comboBoxProvider->currentData().toString();
}

void DlgSimilar::setSeed(TrackPointer pTrack) {
    if (!pTrack) {
        return;
    }
    const TrackId trackId = pTrack->getId();
    if (!trackId.isValid() || trackId == m_seedTrackId) {
        return;
    }
    m_seedTrackId = trackId;
    const QString artist = pTrack->getArtist();
    const QString title = pTrack->getTitle();
    m_seedLabel = artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title;
    labelSeed->setText(m_seedLabel);
    requestForSeed();
}

void DlgSimilar::slotTrackSelected(TrackPointer pTrack) {
    if (m_relayingOwnSelection || !checkBoxFollowSelection->isChecked()) {
        return;
    }
    setSeed(pTrack);
}

void DlgSimilar::requestForSeed() {
    if (!m_seedTrackId.isValid()) {
        labelStatus->setText(tr("Select a track in the library"));
        return;
    }
    if (!m_providersLoaded) {
        // The provider list has not arrived yet; the answer will trigger this
        // again, so there is nothing to do but say so.
        return;
    }
    labelStatus->setText(tr("Asking the stand..."));
    m_pClient->requestSimilar(
            m_seedTrackId, selectedProviderKey(), kRequestedCandidates);
}

void DlgSimilar::slotProvidersReady(
        const QList<mixxx::SimilarityProvider>& providers, int trackCount) {
    const QString configured = m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, kProviderConfigKey), QString());

    const QSignalBlocker blocker(comboBoxProvider);
    comboBoxProvider->clear();
    int indexToSelect = 0;
    for (const auto& provider : providers) {
        comboBoxProvider->addItem(provider.shortName, provider.key);
        const int index = comboBoxProvider->count() - 1;
        comboBoxProvider->setItemData(index, provider.title, Qt::ToolTipRole);
        if (provider.key == configured) {
            indexToSelect = index;
        }
    }
    if (comboBoxProvider->count() > 0) {
        comboBoxProvider->setCurrentIndex(indexToSelect);
    }

    m_providersLoaded = !providers.isEmpty();
    if (!m_providersLoaded) {
        labelStatus->setText(tr("The stand has no providers"));
        return;
    }
    labelStatus->setText(tr("%1 tracks analysed").arg(trackCount));
    requestForSeed();
}

void DlgSimilar::slotSimilarReady(const mixxx::SimilarityResult& result) {
    if (result.seedTrackId != m_seedTrackId) {
        // A late answer for a track that is no longer the seed.
        return;
    }
    if (!result.toolHasTrack) {
        m_pTrackTableModel->clearResult();
        labelStatus->setText(
                tr("This provider has no vector for the track"));
        return;
    }
    m_pTrackTableModel->setResult(result);
    labelStatus->setText(tr("%1 of %2 ranked")
                                 .arg(QString::number(result.tracks.size()),
                                         QString::number(result.total)));
}

void DlgSimilar::slotRequestFailed(const QString& message, bool unreachable) {
    m_pTrackTableModel->clearResult();
    if (unreachable) {
        labelStatus->setText(
                tr("Stand not running - start it: "
                   "cd ~/www/ai/mixxx/tagging && ./venv/bin/python server.py"));
        return;
    }
    // The stand answers 404 for a track it has never seen, which is the normal
    // state of a track added after the last analysis run.
    labelStatus->setText(tr("Not in the analysis set (%1)").arg(message));
}

void DlgSimilar::slotProviderChanged(int index) {
    Q_UNUSED(index);
    const QString key = selectedProviderKey();
    if (key.isEmpty()) {
        return;
    }
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kProviderConfigKey), key);
    requestForSeed();
}

void DlgSimilar::slotReloadClicked() {
    m_providersLoaded = false;
    labelStatus->setText(tr("Asking the stand..."));
    m_pClient->requestProviders();
}
