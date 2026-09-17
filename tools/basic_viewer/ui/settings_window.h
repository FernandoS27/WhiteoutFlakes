#pragma once

// ============================================================================
// The Settings window: a profile list on the left — the General page, then one
// row per game — and the picked row's pages on the right.
//
// The picked game is StorageSession::SettingsProfile(), deliberately NOT the
// provider's active game: editing a profile must not repoint the content layer,
// let alone open its install.
// ============================================================================

#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

#include <string>
#include <vector>

namespace whiteout::flakes::io {
class FileContentProvider;
}

namespace whiteout::flakes {

struct UiContext;

class SettingsWindow {
public:
    explicit SettingsWindow(UiContext& ctx);

    void Open() {
        open_ = true;
    }
    void Build();

    /// The `--ui-shot` harness: the General page, or @p game's page with its IO
    /// tab selected when @p io. Nothing is persisted.
    void ShowGeneralForShot();
    void ShowProfileForShot(ProductId game, bool io);

private:
    // ---- settings_general_pages.cpp ----
    /// What no game owns: the frame the renderer draws for all of them and the
    /// knobs the next launch reads. Its own row above the games, because picking
    /// a game to change the exposure answered the wrong question.
    void BuildGeneralPage();
    /// What one game's data means, from the per-product page table.
    void BuildProfilePage(ProductId game);
    void BuildWc3Page();
    void BuildWowPage();
    void BuildSc2Page();
    void BuildD3Page();
    void BuildBackendChoice();
    void BuildDeviceChoice();

    // ---- settings_io_page.cpp ----
    void BuildIoTab(io::FileContentProvider& provider, ProductId game);
    /// Warcraft III and World of Warcraft: one install root, the per-storage
    /// ignore switches, an editable MPQ load order. WoW adds a listfile and TACT keys.
    void BuildIoArchivePage(io::FileContentProvider& provider, ProductId game);
    /// StarCraft II / Heroes and Diablo III: CASC roots and no MPQs, ever.
    void BuildIoCascPage(io::FileContentProvider& provider, ProductId game);
    /// Live storage state, which only the profile being served has.
    void BuildIoStorageStatus(io::FileContentProvider& provider, ProductId game);
    /// The buffers from the live provider when @p game is the one it serves,
    /// from the ini otherwise.
    void SeedIoBuffers(io::FileContentProvider& provider, ProductId game);
    /// The buffers back as @p game's profile — always to the ini, and to the
    /// provider (reopening its storage) only when it serves @p game.
    void CommitIoProfile(io::FileContentProvider& provider, ProductId game);

    UiContext& ctx_;
    bool open_ = false;
    /// The shared page, or the game SettingsProfile() names. Not persisted: the
    /// shared page is where the window opens.
    bool generalPage_ = true;
    bool shotIoTab_ = false;

    /// The devices the preferred-device combo lists, enumerated again only when
    /// the default backend changes.
    std::vector<std::string> devices_;
    i32 devicesBackend_ = -1;

    // IO edit buffers: the whole of one profile's settings. The page's state,
    // not a mirror of the provider — the provider holds only the ACTIVE profile,
    // and these have to be able to hold any.
    std::string installPath_;
    std::string hotsPath_;   // StarCraft II page: the Heroes root
    std::string listfile_;   // World of Warcraft page: the `id;path` CSV
    std::string tactKeys_;   // World of Warcraft page: the TACT key list
    std::string newMpq_;
    bool ignoreCasc_ = false;
    bool ignoreMpq_ = false;
    std::vector<std::string> mpqList_;
    bool buffersSeeded_ = false;
    /// Which profile the buffers hold, and which product the provider served
    /// when they were filled. Either changing re-seeds them.
    ProductId buffersGame_ = ProductId::Wc3;
    ProductId buffersServing_ = ProductId::Wc3;
};

} // namespace whiteout::flakes
