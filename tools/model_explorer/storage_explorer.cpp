#include "storage_explorer.h"

#include "io/load_task.h"

#include "io/file_content_provider.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_settings.h"

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <imgui.h>

#include <nfd.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    if (ext == "m2")
        return StorageFileKind::M2;
    if (ext == "m3")
        return StorageFileKind::M3;
    return StorageFileKind::Mdx;
}

bool IsEffectKind(StorageFileKind k) {
    return k == StorageFileKind::Pkb || k == StorageFileKind::Pkfx;
}

// The games worth offering, in the order the combo lists them. A game with no
// detected install is shown disabled rather than hidden — "StarCraft II is not
// installed" is a more useful answer than a combo that silently has two
// entries on one machine and three on another.
constexpr ProductId kGames[] = {ProductId::Wc3, ProductId::Wow, ProductId::Sc2, ProductId::D3};

// Icon zoom range. The floor is the point where a thumbnail still reads as a
// model rather than a smudge; the ceiling is about two cells across a default
// panel, past which a grid stops being a grid.
constexpr float kMinIcon = 48.0f;
constexpr float kMaxIcon = 320.0f;
// Live thumbnail cells the pool may hold. Sized to a screenful at the current
// icon size (see BuildGrid) but never past this: every visible cell renders its
// own scene every frame, so the bound is a frame-time bound, not a memory one.
constexpr int kMinCells = 32;
constexpr int kMaxCells = 128;

const char* GameLabel(ProductId game) {
    switch (game) {
    case ProductId::Wow:
        return "World of Warcraft";
    case ProductId::Sc2:
        return "StarCraft II";
    case ProductId::Wc3:
        return "Warcraft III";
    case ProductId::D3:
        return "Diablo III";
    default:
        return "(folder)";
    }
}

// Longest prefix of `text` that fits `width`, ellipsised when it had to cut.
// The cell width is the user's now, so a fixed character count would be a
// clipped label at one zoom level and a stub at another.
std::string FitLabel(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    // ASCII dots, not '…': the atlas bakes the default Latin range plus whatever
    // the language catalogs use, and U+2026 is in neither — it draws as a
    // missing-glyph box on most builds.
    constexpr const char* kCut = "...";
    const float ellipsis = ImGui::CalcTextSize(kCut).x;
    std::size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        const float w = ImGui::CalcTextSize(text.c_str(), text.c_str() + mid).x;
        if (w + ellipsis <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0) == 0x80)
        --lo; // never cut a UTF-8 sequence in half
    return text.substr(0, lo) + kCut;
}

// One label line, centred under a `cell`-wide icon.
void CellLabel(const std::string& label, float cell) {
    const float w = ImGui::CalcTextSize(label.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (cell - w) * 0.5f));
    ImGui::TextUnformatted(label.c_str());
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
    if (opening_)
        return false; // one at a time; the runner would serialise these anyway

    if (tasks_) {
        opening_ = true;
        navPending_ = false; // staged against a listing that is being replaced
        // Cleared HERE, on the host thread, not in the completion: the pool
        // owns GPU targets and Clear() waits for the device, neither of which
        // belongs on the task thread.
        if (pool_)
            pool_->Clear();
        selectedPath_.clear();
        openError_.clear();
        tasks_->Run(
            "Opening " + root,
            [this, root](io::ProgressMonitor& m) {
                // The browser's tree is built here. Nothing may read it until
                // the completion clears `opening_` — see BuildWindow.
                const bool ok = browser_.Open(root, io::StorageKind::Casc, &openError_, &m);
                return ok ? io::TaskResult::Ok()
                          : io::TaskResult::Fail(openError_.empty() ? "open failed" : openError_);
            },
            [this, root](const io::TaskOutcome& out) {
                opening_ = false;
                if (!out.ok) {
                    lastError_ = "Failed to open CASC at '" + root + "': " + out.error;
                    std::fprintf(stderr, "[explorer] %s\n", lastError_.c_str());
                    return;
                }
                FinishOpenCasc(root);
            });
        return true;
    }

    std::string err;
    if (!browser_.Open(root, io::StorageKind::Casc, &err)) {
        lastError_ = "Failed to open CASC at '" + root + "': " + err;
        std::fprintf(stderr, "[explorer] %s\n", lastError_.c_str());
        return false;
    }
    FinishOpenCasc(root);
    return true;
}

