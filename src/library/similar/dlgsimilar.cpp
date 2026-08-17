#include "library/similar/dlgsimilar.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCollator>
#include <QSignalBlocker>
#include <QTimer>
#include <algorithm>

#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_dlgsimilar.cpp"
#include "track/track.h"
#include "util/assert.h"
#include "widget/wlibrary.h"
#include "widget/wtracktableview.h"

namespace {

const char* kPreferenceGroup = "[Similar]";
const char* kServerUrlConfigKey = "ServerUrl";
const char* kProviderConfigKey = "Provider";
const char* kBpmFilterConfigKey = "BpmFilterLevel";
const char* kKeyFilterConfigKey = "KeyFilterLevel";

const QString kDefaultServerUrl = QStringLiteral("http://127.0.0.1:8731");

/// How many candidates to ask for. The stand answers the whole ranking in about
/// ten milliseconds on loopback (measured: 1608 rows, 404 KB, 8-10 ms), so
/// there is nothing to gain from paging - and the weights need a generous list
/// to still have something left at the hard end of a slider.
constexpr int kRequestedCandidates = 5000;

} // anonymous namespace

DlgSimilar::DlgSimilar(WLibrary* parent, UserSettingsPointer pConfig, Library* pLibrary)
        : QWidget(parent),
          m_pConfig(pConfig),
          m_pTrackTableView(nullptr),
          m_pTrackTableModel(nullptr),
          m_pClient(nullptr),
          m_seedPinned(false),
          m_providersLoaded(false) {
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
    // Selection is relayed for the cover art widget only; it never changes the
    // seed, so walking the list with the arrow keys leaves it alone.
    connect(m_pTrackTableView,
            &WTrackTableView::trackSelected,
            this,
            &DlgSimilar::trackSelected);

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

    // Restore the sliders before anything is requested. Both default to their
    // rightmost position, which filters nothing: a pane that greets the user
    // with an almost empty list because his seed has an unusual tempo would be
    // read as "it does not work".
    sliderBpmFilter->setValue(m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, kBpmFilterConfigKey),
            sliderBpmFilter->maximum()));
    sliderKeyFilter->setValue(m_pConfig->getValue(
            ConfigKey(kPreferenceGroup, kKeyFilterConfigKey),
            sliderKeyFilter->maximum()));
    slotFiltersChanged();

    connect(sliderProvider,
            &QSlider::valueChanged,
            this,
            &DlgSimilar::slotProviderChanged);
    connect(sliderBpmFilter,
            &QSlider::valueChanged,
            this,
            &DlgSimilar::slotFiltersChanged);
    connect(sliderKeyFilter,
            &QSlider::valueChanged,
            this,
            &DlgSimilar::slotFiltersChanged);
    connect(pushButtonUnpin,
            &QPushButton::clicked,
            this,
            &DlgSimilar::slotUnpinClicked);
    connect(pushButtonReload,
            &QPushButton::clicked,
            this,
            &DlgSimilar::slotReloadClicked);

    // The seed follows the decks, which is visible from any page of the library
    // stack because the decks are not part of the library. Both signals matter:
    // one for what is playing, one for what was merely loaded.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &DlgSimilar::slotPlayingTrackChanged);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &DlgSimilar::slotPlayerTrackChanged);

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

    pushButtonUnpin->setVisible(false);
    labelStatus->setText(tr("Asking the stand..."));
    m_pClient->requestProviders();

    maybeArmScreenshot(pLibrary);
}

void DlgSimilar::maybeArmScreenshot(Library* pLibrary) {
    // Debug hook, off unless MIXXX_SIMILAR_SHOT names a file:
    //   MIXXX_SIMILAR_SHOT=/tmp/pane.png MIXXX_SIMILAR_SEED=1434,220,777
    // Pins each track id in turn and writes one PNG per seed (the id is
    // appended to the file name when more than one is given), then quits. One
    // launch per picture set, and the process ends by itself - driving this by
    // hand once left seven copies of Mixxx running.
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
                pinSeed(pTrack);
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
        return;
    }
    if (!m_seedTrackId.isValid()) {
        resolveSeed();
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
    const int index = sliderProvider->value();
    if (index < 0 || index >= m_providers.size()) {
        return QString();
    }
    return m_providers.at(index).key;
}

