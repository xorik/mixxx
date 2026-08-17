#include "library/similar/similarfeature.h"

#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/library.h"
#include "library/similar/dlgsimilar.h"
#include "moc_similarfeature.cpp"
#include "widget/wlibrary.h"

namespace {

const QString kViewName = QStringLiteral("Similar");

} // anonymous namespace

SimilarFeature::SimilarFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("promotracks")),
          m_title(tr("Similar")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pSimilarView(nullptr) {
}

QVariant SimilarFeature::title() {
    return m_title;
}

TreeItemModel* SimilarFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void SimilarFeature::bindLibraryWidget(
        WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    m_pSimilarView = new DlgSimilar(pLibraryWidget, m_pConfig, m_pLibrary);
    connect(m_pSimilarView,
            &DlgSimilar::loadTrack,
            this,
            &SimilarFeature::loadTrack);
    connect(m_pSimilarView,
            &DlgSimilar::loadTrackToPlayer,
            this,
            [this](TrackPointer pTrack, const QString& group) {
                emit loadTrackToPlayer(pTrack,
                        group,
#ifdef __STEM__
                        mixxx::StemChannelSelection(),
#endif
                        false);
            });
    connect(m_pSimilarView,
            &DlgSimilar::trackSelected,
            this,
            &SimilarFeature::trackSelected);

    // The seed follows the deck that is playing; DlgSimilar subscribes to
    // PlayerInfo itself. The library selection deliberately seeds nothing - a
    // click in a track list stays a plain click, and a seed picked by hand
    // comes from the track context menu instead.

    m_pSimilarView->installEventFilter(pKeyboard);

    pLibraryWidget->registerView(kViewName, m_pSimilarView);
}

void SimilarFeature::showSimilarTracks(const TrackPointer& pTrack) {
    if (m_pSimilarView) {
        m_pSimilarView->pinSeed(pTrack);
    }
    activate();
}

void SimilarFeature::activate() {
    emit switchToView(kViewName);
    if (m_pSimilarView) {
        emit restoreSearch(m_pSimilarView->currentSearch());
    }
    emit enableCoverArtDisplay(true);
}