// Everything the open implies for the panel and its provider. Host thread
// only — it touches the thumbnail pool's GPU resources and the provider's
// configuration, and it runs after the browser's tree is complete.
void StorageExplorer::FinishOpenCasc(const std::string& root) {
    // Point the panel's own provider at the same game the browser detected.
    // Without this every read goes to whichever product the provider defaulted
    // to (Warcraft III), which for an `.m2` is not a missing texture but a
    // missing *model*: its `.skin` siblings are fileDataIDs only a WoW storage
    // can resolve, so the thumbnail never renders at all.
    //
    // Game first: the listfile, key list and install path all belong to one
    // product's slot, so setting them before the switch writes them into the
    // wrong one.
    const ProductId product = browser_.Product();
    if (product != ProductId::Neutral)
        provider_->SetGame(product);
    provider_->SetListfilePath(FsPathFromUtf8(listfilePath_));
    provider_->SetTactKeyPath(FsPathFromUtf8(tactKeyPath_));
    provider_->SetInstallPath(browser_.Root());
    // The `_hd.w3mod` overlay is Warcraft III's; nothing else has a mod chain.
    provider_->SetHdMode(product == ProductId::Wc3 || product == ProductId::Neutral);
    if (pool_)
        pool_->Clear();
    selectedPath_.clear();
    lastError_.clear();
    openedRoot_ = root;
    // Totals, not the filtered listing: a search left over from the last open
    // must not be read as an empty storage.
    openedEmpty_ = browser_.Current().folderTotal == 0 && browser_.Current().fileTotal == 0;
    navAnimT_ = 0.0f; // fade the first listing in
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

GameStorageKeys StorageExplorer::ResolveGame(ProductId game) const {
    GameStorageKeys keys = gameKeys_ ? gameKeys_(game) : GameStorageKeys{};
    if (keys.installPath.empty())
        keys.installPath = provider_->GamePath(game);
    return keys;
}

bool StorageExplorer::OpenGame(ProductId game) {
    const GameStorageKeys keys = ResolveGame(game);
    if (keys.installPath.empty()) {
        lastError_ = std::string(GameLabel(game)) + " is not installed.";
        return false;
    }
    // Before the open, not after: the listfile and key list are part of the
    // CASC open key, so a storage acquired without them stays without them.
    SetCascKeys(keys.listfilePath, keys.tactKeyPath);
    return OpenCasc(keys.installPath);
}

void StorageExplorer::Sync(ProductId fallback) {
    if (!browser_.IsOpen()) {
        if (fallback != ProductId::Neutral)
            OpenGame(fallback);
        return;
    }
    // Neutral is a hand-picked folder or a storage whose build config names no
    // product — the host has no settings for it to contribute.
    const ProductId game = browser_.Product();
    if (game == ProductId::Neutral)
        return;
    const GameStorageKeys keys = ResolveGame(game);
    if (keys.listfilePath == listfilePath_ && keys.tactKeyPath == tactKeyPath_)
        return; // already showing exactly what the host knows
    SetCascKeys(keys.listfilePath, keys.tactKeyPath);
    OpenCasc(openedRoot_);
}

// Game first, then a checkbox per type that game ships. One row, because they
// are one decision: which types exist at all is decided by the game, so a
// filter offered without it would be a filter for whatever happened to be open.
void StorageExplorer::BuildFilterBar() {
    ImGui::Spacing(); // clear of the menu bar above
    const ProductId open = browser_.Product();

    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Game", GameLabel(open))) {
        for (ProductId game : kGames) {
            const bool installed = !provider_->GamePath(game).empty();
            ImGui::BeginDisabled(!installed);
            if (ImGui::Selectable(GameLabel(game), game == open))
                OpenGame(game);
            ImGui::EndDisabled();
            if (!installed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Not installed");
        }
        ImGui::EndCombo();
    }

    // Only the types the open storage was actually walked for. A Warcraft III
    // install gets models and effects; World of Warcraft gets `.m2` and there
    // is nothing else to offer.
    const io::BrowseType available = browser_.AvailableTypes();
    io::BrowseType enabled = browser_.EnabledTypes();
    for (io::BrowseType type : {io::BrowseType::Models, io::BrowseType::Effects, io::BrowseType::M2,
                                io::BrowseType::M3}) {
        if (!Any(available & type))
            continue;
        ImGui::SameLine();
        bool on = Any(enabled & type);
        if (ImGui::Checkbox(io::BrowseTypeLabel(type), &on))
            enabled = on ? (enabled | type) : (enabled & ~type);
    }
    browser_.SetEnabledTypes(enabled);
}

void StorageExplorer::SetSearchText(const std::string& text) {
    std::snprintf(searchText_, sizeof(searchText_), "%s", text.c_str());
    searchDelay_ = 0.0f; // a host setting it is not typing: apply at once
    browser_.SetFilter(searchText_);
}

void StorageExplorer::SetIconSize(float px) {
    iconSize_ = std::clamp(px, kMinIcon, kMaxIcon);
}

// Search box, match count and zoom — the controls that decide how much of the
// folder you see and how big it is. Separate from BuildFilterBar: those pick
// *what* the storage is walked for and a host may lock them, these are the
// grid's own and always available.
void StorageExplorer::BuildSearchBar() {
    // Ctrl+F focuses the box, but only while this panel has focus — the host's
    // other windows keep the shortcut for themselves.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F))
        focusSearch_ = true;
    if (focusSearch_) {
        focusSearch_ = false;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(260);
    const bool edited =
        ImGui::InputTextWithHint("##search", "Filter (Ctrl+F)", searchText_, sizeof(searchText_));
    // ASCII only: the font atlas bakes the default Latin range, so an em dash
    // would draw as a missing-glyph box.
    ImGui::SetItemTooltip("Case-insensitive substring.\n"
                          "*.mdx / foot?an : wildcards match the whole name\n"
                          "peasant, footman : comma-separated alternatives\n"
                          "-portrait : exclude");
    if (edited)
        searchDelay_ = kSearchDebounce;
    if (searchText_[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            searchText_[0] = '\0';
            searchDelay_ = 0.0f; // a click is not typing: nothing more is coming
        }
    }
    // Only once the typing has settled: a keystroke re-lists the whole
    // folder. Until then the grid keeps showing the filter last applied,
    // match count included, so read that one and not the box.
    if (searchDelay_ <= 0.0f)
        browser_.SetFilter(searchText_);
    if (!browser_.Filter().empty()) {
        const auto& l = browser_.Current();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %zu", l.folders.size() + l.modelFiles.size(),
                            l.folderTotal + l.fileTotal);
    }

    // Zoom, right-aligned so it stays put as the search row grows.
    constexpr float kZoomWidth = 160.0f;
    ImGui::SameLine();
    const float rest = ImGui::GetContentRegionAvail().x;
    if (rest > kZoomWidth)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rest - kZoomWidth);
    ImGui::SetNextItemWidth(kZoomWidth);
    float size = iconSize_;
    if (ImGui::SliderFloat("##zoom", &size, kMinIcon, kMaxIcon, "%.0f px"))
        SetIconSize(size);
    ImGui::SetItemTooltip("Icon size, or Ctrl+scroll over the grid");
}

