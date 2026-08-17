#pragma once

#include <QList>
#include <QWidget>

#include "library/libraryview.h"
#include "library/similar/similarityclient.h"
#include "library/similar/similartracktablemodel.h"
#include "library/similar/ui_dlgsimilar.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class Library;
class WLibrary;
class WTrackTableView;

/// The "Similar" pane: a control strip over a track table, in the shape the
/// Analyze pane already uses (so every skin styles it for free).
///
/// The seed is the track that is playing: the pane follows the decks, which is
/// visible from any page of the library stack, unlike the library selection.
/// A seed picked by hand from the track context menu pins the list until the
/// user unpins it.
class DlgSimilar : public QWidget, public Ui::DlgSimilar, public virtual LibraryView {
    Q_OBJECT

  public:
    DlgSimilar(WLibrary* parent, UserSettingsPointer pConfig, Library* pLibrary);
    ~DlgSimilar() override;

    void onShow() override;
    void onSearch(const QString& text) override;
    bool hasFocus() const override;
    void setFocus() override;
    void saveCurrentViewState() override;
    bool restoreCurrentViewState() override;
    const QString currentSearch();

    void installEventFilter(QObject* pFilter);

  public slots:
    /// Take this track as the seed and stop following the decks until unpinned.
    void pinSeed(TrackPointer pTrack);

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group);
    void trackSelected(TrackPointer pTrack);

  private slots:
    void slotPlayingTrackChanged(TrackPointer pTrack);
    /// Any deck gaining or losing a track, playing or not.
    void slotPlayerTrackChanged(const QString& group,
            TrackPointer pNewTrack,
            TrackPointer pOldTrack);
    void slotProvidersReady(const QList<mixxx::SimilarityProvider>& providers, int trackCount);
    void slotSimilarReady(const mixxx::SimilarityResult& result);
    void slotRequestFailed(const QString& message, bool unreachable);
    void slotProviderChanged();
    void slotFiltersChanged();
    void slotUnpinClicked();
    void slotReloadClicked();

  private:
    void maybeArmScreenshot(Library* pLibrary);
    void requestForSeed();
    void setSeed(TrackPointer pTrack);
    /// Pinned seed, else the playing deck, else the deck loaded last.
    void resolveSeed();
    void updateFilterAvailability();
    void updateHiddenLabel();
    void updateSeedLabel();
    QString selectedProviderKey() const;
    /// Providers in the order the slider shows them: the plain models first,
    /// with any layer probes of a model next to it, and the ensemble last.
    static QList<mixxx::SimilarityProvider> sortedForSlider(
            const QList<mixxx::SimilarityProvider>& providers);

    UserSettingsPointer m_pConfig;
    WTrackTableView* m_pTrackTableView;
    SimilarTrackTableModel* m_pTrackTableModel;
    mixxx::SimilarityClient* m_pClient;

    /// Deck groups that hold a track, most recently loaded first. A deck that
    /// is merely loaded is a perfectly good seed: the DJ is looking at what to
    /// play next, and nothing says he pressed play yet.
    QStringList m_loadedDeckGroups;

    /// Providers as shown on the slider; the slider value is an index here.
    QList<mixxx::SimilarityProvider> m_providers;

    TrackId m_seedTrackId;
    QString m_seedLabel;
    /// True while the seed came from the context menu instead of the decks.
    bool m_seedPinned;
    bool m_providersLoaded;
};
