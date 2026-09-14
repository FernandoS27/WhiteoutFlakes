#include "storage_explorer.h"

#include "io/load_task.h"

#include "io/file_content_provider.h"
#include "io/storage/storage_paths.h"
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
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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
    if (ext == "blp" || ext == "dds" || ext == "tga")
        return StorageFileKind::Texture;
    return StorageFileKind::Mdx;
}

bool IsEffectKind(StorageFileKind k) {
    return k == StorageFileKind::Pkb || k == StorageFileKind::Pkfx;
}

bool IsTextureKind(StorageFileKind k) {
    return k == StorageFileKind::Texture;
}

// Checkerboard behind a texture cell, so a transparent one reads as
// transparent rather than as whatever the panel background happens to be.
// Sized off the cell so the squares stay the same apparent size at any zoom.
void DrawAlphaCheckers(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float alpha) {
    const float sq = std::max(4.0f, (p1.x - p0.x) / 12.0f);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.22f, 0.23f, 0.25f, alpha)));
    const ImU32 light = ImGui::GetColorU32(ImVec4(0.30f, 0.31f, 0.34f, alpha));
    dl->PushClipRect(p0, p1, true);
    int row = 0;
    for (float y = p0.y; y < p1.y; y += sq, ++row) {
        for (int col = (row & 1); ; col += 2) {
            const float x = p0.x + static_cast<float>(col) * sq;
            if (x >= p1.x)
                break;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + sq, p1.x), std::min(y + sq, p1.y)),
                              light);
        }
    }
    dl->PopClipRect();
}

// The rect inside [p0,p1] that shows a `w`x`h` image without distorting it.
// A texture browser that stretched a 256x1024 gradient to a square cell would
// be showing something the file is not.
void FitRect(const ImVec2& p0, const ImVec2& p1, int w, int h, ImVec2& outMin, ImVec2& outMax) {
    const float boxW = p1.x - p0.x;
    const float boxH = p1.y - p0.y;
    if (w <= 0 || h <= 0 || boxW <= 0.0f || boxH <= 0.0f) {
        outMin = p0;
        outMax = p1;
        return;
    }
    const float scale =
        std::min(boxW / static_cast<float>(w), boxH / static_cast<float>(h));
    const float dw = static_cast<float>(w) * scale;
    const float dh = static_cast<float>(h) * scale;
    outMin = ImVec2(p0.x + (boxW - dw) * 0.5f, p0.y + (boxH - dh) * 0.5f);
    outMax = ImVec2(outMin.x + dw, outMin.y + dh);
}

// The games worth offering, in the order the combo lists them. A game with no
// detected install is shown disabled rather than hidden — "StarCraft II is not
// installed" is a more useful answer than a combo that silently has two
// entries on one machine and three on another.
//
// A target is a (product, install) pair rather than a bare ProductId because
// one product covers two games: StarCraft II and Heroes of the Storm share
// ProductId::Sc2 — the enum picks a RENDER PROFILE, and Heroes renders through
// sc2_heroes — while being separate installs that no single path names. Giving
// Heroes its own ProductId would be a bound-enum change (see
// io/product_detect.h, where `hero` maps to Sc2 on purpose); giving it its own
// combo entry costs a bool.
struct BrowseTarget {
    ProductId game;
    bool heroes;
};
constexpr BrowseTarget kGames[] = {
    {ProductId::Wc3, false}, {ProductId::Wow, false},   {ProductId::Sc2, false},
    {ProductId::Sc2, true},  {ProductId::D3, false},
};

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

// Outline row indent per depth level, in logical pixels.
constexpr float kTreeIndent = 14.0f;
// Rows one flatten will produce before it gives up. A backstop, not a budget:
// the outline only walks folders that are open, so reaching this means a
// filter auto-expanded something enormous, and a truncated tree that SAYS it
// is truncated beats one that quietly stops.
constexpr std::size_t kMaxTreeRows = 20000;
// Folders a filter may leave standing and still be treated as a result set
// worth expanding to. Past this the filter goes back to merely pruning; see
// RebuildTreeRows.
constexpr std::size_t kAutoExpandMax = 400;

std::string LowerPath(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Two spellings of one install directory. Lexical and case-insensitive on
// purpose: what is compared here is a root a host configured against a root an
// open was asked for, which differ in separators, case and a trailing slash and
// in nothing else - no symlink to resolve, and nothing worth a stat per menu
// item per frame. Used only to check-mark the File menu, so the worst a
// case-sensitive filesystem can make of it is a mark on the wrong entry.
bool SamePath(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty())
        return false;
    auto norm = [](const std::string& in) {
        std::string out = LowerPath(std::filesystem::path(in).lexically_normal().string());
        for (char& c : out)
            if (c == '/')
                c = '\\';
        while (!out.empty() && out.back() == '\\')
            out.pop_back();
        return out;
    };
    return norm(a) == norm(b);
}

// The folder holding a display path, and the entry name inside it - the two
// halves ChildPathAt takes, and what a restored selection arrives as one of.
std::string ParentOfPath(const std::string& path) {
    const auto cut = path.find_last_of("\\/");
    return cut == std::string::npos ? std::string{} : path.substr(0, cut);
}

