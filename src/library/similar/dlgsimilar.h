#pragma once

#include <QList>
#include <QWidget>

#include "library/libraryview.h"
#include "library/similar/similarityclient.h"
#include "library/similar/ui_dlgsimilar.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class Library;
class SimilarTrackTableModel;
class WLibrary;
class WTrackTableView;

/// The "Similar" pane: a control strip over a track table, in the shape the
/// Analyze pane already uses (so every skin styles it for free).
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
    /// The seed follows the library selection while the checkbox is ticked.
    void slotTrackSelected(TrackPointer pTrack);

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group);
    void trackSelected(TrackPointer pTrack);

  private slots:
    void slotProvidersReady(const QList<mixxx::SimilarityProvider>& providers, int trackCount);
    void slotSimilarReady(const mixxx::SimilarityResult& result);
    void slotRequestFailed(const QString& message, bool unreachable);
    void slotProviderChanged(int index);
    void slotReloadClicked();

  private:
    void maybeArmScreenshot(Library* pLibrary);
    void requestForSeed();
    void setSeed(TrackPointer pTrack);
    QString selectedProviderKey() const;

    UserSettingsPointer m_pConfig;
    WTrackTableView* m_pTrackTableView;
    SimilarTrackTableModel* m_pTrackTableModel;
    mixxx::SimilarityClient* m_pClient;

    TrackId m_seedTrackId;
    QString m_seedLabel;
    bool m_providersLoaded;
    /// True while our own table is announcing its selection. Library relays that
    /// announcement back to us, and without this guard walking down the result
    /// list with the arrow keys would reseed the search on every row.
    bool m_relayingOwnSelection;
};