// An open that enumerated nothing is indistinguishable from an empty folder,
// and for the one product where it is routine it is not the storage's fault but
// a missing setting. Say which.
void StorageExplorer::BuildEmptyHint() {
    ImGui::Spacing();
    ImGui::TextWrapped("Nothing browsable in '%s'.", browser_.Root().c_str());
    ImGui::Spacing();
    if (browser_.Product() == ProductId::Wow && listfilePath_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "No listfile is configured.");
        ImGui::TextWrapped(
            "A World of Warcraft root is keyed by fileDataID, not by path: the names to "
            "browse are not in the install at all. Point the host at a community listfile "
            "CSV (in the Basic Viewer: Settings > IO > World of Warcraft > Listfile), then "
            "reopen this panel.");
    } else {
        ImGui::TextWrapped("The storage opened, but held no file of a type this game is "
                           "browsed for.");
    }
}

void StorageExplorer::NewFrame(float dt) {
    // The search box's typing debounce, counted down here rather than in
    // BuildSearchBar so a panel the host stopped drawing still settles.
    if (searchDelay_ > 0.0f)
        searchDelay_ = std::max(0.0f, searchDelay_ - dt);

    // Apply navigation staged by last frame's UI BEFORE anything references the
    // (about-to-be-cleared) thumbnails. Clear() waits for the GPU to finish with
    // the old cells' targets first.
    // Not while an open is in flight: the task thread is rebuilding the very
    // tree Ascend/NavigateTo would walk. Staged navigation can outlive the
    // frame that staged it — BuildGrid draws the old listing in the same frame
    // the game combo starts a new open — so this needs its own guard rather
    // than relying on BuildWindow returning early.
    if (navPending_ && !opening_) {
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

    if (opening_) {
        // The listing is being built on the task thread; walking the tree now
        // would race it. The host's progress modal is what shows the bar.
        ImGui::TextUnformatted("Opening storage…");
        ImGui::End();
        return;
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

    if (filterUiVisible_) {
        BuildFilterBar();
        ImGui::Separator();
    }
    // The filter bar first, so the game combo stays reachable: switching game is
    // the way out of a storage that has nothing to show.
    if (openedEmpty_) {
        BuildEmptyHint();
        return;
    }
    BuildSearchBar();

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

    // Ctrl+wheel zooms. ImGui leaves the wheel unclaimed while Ctrl is down
    // (UpdateMouseWheel returns early unless io.FontAllowUserScaling, which is
    // off), so reading it here does not also scroll the grid.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && io.MouseWheel != 0.0f &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        SetIconSize(iconSize_ * std::pow(1.1f, io.MouseWheel));

    ImGuiStyle& style = ImGui::GetStyle();
    // The user picks the cell size; how many fit across follows from it. A cell
    // never exceeds the width available, so a narrow panel still shows one
    // column rather than clipping.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cell = (std::min)(iconSize_, (std::max)(kMinIcon, avail));
    const int cols = (std::max)(1, static_cast<int>((avail + style.ItemSpacing.x) /
                                                    (cell + style.ItemSpacing.x)));

    // Keep the pool able to hold everything on screen at once: a cell that finds
    // no free slot draws a placeholder forever, which at small icon sizes would
    // be most of the grid.
    const float rowHeight = cell + ImGui::GetTextLineHeightWithSpacing() + style.ItemSpacing.y;
    const int rows = static_cast<int>(ImGui::GetContentRegionAvail().y / rowHeight) + 2;
    if (pool_)
        pool_->SetCap(std::clamp(cols * rows, kMinCells, kMaxCells));

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
            dl->AddRectFilled(
                S(0.06f, 0.06f), S(0.94f, 0.94f),
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
        CellLabel("..", cell);
        ImGui::EndGroup();
        endCell();
    }

    for (const auto& folder : listing.folders) {
        beginCell();
        ImGui::PushID(idx++);
        ImGui::BeginGroup();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool onScreen = ImGui::IsRectVisible(ImVec2(cell, cell));
        ImGui::InvisibleButton("##f", ImVec2(cell, cell));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            navAction = Nav::To;
            navTarget = browser_.CurrentPath();
            if (!navTarget.empty())
                navTarget.push_back('\\');
            navTarget += folder;
        }
        // Off-screen cells cost layout and nothing else. Not an optimisation:
        // `creature/` in a World of Warcraft install holds three thousand
        // subfolders, and a folder icon is a dozen vertices — emitting them all
        // overruns ImGui's 16-bit index buffer and the whole draw list comes out
        // as stretched garbage. Warcraft III never had a folder big enough.
        if (onScreen) {
            const std::string label = FitLabel(folder, cell);
            if (label != folder)
                ImGui::SetItemTooltip("%s", folder.c_str());
            drawFolder(p0, ImGui::IsItemHovered(), ImGui::IsItemActive(), ImGui::GetID("##fa"));
            CellLabel(label, cell);
        } else {
            ImGui::NewLine(); // hold the label's row so the grid does not reflow
        }
        ImGui::EndGroup();
        ImGui::PopID();
        endCell();
    }

    // Model / effect files — live thumbnails. Already filtered: the browser's
    // listing only carries the enabled types.
    for (const auto& file : listing.modelFiles) {
        const StorageFileKind kind = KindOf(file);
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

        // Same bound as the folders above, for the same reason.
        if (onScreen) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tex != gfx::TextureHandle::Invalid) {
                dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), p0, p1,
                             ImVec2(0, 0), ImVec2(1, 1),
                             ImGui::GetColorU32(ImVec4(1, 1, 1, gridAlpha)));
            } else {
                dl->AddRectFilled(p0, p1, placeholderColor, 4.0f);
            }
            if (selectedPath_ == archivePath) {
                dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.4f, 0.7f, 1.0f, gridAlpha)), 4.0f,
                            0, 2.0f);
            }
            const std::string label = FitLabel(file, cell);
            if (label != file)
                ImGui::SetItemTooltip("%s", file.c_str());
            CellLabel(label, cell);
        } else {
            ImGui::NewLine();
        }
        ImGui::EndGroup();
        ImGui::PopID();
        endCell();
    }

    // An empty grid should say which kind of empty it is — a folder with nothing
    // in it reads exactly like a filter that matched nothing.
    if (listing.folders.empty() && listing.modelFiles.empty()) {
        ImGui::TextDisabled("%s", !browser_.Filter().empty() ? "Nothing here matches the filter."
                                                             : "Nothing to show in this folder.");
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