std::string NameOfPath(const std::string& path) {
    const auto cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// Join a folder path and one of its entries, both in display form.
std::string ChildDisplayPath(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + '\\' + name;
}

const char* GameLabel(ProductId game, bool heroes = false) {
    switch (game) {
    case ProductId::Wow:
        return "World of Warcraft";
    case ProductId::Sc2:
        return heroes ? "Heroes of the Storm" : "StarCraft II";
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
    // Far larger a cap than the pool's: an image cell costs one small texture
    // and no per-frame work, so keeping a few folders' worth is what makes
    // scrolling back up instant instead of a re-decode.
    textures_ = std::make_unique<TextureThumbnailCache>(svc_, provider_, /*cap*/ 512);
}

StorageExplorer::~StorageExplorer() {
    // The pool's scenes/targets and the cache's textures must be released while
    // the gfx device is still alive; the host guarantees that by destroying us
    // before Pipeline shutdown.
    textures_.reset();
    pool_.reset();
}

std::shared_ptr<io::IContentProvider> StorageExplorer::Provider() const {
    return provider_;
}

bool StorageExplorer::OpenCasc(const std::string& root) {
    return OpenInternal(root, io::StorageKind::Casc);
}

bool StorageExplorer::OpenStorage(const std::string& root) {
    return OpenInternal(root, std::nullopt);
}

bool StorageExplorer::OpenInternal(const std::string& root,
                                   std::optional<io::StorageKind> kind) {
    if (opening_)
        return false; // one at a time; the runner would serialise these anyway

    // "Failed to open CASC" is a lie when the caller asked us to work out what
    // the path was, and the answer a user needs from a classic install is not
    // that its .build.info is missing.
    const std::string what = kind ? "CASC" : "the storage";

    if (tasks_) {
        opening_ = true;
        navPending_ = false; // staged against a listing that is being replaced
        // Cleared HERE, on the host thread, not in the completion: these own
        // GPU resources and Clear() waits for the device, neither of which
        // belongs on the task thread.
        if (pool_)
            pool_->Clear();
        if (textures_)
            textures_->Clear();
        ClearSelection();
        openError_.clear();
        tasks_->Run(
            "Opening " + root,
            [this, root, kind](io::ProgressMonitor& m) {
                // The browser's tree is built here. Nothing may read it until
                // the completion clears `opening_` — see BuildWindow.
                const bool ok = io::OpenWithArchives(browser_, root, kind, archiveOverride_,
                                                     &openError_, &m);
                return ok ? io::TaskResult::Ok()
                          : io::TaskResult::Fail(openError_.empty() ? "open failed" : openError_);
            },
            [this, root, what](const io::TaskOutcome& out) {
                opening_ = false;
                if (!out.ok) {
                    lastError_ = "Failed to open " + what + " at '" + root + "': " + out.error;
                    std::fprintf(stderr, "[explorer] %s\n", lastError_.c_str());
                    return;
                }
                FinishOpenCasc(root);
            });
        return true;
    }

    std::string err;
    const bool ok = io::OpenWithArchives(browser_, root, kind, archiveOverride_, &err);
    if (!ok) {
        lastError_ = "Failed to open " + what + " at '" + root + "': " + err;
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
    // For an MpqSet the browser's root IS the install directory the provider
    // searches its MPQ chain under, so the same call serves both generations.
    // A single archive is a file, not a directory; hand the provider the
    // directory holding it so the rest of the load order is still reachable.
    provider_->SetInstallPath(browser_.Kind() == io::StorageKind::Mpq
                                  ? std::filesystem::path(browser_.Root()).parent_path().string()
                                  : browser_.Root());
    // Warcraft III's overlays only exist in a CASC install; a classic
    // MPQ-only one has no mod chain and every overlay lookup in it misses.
    //
    // Reforged and not Definitive, even though Definitive reaches more files:
    // this is only the tier a read falls back to when nothing else has said,
    // and every cell re-arms its own from the path its model came from
    // (ThumbnailPool::Spawn). Leaving the fallback where it has always been
    // keeps a browse of a pre-3.0.0 install, and of every non-Definitive
    // model, resolving exactly as it did.
    const bool browseHd = (product == ProductId::Wc3 || product == ProductId::Neutral) &&
                          browser_.Kind() == io::StorageKind::Casc;
    provider_->SetArtTier(browseHd ? Wc3ArtTier::Reforged : Wc3ArtTier::Classic);
    if (pool_) {
        // Before Clear, so the cells this open builds already know which game
        // they are showing: it decides whether a new cell scene stands the
        // Warcraft III lighting up instead of waiting for a model spawn to.
        pool_->SetProduct(product);
        // The mode a fresh cell scene starts in — which overlay its FIRST
        // read (the parse) resolves through. Same predicate as the provider
        // arm above; the loader trues each cell up from what it parses.
        pool_->SetDefaultCellMode(browseHd ? renderer::RenderMode::HD
                                           : renderer::RenderMode::SD);
        pool_->Clear();
    }
    if (textures_)
        textures_->Clear();
    openedKind_ = browser_.Kind();
    ClearSelection();
    lastError_.clear();
    // A new storage is a new outline: the folders the user had expanded name
    // paths that no longer exist.
    treeOpen_.clear();
    treeRowsDirty_ = true;
    openedRoot_ = root;
    // Totals, not the filtered listing: a search left over from the last open
    // must not be read as an empty storage.
    openedEmpty_ = browser_.Current().folderTotal == 0 && browser_.Current().fileTotal == 0;
    navAnimT_ = 0.0f; // fade the first listing in
    // Last, because it navigates and selects on top of everything cleared above.
    ApplyStagedRestore();
}

ExplorerState StorageExplorer::State() const {
    ExplorerState st;
    st.view = view_;
    st.game = browser_.Product();
    st.heroes = browser_.IsHeroes();
    st.browseTypes = browser_.EnabledTypes();
    st.folder = browser_.CurrentPath();
    // The applied filter, not searchText_: mid-debounce the box holds something
    // the user has not finished typing, and a session should not come back to
    // half a word.
    st.filter = browser_.Filter();
    st.selected = treeSelectedDisplay_;
    st.iconSize = iconSize_;
    st.treeSplit = treeSplit_;
    return st;
}

void StorageExplorer::RestoreState(const ExplorerState& state) {
    // These describe the panel itself and apply whether or not anything is
    // open. SetView is deliberately not used: it would reveal the folder the
    // grid is in, which at this point is the root.
    view_ = state.view;
    treeRowsDirty_ = true;
    SetIconSize(state.iconSize);
    treeSplit_ = state.treeSplit;

    // The rest name things inside a storage nobody has opened yet.
    restorePending_ = true;
    restoreGame_ = state.game;
    restoreHeroes_ = state.heroes;
    restoreTypes_ = state.browseTypes;
    restoreFolder_ = state.folder;
    restoreFilter_ = state.filter;
    restoreSelected_ = state.selected;
}

// The staged half of RestoreState, run once the browser's tree is complete.
void StorageExplorer::ApplyStagedRestore() {
    if (!restorePending_)
        return;
    restorePending_ = false; // one shot, applied or not
    // A folder, filter and selection belong to the storage they were recorded
    // in. If the user opened a different game first, they name nothing here.
    if (restoreGame_ != ProductId::Neutral && restoreGame_ != browser_.Product())
        return;
    // Same rule one level finer: a folder recorded in Heroes names nothing in
    // StarCraft II, and both report ProductId::Sc2.
    if (restoreGame_ == ProductId::Sc2 && restoreHeroes_ != browser_.IsHeroes())
        return;

    if (Any(restoreTypes_))
        browser_.SetEnabledTypes(restoreTypes_);

    if (!restoreFolder_.empty()) {
        browser_.NavigateTo(restoreFolder_);
        // A folder that is no longer in the storage leaves the breadcrumb
        // pointing at nothing and the grid empty. Totals are before the text
        // filter, so this tests the folder and not the filter.
        const auto& l = browser_.Current();
        if (l.folderTotal == 0 && l.fileTotal == 0)
            browser_.NavigateTo({});
    }
    if (!restoreFilter_.empty())
        SetSearchText(restoreFilter_);

    if (!restoreSelected_.empty()) {
        const std::string folder = ParentOfPath(restoreSelected_);
        const std::string name = NameOfPath(restoreSelected_);
        const std::string archive = browser_.ChildPathAt(folder, name);
        if (!archive.empty()) {
            selectedPath_ = archive;
            treeSelectedDisplay_ = restoreSelected_;
            selectedKind_ = KindOf(name);
        }
    }

    // Where the outline should land. In the tree the grid's folder never moves,
    // so it is the SELECTION that says where the user was; in the grid it is
    // the folder.
    RevealFolder(view_ == ExplorerView::Tree && !treeSelectedDisplay_.empty()
                     ? ParentOfPath(treeSelectedDisplay_)
                     : browser_.CurrentPath());
    treeRowsDirty_ = true;
}

void StorageExplorer::NavigateTo(const std::string& displayPath) {
    if (!browser_.IsOpen())
        return;
    browser_.NavigateTo(displayPath);
    if (pool_)
        pool_->Clear();
    ClearSelection();
}

void StorageExplorer::OpenCascDialog() {
    NFD::UniquePathU8 outPath;
    if (NFD::PickFolder(outPath) == NFD_OKAY && outPath)
        OpenStorage(outPath.get());
}

GameStorageKeys StorageExplorer::ResolveGame(ProductId game, bool heroes) const {
    GameStorageKeys keys = gameKeys_ ? gameKeys_(game) : GameStorageKeys{};
    if (heroes && game == ProductId::Sc2) {
        // Heroes has its own override and its own detected root, for the same
        // reason it has its own combo entry: one install path cannot name two
        // installs. A host's Sc2 keys answer for StarCraft II, so the path is
        // replaced outright rather than filled in only when empty.
        keys.installPath = provider_->HotsInstallPath();
        if (keys.installPath.empty())
            keys.installPath = provider_->HotsPath();
        return keys;
    }
    if (keys.installPath.empty())
        keys.installPath = provider_->GamePath(game);
    return keys;
}

bool StorageExplorer::OpenGame(ProductId game, bool heroes) {
    heroes = heroes && game == ProductId::Sc2;
    const GameStorageKeys keys = ResolveGame(game, heroes);
    if (keys.installPath.empty()) {
        lastError_ = std::string(GameLabel(game, heroes)) + " is not installed.";
        return false;
    }

    // Before the open, not after: the listfile and key list are part of the
    // CASC open key, so a storage acquired without them stays without them.
    SetCascKeys(keys.listfilePath, keys.tactKeyPath);
    // Auto: a detected Warcraft III root is CASC on Reforged and a directory
    // of MPQs on 1.2x, and the game finder does not distinguish them. Which of
    // the two Sc2 installs this turned out to be is not passed along — the
    // browser reads it off the build config either way.
    return OpenStorage(keys.installPath);
}

void StorageExplorer::Sync(ProductId fallback) {
    if (!browser_.IsOpen()) {
        // A restored panel remembers which game its own combo was on, which is
        // a more specific answer than the host's current profile.
        const bool restoring = restorePending_ && restoreGame_ != ProductId::Neutral;
        const ProductId want = restoring ? restoreGame_ : fallback;
        // A host's fallback is a ProductId and so cannot ask for Heroes; only a
        // restored panel can, which is the whole reason the flag is persisted.
        if (want != ProductId::Neutral)
            OpenGame(want, restoring && restoreHeroes_);
        return;
    }
    // Neutral is a hand-picked folder or a storage whose build config names no
    // product — the host has no settings for it to contribute.
    const ProductId game = browser_.Product();
    if (game == ProductId::Neutral)
        return;
    const GameStorageKeys keys = ResolveGame(game, browser_.IsHeroes());
    if (keys.listfilePath == listfilePath_ && keys.tactKeyPath == tactKeyPath_)
        return; // already showing exactly what the host knows
    SetCascKeys(keys.listfilePath, keys.tactKeyPath);
    // The kind it opened as, not CASC: a classic Warcraft III install is a
    // directory of MPQs, and reopening it as CASC here would close a working
    // browse over a listfile change that does not even apply to it.
    OpenInternal(openedRoot_, openedKind_);
}

// Game first, then a checkbox per type that game ships. One row, because they
// are one decision: which types exist at all is decided by the game, so a
// filter offered without it would be a filter for whatever happened to be open.
void StorageExplorer::BuildFilterBar() {
    ImGui::Spacing(); // clear of the menu bar above
    const ProductId open = browser_.Product();

    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Game", GameLabel(open, browser_.IsHeroes()))) {
        for (const BrowseTarget& target : kGames) {
            // Asked through ResolveGame rather than GamePath, so a host override
            // and Heroes' separate root both count as installed. A target with
            // no install is shown disabled, not hidden.
            const bool installed = !ResolveGame(target.game, target.heroes).installPath.empty();
            const bool current = target.game == open && target.heroes == browser_.IsHeroes();
            ImGui::BeginDisabled(!installed);
            if (ImGui::Selectable(GameLabel(target.game, target.heroes), current))
                OpenGame(target.game, target.heroes);
            ImGui::EndDisabled();
            if (!installed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Not installed");
        }
        ImGui::EndCombo();
    }

    // Only the types the open storage was actually walked for. A Warcraft III
    // install gets models, effects and textures; World of Warcraft gets `.m2`
    // and textures, and there is nothing else to offer. Textures come last and
    // start unticked (io::DefaultEnabledTypes): they are what you go looking
    // for, not what you browse through.
    const io::BrowseType available = browser_.AvailableTypes();
    io::BrowseType enabled = browser_.EnabledTypes();
    // The right edge of the row, read while the cursor is still at the start of
    // a line: six labels do not fit a narrow panel, and with no horizontal
    // scrollbar a checkbox past the edge is one nobody can tick. So they wrap
    // onto a second line instead.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (io::BrowseType type :
         {io::BrowseType::Models, io::BrowseType::Effects, io::BrowseType::M2, io::BrowseType::M3,
          io::BrowseType::Actor, io::BrowseType::Textures}) {
        if (!Any(available & type))
            continue;
        const char* label = io::BrowseTypeLabel(type);
        const float width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                            ImGui::CalcTextSize(label).x;
        if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width < rightEdge)
            ImGui::SameLine();
        bool on = Any(enabled & type);
        if (ImGui::Checkbox(label, &on))
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
                          "-portrait : exclude\n"
                          "%s",
                          view_ == ExplorerView::Tree
                              ? "Matched against the WHOLE path, so it prunes subtrees."
                              : "Matched against the names in this folder.");
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
        ImGui::SameLine();
        if (view_ == ExplorerView::Tree) {
            // The tree filters the whole storage, so "12 of 340 in this folder"
            // would be counting the wrong thing entirely.
            const std::size_t hits = browser_.TreeMatchCount();
            // Say when the filter was too broad to expand to, or a pruned tree
            // of collapsed folders reads as a search that found four things.
            ImGui::TextDisabled("%zu match%s%s", hits, hits == 1 ? "" : "es",
                                treeAutoExpand_ ? "" : " (too many to expand)");
        } else {
            const auto& l = browser_.Current();
            ImGui::TextDisabled("%zu of %zu", l.folders.size() + l.modelFiles.size(),
                                l.folderTotal + l.fileTotal);
        }
    }

    // Zoom, right-aligned so it stays put as the search row grows. The grid's
    // own: the tree has one thumbnail and it fills whatever its pane is.
    if (view_ == ExplorerView::Grid) {
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
}

// An open that enumerated nothing is indistinguishable from an empty folder,
// and for the one product where it is routine it is not the storage's fault but
// a missing setting. Say which.
void StorageExplorer::BuildEmptyHint() {
    ImGui::Spacing();
    ImGui::TextWrapped("Nothing browsable in '%s'.", browser_.Root().c_str());
    ImGui::Spacing();
    // Asked of the storage, not of the host's setting: an empty configured
    // path resolves to a conventionally placed CSV inside the CASC registry,
    // so "no path set" and "no listfile loaded" are different facts and only
    // the second one explains an empty tree.
    if (browser_.Product() == ProductId::Wow && !browser_.HasListfile()) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "No listfile was found.");
        ImGui::TextWrapped(
            "A World of Warcraft root is keyed by fileDataID, not by path: the names to "
            "browse are not in the install at all. Point the host at a community listfile "
            "CSV (in the Basic Viewer: Settings > IO > World of Warcraft > Listfile), or "
            "drop a listfile.csv / community-listfile.csv into the install folder or next "
            "to this application, then reopen this panel.");
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
        ClearSelection();
        navAnimT_ = 0.0f; // play the open transition for the new listing
    }

    if (provider_)
        provider_->Pump();

    ++frameCounter_;
    if (pool_)
        pool_->BeginFrame(frameCounter_);
    if (textures_)
        textures_->BeginFrame(frameCounter_);
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
            // The host's own installs first: switching between the two
            // generations of Warcraft III, which a machine may well have both
            // of, is then one click and no path typed. Check-marked rather
            // than radio-selected, because the user can also be somewhere
            // neither entry describes — and then none of them is marked.
            if (!namedRoots_.empty()) {
                for (const NamedRoot& entry : namedRoots_) {
                    const bool current = SamePath(entry.root, openedRoot_);
                    if (ImGui::MenuItem(entry.label.c_str(), nullptr, current) && !current)
                        OpenStorage(entry.root);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", entry.root.c_str());
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Open game folder…"))
                OpenCascDialog();
            ImGui::EndMenu();
        }
        // Before the early returns below: which browser you want is a decision
        // you may well be making *because* the other one showed nothing.
        BuildViewSwitch();
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
        ImGui::TextWrapped("Open a game folder (File ▸ Open game folder…) to browse its "
                           "models, effects and textures. A CASC install, a directory of "
                           "MPQ archives, or any folder of loose files.");
        if (!lastError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", lastError_.c_str());
        }
        ImGui::End();
        return;
    }

    if (view_ == ExplorerView::Tree)
        BuildTree();
    else
        BuildGrid();
    ImGui::End();
}

