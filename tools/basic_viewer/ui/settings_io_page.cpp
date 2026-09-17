// The Settings window's IO tab.
//
// These edit a PROFILE, not "the provider". A profile is an ini section plus,
// for whichever product the provider is serving, a live slot inside it. Every
// edit commits to the ini, because that is what a profile is; it reaches the
// provider only when the profile is the active one, and then its storage has to
// reopen, since where it reads from just moved. A profile that is not active has
// nothing open to reopen: it picks the settings up through ApplyIoPathOverrides
// when content first needs it, which is the only moment its CASC should open.

#include "ui/settings_window.h"

#include "app/viewer_app.h"
#include "io/file_content_provider.h"
#include "io/storage/game_rules.h" // ScanArchives, for a profile that is not the active one
#include "localization.h"
#include "session/game_profiles.h"
#include "settings_ini.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/widgets.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace whiteout::flakes {

void SettingsWindow::SeedIoBuffers(io::FileContentProvider& provider, ProductId game) {
    // The provider's getters answer for its ACTIVE slot: the truth for exactly
    // one profile, and the wrong product's answer for the others.
    if (game == provider.Game()) {
        installPath_ = provider.InstallPath();
        hotsPath_ = provider.HotsInstallPath();
        listfile_ = provider.ListfilePath();
        tactKeys_ = provider.TactKeyPath();
        ignoreCasc_ = provider.IgnoreCasc();
        ignoreMpq_ = provider.IgnoreMpq();
        mpqList_ = provider.MpqList();
    } else {
        const IoPathOverrides o = LoadIoPathOverrides(game);
        installPath_ = o.installPath.empty() ? provider.GamePath(game) : o.installPath;
        hotsPath_ = o.hotsInstallPath.empty() ? provider.HotsPath() : o.hotsInstallPath;
        listfile_ = o.listfilePath;
        tactKeys_ = o.tactKeyPath;
        ignoreCasc_ = o.ignoreCasc;
        ignoreMpq_ = o.ignoreMpq;
        // No saved order means the answer the provider would reach: what is on
        // disk for this game. Reading a directory is not opening a storage.
        mpqList_ = o.mpqListSet ? o.mpqList : io::ScanArchives(game, installPath_);
    }
    newMpq_.clear();
}

void SettingsWindow::CommitIoProfile(io::FileContentProvider& provider, ProductId game) {
    IoPathOverrides o;
    // "Same as auto-detected" is stored as no override, so a later reinstall
    // elsewhere is picked up instead of pinned to a stale path.
    o.installPath = (installPath_ == provider.GamePath(game)) ? std::string{} : installPath_;
    o.ignoreCasc = ignoreCasc_;
    o.ignoreMpq = ignoreMpq_;
    o.listfilePath = listfile_;
    o.tactKeyPath = tactKeys_;
    if (game == ProductId::Sc2)
        o.hotsInstallPath = (hotsPath_ == provider.HotsPath()) ? std::string{} : hotsPath_;
    if (!GameProfileOf(game).cascOnly) {
        o.mpqListSet = true;
        o.mpqList = mpqList_;
    }
    SaveIoPathOverrides(game, o);

    if (game != provider.Game())
        return; // not the active profile: nothing is open, so nothing reopens

    // It IS the active profile, so its storage is in use and now points somewhere
    // else. ApplyIoPathOverrides invalidates the slot; the retry's reads rebuild it.
    StorageSession& session = ctx_.app.Session();
    session.ApplyProfile(game, /*force=*/true);
    // The two roots ApplyIoPathOverrides skips when empty: at startup "no
    // override" means "leave the detected path alone", but here the user pressed
    // Reset, and the provider still holds the override they just cleared.
    provider.SetInstallPath(o.installPath);
    if (game == ProductId::Sc2)
        provider.SetHotsInstallPath(o.hotsInstallPath);
    // Opened now, on the task thread behind a bar, rather than left to the next
    // read: this commit used to freeze the window for the length of a retail
    // World of Warcraft open, because whichever thread read next paid for it.
    session.OpenStoragesAsync();
}

