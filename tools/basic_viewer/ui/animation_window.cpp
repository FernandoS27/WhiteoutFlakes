#include "ui/animation_window.h"

#include "app/viewer_app.h"
#include "features/sc2_animation_files.h"
#include "localization.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/widgets.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>
#include <nfd.hpp>

namespace whiteout::flakes {

AnimationWindow::AnimationWindow(UiContext& ctx) : ctx_(ctx) {}

void AnimationWindow::Build() {
    Sc2AnimationFiles* sc2 = ctx_.app.Features().Sc2();
    if (!open_ || !sc2 || !sc2->CanAttach())
        return;
    PlaybackController& playback = ctx_.app.Playback();

    ImGui::SetNextWindowSize(ImVec2(ui::kAnimationWindowWidth, ui::kAnimationWindowHeight), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(i18n::tr("anim.title"), &open_)) {
        ImGui::End();
        return;
    }

    // ---- Global loops ----
    const auto globals = playback.GlobalLoops();
    if (!globals.empty() && ImGui::CollapsingHeader(i18n::tr("anim.globals"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", i18n::tr("anim.globals.help"));
        for (const auto& g : globals) {
            bool on = g.enabled;
            ImGui::PushID(g.sequence);
            if (ImGui::Checkbox(g.name.c_str(), &on))
                playback.SetGlobalLoopEnabled(g.sequence, on);
            if (const auto subs = sc2->SubtracksOf(g.sequence); !subs.empty()) {
                std::string parts;
                for (const auto& s : subs) {
                    if (!parts.empty())
                        parts += ", ";
                    parts += s.name;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", parts.c_str());
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
    }

    // ---- Tracks ----
    if (ImGui::CollapsingHeader(i18n::tr("anim.tracks"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", i18n::tr("anim.tracks.help"));
        const auto& tracks = playback.Tracks();
        if (!tracks.empty() && ImGui::BeginTable("##tracks", 5, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(i18n::tr("anim.track.sequence"), ImGuiTableColumnFlags_WidthStretch,
                                    ui::kTrackSequenceWeight);
            ImGui::TableSetupColumn(i18n::tr("anim.track.subtrack"), ImGuiTableColumnFlags_WidthStretch,
                                    ui::kTrackSubtrackWeight);
            ImGui::TableSetupColumn(i18n::tr("anim.track.weight"), ImGuiTableColumnFlags_WidthStretch,
                                    ui::kTrackWeightWeight);
            ImGui::TableSetupColumn(i18n::tr("anim.track.loop"), ImGuiTableColumnFlags_WidthFixed,
                                    ui::kTrackLoopColumnWidth);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, ui::kTrackRemoveColumnWidth);
            ImGui::TableHeadersRow();
            // One removal per frame: a row that removed itself invalidated the
            // list this loop walks.
            for (usize i = 0; i < tracks.size(); ++i) {
                if (!BuildTrackRow(*sc2, i))
                    break;
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(i18n::tr("anim.tracks.add")))
            playback.AddTrack();
        ImGui::Spacing();
    }

    // ---- Animation files ----
    if (ImGui::CollapsingHeader(i18n::tr("anim.files"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto attached = sc2->AttachedFiles();
        if (attached.empty()) {
            ImGui::TextDisabled("%s", i18n::tr("toolbar.animfiles.none"));
        } else {
            for (usize i = 0; i < attached.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                // Detach, then stop building this list: it is a snapshot, and the
                // entries after the removed one shift.
                const bool drop = ImGui::SmallButton("x");
                ImGui::SameLine();
                ImGui::Text("%s", attached[i].label.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu)", attached[i].sequenceCount);
                ImGui::PopID();
                if (drop) {
                    sc2->Detach(i);
                    break;
                }
            }
        }
        if (ImGui::Button(i18n::tr("toolbar.animfiles.add")))
            AttachFiles(*sc2);
        if (!attachError_.empty())
            ui::ErrorText(i18n::tr("toolbar.animfiles.failed"), attachError_.c_str());
    }

    ImGui::End();
}

bool AnimationWindow::BuildTrackRow(Sc2AnimationFiles& sc2, usize index) {
    PlaybackController& playback = ctx_.app.Playback();
    const auto& tracks = playback.Tracks();
    if (index >= tracks.size())
        return true;
    AnimTrackInfo t = tracks[index].info;
    const auto& seqs = playback.SequenceNames();
    bool changed = false;
    bool keep = true;

    ImGui::PushID(static_cast<int>(index));
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* preview =
        (t.sequence >= 0 && t.sequence < static_cast<i32>(seqs.size())) ? seqs[t.sequence].c_str() : "";
    if (ImGui::BeginCombo("##seq", preview)) {
        for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
            if (ImGui::Selectable(seqs[i].c_str(), i == t.sequence) && i != t.sequence) {
                t.sequence = i;
                // The old index named a container of the old sequence, and the
                // groups are not parallel. Back to the whole sequence.
                t.subtrack.reset();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    // Sub-track. A sequence with one container has nothing to choose, so the
    // cell names it instead of offering a combo with one entry.
    ImGui::TableNextColumn();
    const auto subs = sc2.SubtracksOf(t.sequence);
    if (subs.size() <= 1) {
        ImGui::TextDisabled("%s", subs.empty() ? "-" : subs[0].name.c_str());
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* subPreview = i18n::tr("anim.track.all");
        if (t.subtrack && *t.subtrack >= 0 && *t.subtrack < static_cast<i32>(subs.size()))
            subPreview = subs[static_cast<usize>(*t.subtrack)].name.c_str();
        if (ImGui::BeginCombo("##sub", subPreview)) {
            if (ImGui::Selectable(i18n::tr("anim.track.all"), !t.subtrack) && t.subtrack) {
                t.subtrack.reset();
                changed = true;
            }
            for (i32 i = 0; i < static_cast<i32>(subs.size()); ++i) {
                if (ImGui::Selectable(subs[i].name.c_str(), t.subtrack == i) && t.subtrack != i) {
                    t.subtrack = i;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    // Priority decides which container wins a property; concurrency
                    // decides whether it leaves the rest of the skeleton alone.
                    ImGui::SetTooltip("%s %u  -  %s  -  %s %zu", i18n::tr("anim.track.priority"),
                                      static_cast<unsigned>(subs[i].priority),
                                      i18n::tr(subs[i].concurrent ? "anim.track.concurrent" : "anim.track.exclusive"),
                                      i18n::tr("anim.track.tracks"), subs[i].trackCount);
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragFloat("##w", &t.weight, 0.01f, 0.0f, 1.0f, "%.2f"))
        changed = true;

    ImGui::TableNextColumn();
    if (ImGui::Checkbox("##loop", &t.loop))
        changed = true;

    ImGui::TableNextColumn();
    if (ImGui::SmallButton("x"))
        keep = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", i18n::tr("anim.track.remove"));

    ImGui::PopID();

    if (!keep) {
        playback.RemoveTrack(index);
        return false;
    }
    if (changed)
        playback.SetTrack(index, t);
    return true;
}

void AnimationWindow::AttachFiles(Sc2AnimationFiles& sc2) {
    attachError_.clear();
    NFD::UniquePathSet outPaths;
    nfdu8filteritem_t filter[1] = {{"StarCraft II animations", "m3a"}};
    if (NFD::OpenDialogMultiple(outPaths, filter, 1) != NFD_OKAY)
        return;
    nfdpathsetsize_t count = 0;
    if (NFD::PathSet::Count(outPaths, count) != NFD_OKAY)
        return;
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        NFD::UniquePathSetPathU8 path;
        if (NFD::PathSet::GetPath(outPaths, i, path) != NFD_OKAY)
            continue;
        // A pick can be refused — unparseable, no sequences, or a stem already
        // attached — and the window keeps the last one refused.
        const std::filesystem::path p = io::FsPathFromUtf8(path.get());
        if (!sc2.Attach(p))
            attachError_ = io::PathToUtf8(p.filename());
    }
}

} // namespace whiteout::flakes