// Right-aligned in the menu bar, which is the one strip that survives every
// early return in BuildWindow.
void StorageExplorer::BuildViewSwitch() {
    constexpr float kWidth = 110.0f;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float x = ImGui::GetWindowWidth() - kWidth - style.WindowPadding.x - style.ItemSpacing.x;
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), x));
    ImGui::SetNextItemWidth(kWidth);
    const bool tree = view_ == ExplorerView::Tree;
    if (ImGui::BeginCombo("##view", tree ? "Tree View" : "Grid View")) {
        if (ImGui::Selectable("Grid View", !tree))
            SetView(ExplorerView::Grid);
        if (ImGui::Selectable("Tree View", tree))
            SetView(ExplorerView::Tree);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Grid: one folder of thumbnails.\n"
                          "Tree: the whole storage, with one large preview.");
}

void StorageExplorer::SetView(ExplorerView view) {
    if (view == view_)
        return;
    view_ = view;
    if (view_ != ExplorerView::Tree)
        return;
    // Land on the folder the grid was showing. The two are one browser, and an
    // outline that opened collapsed at the root would throw away the place the
    // user had already navigated to.
    RevealFolder(browser_.CurrentPath());
    treeRowsDirty_ = true;
}

void StorageExplorer::ClearSelection() {
    selectedPath_.clear();
    treeSelectedDisplay_.clear();
    selectedKind_ = StorageFileKind::Mdx;
}