void SettingsWindow::BuildIoTab(io::FileContentProvider& provider, ProductId game) {
    // Re-seed on a change of EITHER: a different profile is being edited, or the
    // provider moved onto a different product — which moves where this profile's
    // truth lives, and can bring settings the ini never saw, like keys adopted
    // beside a loose model.
    if (!buffersSeeded_ || buffersGame_ != game || buffersServing_ != provider.Game()) {
        SeedIoBuffers(provider, game);
        buffersSeeded_ = true;
        buffersGame_ = game;
        buffersServing_ = provider.Game();
    }
    if (GameProfileOf(game).cascOnly)
        BuildIoCascPage(provider, game);
    else
        BuildIoArchivePage(provider, game);
    BuildIoStorageStatus(provider, game);
}

// For the profiles not being served the honest answer is that nothing is open —
// "not loaded" would read as a failure when it is the design.
void SettingsWindow::BuildIoStorageStatus(io::FileContentProvider& provider, ProductId game) {
    ImGui::Spacing();
    ImGui::Separator();
    if (game != provider.Game()) {
        ImGui::TextDisabled("%s", i18n::tr("settings.io.inactive_profile"));
        return;
    }
    const bool cascOnly = GameProfileOf(game).cascOnly;
    // Opening first, answered from an atomic: every call below takes the storage
    // lock, which an open holds throughout — asking one of them here would block
    // the UI thread on the very operation the line describes. Pending next:
    // storages open on demand, and reading the status must not be the demand.
    if (provider.StoragesOpening()) {
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"), "opening...");
        return;
    }
    if (provider.StoragesState() == io::StorageState::Cancelled) {
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"), "cancelled");
        if (ImGui::SmallButton("Retry"))
            ctx_.app.Session().OpenStoragesAsync();
        return;
    }
    if (provider.StoragesPending()) {
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"), i18n::tr("settings.io.pending"));
        if (!cascOnly)
            ImGui::TextDisabled(i18n::tr("settings.io.mpq_status"), i18n::tr("settings.io.pending"));
        return;
    }
    // Which roots opened, not just whether any did: StarCraft II offers two and
    // either can fail on its own.
    const auto roots = provider.OpenCascRoots();
    ImGui::TextDisabled(i18n::tr("settings.io.casc_status"),
                        roots.empty() ? i18n::tr("settings.io.not_loaded") : i18n::tr("settings.io.open"));
    for (const auto& r : roots)
        ImGui::TextDisabled("    %s", r.c_str());
    if (!cascOnly)
        ImGui::TextDisabled(i18n::tr("settings.io.mpq_status"),
                            provider.HasMpq() ? i18n::tr("app.yes") : i18n::tr("app.no"));
}

