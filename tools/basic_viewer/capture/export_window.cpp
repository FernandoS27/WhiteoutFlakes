#include "capture/export_window.h"

#include "capture/capture_sinks.h"
#include "capture/export_ini.h"
#include "capture/export_runner.h"
#include "io/load_task.h"
#include "localization.h"
#include "renderer/model/model_instance.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "color_pack.h"
#include "string_util.h"
#include "app/viewer_app.h"
#include "ui/ui_metrics.h"
#include "ui/widgets.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nfd.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;

namespace {

namespace fs = std::filesystem;

const char* T(const char* key) {
    return i18n::tr(key);
}

// One formatted label, living as long as the expression that passes it on —
// ImGui reads the text inside the call it is handed to.
class Fmt {
public:
    explicit Fmt(const char* format, ...) {
        va_list args;
        va_start(args, format);
        std::vsnprintf(text_, sizeof(text_), format, args);
        va_end(args);
    }
    operator const char*() const {
        return text_;
    }

private:
    char text_[512];
};

// The timeline's clip blocks alternate, and the fill after the last clip is greyed.
constexpr ImU32 kTimelineBackground = IM_COL32(35, 35, 40, 255);
constexpr ImU32 kTimelineBlockA = IM_COL32(80, 110, 150, 255);
constexpr ImU32 kTimelineBlockB = IM_COL32(70, 130, 120, 255);
constexpr ImU32 kTimelineFill = IM_COL32(90, 90, 100, 200);
constexpr ImU32 kTimelineSecondTick = IM_COL32(200, 200, 200, 120);
constexpr ImU32 kReportSuccess = IM_COL32(120, 200, 120, 255);

std::string HumanBytes(u64 bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    f64 v = static_cast<f64>(bytes);
    i32 u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), (u == 0 || v >= 100.0) ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

// Rough output size, for the footer. Deliberately labelled "~": the point is
// to separate "a few megabytes" from "two gigabytes", not to be right.
u64 EstimateOutputBytes(ExportFormat format, i32 frames, i32 w, i32 h) {
    const f64 px = static_cast<f64>(w) * h * std::max(0, frames);
    switch (format) {
    case ExportFormat::PngFrames:
    case ExportFormat::PngSheet:
        return static_cast<u64>(px * 1.6);
    case ExportFormat::Gif:
        return static_cast<u64>(px * 0.45);
    case ExportFormat::Apng:
        return static_cast<u64>(px * 1.8);
    case ExportFormat::Webp:
        return static_cast<u64>(px * 1.1);
    case ExportFormat::Mp4:
    case ExportFormat::WebmVp9:
        return static_cast<u64>(px * 0.12);
    }
    return static_cast<u64>(px);
}

using ui::HelpMarker;
using ui::Warn;
constexpr ImU32 kAmber = ui::kWarnAmber;
constexpr ImU32 kRed = ui::kWarnRed;

} // namespace

// ---------------------------------------------------------------------------

ExportWindow::ExportWindow(ViewerApp& app) : app_(app) {}

void ExportWindow::LoadIfNeeded() {
    if (loaded_)
        return;
    loaded_ = true;
    ExportRecipeLoadReport report;
    LoadExportRecipe(recipe_, app_.Playback().SequenceNames(), &report);
    resolvedAgainst_ = app_.Playback().SequenceNames();
    folderBuf_ = io::PathToUtf8(recipe_.output.folder);
    nameBuf_ = recipe_.output.nameTemplate;
    resMode_ = (recipe_.output.width > 0 && recipe_.output.height > 0) ? 1 : 0;
    preset_ = report.hadSection ? 4 /* Custom */ : 0;
}

void ExportWindow::Open(i32 seedSequence) {
    LoadIfNeeded();
    OnModelChanged();
    if (recipe_.clips.empty()) {
        // Opening with an empty queue and pressing Export must do what the
        // old dialog did, so seed it with what the toolbar is showing.
        ApplyPreset(0);
        if (!recipe_.clips.empty())
            recipe_.clips[0].sequence = seedSequence;
    }
    if (folderBuf_.empty() && !app_.Documents().ActiveState().modelPath.empty())
        folderBuf_ = io::PathToUtf8(app_.Documents().ActiveState().modelPath.parent_path());
    // Whether the video formats can be offered takes a process launch to learn,
    // so the task thread asks and the format row says it is checking until then.
    if (!FfmpegProbed()) {
        app_.Tasks().Run(
            "Checking for ffmpeg",
            [](io::ProgressMonitor&) {
                FfmpegAvailable();
                return io::TaskResult::Ok();
            },
            {}, /*cancellable=*/false, /*modal=*/false);
    }
    open_ = true;
}

void ExportWindow::Close() {
    if (!open_)
        return;
    open_ = false;
    previewing_ = false;
    Save();
}

void ExportWindow::Save() {
    if (!loaded_)
        return;
    recipe_.output.folder = io::FsPathFromUtf8(folderBuf_);
    recipe_.output.nameTemplate = nameBuf_;
    SaveExportRecipe(recipe_, app_.Playback().SequenceNames(), app_.Documents().ActiveState().modelPath);
}

void ExportWindow::OnModelChanged() {
    const auto& names = app_.Playback().SequenceNames();
    if (names == resolvedAgainst_)
        return;
    // Re-point every clip by name. A clip the new model does not have stays in
    // the queue as an unresolved row: dropping it silently is how a user ends
    // up exporting the wrong thing without knowing.
    for (ExportClip& clip : recipe_.clips) {
        std::string key = clip.savedName;
        if (key.empty())
            key = SequenceKey(resolvedAgainst_, clip.sequence);
        clip.savedName = key;
        clip.sequence = ResolveSequenceKey(names, key);
    }
    resolvedAgainst_ = names;
    previewing_ = false;
    scrubFrame_ = 0;
}

void ExportWindow::MarkCustom() {
    preset_ = 4;
}

void ExportWindow::RebuildSchedule() {
    schedule_ = ExportSchedule::Build(recipe_, app_.Playback().Sequences());
}

