#pragma once

#include <QVariant>

#include "track/track_decl.h"

#include "track/track_decl.h"

#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class DlgSimilar;
class Library;
class WLibrary;
class KeyboardEventFilter;

/// Sidebar entry and pane listing the tracks the annotation stand considers
/// closest to the selected one.
///
/// The feature owns no data: it forwards the selected track to DlgSimilar,
/// which asks the stand over HTTP. See research/mixxx-similar-integration.md.
class SimilarFeature : public LibraryFeature {
    Q_OBJECT

  public:
    SimilarFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SimilarFeature() override = default;

    QVariant title() override;

    void bindLibraryWidget(WLibrary* pLibraryWidget,
            KeyboardEventFilter* pKeyboard) override;
    TreeItemModel* sidebarModel() const override;

    /// Seed the pane with this track and pin it, then show the pane.
    void showSimilarTracks(const TrackPointer& pTrack);

  public slots:
    void activate() override;

  private:
    const QString m_title;
    parented_ptr<TreeItemModel> m_pSidebarModel;
    DlgSimilar* m_pSimilarView;
};