QList<mixxx::SimilarityProvider> DlgSimilar::sortedForSlider(
        const QList<mixxx::SimilarityProvider>& providers) {
    // Numeric mode, so that a layer probe muq_L6 sits before muq_L10 - plain
    // alphabetical order gets that backwards.
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    QList<mixxx::SimilarityProvider> sorted = providers;
    std::sort(sorted.begin(),
            sorted.end(),
            [&collator](const mixxx::SimilarityProvider& left,
                    const mixxx::SimilarityProvider& right) {
                // The ensemble is an opinion about the others, so it belongs at
                // the end whatever it is called.
                if (left.scoreIsMeanRank != right.scoreIsMeanRank) {
                    return right.scoreIsMeanRank;
                }
                return collator.compare(left.key, right.key) < 0;
            });
    return sorted;
}

void DlgSimilar::updateSeedLabel() {
    if (m_seedLabel.isEmpty()) {
        labelSeed->setText(tr("<no track loaded>"));
    } else {
        labelSeed->setText(m_seedLabel);
    }
    pushButtonUnpin->setVisible(m_seedPinned);
}

void DlgSimilar::setSeed(TrackPointer pTrack) {
    if (!pTrack) {
        watchSeedTrack(TrackPointer());
        m_seedTrackId = TrackId();
        m_seedLabel.clear();
        updateSeedLabel();
        m_pTrackTableModel->clearResult();
        labelStatus->setText(tr("No track loaded"));
        labelHidden->clear();
        return;
    }
    const TrackId trackId = pTrack->getId();
    if (!trackId.isValid() || trackId == m_seedTrackId) {
        return;
    }
    m_seedTrackId = trackId;
    watchSeedTrack(pTrack);
    pushSeedAttributes();
    const QString artist = pTrack->getArtist();
    const QString title = pTrack->getTitle();
    m_seedLabel = artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title;
    updateSeedLabel();
    requestForSeed();
}

void DlgSimilar::watchSeedTrack(const TrackPointer& pTrack) {
    if (m_pSeedTrack == pTrack) {
        return;
    }
    if (m_pSeedTrack) {
        disconnect(m_pSeedTrack.get(), nullptr, this, nullptr);
    }
    m_pSeedTrack = pTrack;
    if (!m_pSeedTrack) {
        return;
    }
    // Tempo and key usually arrive after the track is loaded, when the analyser
    // is done. Reading them once at load time leaves both filters looking dead
    // for a track whose numbers the rest of the UI already shows.
    connect(m_pSeedTrack.get(),
            &Track::bpmChanged,
            this,
            &DlgSimilar::slotSeedAttributesChanged);
    connect(m_pSeedTrack.get(),
            &Track::beatsUpdated,
            this,
            &DlgSimilar::slotSeedAttributesChanged);
    connect(m_pSeedTrack.get(),
            &Track::keyChanged,
            this,
            &DlgSimilar::slotSeedAttributesChanged);
}

void DlgSimilar::pushSeedAttributes() {
    const double bpm = m_pSeedTrack ? m_pSeedTrack->getBpm() : 0.0;
    const int keyId = m_pSeedTrack ? static_cast<int>(m_pSeedTrack->getKey()) : 0;
    m_pTrackTableModel->setSeedAttributes(bpm, keyId);
}

void DlgSimilar::slotSeedAttributesChanged() {
    pushSeedAttributes();
    updateFilterAvailability();
    updateHiddenLabel();
}

void DlgSimilar::slotPlayingTrackChanged(TrackPointer pTrack) {
    Q_UNUSED(pTrack);
    resolveSeed();
}