const char* ExportWindow::SequenceLabel(const ExportClip& clip) const {
    const auto& names = app_.Playback().SequenceNames();
    if (clip.Resolved() && clip.sequence < static_cast<i32>(names.size()))
        return names[static_cast<usize>(clip.sequence)].c_str();
    return clip.savedName.empty() ? "(missing)" : clip.savedName.c_str();
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

void ExportWindow::ApplyPreset(i32 preset) {
    preset_ = preset;
    const auto& names = app_.Playback().SequenceNames();
    const renderer::model::Actor* focus = app_.Playback().FocusActor();
    i32 current = focus ? focus->animation.ActiveSequenceIndex() : 0;
    if (!names.empty())
        current = std::clamp(current, 0, static_cast<i32>(names.size()) - 1);

    const auto oneClip = [&](i32 seq) {
        recipe_.clips.clear();
        ExportClip c;
        c.sequence = seq;
        c.savedName = SequenceKey(names, seq);
        recipe_.clips.push_back(std::move(c));
    };

    switch (preset) {
    case 0: // Current animation — byte-identical to the original export
        oneClip(current);
        recipe_.timing.duration = ExportDuration::Clips;
        recipe_.camera.mode = ExportCameraMode::Viewport;
        recipe_.camera.angleCount = 1;
        recipe_.output.autoCrop = false;
        break;
    case 1: // All animations
        recipe_.clips.clear();
        for (i32 i = 0; i < static_cast<i32>(names.size()); ++i) {
            ExportClip c;
            c.sequence = i;
            c.savedName = SequenceKey(names, i);
            recipe_.clips.push_back(std::move(c));
        }
        recipe_.timing.duration = ExportDuration::Clips;
        recipe_.camera.mode = ExportCameraMode::Viewport;
        recipe_.camera.angleCount = 1;
        break;
    case 2: // Turntable
        oneClip(current);
        recipe_.timing.duration = ExportDuration::Fixed;
        recipe_.timing.durationMs = 5000;
        recipe_.timing.fill = ExportFill::LoopLast;
        recipe_.camera.mode = ExportCameraMode::Orbit;
        recipe_.camera.subject = OrbitSubject::Camera;
        recipe_.camera.timing = OrbitTiming::Revolutions;
        recipe_.camera.revolutions = 1.0f;
        recipe_.camera.fitToBounds = true;
        recipe_.camera.angleCount = 1;
        break;
    case 3: // Sprite sheet
        oneClip(current);
        recipe_.timing.duration = ExportDuration::Clips;
        recipe_.camera.mode = ExportCameraMode::Orbit;
        recipe_.camera.subject = OrbitSubject::Model;
        recipe_.camera.timing = OrbitTiming::Velocity;
        recipe_.camera.degPerSec = 0.0f;
        recipe_.camera.fitToBounds = true;
        recipe_.camera.angleCount = 8;
        recipe_.output.format = ExportFormat::PngSheet;
        recipe_.output.transparent = true;
        recipe_.output.autoCrop = true;
        break;
    default:
        break;
    }
    rowExpanded_.assign(recipe_.clips.size(), 0);
    scrubFrame_ = 0;
}

void ExportWindow::BuildPresetRow() {
    static const char* kPresetKeys[] = {
        "dialog.export.preset.current", "dialog.export.preset.all", "dialog.export.preset.turntable",
        "dialog.export.preset.sheet",   "dialog.export.preset.custom",
    };
    ImGui::TextUnformatted(T("dialog.export.preset"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ui::kCapturePresetComboWidth);
    if (ImGui::BeginCombo("##exportpreset", T(kPresetKeys[std::clamp(preset_, 0, 4)]))) {
        for (i32 i = 0; i < 5; ++i) {
            // Custom is a state, not a choice: it lights up when you edit
            // something, and picking it would mean nothing.
            ImGui::BeginDisabled(i == 4);
            if (ImGui::Selectable(T(kPresetKeys[i]), preset_ == i))
                ApplyPreset(i);
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0.0f, 24.0f);
    if (ImGui::Button(T("dialog.export.recipe_save"))) {
        NFD::UniquePathU8 out;
        nfdu8filteritem_t filter[] = {{"Export recipe", "ini"}};
        if (NFD::SaveDialog(out, filter, 1, nullptr, "recipe.ini") == NFD_OKAY) {
            recipe_.output.folder = io::FsPathFromUtf8(folderBuf_);
            recipe_.output.nameTemplate = nameBuf_;
            WriteExportRecipeFile(io::FsPathFromUtf8(out.get()), recipe_, app_.Playback().SequenceNames(),
                                  app_.Documents().ActiveState().modelPath);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("dialog.export.recipe_load"))) {
        NFD::UniquePathU8 out;
        nfdu8filteritem_t filter[] = {{"Export recipe", "ini"}};
        if (NFD::OpenDialog(out, filter, 1) == NFD_OKAY) {
            ExportRecipeLoadReport report;
            if (ReadExportRecipeFile(io::FsPathFromUtf8(out.get()), recipe_, app_.Playback().SequenceNames(),
                                     &report)) {
                folderBuf_ = io::PathToUtf8(recipe_.output.folder);
                nameBuf_ = recipe_.output.nameTemplate;
                resMode_ = (recipe_.output.width > 0 && recipe_.output.height > 0) ? 1 : 0;
                rowExpanded_.assign(recipe_.clips.size(), 0);
                resolvedAgainst_ = app_.Playback().SequenceNames();
                MarkCustom();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The queue
// ---------------------------------------------------------------------------

void ExportWindow::BuildQueue() {
    ImGui::SeparatorText(T("dialog.export.animations"));

    const auto& ranges = app_.Playback().Sequences();
    rowExpanded_.resize(recipe_.clips.size(), 0);

    i32 removeAt = -1, moveFrom = -1, moveTo = -1;
    for (usize i = 0; i < recipe_.clips.size(); ++i) {
        ExportClip& clip = recipe_.clips[i];
        ImGui::PushID(static_cast<i32>(i));
        // Where the row's ↑ ↓ x group starts, measured before anything is on
        // the line (see below).
        const f32 rowButtonsX =
            ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 88.0f;

        const bool resolved = clip.Resolved() && clip.sequence < static_cast<i32>(ranges.size());
        bool expanded = rowExpanded_[i] != 0;
        if (ImGui::ArrowButton("##expand", expanded ? ImGuiDir_Down : ImGuiDir_Right))
            rowExpanded_[i] = expanded ? 0 : 1;
        ImGui::SameLine();

        if (!resolved) {
            // An unresolved clip stays visible with its reason. It contributes
            // no frames and the footer says so.
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted(Fmt("%s  -  %s", SequenceLabel(clip),
                                       T("dialog.export.clip_missing")));
            ImGui::PopStyleColor();
        } else {
            ImGui::SetNextItemWidth(ui::kCaptureClipComboWidth);
            if (ImGui::BeginCombo("##seq", SequenceLabel(clip))) {
                const auto& names = app_.Playback().SequenceNames();
                for (i32 s = 0; s < static_cast<i32>(names.size()); ++s) {
                    if (ImGui::Selectable(Fmt("%s##%d", names[static_cast<usize>(s)].c_str(), s),
                                          s == clip.sequence)) {
                        clip.sequence = s;
                        clip.savedName = SequenceKey(names, s);
                        MarkCustom();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::kCaptureRepeatsWidth);
            if (ImGui::DragInt("##repeats", &clip.repeats, 0.1f, 1, 999, "x%d")) {
                clip.repeats = std::clamp(clip.repeats, 1, 999);
                MarkCustom();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("dialog.export.repeats_tip"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::kCaptureSpeedWidth);
            if (ImGui::DragFloat("##speed", &clip.speed, 0.01f, 0.05f, 10.0f, "%.2fx")) {
                clip.speed = std::clamp(clip.speed, 0.05f, 10.0f);
                MarkCustom();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("dialog.export.speed_tip"));

            // The clip's own contributed duration, live.
            const SequenceInfo& seq = ranges[static_cast<usize>(clip.sequence)];
            const i32 len = std::max(0, seq.endMs - seq.startMs);
            const i32 trimEnd = clip.trimEndMs > 0 ? std::min(clip.trimEndMs, len) : len;
            const i32 body = std::max(0, trimEnd - std::clamp(clip.trimStartMs, 0, len));
            const f32 total =
                (body / std::max(0.05f, clip.speed)) * clip.repeats + clip.holdMs;
            ImGui::SameLine();
            ImGui::TextDisabled("%.2f s", total / 1000.0f);
        }

        ImGui::SameLine(rowButtonsX);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
            moveFrom = static_cast<i32>(i);
            moveTo = static_cast<i32>(i) - 1;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < recipe_.clips.size()) {
            moveFrom = static_cast<i32>(i);
            moveTo = static_cast<i32>(i) + 1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            removeAt = static_cast<i32>(i);

        // The rare fields, behind the row's own expander: 95% of rows never
        // need them, and putting them in the row makes every row unreadable.
        if (rowExpanded_[i] && resolved) {
            ImGui::Indent(24.0f);
            const SequenceInfo& seq = ranges[static_cast<usize>(clip.sequence)];
            const i32 len = std::max(0, seq.endMs - seq.startMs);
            ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
            if (ImGui::DragInt(T("dialog.export.hold"), &clip.holdMs, 5.0f, 0, 60000, "%d ms"))
                MarkCustom();
            HelpMarker(T("dialog.export.hold_tip"));
            ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
            if (ImGui::DragInt(T("dialog.export.blend"), &clip.blendMs, 1.0f, 0, 5000, "%d ms"))
                MarkCustom();
            HelpMarker(T("dialog.export.blend_tip"));
            ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
            if (ImGui::DragInt(T("dialog.export.trim_start"), &clip.trimStartMs, 1.0f, 0, len,
                               "%d ms"))
                MarkCustom();
            ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
            if (ImGui::DragInt(T("dialog.export.trim_end"), &clip.trimEndMs, 1.0f, 0, len,
                               clip.trimEndMs > 0 ? "%d ms" : "(end)"))
                MarkCustom();
            ImGui::Unindent(24.0f);
        }
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        recipe_.clips.erase(recipe_.clips.begin() + removeAt);
        rowExpanded_.erase(rowExpanded_.begin() + removeAt);
        MarkCustom();
    } else if (moveFrom >= 0 && moveTo >= 0) {
        std::swap(recipe_.clips[static_cast<usize>(moveFrom)],
                  recipe_.clips[static_cast<usize>(moveTo)]);
        std::swap(rowExpanded_[static_cast<usize>(moveFrom)],
                  rowExpanded_[static_cast<usize>(moveTo)]);
        MarkCustom();
    }

    if (recipe_.clips.empty())
        ImGui::TextDisabled("%s", T("dialog.export.queue_empty"));

    if (ImGui::Button(T("dialog.export.add"))) {
        addSelected_.assign(app_.Playback().SequenceNames().size(), 0);
        addSearch_.clear();
        openAddPopup_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("dialog.export.add_all"))) {
        const auto& names = app_.Playback().SequenceNames();
        for (i32 i = 0; i < static_cast<i32>(names.size()); ++i) {
            ExportClip c;
            c.sequence = i;
            c.savedName = SequenceKey(names, i);
            recipe_.clips.push_back(std::move(c));
        }
        rowExpanded_.assign(recipe_.clips.size(), 0);
        MarkCustom();
    }
    BuildAddClipPopup();
}

void ExportWindow::BuildAddClipPopup() {
    if (openAddPopup_) {
        ImGui::OpenPopup("##addclips");
        openAddPopup_ = false;
    }
    if (!ImGui::BeginPopup("##addclips"))
        return;

    const auto& names = app_.Playback().SequenceNames();
    addSelected_.resize(names.size(), 0);

    ImGui::SetNextItemWidth(ui::kCaptureSearchWidth);
    ImGui::InputTextWithHint("##search", T("dialog.export.search"), &addSearch_);


    ImGui::BeginChild("##addlist", ImVec2(ui::kCaptureAddListWidth, ui::kCaptureAddListHeight), ImGuiChildFlags_Borders);
    for (i32 i = 0; i < static_cast<i32>(names.size()); ++i) {
        if (!tools::ContainsIgnoreCase(names[static_cast<usize>(i)], addSearch_))
            continue;
        bool sel = addSelected_[static_cast<usize>(i)] != 0;
        if (ImGui::Checkbox(Fmt("%s##add%d", names[static_cast<usize>(i)].c_str(), i), &sel))
            addSelected_[static_cast<usize>(i)] = sel ? 1 : 0;
    }
    ImGui::EndChild();

    if (ImGui::Button(T("dialog.export.add_selected"))) {
        // Multi-select, so building a five-clip queue is one dialog rather
        // than five round trips.
        for (i32 i = 0; i < static_cast<i32>(addSelected_.size()); ++i) {
            if (!addSelected_[static_cast<usize>(i)])
                continue;
            ExportClip c;
            c.sequence = i;
            c.savedName = SequenceKey(names, i);
            recipe_.clips.push_back(std::move(c));
        }
        rowExpanded_.assign(recipe_.clips.size(), 0);
        MarkCustom();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("app.cancel")))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

void ExportWindow::BuildTimeline() {
    const renderer::model::Actor* focus = app_.Playback().FocusActor();
    if (!focus)
        return;
    const ExportSchedule& schedule = schedule_;
    if (schedule.FrameCount() <= 0)
        return;

    const f32 width = ImGui::GetContentRegionAvail().x;
    const f32 height = 26.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const i32 frames = schedule.FrameCount();
    const auto starts = schedule.SegmentStarts();
    const auto clips = schedule.SegmentClips();
    const i32 fillStart = schedule.FillStartFrame();

    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), kTimelineBackground);

    for (usize s = 0; s < clips.size(); ++s) {
        const i32 begin = starts[s];
        const i32 end = (s + 1 < starts.size()) ? starts[s + 1] : frames;
        const f32 x0 = origin.x + width * (static_cast<f32>(begin) / frames);
        const f32 x1 = origin.x + width * (static_cast<f32>(end) / frames);
        const bool isFill = begin >= fillStart;
        const ImU32 col = isFill ? kTimelineFill : ((s % 2) ? kTimelineBlockB : kTimelineBlockA);
        dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1 - 1.0f, origin.y + height), col);
        // Name in-block when it fits — that is what makes a queue legible at
        // a glance rather than a row of coloured bars.
        const i32 c = clips[s];
        if (!isFill && c >= 0 && c < static_cast<i32>(recipe_.clips.size())) {
            const char* name = SequenceLabel(recipe_.clips[static_cast<usize>(c)]);
            if (ImGui::CalcTextSize(name).x < (x1 - x0) - 6.0f)
                dl->AddText(ImVec2(x0 + 4.0f, origin.y + 5.0f), IM_COL32_WHITE, name);
        }
    }

    // One tick a second.
    const i32 seconds = schedule.DurationMs() / 1000;
    for (i32 s = 0; s <= seconds; ++s) {
        const f32 x = origin.x + width * (static_cast<f32>(s * schedule.Fps()) / frames);
        dl->AddLine(ImVec2(x, origin.y + height - 5.0f), ImVec2(x, origin.y + height), kTimelineSecondTick);
    }

    // The caret.
    scrubFrame_ = std::clamp(scrubFrame_, 0, frames - 1);
    const f32 caretX = origin.x + width * (static_cast<f32>(scrubFrame_) / frames);
    dl->AddLine(ImVec2(caretX, origin.y), ImVec2(caretX, origin.y + height), IM_COL32_WHITE, 2.0f);

    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    // The export builds this same ImGui frame when capturing the UI overlay.
    // Drawing the dialog into the recording is fine; scrubbing the model from
    // inside it, while the runner owns the clock, is not.
    if (ImGui::IsItemActive() && width > 0.0f && !app_.Capture().IsRunning()) {
        // Dragging the caret scrubs the recipe: the model poses at that point
        // in the recording and, in orbit mode, the camera moves to that
        // frame's yaw. It is At(i) plus a scrub, both of which the runner
        // already needs.
        const f32 t = (ImGui::GetIO().MousePos.x - origin.x) / width;
        scrubFrame_ = std::clamp(static_cast<i32>(t * frames), 0, frames - 1);
        app_.Capture().Scrub(recipe_, scrubFrame_, true);
        previewing_ = false;
    }
    ImGui::TextDisabled(T("dialog.export.timeline_hint"), scrubFrame_,
                        ExportSchedule::ClockAtFrame(scrubFrame_, schedule.Fps()) / 1000.0f);
}

// ---------------------------------------------------------------------------
// Length
// ---------------------------------------------------------------------------

void ExportWindow::BuildLength() {
    ImGui::SeparatorText(T("dialog.export.length"));
    const auto& ranges = app_.Playback().Sequences();
    const ExportSchedule& schedule = schedule_;

    bool clipsMode = recipe_.timing.duration == ExportDuration::Clips;
    if (ImGui::RadioButton(T("dialog.export.len_clips"), clipsMode)) {
        recipe_.timing.duration = ExportDuration::Clips;
        MarkCustom();
    }
    ImGui::SameLine();
    {
        // The queue's own length, so "Match the animations" states its result
        // rather than making the user infer it.
        ExportRecipe probe = recipe_;
        probe.timing.duration = ExportDuration::Clips;
        const ExportSchedule natural = ExportSchedule::Build(probe, ranges);
        ImGui::TextDisabled("%.2f s", natural.DurationMs() / 1000.0f);
    }

    if (ImGui::RadioButton(T("dialog.export.len_fixed"), !clipsMode)) {
        recipe_.timing.duration = ExportDuration::Fixed;
        MarkCustom();
    }
    ImGui::SameLine();
    // Disabled rather than hidden: the shape of the dialog must not change as
    // you click around it.
    ImGui::BeginDisabled(clipsMode);
    f32 seconds = recipe_.timing.durationMs / 1000.0f;
    ImGui::SetNextItemWidth(ui::kCaptureDurationWidth);
    if (ImGui::DragFloat("##dur", &seconds, 0.05f, 0.05f, 3600.0f, "%.2f s")) {
        recipe_.timing.durationMs = std::max(50, static_cast<i32>(std::lround(seconds * 1000.0f)));
        MarkCustom();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(T("dialog.export.then"));
    ImGui::SameLine();
    {
        static const char* kFillKeys[] = {"dialog.export.fill_loop_last",
                                          "dialog.export.fill_loop_queue",
                                          "dialog.export.fill_hold_last"};
        ImGui::SetNextItemWidth(ui::kCaptureClipComboWidth);
        if (ImGui::BeginCombo("##fill", T(kFillKeys[static_cast<i32>(recipe_.timing.fill)]))) {
            for (i32 i = 0; i < 3; ++i)
                if (ImGui::Selectable(T(kFillKeys[i]), static_cast<i32>(recipe_.timing.fill) == i)) {
                    recipe_.timing.fill = static_cast<ExportFill>(i);
                    MarkCustom();
                }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("dialog.export.snap"))) {
        // Round the length to a whole number of loops — and, when the camera
        // is orbiting by velocity, to a whole number of revolutions too. The
        // difference between a GIF that loops and a GIF that hitches.
        ExportRecipe probe = recipe_;
        probe.timing.duration = ExportDuration::Clips;
        const ExportSchedule natural = ExportSchedule::Build(probe, ranges);
        i32 unitMs = natural.DurationMs();
        if (recipe_.timing.fill == ExportFill::LoopLast && !recipe_.clips.empty()) {
            ExportRecipe lastOnly = recipe_;
            lastOnly.timing.duration = ExportDuration::Clips;
            lastOnly.clips = {recipe_.clips.back()};
            unitMs = ExportSchedule::Build(lastOnly, ranges).DurationMs();
        }
        if (unitMs > 0) {
            const i32 n = std::max(1, static_cast<i32>(std::lround(
                                          static_cast<f64>(recipe_.timing.durationMs) / unitMs)));
            recipe_.timing.durationMs = unitMs * n;
        }
        if (recipe_.camera.mode == ExportCameraMode::Orbit &&
            recipe_.camera.timing == OrbitTiming::Velocity &&
            std::fabs(recipe_.camera.degPerSec) > 0.01f) {
            const f64 turnMs = 360000.0 / std::fabs(recipe_.camera.degPerSec);
            const i32 turns = std::max(
                1, static_cast<i32>(std::lround(recipe_.timing.durationMs / turnMs)));
            recipe_.timing.durationMs = static_cast<i32>(std::lround(turnMs * turns));
        }
        MarkCustom();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("dialog.export.snap_tip"));
    ImGui::EndDisabled();

    if (schedule.TruncatedFromClip() >= 0) {
        const i32 c = schedule.TruncatedFromClip();
        Warn(kAmber, Fmt(T("dialog.export.truncated"), c + 1,
                         static_cast<i32>(recipe_.clips.size())));
    }
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void ExportWindow::BuildCamera() {
    ImGui::SeparatorText(T("dialog.export.camera"));
    ExportCameraMotion& c = recipe_.camera;

    if (ImGui::RadioButton(T("dialog.export.cam_viewport"), c.mode == ExportCameraMode::Viewport)) {
        c.mode = ExportCameraMode::Viewport;
        MarkCustom();
    }

    const auto& presets = app_.Playback().CameraPresets();
    ImGui::BeginDisabled(presets.empty());
    if (ImGui::RadioButton(T("dialog.export.cam_preset"), c.mode == ExportCameraMode::Preset)) {
        c.mode = ExportCameraMode::Preset;
        if (c.preset < 0)
            c.preset = 0;
        MarkCustom();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(c.mode != ExportCameraMode::Preset);
    ImGui::SetNextItemWidth(ui::kCaptureWideComboWidth);
    const char* presetLabel =
        (c.preset >= 0 && c.preset < static_cast<i32>(presets.size()))
            ? presets[static_cast<usize>(c.preset)].name.c_str()
            : T("dialog.export.cam_none");
    if (ImGui::BeginCombo("##campreset", presetLabel)) {
        for (i32 i = 0; i < static_cast<i32>(presets.size()); ++i)
            if (ImGui::Selectable(presets[static_cast<usize>(i)].name.c_str(), c.preset == i)) {
                c.preset = i;
                MarkCustom();
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (ImGui::RadioButton(T("dialog.export.cam_orbit"), c.mode == ExportCameraMode::Orbit)) {
        c.mode = ExportCameraMode::Orbit;
        MarkCustom();
    }
    ImGui::BeginDisabled(c.mode != ExportCameraMode::Orbit);
    ImGui::Indent(24.0f);

    const f32 totalSec = schedule_.DurationMs() / 1000.0f;

    // Velocity and revolutions are the same number seen two ways; whichever is
    // not being edited shows its derived value, because a turntable that does
    // not close is the most common failure of this feature.
    if (ImGui::RadioButton(T("dialog.export.orbit_velocity"), c.timing == OrbitTiming::Velocity)) {
        c.timing = OrbitTiming::Velocity;
        MarkCustom();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(c.timing != OrbitTiming::Velocity);
    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    if (ImGui::DragFloat("##degpersec", &c.degPerSec, 0.5f, -720.0f, 720.0f, "%.1f deg/s"))
        MarkCustom();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (c.timing == OrbitTiming::Velocity && totalSec > 0.0f)
        ImGui::TextDisabled(T("dialog.export.orbit_turns_derived"),
                            c.degPerSec * totalSec / 360.0f);

    if (ImGui::RadioButton(T("dialog.export.orbit_revolutions"),
                           c.timing == OrbitTiming::Revolutions)) {
        c.timing = OrbitTiming::Revolutions;
        MarkCustom();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(c.timing != OrbitTiming::Revolutions);
    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    if (ImGui::DragFloat("##revs", &c.revolutions, 0.05f, -20.0f, 20.0f, "%.2f turns"))
        MarkCustom();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (c.timing == OrbitTiming::Revolutions && totalSec > 0.0f)
        ImGui::TextDisabled(T("dialog.export.orbit_speed_derived"),
                            360.0f * c.revolutions / totalSec);

    if (ImGui::Checkbox(T("dialog.export.orbit_fit"), &c.fitToBounds))
        MarkCustom();
    ImGui::SameLine();
    ImGui::BeginDisabled(!c.fitToBounds);
    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    // The fit is the viewer's own framing helper, which leaves a model's
    // generous bounds a lot of air. This is the dial for that, and it is the
    // difference between a usable sprite sheet and a subject in the middle
    // distance.
    if (ImGui::DragFloat(T("dialog.export.orbit_margin"), &c.fitMargin, 0.01f, 0.2f, 4.0f,
                         "%.2fx"))
        MarkCustom();
    ImGui::EndDisabled();

    bool rotateModel = c.subject == OrbitSubject::Model;
    if (ImGui::Checkbox(T("dialog.export.orbit_rotate_model"), &rotateModel)) {
        c.subject = rotateModel ? OrbitSubject::Model : OrbitSubject::Camera;
        MarkCustom();
    }
    HelpMarker(T("dialog.export.orbit_rotate_model_tip"));

    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    if (ImGui::DragFloat(T("dialog.export.orbit_start"), &c.startYawDeg, 1.0f, -360.0f, 360.0f,
                         "%.0f deg"))
        MarkCustom();
    ImGui::SameLine();
    if (ImGui::Checkbox(T("dialog.export.orbit_relative"), &c.yawRelative))
        MarkCustom();

    if (ImGui::Checkbox("##ovpitch", &c.overridePitch))
        MarkCustom();
    ImGui::SameLine();
    ImGui::BeginDisabled(!c.overridePitch);
    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    if (ImGui::DragFloat(T("dialog.export.orbit_pitch"), &c.pitchDeg, 0.5f, -89.0f, 89.0f,
                         "%.1f deg"))
        MarkCustom();
    ImGui::EndDisabled();

    if (ImGui::Checkbox("##ovdist", &c.overrideDistance))
        MarkCustom();
    ImGui::SameLine();
    ImGui::BeginDisabled(!c.overrideDistance);
    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    if (ImGui::DragFloat(T("dialog.export.orbit_distance"), &c.distance, 1.0f, 15.0f, 8000.0f,
                         "%.0f"))
        MarkCustom();
    ImGui::EndDisabled();
    // The camera clamps both; echo the clamped value rather than hiding it,
    // because a fit distance of 12000 silently becoming 8000 is a bug report.
    if (c.overrideDistance && (c.distance < 15.0f || c.distance > 8000.0f))
        Warn(kAmber, T("dialog.export.orbit_distance_clamped"));

    ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
    if (ImGui::DragInt(T("dialog.export.angles"), &c.angleCount, 0.1f, 1, 64))
        MarkCustom();
    HelpMarker(T("dialog.export.angles_tip"));

    ImGui::Unindent(24.0f);
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Output / Advanced
// ---------------------------------------------------------------------------

void ExportWindow::BuildOutput() {
    ExportOutput& o = recipe_.output;
    const i32 w = (resMode_ == 1) ? o.width : app_.Service().Pipeline().Width();
    const i32 h = (resMode_ == 1) ? o.height : app_.Service().Pipeline().Height();

    // The summary line is the point: you can see the whole configuration
    // without expanding anything.
    // The summary rides in the header label, so a collapsed section still
    // states what is inside it.
    const std::string summary =
        std::string(GetExportFormatInfo(o.format).label) + "  -  " + std::to_string(w) + "x" +
        std::to_string(h) +
        (o.transparent ? "  -  " + std::string(T("dialog.export.transparent")) : std::string()) +
        (folderBuf_.empty() ? std::string() : "  -  " + folderBuf_);
    if (!ImGui::CollapsingHeader(
            Fmt("%s     %s###output", T("dialog.export.output"), summary.c_str())))
        return;

    ImGui::Indent(ui::kCaptureSectionIndent);
    {
        const char* formats[kExportFormatCount];
        for (i32 i = 0; i < kExportFormatCount; ++i)
            formats[i] = GetExportFormatInfo(static_cast<ExportFormat>(i)).label;
        i32 fmt = static_cast<i32>(o.format);
        ImGui::SetNextItemWidth(ui::kCapturePresetComboWidth);
        if (ImGui::Combo(T("dialog.export.format"), &fmt, formats, kExportFormatCount)) {
            o.format = static_cast<ExportFormat>(fmt);
            MarkCustom();
        }
        const FormatAvailability availability = ExportFormatAvailableNow(o.format);
        if (!availability.available)
            Warn(kRed, availability.reason.c_str());
    }

    ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
    if (ImGui::DragInt(T("dialog.export.fps"), &recipe_.timing.fps, 0.2f, 1, 240)) {
        recipe_.timing.fps = std::clamp(recipe_.timing.fps, 1, 240);
        MarkCustom();
    }

    {
        const char* modes[] = {T("dialog.export.res_current"), T("dialog.export.res_custom")};
        ImGui::SetNextItemWidth(ui::kCaptureWideComboWidth);
        if (ImGui::Combo(T("dialog.export.resolution"), &resMode_, modes, 2)) {
            if (resMode_ == 0) {
                o.width = 0;
                o.height = 0;
            } else if (o.width <= 0) {
                o.width = 1280;
                o.height = 960;
            }
            MarkCustom();
        }
        if (resMode_ == 1) {
            ImGui::SetNextItemWidth(ui::kCaptureResolutionFieldWidth);
            if (ImGui::InputInt(T("dialog.export.w"), &o.width, 0))
                MarkCustom();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ui::kCaptureResolutionFieldWidth);
            if (ImGui::InputInt(T("dialog.export.h"), &o.height, 0))
                MarkCustom();
            o.width = std::clamp(o.width, 16, 8192);
            o.height = std::clamp(o.height, 16, 8192);
        }
    }

    if (ImGui::Checkbox(T("dialog.export.transparent"), &o.transparent))
        MarkCustom();
    if (o.transparent && o.format == ExportFormat::Mp4)
        Warn(kAmber, T("dialog.export.mp4_no_alpha"));
    ImGui::SameLine();
    if (ImGui::Checkbox(T("dialog.export.capture_ui"), &o.captureUi))
        MarkCustom();

    ImGui::BeginDisabled(o.transparent);
    if (ImGui::Checkbox(T("dialog.export.override_bg"), &o.overrideBackground))
        MarkCustom();
    if (o.overrideBackground) {
        ImGui::SameLine();
        std::array<f32, 3> col = tools::ToUnitRgb({o.backgroundR, o.backgroundG, o.backgroundB});
        ImGui::SetNextItemWidth(ui::kCaptureColorWidth);
        if (ImGui::ColorEdit3("##bg", col.data(), ImGuiColorEditFlags_NoInputs)) {
            const tools::Rgb8 picked = tools::FromUnitRgbTruncated(col.data());
            o.backgroundR = picked.r;
            o.backgroundG = picked.g;
            o.backgroundB = picked.b;
            MarkCustom();
        }
    }
    ImGui::EndDisabled();

    if (o.format == ExportFormat::PngSheet) {
        ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
        if (ImGui::DragInt(T("dialog.export.sheet_columns"), &o.sheetColumns, 0.1f, 0, 64,
                           o.sheetColumns > 0 ? "%d" : "(auto)"))
            MarkCustom();
    }

    {
        ImGui::SetNextItemWidth(ui::kCapturePathWidth);
        if (ImGui::InputText("##folder", &folderBuf_))
            MarkCustom();
        ImGui::SameLine();
        if (ImGui::Button(T("dialog.export.browse"))) {
            NFD::UniquePathU8 out;
            if (NFD::PickFolder(out) == NFD_OKAY) {
                folderBuf_ = out.get();
                MarkCustom();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(T("dialog.export.output_folder"));
    }
    {
        ImGui::SetNextItemWidth(ui::kCapturePathWidth);
        if (ImGui::InputTextWithHint("##nametmpl", DefaultNameTemplate(o.format), &nameBuf_))
            MarkCustom();
        ImGui::SameLine();
        ImGui::TextUnformatted(T("dialog.export.name_template"));
        HelpMarker(T("dialog.export.name_template_tip"));
    }
    ImGui::Unindent(ui::kCaptureSectionIndent);
}

void ExportWindow::BuildAdvanced() {
    ExportOutput& o = recipe_.output;
    ExportTiming& t = recipe_.timing;
    const std::string summary =
        std::string(t.preRollMs > 0 ? Fmt(T("dialog.export.preroll_summary"), t.preRollMs)
                                    : T("dialog.export.no_preroll")) +
        "  -  " +
        (t.frameStep > 1 ? Fmt(T("dialog.export.every_nth"), t.frameStep)
                         : T("dialog.export.every_frame")) +
        "  -  " + (o.autoCrop ? T("dialog.export.crop_on") : T("dialog.export.crop_off")) +
        "  -  " +
        (o.hideOverlays ? T("dialog.export.overlays_hidden") : T("dialog.export.overlays_shown"));
    if (!ImGui::CollapsingHeader(
            Fmt("%s     %s###advanced", T("dialog.export.advanced"), summary.c_str())))
        return;

    ImGui::Indent(ui::kCaptureSectionIndent);
    ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
    if (ImGui::DragInt(T("dialog.export.preroll"), &t.preRollMs, 5.0f, 0, 10000, "%d ms"))
        MarkCustom();
    HelpMarker(T("dialog.export.preroll_tip"));

    ImGui::SetNextItemWidth(ui::kCaptureFieldWidth);
    if (ImGui::DragInt(T("dialog.export.frame_step"), &t.frameStep, 0.1f, 1, 16)) {
        t.frameStep = std::clamp(t.frameStep, 1, 16);
        MarkCustom();
    }
    HelpMarker(T("dialog.export.frame_step_tip"));

    if (ImGui::Checkbox(T("dialog.export.auto_crop"), &o.autoCrop))
        MarkCustom();
    HelpMarker(T("dialog.export.auto_crop_tip"));
    if (o.autoCrop) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::kCaptureOrbitFieldWidth);
        if (ImGui::DragInt(T("dialog.export.crop_padding"), &o.cropPadding, 0.2f, 0, 64))
            MarkCustom();
        if (!o.transparent)
            Warn(kAmber, T("dialog.export.crop_needs_alpha"));
    }

    if (ImGui::Checkbox(T("dialog.export.hide_overlays"), &o.hideOverlays))
        MarkCustom();
    if (ImGui::Checkbox(T("dialog.export.sidecar"), &o.writeSidecar))
        MarkCustom();
    HelpMarker(T("dialog.export.sidecar_tip"));
    ImGui::Unindent(ui::kCaptureSectionIndent);
}

// ---------------------------------------------------------------------------
// Footer
// ---------------------------------------------------------------------------

std::string ExportWindow::ResolvedOutputPath() const {
    NameTokens tokens;
    tokens.model = SanitizeExportName(io::PathToUtf8(app_.Documents().ActiveState().modelPath.stem()));
    i32 resolved = 0;
    std::string first = "queue";
    for (const ExportClip& c : recipe_.clips)
        if (c.Resolved()) {
            if (resolved == 0)
                first = SanitizeExportName(SequenceLabel(c));
            ++resolved;
        }
    tokens.anim = (resolved == 1) ? first : std::string("queue");
    tokens.clip = first;
    tokens.fps = recipe_.timing.fps;
    tokens.index = 0;
    const std::string tmpl =
        nameBuf_.empty() ? DefaultNameTemplate(recipe_.output.format) : nameBuf_;
    const std::string name = ExpandNameTemplate(tmpl, tokens);
    const std::string ext = GetExportFormatInfo(recipe_.output.format).extension;
    const std::string base = folderBuf_.empty() ? std::string("<no folder>") : folderBuf_;
    return base + "\\" + name + (ext.empty() ? ".png" : ext);
}

void ExportWindow::BuildFooter() {
    const f32 footerTop = ImGui::GetCursorPosY();
    ImGui::Separator();
    const ExportSchedule& schedule = schedule_;
    const i32 w = (resMode_ == 1) ? recipe_.output.width : app_.Service().Pipeline().Width();
    const i32 h = (resMode_ == 1) ? recipe_.output.height : app_.Service().Pipeline().Height();
    const i32 captured = schedule.CapturedFrameCount();

    ImGui::Text(T("dialog.export.summary"), schedule.DurationMs() / 1000.0f, captured, w, h,
                HumanBytes(EstimateOutputBytes(recipe_.output.format, captured, w, h)).c_str());
    ImGui::TextDisabled("-> %s", ResolvedOutputPath().c_str());

    // The three things the user cannot otherwise see coming.
    const u64 mem = EstimateSinkMemory(recipe_.output, captured, w, h);
    if (mem > (1ull << 30))
        Warn(kAmber, Fmt(T("dialog.export.memory_warn"), HumanBytes(mem).c_str()));
    const f32 effective = EffectiveFrameRate(recipe_.output.format, schedule.OutputFps());
    if (std::fabs(effective - schedule.OutputFps()) > 0.05f)
        Warn(kAmber, Fmt(T("dialog.export.rate_warn"), schedule.OutputFps(), effective));
    if (NameTemplateCollides(nameBuf_.empty() ? DefaultNameTemplate(recipe_.output.format)
                                              : nameBuf_,
                             recipe_.output.format))
        Warn(kAmber, T("dialog.export.name_collides"));
    if (schedule.TotalFrameCount() > kMaxExportFrames)
        Warn(kRed, Fmt(T("dialog.export.frame_cap"), schedule.TotalFrameCount(), kMaxExportFrames));
    for (const std::string& warning : schedule.Warnings())
        Warn(kAmber, warning.c_str());

    if (hasReport_) {
        ImGui::Separator();
        Warn(reportOk_ ? kReportSuccess : kRed, reportLine_.c_str());
        if (reportOk_ && !reportPath_.empty() && ImGui::SmallButton(T("dialog.export.reveal"))) {
#if defined(_WIN32)
            const std::string cmd = "explorer /select,\"" + reportPath_ + "\"";
            std::system(cmd.c_str());
#elif defined(__APPLE__)
            std::system(("open -R \"" + reportPath_ + "\"").c_str());
#else
            std::system(("xdg-open \"" + reportPath_ + "\"").c_str());
#endif
        }
    }

    ImGui::Separator();
    // Preview runs the recipe in the viewport at real time — the same
    // schedule, the same transitions, the same camera — and captures nothing.
    const f32 actionsX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui::kCaptureActionsReserve;
    if (ImGui::Button(previewing_ ? T("dialog.export.preview_stop") : T("dialog.export.preview"),
                      ImVec2(ui::kDialogButtonWidth, 0))) {
        previewing_ = !previewing_;
        previewFrame_ = 0;
        previewAccum_ = 0.0f;
    }

    const bool canExport =
        !folderBuf_.empty() && captured > 0 && schedule.TotalFrameCount() <= kMaxExportFrames &&
        ExportFormatAvailableNow(recipe_.output.format).available && !app_.Capture().IsRunning();
    ImGui::SameLine(actionsX);
    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button(T("dialog.export.export"), ImVec2(ui::kCaptureExportButtonWidth, 0))) {
        previewing_ = false;
        recipe_.output.folder = io::FsPathFromUtf8(folderBuf_);
        recipe_.output.nameTemplate = nameBuf_;
        Save();
        app_.Capture().Request(recipe_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("app.close"), ImVec2(ui::kDialogCancelWidth, 0)))
        Close();

    footerHeight_ = ImGui::GetCursorPosY() - footerTop + ImGui::GetFrameHeightWithSpacing();
}

// ---------------------------------------------------------------------------

void ExportWindow::Tick(f32 dtSec) {
    if (!previewing_ || !open_ || app_.Capture().IsRunning())
        return;
    const ExportSchedule schedule = ExportSchedule::Build(recipe_, app_.Playback().Sequences());
    if (schedule.TotalFrameCount() <= 0) {
        previewing_ = false;
        return;
    }
    previewAccum_ += dtSec * schedule.Fps();
    while (previewAccum_ >= 1.0f) {
        previewAccum_ -= 1.0f;
        previewFrame_ = (previewFrame_ + 1) % schedule.TotalFrameCount();
    }
    app_.Capture().Scrub(recipe_, previewFrame_, true);
    scrubFrame_ = previewFrame_ % std::max(1, schedule.FrameCount());
}

void ExportWindow::Build() {
    if (!open_)
        return;
    LoadIfNeeded();
    OnModelChanged();

    if (app_.Capture().ConsumeFinished()) {
        const ExportReport& r = app_.Capture().LastReport();
        hasReport_ = true;
        reportOk_ = r.ok;
        reportPath_ = r.files.empty() ? io::PathToUtf8(r.folder) : io::PathToUtf8(r.files.front());
        char buf[512];
        if (r.ok)
            std::snprintf(buf, sizeof(buf), T("dialog.export.report_ok"), r.framesCaptured,
                          r.elapsedSec, HumanBytes(r.outputBytes).c_str());
        else if (r.cancelled)
            std::snprintf(buf, sizeof(buf), T("dialog.export.report_cancelled"), r.framesCaptured);
        else
            std::snprintf(buf, sizeof(buf), T("dialog.export.report_failed"),
                          r.error.empty() ? "unknown error" : r.error.c_str());
        reportLine_ = buf;
    }

    ImGui::SetNextWindowSize(ImVec2(ui::kCaptureWindowWidth, ui::kCaptureWindowHeight), ImGuiCond_FirstUseEver);
    bool stayOpen = true;
    if (!ImGui::Begin(T("dialog.export.title"), &stayOpen)) {
        ImGui::End();
        if (!stayOpen)
            Close();
        return;
    }
    if (app_.Playback().SequenceNames().empty()) {
        ImGui::TextUnformatted(T("dialog.export.no_anims"));
        ImGui::End();
        if (!stayOpen)
            Close();
        return;
    }

    RebuildSchedule();
    BuildPresetRow();

    // The footer is the dialog's promise: it always states what will happen.
    // That only holds if it is always on screen, so the sections scroll under
    // it rather than pushing it off the bottom. The reservation is the footer's
    // own height plus the separator above it.
    const f32 reserve = (footerHeight_ > 0.0f) ? footerHeight_
                                               : ImGui::GetFrameHeightWithSpacing() * 4.5f;
    // Never let the footer eat the whole window: below this the scroll region
    // is unusable and the user cannot reach the controls the warnings are
    // about.
    const f32 maxReserve = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() * 4.0f;
    ImGui::BeginChild("##sections", ImVec2(0.0f, -std::min(reserve, std::max(0.0f, maxReserve))),
                      ImGuiChildFlags_Borders);
    BuildQueue();
    BuildTimeline();
    BuildLength();
    BuildCamera();
    BuildOutput();
    BuildAdvanced();
    ImGui::EndChild();

    RebuildSchedule();
    BuildFooter();

    ImGui::End();
    if (!stayOpen)
        Close();
}

} // namespace whiteout::flakes
