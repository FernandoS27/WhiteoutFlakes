#include "storage_explorer.h"

#include "io/file_content_provider.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_settings.h"

#include "whiteout/flakes/content_provider.h"

#include "gfx/gfx.h"

#include <imgui.h>

#include <nfd.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace whiteout::flakes::tools {

namespace {
// Lowercased extension without the dot ("" if none).
std::string ExtOf(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

StorageFileKind KindOf(const std::string& name) {
    const std::string ext = ExtOf(name);
    if (ext == "pkb")
        return StorageFileKind::Pkb;
    if (ext == "pkfx")
        return StorageFileKind::Pkfx;
    if (ext == "mdl")
        return StorageFileKind::Mdl;
    return StorageFileKind::Mdx;
}

bool IsEffectKind(StorageFileKind k) {
    return k == StorageFileKind::Pkb || k == StorageFileKind::Pkfx;
}

bool MatchesFilter(StorageFileKind k, StorageFileFilter f) {
    switch (f) {
    case StorageFileFilter::ModelsOnly:
        return !IsEffectKind(k);
    case StorageFileFilter::EffectsOnly:
        return IsEffectKind(k);
    case StorageFileFilter::All:
    default:
        return true;
    }
}
} // namespace

StorageExplorer::StorageExplorer(renderer::RenderService& svc) : svc_(svc) {
    // One shared CASC-backed provider for every cell scene. Pool cap is sized
    // above a full screen of cells so scrolling within a folder doesn't recycle
    // (and thus destroy) a scene whose GPU work may still be in flight.
    provider_ = std::make_shared<io::FileContentProvider>();
    pool_ = std::make_unique<ThumbnailPool>(svc_, provider_, /*cap*/ 32, /*res*/ 256);
}

StorageExplorer::~StorageExplorer() {
    // The pool's scenes/targets must be released while the gfx device is still
    // alive; the host guarantees that by destroying us before Pipeline shutdown.
    pool_.reset();
}

std::shared_ptr<io::IContentProvider> StorageExplorer::Provider() const {
    return provider_;
}

bool StorageExplorer::OpenCasc(const std::string& root) {
    std::string err;
    if (!browser_.Open(root, &err)) {
        lastError_ = "Failed to open CASC at '" + root + "': " + err;
        std::fprintf(stderr, "[explorer] %s\n", lastError_.c_str());
        return false;
    }
    provider_->SetInstallPath(browser_.Root());
    provider_->SetHdMode(true); // prefer the _hd.w3mod overlay so HD textures resolve
    if (pool_)
        pool_->Clear();
    selectedPath_.clear();
    lastError_.clear();
    navAnimT_ = 0.0f; // fade the first listing in
    return true;
}

void StorageExplorer::NavigateTo(const std::string& displayPath) {
    if (!browser_.IsOpen())
        return;
    browser_.NavigateTo(displayPath);
    if (pool_)
        pool_->Clear();
    selectedPath_.clear();
}

void StorageExplorer::OpenCascDialog() {
    NFD::UniquePathU8 outPath;
    if (NFD::PickFolder(outPath) == NFD_OKAY && outPath)
        OpenCasc(outPath.get());
}

void StorageExplorer::NewFrame(float /*dt*/) {
    // Apply navigation staged by last frame's UI BEFORE anything references the
    // (about-to-be-cleared) thumbnails. Clear() waits for the GPU to finish with
    // the old cells' targets first.
    if (navPending_) {
        navPending_ = false;
        if (navAscend_)
            browser_.Ascend();
        else
            browser_.NavigateTo(navTarget_);
        if (pool_)
            pool_->Clear();
        selectedPath_.clear();
        navAnimT_ = 0.0f; // play the open transition for the new listing
    }

    if (provider_)
        provider_->Pump();

    ++frameCounter_;
    if (pool_)
        pool_->BeginFrame(frameCounter_);
}

void StorageExplorer::BuildWindow(bool* pOpen) {
    if (pOpen && !*pOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(960, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Storage Explorer", pOpen, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open CASC folder…"))
                OpenCascDialog();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (!browser_.IsOpen()) {
        ImGui::TextWrapped("Open a CASC archive folder (File ▸ Open CASC folder…) to browse "
                           "models and effects.");
        if (!lastError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", lastError_.c_str());
        }
        ImGui::End();
        return;
    }

    BuildGrid();
    ImGui::End();
}

void StorageExplorer::BuildGrid() {
    // Navigation is DEFERRED: changing the current folder rebuilds the browser's
    // listing, so doing it mid-loop would invalidate the very container we're
    // iterating. Record the request here and apply it in NewFrame next frame.
    enum class Nav { None, To, Ascend };
    Nav navAction = Nav::None;
    std::string navTarget;

    // File-type filter combo (end-user can switch what the grid shows).
    if (filterUiVisible_) {
        const char* kFilterLabels[] = {"All", "Models (.mdx/.mdl)", "Effects (.pkb/.pkfx)"};
        int fi = static_cast<int>(fileFilter_);
        ImGui::SetNextItemWidth(180);
        if (ImGui::Combo("Show", &fi, kFilterLabels, IM_ARRAYSIZE(kFilterLabels)))
            fileFilter_ = static_cast<StorageFileFilter>(fi);
        ImGui::Separator();
    }

    // Breadcrumb.
    if (ImGui::SmallButton("root")) {
        navAction = Nav::To;
        navTarget.clear();
    }
    const auto& crumbs = browser_.Breadcrumb();
    std::string acc;
    for (size_t i = 0; i < crumbs.size(); ++i) {
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        if (!acc.empty())
            acc.push_back('\\');
        acc += crumbs[i];
        if (ImGui::SmallButton((crumbs[i] + "##bc" + std::to_string(i)).c_str())) {
            navAction = Nav::To;
            navTarget = acc;
        }
    }
    ImGui::Separator();

    ImGui::BeginChild("grid", ImVec2(0, 0), false);

    ImGuiStyle& style = ImGui::GetStyle();
    // Fixed 4-column grid (the cell size follows the window width).
    constexpr int cols = 4;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cell = (std::max)(64.0f, (avail - style.ItemSpacing.x * (cols - 1)) / cols);

    // Open transition: when the listing changes the grid fades + slides up
    // (navAnimT_ reset to 0 in NewFrame / OpenCasc). dt comes from ImGui so the
    // panel needs no external clock.
    const float dt = ImGui::GetIO().DeltaTime;
    constexpr float kOpenDur = 0.16f;
    navAnimT_ = (std::min)(navAnimT_ + dt, kOpenDur);
    const float ot = navAnimT_ / kOpenDur;
    const float gridAlpha = ot * ot * (3.0f - 2.0f * ot); // smoothstep ease-in
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - gridAlpha) * 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * gridAlpha); // fades labels

    const auto placeholderColor = ImGui::GetColorU32(ImVec4(0.25f, 0.28f, 0.34f, gridAlpha));

    const auto& listing = browser_.Current();
    int col = 0;
    int idx = 0;
    auto beginCell = [&]() {
        if (col > 0)
            ImGui::SameLine();
    };
    auto endCell = [&]() {
        if (++col >= cols)
            col = 0;
    };

    // Animated folder icon: grows + highlights on hover, dips on press, and
    // fades with the open transition. `animId` keys the per-folder hover value in
    // ImGui's state storage so each cell eases independently.
    auto drawFolder = [&](ImVec2 p0, bool hovered, bool active, ImGuiID animId) {
        float* h = ImGui::GetStateStorage()->GetFloatRef(animId, 0.0f);
        *h += ((hovered ? 1.0f : 0.0f) - *h) * (std::min)(1.0f, dt * 14.0f);
        const float scale = 1.0f + 0.07f * (*h) - (active ? 0.05f : 0.0f);
        const ImVec2 ctr = ImVec2(p0.x + cell * 0.5f, p0.y + cell * 0.5f);
        auto S = [&](float fx, float fy) { // folder-space → screen, scaled about centre
            return ImVec2(ctr.x + (p0.x + cell * fx - ctr.x) * scale,
                          ctr.y + (p0.y + cell * fy - ctr.y) * scale);
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (*h > 0.004f) { // selection highlight + border, eased in
            dl->AddRectFilled(S(0.06f, 0.06f), S(0.94f, 0.94f),
                              ImGui::GetColorU32(ImVec4(0.40f, 0.66f, 1.0f, 0.16f * *h * gridAlpha)),
                              cell * 0.06f);
            dl->AddRect(S(0.06f, 0.06f), S(0.94f, 0.94f),
                        ImGui::GetColorU32(ImVec4(0.46f, 0.73f, 1.0f, 0.75f * *h * gridAlpha)),
                        cell * 0.06f, 0, 1.5f);
        }
        const ImU32 fc = ImGui::GetColorU32(ImVec4(0.85f, 0.72f, 0.35f, gridAlpha));
        dl->AddRectFilled(S(0.16f, 0.34f), S(0.84f, 0.72f), fc, cell * 0.05f); // body
        dl->AddRectFilled(S(0.16f, 0.26f), S(0.46f, 0.36f), fc, cell * 0.04f); // tab
    };

    // ".." up entry.
    if (!browser_.CurrentPath().empty()) {
        beginCell();
        ImGui::BeginGroup();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##up", ImVec2(cell, cell));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            navAction = Nav::Ascend;
        drawFolder(p0, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetID("##upa"));
        ImGui::TextUnformatted("..");
        ImGui::EndGroup();
        endCell();
    }

    for (const auto& folder : listing.folders) {
        beginCell();
        ImGui::PushID(idx++);
        ImGui::BeginGroup();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##f", ImVec2(cell, cell));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            navAction = Nav::To;
            navTarget = browser_.CurrentPath();
            if (!navTarget.empty())
                navTarget.push_back('\\');
            navTarget += folder;
        }
        drawFolder(p0, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetID("##fa"));
        std::string label = folder;
        if (label.size() > 18)
            label = label.substr(0, 17) + "…";
        ImGui::TextWrapped("%s", label.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        endCell();
    }

    // Model / effect files — live thumbnails, honouring the type filter.
    for (const auto& file : listing.modelFiles) {
        const StorageFileKind kind = KindOf(file);
        if (!MatchesFilter(kind, fileFilter_))
            continue;
        const bool isEffect = IsEffectKind(kind);
        beginCell();
        ImGui::PushID(idx++);
        ImGui::BeginGroup();
        const std::string archivePath = browser_.ChildPath(file);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + cell, p0.y + cell);

        const bool onScreen = ImGui::IsRectVisible(p0, p1);
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        if (onScreen) // render the cell at its on-screen size so it stays crisp
            tex = pool_->Acquire(archivePath, isEffect, frameCounter_, static_cast<int>(cell));

        ImGui::InvisibleButton("##m", ImVec2(cell, cell));
        if (ImGui::IsItemClicked())
            selectedPath_ = archivePath;
        // Double-click opens the file — the host decides what that means. Hand
        // back the path + provider, plus the file's bytes when DeliverBytes is on.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            onActivate_) {
            ActivatedFile af;
            af.path = archivePath;
            af.kind = kind;
            af.isEffect = isEffect;
            af.provider = provider_;
            if (deliverBytes_ && provider_) {
                if (auto data = provider_->ReadFile(archivePath)) {
                    af.bytes = std::move(*data);
                    af.hasBytes = true;
                }
            }
            onActivate_(af);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (tex != gfx::TextureHandle::Invalid) {
            dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), p0, p1,
                         ImVec2(0, 0), ImVec2(1, 1),
                         ImGui::GetColorU32(ImVec4(1, 1, 1, gridAlpha)));
        } else {
            dl->AddRectFilled(p0, p1, placeholderColor, 4.0f);
        }
        if (selectedPath_ == archivePath)
            dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.4f, 0.7f, 1.0f, gridAlpha)), 4.0f, 0,
                        2.0f);

        std::string label = file;
        if (label.size() > 18)
            label = label.substr(0, 17) + "…";
        ImGui::TextWrapped("%s", label.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        endCell();
    }

    ImGui::PopStyleVar(); // grid alpha
    ImGui::EndChild();

    // Stage navigation for the start of the next frame (applied in NewFrame).
    if (navAction == Nav::Ascend) {
        navPending_ = true;
        navAscend_ = true;
    } else if (navAction == Nav::To) {
        navPending_ = true;
        navAscend_ = false;
        navTarget_ = navTarget;
    }
}