void DlgSimilar::slotPlayerTrackChanged(const QString& group,
        TrackPointer pNewTrack,
        TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    if (!PlayerManager::isDeckGroup(group)) {
        // Samplers and preview decks are not what the DJ is mixing towards.
        return;
    }
    m_loadedDeckGroups.removeAll(group);
    if (pNewTrack) {
        // Most recently loaded first, which is the deck the DJ just prepared.
        m_loadedDeckGroups.prepend(group);
    }
    resolveSeed();
}

void DlgSimilar::resolveSeed() {
    if (m_seedPinned) {
        // A hand-picked seed wins until the user unpins it.
        return;
    }
    const TrackPointer pPlaying = PlayerInfo::instance().getCurrentPlayingTrack();
    if (pPlaying) {
        setSeed(pPlaying);
        return;
    }
    // Nothing is playing: fall back to the deck loaded last, so the pane has
    // something to say while the DJ is still choosing.
    for (const auto& group : std::as_const(m_loadedDeckGroups)) {
        const TrackPointer pLoaded = PlayerInfo::instance().getTrackInfo(group);
        if (pLoaded && pLoaded->getId().isValid()) {
            setSeed(pLoaded);
            return;
        }
    }
    setSeed(TrackPointer());
}

void DlgSimilar::pinSeed(TrackPointer pTrack) {
    if (!pTrack || !pTrack->getId().isValid()) {
        return;
    }
    m_seedPinned = true;
    // setSeed() ignores a repeated id, so clear it first: asking for the same
    // track again is exactly what the menu entry means.
    m_seedTrackId = TrackId();
    setSeed(pTrack);
}

void DlgSimilar::slotUnpinClicked() {
    m_seedPinned = false;
    updateSeedLabel();
    resolveSeed();
}

void DlgSimilar::requestForSeed() {
    if (!m_seedTrackId.isValid()) {
        labelStatus->setText(tr("No track loaded"));
        return;
    }
    if (!m_providersLoaded) {
        // The provider list has not arrived yet; its answer triggers this again.
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

    m_providers = sortedForSlider(providers);

    QStringList rankToolKeys;
    QStringList rankToolNames;
    for (const auto& provider : std::as_const(m_providers)) {
        if (!provider.scoreIsMeanRank) {
            // The rank columns are the plain models; the ensemble has no rank
            // of its own.
            rankToolKeys << provider.key;
            rankToolNames << provider.shortName;
        }
    }
    m_pTrackTableModel->setRankTools(rankToolKeys, rankToolNames);

    m_providersLoaded = !m_providers.isEmpty();
    if (!m_providersLoaded) {
        labelStatus->setText(tr("The stand has no providers"));
        return;
    }

    int indexToSelect = 0;
    for (int i = 0; i < m_providers.size(); ++i) {
        if (m_providers.at(i).key == configured) {
            indexToSelect = i;
            break;
        }
    }
    {
        const QSignalBlocker blocker(sliderProvider);
        sliderProvider->setMaximum(m_providers.size() - 1);
        sliderProvider->setValue(indexToSelect);
    }
    labelProvider->setText(m_providers.at(indexToSelect).shortName);
    sliderProvider->setToolTip(m_providers.at(indexToSelect).title);

    labelStatus->setText(tr("%1 tracks analysed").arg(trackCount));
    if (!m_seedTrackId.isValid()) {
        resolveSeed();
    } else {
        requestForSeed();
    }
}

void DlgSimilar::slotSimilarReady(const mixxx::SimilarityResult& result) {
    if (result.seedTrackId != m_seedTrackId) {
        // A late answer for a track that is no longer the seed.
        return;
    }
    if (!result.toolHasTrack) {
        m_pTrackTableModel->clearResult();
        labelStatus->setText(tr("This provider has no vector for the track"));
        labelHidden->clear();
        return;
    }
    m_pTrackTableModel->setResult(result);
    updateFilterAvailability();
    labelStatus->setText(tr("%1 of %2 ranked")
                                 .arg(QString::number(result.tracks.size()),
                                         QString::number(result.total)));
    updateHiddenLabel();
}

void DlgSimilar::slotRequestFailed(const QString& message, bool unreachable) {
    m_pTrackTableModel->clearResult();
    labelHidden->clear();
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

void DlgSimilar::slotProviderChanged() {
    const int index = sliderProvider->value();
    if (index < 0 || index >= m_providers.size()) {
        return;
    }
    const auto& provider = m_providers.at(index);
    labelProvider->setText(provider.shortName);
    sliderProvider->setToolTip(provider.title);
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kProviderConfigKey), provider.key);
    requestForSeed();
}