void StorageExplorer::Activate(const std::string& archivePath, StorageFileKind kind) {
    if (!onActivate_ || archivePath.empty())
        return;
    ActivatedFile af;
    af.path = archivePath;
    af.kind = kind;
    af.isEffect = IsEffectKind(kind);
    af.isTexture = IsTextureKind(kind);
    af.provider = provider_;
    if (deliverBytes_ && provider_) {
        if (auto data = provider_->ReadFile(archivePath)) {
            af.bytes = std::move(*data);
            af.hasBytes = true;
        }
    }
    onActivate_(af);
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
    // Several screenfuls of images, for the same reason but a different price:
    // a texture cell is an idle texture, not a scene rendered every frame, so
    // the cap can be generous — and generous is what makes scrolling back up
    // instant instead of a wall of re-decodes.
    if (textures_)
        textures_->SetCap(std::max(512, cols * rows * 4));

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
        const bool isTexture = IsTextureKind(kind);
        beginCell();
        ImGui::PushID(idx++);
        ImGui::BeginGroup();
        const std::string archivePath = browser_.ChildPath(file);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + cell, p0.y + cell);

        const bool onScreen = ImGui::IsRectVisible(p0, p1);
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        // An image cell is the decoded file itself, so it needs the source
        // dimensions to letterbox with; a model cell renders square into its
        // own target and has none.
        const TextureThumbnail* image = nullptr;
        if (onScreen) {
            if (isTexture) {
                image = &textures_->Acquire(archivePath, frameCounter_);
                tex = image->texture;
            } else { // render the cell at its on-screen size so it stays crisp
                tex = pool_->Acquire(archivePath, isEffect, frameCounter_,
                                     static_cast<int>(cell));
            }
        }

        ImGui::InvisibleButton("##m", ImVec2(cell, cell));
        if (ImGui::IsItemClicked()) {
            selectedPath_ = archivePath;
            // The tree view previews whatever the grid last selected, so the
            // selection has to carry everything that view needs to draw it.
            treeSelectedDisplay_ = ChildDisplayPath(browser_.CurrentPath(), file);
            selectedKind_ = kind;
        }
        // Double-click opens the file — the host decides what that means.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            Activate(archivePath, kind);

        // Same bound as the folders above, for the same reason.
        if (onScreen) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tex != gfx::TextureHandle::Invalid) {
                if (image) {
                    // Letterboxed onto a checkerboard: an image cell shows the
                    // file's own aspect, and the squares behind it are how a
                    // transparent texture tells itself apart from a black one.
                    ImVec2 q0, q1;
                    FitRect(p0, p1, image->width, image->height, q0, q1);
                    if (image->hasAlpha)
                        DrawAlphaCheckers(dl, q0, q1, gridAlpha);
                    dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), q0, q1,
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 ImGui::GetColorU32(ImVec4(1, 1, 1, gridAlpha)));
                } else {
                    dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), p0, p1,
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 ImGui::GetColorU32(ImVec4(1, 1, 1, gridAlpha)));
                }
            } else if (image && !image->error.empty()) {
                // A texture that will not decode is a permanent state, not a
                // pending one — say so rather than leaving a placeholder that
                // reads as "still loading" forever.
                dl->AddRectFilled(p0, p1, placeholderColor, 4.0f);
                const ImVec2 sz = ImGui::CalcTextSize("?");
                dl->AddText(ImVec2(p0.x + (cell - sz.x) * 0.5f, p0.y + (cell - sz.y) * 0.5f),
                            ImGui::GetColorU32(ImVec4(0.75f, 0.45f, 0.45f, gridAlpha)), "?");
                ImGui::SetItemTooltip("%s", image->error.c_str());
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

// ---------------------------------------------------------------------------
// Tree view
// ---------------------------------------------------------------------------

std::string StorageExplorer::TreeSignature() const {
    return browser_.Root() + '\n' + browser_.Filter() + '\n' +
           std::to_string(static_cast<std::uint32_t>(browser_.EnabledTypes()));
}

void StorageExplorer::RevealFolder(const std::string& displayPath) {
    if (displayPath.empty())
        return; // the root is always open
    // Every ancestor prefix, so the row for `displayPath` is reachable at all.
    std::string acc;
    for (char c : displayPath) {
        if (c == '\\' || c == '/') {
            treeOpen_.insert(LowerPath(acc));
            acc.push_back('\\');
        } else {
            acc.push_back(c);
        }
    }
    treeOpen_.insert(LowerPath(acc));
    treeScrollTo_ = displayPath;
}

void StorageExplorer::RebuildTreeRows() {
    treeRows_.clear();
    // While a filter is narrow enough to BE a result set, it — and not the
    // user's arrows — decides what is expanded: an outline that hid its own
    // matches behind collapsed folders would be a search that shows nothing.
    // Past kAutoExpandMax it stops being a result set (a one-letter pattern
    // over a World of Warcraft install matches most of it), so the filter goes
    // back to merely pruning and the user's expansion drives the shape again.
    treeAutoExpand_ =
        !browser_.Filter().empty() && browser_.TreeVisibleFolderCount() <= kAutoExpandMax;
    if (browser_.IsOpen())
        AppendTreeRows({}, 0, treeAutoExpand_);
    treeTruncated_ = treeRows_.size() >= kMaxTreeRows;
}

void StorageExplorer::AppendTreeRows(const std::string& displayPath, int depth, bool autoExpand) {
    const io::StorageBrowser::TreeListing kids = browser_.TreeChildren(displayPath);
    // Folders first, each immediately followed by its own subtree, then this
    // folder's files — the shape every file tree has.
    for (const auto& folder : kids.folders) {
        if (treeRows_.size() >= kMaxTreeRows)
            return;
        TreeRow row;
        row.name = folder;
        row.path = ChildDisplayPath(displayPath, folder);
        row.depth = depth;
        row.isFolder = true;
        row.open = autoExpand || treeOpen_.count(LowerPath(row.path)) != 0;
        const bool open = row.open;
        const std::string child = row.path;
        treeRows_.push_back(std::move(row));
        if (open)
            AppendTreeRows(child, depth + 1, autoExpand);
    }
    for (const auto& file : kids.files) {
        if (treeRows_.size() >= kMaxTreeRows)
            return;
        TreeRow row;
        row.name = file;
        row.path = ChildDisplayPath(displayPath, file);
        row.archive = browser_.ChildPathAt(displayPath, file);
        row.depth = depth;
        treeRows_.push_back(std::move(row));
    }
}

void StorageExplorer::BuildTree() {
    if (filterUiVisible_) {
        BuildFilterBar();
        ImGui::Separator();
    }
    if (openedEmpty_) {
        BuildEmptyHint();
        return;
    }
    BuildSearchBar();
    ImGui::Separator();

    const std::string sig = TreeSignature();
    if (sig != treeSig_) {
        treeSig_ = sig;
        treeRowsDirty_ = true;
    }
    if (treeRowsDirty_) {
        treeRowsDirty_ = false;
        RebuildTreeRows();
    }

    // Outline | splitter | preview.
    constexpr float kSplitterW = 6.0f;
    constexpr float kMinPane = 140.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    treeSplit_ =
        std::clamp(treeSplit_, kMinPane, (std::max)(kMinPane, avail.x - kMinPane - kSplitterW));

    BuildTreePane(treeSplit_, avail.y);

    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    const float barH = (std::max)(1.0f, avail.y);
    ImGui::InvisibleButton("##split", ImVec2(kSplitterW, barH));
    if (ImGui::IsItemActive())
        treeSplit_ += ImGui::GetIO().MouseDelta.x;
    const bool grabbed = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (grabbed)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(sp.x + kSplitterW * 0.5f - 1.0f, sp.y),
        ImVec2(sp.x + kSplitterW * 0.5f + 1.0f, sp.y + barH),
        ImGui::GetColorU32(grabbed ? ImGuiCol_SeparatorActive : ImGuiCol_Separator));

    ImGui::SameLine(0.0f, 0.0f);
    BuildPreviewPane();
}

