#include "progress_dialog.h"

#include "io/load_task.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace whiteout::flakes::tools {

namespace {

// Three hashes, not two: with "##" ImGui hashes the WHOLE string, so a
// varying title would give OpenPopup and BeginPopupModal different ids and the
// modal would never appear. "###" hashes only the part from the marker on.
constexpr const char* kModalId = "###load_progress";

// Trim the middle of a long path so both the folder that identifies it and the
// filename that changes stay readable. A CASC object is routinely longer than
// the dialog is wide, and eliding the tail hides the only part that moves.
std::string ElideMiddle(const std::string& text, float width) {
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    const float ellipsis = ImGui::CalcTextSize("...").x;
    if (width <= ellipsis)
        return "...";
    std::size_t head = 0, tail = text.size();
    // Grow from both ends until the next character would overflow.
    while (head + (text.size() - tail) < text.size()) {
        const std::size_t nextHead = head + 1;
        const std::size_t nextTail = tail - 1;
        const float w = ImGui::CalcTextSize(text.c_str(), text.c_str() + nextHead).x +
                        ImGui::CalcTextSize(text.c_str() + nextTail).x + ellipsis;
        if (w > width)
            break;
        head = nextHead;
        tail = nextTail;
    }
    // Never cut a UTF-8 sequence in half.
    while (head > 0 && (static_cast<unsigned char>(text[head]) & 0xC0) == 0x80)
        --head;
    while (tail < text.size() && (static_cast<unsigned char>(text[tail]) & 0xC0) == 0x80)
        ++tail;
    return text.substr(0, head) + "..." + text.substr(tail);
}

std::string CountLabel(const io::ProgressSnapshot& s) {
    char buf[128];
    if (s.bytesTotal != 0) {
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f MB",
                      static_cast<double>(s.bytesDone) / (1024.0 * 1024.0),
                      static_cast<double>(s.bytesTotal) / (1024.0 * 1024.0));
        return buf;
    }
    if (s.total != 0) {
        std::snprintf(buf, sizeof(buf), "%llu / %llu", static_cast<unsigned long long>(s.current),
                      static_cast<unsigned long long>(s.total));
        return buf;
    }
    return {};
}

void DrawBody(io::LoadTaskRunner& runner, const io::ProgressSnapshot& s, float width) {
    if (!s.stage.empty())
        ImGui::TextUnformatted(s.stage.c_str());
    else
        ImGui::TextUnformatted("Working...");

    // A negative fraction is ImGui's indeterminate animation. Drawing 0% for a
    // step that cannot count itself reads as a hang, which is the thing this
    // whole dialog exists to stop looking like.
    const float fraction =
        s.indeterminate ? -1.0f * static_cast<float>(ImGui::GetTime()) : s.fraction;
    const std::string counts = CountLabel(s);
    ImGui::ProgressBar(fraction, ImVec2(width, 0.0f), counts.empty() ? nullptr : counts.c_str());

    // The object line is reserved whether or not there is one, so the dialog
    // does not change height every time a step stops naming what it is on.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(s.object.empty() ? " " : ElideMiddle(s.object, width).c_str());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::TextDisabled("%.1f s", s.elapsedMs / 1000.0);

    if (!s.cancellable)
        return;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 80.0f);
    // Disabled rather than hidden after a press: the body may be between two
    // cancellation checks, and a button that vanishes suggests it did nothing.
    ImGui::BeginDisabled(s.cancelRequested);
    if (ImGui::Button(s.cancelRequested ? "Cancelling..." : "Cancel", ImVec2(80.0f, 0.0f)))
        runner.RequestCancel();
    ImGui::EndDisabled();
}

} // namespace

void DrawProgressModal(io::LoadTaskRunner& runner) {
    // ImGui itself is the state, rather than a local static: a static would be
    // shared by every runner in the process, so a second host drawing its own
    // modal would suppress the first. OpenPopup must fire once, on the frame
    // the work starts, and BeginPopupModal has to keep being called every
    // frame after that or ImGui closes it itself.
    const io::ProgressSnapshot s = runner.Poll();
    // Opportunistic work draws in the status line instead. Checked before
    // OpenPopup, so a prewarm never takes the screen at all.
    const bool busy = runner.Busy() && s.modal;
    const bool open = ImGui::IsPopupOpen(kModalId);
    if (busy && !open)
        ImGui::OpenPopup(kModalId);
    else if (!busy && !open)
        return;

    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    const char* title = s.title.empty() ? "Loading" : s.title.c_str();
    // The id is fixed and the visible label is not, so the popup survives a
    // title that changes between queued tasks.
    const std::string label = std::string(title) + kModalId;
    if (ImGui::BeginPopupModal(label.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        DrawBody(runner, s, 420.0f);
        if (const usize queued = runner.Queued(); queued > 0)
            ImGui::TextDisabled("%zu more queued", queued);
        if (!busy)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DrawProgressStatus(io::LoadTaskRunner& runner) {
    if (!runner.Busy())
        return;
    const io::ProgressSnapshot s = runner.Poll();
    if (s.modal)
        return; // the modal is already saying this, in front of everything
    ImGui::SameLine();
    const float fraction =
        s.indeterminate ? -1.0f * static_cast<float>(ImGui::GetTime()) : s.fraction;
    ImGui::ProgressBar(fraction, ImVec2(120.0f, ImGui::GetTextLineHeight()), "");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", s.stage.empty() ? s.title.c_str() : s.stage.c_str());
    if (ImGui::IsItemHovered() && !s.object.empty())
        ImGui::SetTooltip("%s", s.object.c_str());
}

} // namespace whiteout::flakes::tools