void DlgSimilar::slotFiltersChanged() {
    const auto bpmLevel = static_cast<SimilarTrackTableModel::BpmLevel>(
            sliderBpmFilter->value());
    const auto keyLevel = static_cast<SimilarTrackTableModel::KeyLevel>(
            sliderKeyFilter->value());

    // The label is the whole readout of a stepped slider, so it says what the
    // position means rather than which number it is.
    static const char* const kBpmLabels[] = {"\u00b11.5%", "\u00b13%", "\u00b14.5%", "\u00b16%", "any"};
    static const char* const kKeyLabels[] = {"harmonic", "wide", "all"};
    labelBpmFilter->setText(QString::fromUtf8(
            kBpmLabels[std::clamp(sliderBpmFilter->value(), 0, 4)]));
    labelKeyFilter->setText(QString::fromUtf8(
            kKeyLabels[std::clamp(sliderKeyFilter->value(), 0, 2)]));

    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kBpmFilterConfigKey),
            sliderBpmFilter->value());
    m_pConfig->setValue(ConfigKey(kPreferenceGroup, kKeyFilterConfigKey),
            sliderKeyFilter->value());

    // Purely local: the whole ranking is already in the model and every
    // candidate carries how close it is, so a slider only moves a threshold.
    // No request is sent.
    m_pTrackTableModel->setLevels(bpmLevel, keyLevel);
    updateHiddenLabel();
}

void DlgSimilar::updateFilterAvailability() {
    // A seed without a key cannot be compared to anything, so the key filter is
    // inert whatever the slider says. Saying that out loud is the difference
    // between "this build is broken" and "this track has no key".
    const bool canJudgeKey = m_pTrackTableModel->seedHasKey();
    const bool canJudgeBpm = m_pTrackTableModel->seedHasBpm();
    sliderKeyFilter->setEnabled(canJudgeKey);
    labelKeyFilterCaption->setEnabled(canJudgeKey);
    sliderBpmFilter->setEnabled(canJudgeBpm);
    labelBpmFilterCaption->setEnabled(canJudgeBpm);
    if (!canJudgeKey) {
        labelKeyFilter->setText(tr("seed has no key"));
    }
    if (!canJudgeBpm) {
        labelBpmFilter->setText(tr("seed has no BPM"));
    }
    if (canJudgeKey && canJudgeBpm) {
        // Put the position names back.
        slotFiltersChanged();
    }
}

void DlgSimilar::updateHiddenLabel() {
    const auto counts = m_pTrackTableModel->hiddenCounts();
    if (counts.isEmpty()) {
        labelHidden->clear();
        return;
    }
    QStringList parts;
    if (counts.withoutKey > 0) {
        parts << tr("%1 without key").arg(counts.withoutKey);
    }
    if (counts.keyTooFar > 0) {
        parts << tr("%1 off key").arg(counts.keyTooFar);
    }
    if (counts.outsideBpmRange > 0) {
        parts << tr("%1 outside BPM range").arg(counts.outsideBpmRange);
    }
    labelHidden->setText(tr("hidden: %1").arg(parts.join(QStringLiteral(", "))));
}

void DlgSimilar::slotReloadClicked() {
    m_providersLoaded = false;
    labelStatus->setText(tr("Asking the stand..."));
    m_pClient->requestProviders();
}