void StorageExplorer::BuildTreePane(float width, float height) {
    ImGui::BeginChild("outline", ImVec2(width, height), ImGuiChildFlags_Borders);

    if (treeRows_.empty()) {
        ImGui::TextDisabled("%s", !browser_.Filter().empty()
                                      ? "Nothing in this storage matches the filter."
                                      : "Nothing to show in this storage.");
        ImGui::EndChild();
        return;
    }

    const float pitch = ImGui::GetTextLineHeightWithSpacing();
    const float glyph = ImGui::GetTextLineHeight(); // the disclosure column
    // A reveal nobody scrolled to is a reveal that did not happen. The uniform
    // row pitch is what makes this a multiply rather than a measure.
    if (!treeScrollTo_.empty()) {
        const float view = ImGui::GetContentRegionAvail().y;
        for (std::size_t i = 0; i < treeRows_.size(); ++i) {
            if (treeRows_[i].path != treeScrollTo_)
                continue;
            ImGui::SetScrollY((std::max)(0.0f, static_cast<float>(i) * pitch - view * 0.35f));
            break;
        }
        treeScrollTo_.clear();
    }

    const ImU32 arrowCol = ImGui::GetColorU32(ImVec4(0.85f, 0.72f, 0.35f, 1.0f));
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    // Toggles are DEFERRED: opening a folder reflattens treeRows_, which is the
    // very vector the clipper is walking.
    std::string toggle;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(treeRows_.size()), pitch);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const TreeRow& row = treeRows_[static_cast<std::size_t>(i)];
            const float indent = kTreeIndent * static_cast<float>(row.depth);
            ImGui::PushID(i);
            // Guarded, because Indent(0.0f) does NOT indent by zero: ImGui reads
            // 0 as "unspecified" and applies style.IndentSpacing. Unguarded, a
            // depth-0 row is pushed 21px in and a depth-1 row only 14, so the
            // second level draws LEFT of the first while every level below it
            // nests correctly off its own parent.
            if (indent > 0.0f)
                ImGui::Indent(indent);

            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const bool selected = !row.isFolder && row.path == treeSelectedDisplay_;
            ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick);
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                if (!row.isFolder) {
                    selectedPath_ = row.archive;
                    treeSelectedDisplay_ = row.path;
                    selectedKind_ = KindOf(row.name);
                } else if (!treeAutoExpand_) {
                    toggle = row.path;
                }
            }
            if (!row.isFolder && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                Activate(row.archive, KindOf(row.name));

            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (row.isFolder) {
                // Disclosure triangle, drawn rather than typed: the font atlas
                // bakes the Latin range and no arrow glyph is in it.
                const ImVec2 c(p.x + glyph * 0.5f, p.y + glyph * 0.5f);
                const float r = glyph * 0.28f;
                if (row.open) {
                    dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f),
                                          ImVec2(c.x + r, c.y - r * 0.6f),
                                          ImVec2(c.x, c.y + r * 0.8f), arrowCol);
                } else {
                    dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r),
                                          ImVec2(c.x + r * 0.8f, c.y),
                                          ImVec2(c.x - r * 0.6f, c.y + r), arrowCol);
                }
            }
            const float labelX = p.x + glyph + ImGui::GetStyle().ItemInnerSpacing.x;
            const std::string label = FitLabel(row.name, p.x + rowWidth - labelX);
            dl->AddText(ImVec2(labelX, p.y), textCol, label.c_str());
            // The whole point of this view is the path, so hovering a file shows
            // it in full; a folder only when its own name had to be cut.
            if (hovered && (!row.isFolder || label != row.name))
                ImGui::SetTooltip("%s", row.path.c_str());

            if (indent > 0.0f)
                ImGui::Unindent(indent);
            ImGui::PopID();
        }
    }

    if (treeTruncated_) {
        ImGui::Spacing();
        ImGui::TextDisabled("(stopped at %zu rows - narrow the filter)", kMaxTreeRows);
    }
    ImGui::EndChild();

    if (toggle.empty())
        return;
    const std::string key = LowerPath(toggle);
    if (!treeOpen_.insert(key).second)
        treeOpen_.erase(key);
    treeRowsDirty_ = true;
}