void StorageExplorer::RenderThumbnails(float dt) {
    if (!pool_)
        return;

    // ThumbnailPool::RenderVisible mutates GLOBAL RenderSettings per cell (render
    // mode, SD-HDR) and we force the floor grid off + clear the cells to the
    // panel background. Those settings are shared with the host's main view, so
    // snapshot and restore them around the cell render. (SetDisplayFlags also
    // carries renderMode, so restoring it restores the host's mode too.)
    auto& s = svc_.Settings();
    const auto savedFlags = s.GetDisplayFlags();
    const std::uint32_t savedBg = s.BackgroundColorRaw();
    const bool savedHdr = s.SceneHdrInSd();

    auto tf = savedFlags;
    tf.showGrid = false;
    s.SetDisplayFlags(tf);
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    s.SetBackgroundColor(static_cast<unsigned char>(bg.x * 255.0f),
                         static_cast<unsigned char>(bg.y * 255.0f),
                         static_cast<unsigned char>(bg.z * 255.0f));

    pool_->RenderVisible(dt);

    s.SetDisplayFlags(savedFlags);
    s.SetBackgroundColor(static_cast<unsigned char>(savedBg & 0xFF),
                         static_cast<unsigned char>((savedBg >> 8) & 0xFF),
                         static_cast<unsigned char>((savedBg >> 16) & 0xFF));
    s.SetSceneHdrInSd(savedHdr);
}

} // namespace whiteout::flakes::tools
