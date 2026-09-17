#include "app/storage_explorer_host.h"

#include "app/viewer_tuning.h"
#include "io/file_content_provider.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "session/storage_session.h"
#include "settings_ini.h"
#include "storage_explorer.h"
#include "storage_explorer_ini.h"

namespace whiteout::flakes {

StorageExplorerHost::StorageExplorerHost(renderer::RenderService& service, io::LoadTaskRunner& tasks,
                                         StorageSession& session, OnActivate onActivate)
    : service_(service), tasks_(tasks), session_(session), onActivate_(std::move(onActivate)) {}

StorageExplorerHost::~StorageExplorerHost() = default;

void StorageExplorerHost::SetOpen(bool on) {
    open_ = on;
    if (!on)
        return;
    if (!explorer_) {
        explorer_ = std::make_unique<tools::StorageExplorer>(service_);
        // Its opens go on the viewer's task thread, behind the viewer's modal. A
        // StarCraft II switch walks three quarters of a million manifest entries.
        explorer_->SetTaskRunner(&tasks_);
        // A double-clicked model opens as a new tab through the explorer's own
        // provider: the viewer wants the path, not the bytes.
        explorer_->SetOnActivate(onActivate_);
        // A product's install path and (for World of Warcraft, the difference
        // between a browse and an empty grid) its listfile, answered per product
        // on demand. The live provider is authoritative for the game it serves —
        // Settings ▸ IO edits and the session-only keys adopted beside a loose
        // model both land there — while the ini is the only record of the games
        // it is not on, which the panel's own combo can still browse.
        explorer_->SetGameKeys([this](ProductId game) {
            auto& provider = session_.Provider();
            tools::GameStorageKeys keys;
            if (provider.Game() == game) {
                keys.installPath = provider.InstallPath();
                keys.listfilePath = provider.ListfilePath();
                keys.tactKeyPath = provider.TactKeyPath();
            } else {
                const IoPathOverrides o = LoadIoPathOverrides(game);
                keys.installPath = o.installPath;
                keys.listfilePath = o.listfilePath;
                keys.tactKeyPath = o.tactKeyPath;
            }
            return keys;
        });
        // Before Sync, which prefers the restored game over the settings
        // profile: where the panel points is the user's own setting.
        explorer_->RestoreState(LoadStorageExplorerState());
        stateKey_.clear();
    }
    explorer_->Sync(session_.SettingsProfile());
}

void StorageExplorerHost::Shutdown() {
    explorer_.reset();
}

void StorageExplorerHost::NewFrame(f32 dt) {
    if (!open_ || !explorer_)
        return;
    explorer_->NewFrame(dt);
    PollState(dt);
}

void StorageExplorerHost::BuildWindow() {
    if (open_ && explorer_)
        explorer_->BuildWindow(&open_);
}

bool StorageExplorerHost::RenderThumbnails(f32 dt) {
    if (!open_ || !explorer_)
        return false;
    explorer_->RenderThumbnails(dt);
    return true;
}

void StorageExplorerHost::PollState(f32 dt) {
    if (!explorer_->IsOpen() || explorer_->Opening())
        return;
    std::string key = ExplorerStateKey(explorer_->State());
    if (key != stateKey_) {
        stateKey_ = std::move(key);
        saveDelay_ = tuning::kExplorerStateSettleSeconds;
        return;
    }
    if (saveDelay_ <= 0.0f)
        return;
    saveDelay_ -= dt;
    if (saveDelay_ <= 0.0f)
        SaveStorageExplorerState(explorer_->State());
}

} // namespace whiteout::flakes