void StorageExplorer::BuildPreviewPane() {
    ImGui::BeginChild("preview", ImVec2(0, 0), ImGuiChildFlags_Borders);

    if (selectedPath_.empty()) {
        ImGui::TextDisabled("Select a file to preview it.");
        ImGui::Spacing();
        ImGui::TextDisabled("Double-click opens it in the viewer.");
        ImGui::EndChild();
        return;
    }

    ImGui::TextWrapped("%s", treeSelectedDisplay_.empty() ? selectedPath_.c_str()
                                                          : treeSelectedDisplay_.c_str());
    const bool isTexture = IsTextureKind(selectedKind_);
    if (ImGui::SmallButton(isTexture ? "Use this texture" : "Open in viewer"))
        Activate(selectedPath_, selectedKind_);

    // What a texture preview is for: the numbers you cannot read off the
    // picture. Drawn before the separator so the image still gets the pane.
    const TextureThumbnail* image = nullptr;
    if (isTexture && textures_) {
        image = &textures_->Acquire(selectedPath_, frameCounter_);
        if (image->ok) {
            ImGui::SameLine();
            ImGui::TextDisabled("%dx%d  %s%s", image->width, image->height, image->format.c_str(),
                                image->hasAlpha ? "  alpha" : "");
        }
    }
    ImGui::Separator();

    // One cell is all this view shows, but the pool keeps the last few alive so
    // clicking back and forth along a folder does not reload each time.
    if (pool_)
        pool_->SetCap(kMinCells);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float edge = (std::max)(kMinIcon, (std::min)(avail.x, avail.y));
    const ImVec2 p0(origin.x + (std::max)(0.0f, (avail.x - edge) * 0.5f),
                    origin.y + (std::max)(0.0f, (avail.y - edge) * 0.5f));
    const ImVec2 p1(p0.x + edge, p0.y + edge);

    const gfx::TextureHandle tex =
        image ? image->texture
              : pool_->Acquire(selectedPath_, IsEffectKind(selectedKind_), frameCounter_,
                               static_cast<int>(edge));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex != gfx::TextureHandle::Invalid) {
        if (image) {
            ImVec2 q0, q1;
            FitRect(p0, p1, image->width, image->height, q0, q1);
            if (image->hasAlpha)
                DrawAlphaCheckers(dl, q0, q1, 1.0f);
            dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), q0, q1);
        } else {
            dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), p0, p1);
        }
    } else {
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(0.25f, 0.28f, 0.34f, 1.0f)), 4.0f);
        const char* status =
            (image && !image->error.empty()) ? image->error.c_str() : "Loading...";
        const ImVec2 ts = ImGui::CalcTextSize(status);
        dl->AddText(ImVec2(p0.x + (edge - ts.x) * 0.5f, p0.y + (edge - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), status);
    }
    ImGui::EndChild();
}

renderer::SceneId StorageExplorer::DebugFirstCellScene() const {
    return pool_ ? pool_->DebugFirstCellScene() : 0;
}

void StorageExplorer::RenderThumbnails(float dt) {
    // Images first, and outside the settings guard below: a decode reads bytes
    // and uploads a texture, which touches neither the render settings nor the
    // active scene. Here rather than in BuildWindow because the budget should
    // be spent on what the frame ACTUALLY asked for, which is only known once
    // the draw list is built.
    if (textures_)
        textures_->EndFrame();

    if (!pool_)
        return;

    // Cell render state (mode, SD-HDR, HD overlay) is each cell SCENE's own
    // now — nothing to save or restore on that axis. What is still global is
    // the display styling: force the floor grid off and clear the cells to
    // the panel background, then put both back for the host's main view.
    auto& s = svc_.Settings();
    const auto savedFlags = s.GetDisplayFlags();
    const std::uint32_t savedBg = s.BackgroundColorRaw();

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
}

} // namespace whiteout::flakes::tools