void SettingsWindow::BuildIoArchivePage(io::FileContentProvider& provider, ProductId game) {
    const std::string autoDetected = provider.GamePath(game);
    if (autoDetected.empty())
        ImGui::TextDisabled("%s", i18n::tr(game == ProductId::Wow ? "settings.io.wow_not_detected"
                                                                  : "settings.io.not_detected"));
    else
        ImGui::TextDisabled(i18n::tr("settings.io.auto_detected"), autoDetected.c_str());
    ImGui::Spacing();

    bool commit = false;
    commit |= ui::PathRow({"##install", nullptr, nullptr, "settings.io.browse_install", "settings.io.reset_install",
                           i18n::tr("settings.io.install_path"), ui::kPathRowReserve},
                          installPath_, autoDetected);

    // A World of Warcraft CASC root stores fileDataIDs and name hashes, not
    // paths, so nothing can browse or path-read it without a community `id;path`
    // CSV. Reads by id work regardless.
    if (game == ProductId::Wow) {
        commit |= ui::PathRow({"##listfile", "Listfile", "csv,txt", "settings.io.browse_listfile",
                               "settings.io.clear_listfile", i18n::tr("settings.io.listfile"), ui::kPathRowReserve},
                              listfile_, {});
        // "A path is set" and "it loaded" are different facts, and only the second
        // makes the root browsable. The listfile is read while the storage opens,
        // so before that it is pending like the rest.
        ImGui::TextDisabled(i18n::tr("settings.io.listfile_status"),
                            (game != provider.Game() || provider.StoragesPending()) ? i18n::tr("settings.io.pending")
                            : provider.HasListfile()                               ? i18n::tr("settings.io.loaded")
                                                                                   : i18n::tr("settings.io.not_loaded"));
        // The listfile's twin one layer down: a file holding an encrypted frame
        // reads back as *missing* rather than as an error, so without the keys
        // part of the install is invisible, with nothing to say why.
        commit |= ui::PathRow({"##tactkeys", "TACT keys", "txt,csv", "settings.io.browse_tactkeys",
                               "settings.io.clear_tactkeys", i18n::tr("settings.io.tactkeys"), ui::kPathRowReserve},
                              tactKeys_, {});
    }

    ImGui::Spacing();
    ImGui::Separator();

    commit |= ImGui::Checkbox(i18n::tr("settings.io.ignore_casc"), &ignoreCasc_);
    commit |= ImGui::Checkbox(i18n::tr("settings.io.ignore_mpq"), &ignoreMpq_);

    ImGui::Spacing();
    ImGui::Separator();

    // ---- MPQ load list ----
    // Earlier entries win. Each edit commits, which for the active profile
    // reopens its storages — fine for a settings dialog's edit rate.
    ImGui::TextUnformatted(i18n::tr("settings.io.mpq_header"));
    ImGui::BeginDisabled(ignoreMpq_);

    i32 swapWith = -1; // [i, i+1]
    i32 removeAt = -1;
    for (usize i = 0; i < mpqList_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("up", ImGuiDir_Up))
            swapWith = static_cast<i32>(i) - 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == mpqList_.size());
        if (ImGui::ArrowButton("down", ImGuiDir_Down))
            swapWith = static_cast<i32>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("X"))
            removeAt = static_cast<i32>(i);
        ImGui::SameLine();
        ImGui::TextUnformatted(mpqList_[i].c_str());
        ImGui::PopID();
    }
    if (swapWith >= 0 && swapWith + 1 < static_cast<i32>(mpqList_.size())) {
        std::swap(mpqList_[swapWith], mpqList_[swapWith + 1]);
        commit = true;
    }
    if (removeAt >= 0 && removeAt < static_cast<i32>(mpqList_.size())) {
        mpqList_.erase(mpqList_.begin() + removeAt);
        commit = true;
    }

    ImGui::SetNextItemWidth(-ui::kAddMpqRowReserve);
    ImGui::InputText("##newmpq", &newMpq_);
    ImGui::SameLine();
    ImGui::BeginDisabled(newMpq_.empty());
    if (ImGui::Button(i18n::tr("settings.io.add_mpq"))) {
        mpqList_.push_back(newMpq_);
        newMpq_.clear();
        commit = true;
    }
    ImGui::EndDisabled();

    // "Defaults" is the fixed three for Warcraft III, but for World of Warcraft
    // what is actually in Data/ — its archive names changed twice across the
    // MPQ era, so a static list is wrong for most installs.
    if (ImGui::SmallButton(i18n::tr("settings.io.reset_defaults"))) {
        mpqList_ = io::ScanArchives(game, installPath_);
        commit = true;
    }

    ImGui::EndDisabled();

    if (commit)
        CommitIoProfile(provider, game);
}

// No archives: neither StarCraft II nor Diablo III ever shipped an MPQ.
// StarCraft II has *two* roots — it and Heroes of the Storm are two installs
// behind one product, because they share a render profile.
void SettingsWindow::BuildIoCascPage(io::FileContentProvider& provider, ProductId game) {
    bool commit = false;
    const auto rootRow = [&](const char* id, std::string& path, const std::string& discovered, const char* label) {
        ImGui::PushID(id);
        commit |= ui::PathRow({"##root", nullptr, nullptr, "settings.io.browse_install", "settings.io.reset_install",
                               label, ui::kPathRowReserve},
                              path, discovered);
        if (discovered.empty())
            ImGui::TextDisabled(i18n::tr("settings.io.casc_not_detected"), label);
        ImGui::PopID();
    };

    if (game == ProductId::D3) {
        rootRow("d3", installPath_, provider.GamePath(ProductId::D3), "Diablo III");
    } else {
        rootRow("sc2", installPath_, provider.GamePath(ProductId::Sc2), "StarCraft II");
        ImGui::Spacing();
        rootRow("hots", hotsPath_, provider.HotsPath(), "Heroes of the Storm");
    }

    ImGui::Spacing();
    ImGui::Separator();

    commit |= ImGui::Checkbox(i18n::tr("settings.io.ignore_casc"), &ignoreCasc_);

    if (commit)
        CommitIoProfile(provider, game);
}

} // namespace whiteout::flakes
