#include "viewer_ui.h"

#include "io/wem/wem_import.h" // WemDocument, for the profile picker

#include "imgui_viewcube.h"
#include "io/load_task.h" // Poll(), for the equip popup's registry-build bar
#include "io/mdx_model_adapter.h"
#include "io/storage/game_rules.h" // ScanArchives, for a profile that is not the active one
#include "localization.h"
#include "log_console.h"
#include "mdx_export.h"
#include "progress_dialog.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/dnc/dnc_catalog.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/effects/splat_service.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "settings_ini.h"
#include "viewer_app.h"

#include <whiteout/models/mdx/writer.h>
#include "renderer/model/model_source_utils.h" // DispatchTextureParser (decode)
// Texture encoders for the Save As "export textures" option (every WhiteoutLib
// image writer except GIF).
#include <whiteout/textures/blp/writer.h>
#include <whiteout/textures/bmp/writer.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/jpeg/writer.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/tga/writer.h>
#include <whiteout/textures/tiff/writer.h>
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/display.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/util/path_utf8.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include <imgui.h>
#include <nfd.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes {

namespace {

// ---- Toolbar icons ---------------------------------------------------------
//
// Drawn, not typed. The viewer bakes ONE static atlas (Noto SC + the merged KR
// cut, or embedded Roboto when the fonts directory is missing), so an icon font
// would mean a second TTF merged into that atlas plus an install rule for it —
// and it would still be absent on the Roboto fallback. A handful of vector
// glyphs cost nothing, scale with the DPI-baked frame height, and take the
// theme's text colour for free.
enum class ToolbarIcon { Play, Pause, Restart, Tracks };

void DrawToolbarIcon(ImDrawList* dl, ToolbarIcon icon, ImVec2 c, f32 s, ImU32 col) {
    constexpr f32 kPi = 3.14159265358979323846f;
    switch (icon) {
    case ToolbarIcon::Play: {
        // Nudged right: a triangle centred on its bounding box reads as sitting
        // too far left, because its mass is in the flat edge.
        c.x += s * 0.03f;
        const f32 w = s * 0.30f;
        const f32 h = s * 0.30f;
        dl->AddTriangleFilled(ImVec2(c.x - w * 0.6f, c.y - h), ImVec2(c.x + w, c.y),
                              ImVec2(c.x - w * 0.6f, c.y + h), col);
        break;
    }
    case ToolbarIcon::Pause: {
        const f32 bar = s * 0.12f;
        const f32 gap = s * 0.07f; // half the space between the two bars
        const f32 h = s * 0.30f;
        dl->AddRectFilled(ImVec2(c.x - gap - bar, c.y - h), ImVec2(c.x - gap, c.y + h), col,
                          bar * 0.3f);
        dl->AddRectFilled(ImVec2(c.x + gap, c.y - h), ImVec2(c.x + gap + bar, c.y + h), col,
                          bar * 0.3f);
        break;
    }
    case ToolbarIcon::Restart: {
        // The ↻ everything else uses for "play it again": most of a circle with
        // the gap across the top, and the head at the END of travel, so the
        // arrow points the way the stroke was going.
        const f32 r = s * 0.28f;
        const f32 th = std::max(1.5f, s * 0.095f);
        constexpr f32 kFrom = -0.28f * kPi;
        constexpr f32 kTo = 1.28f * kPi;
        dl->PathArcTo(c, r, kFrom, kTo);
        dl->PathStroke(col, ImDrawFlags_None, th);

        // Tangent at kTo, in the direction the arc was drawn, and its normal.
        // The head's base sits ON the arc's last point so the two read as one
        // stroke; longer than it is wide, or it looks like a blob.
        const ImVec2 end(c.x + r * std::cos(kTo), c.y + r * std::sin(kTo));
        const ImVec2 dir(-std::sin(kTo), std::cos(kTo));
        const ImVec2 nrm(-dir.y, dir.x);
        const f32 len = th * 2.3f;
        const f32 wide = th * 1.15f;
        dl->AddTriangleFilled(ImVec2(end.x + dir.x * len, end.y + dir.y * len),
                              ImVec2(end.x + nrm.x * wide, end.y + nrm.y * wide),
                              ImVec2(end.x - nrm.x * wide, end.y - nrm.y * wide), col);
        break;
    }
    case ToolbarIcon::Tracks: {
        // Three stacked bars of unequal length — the track list a timeline puts
        // beside its transport, which is what the window behind this button is:
        // layered plays, their sub-tracks, and the files they came from.
        const f32 th = std::max(1.5f, s * 0.10f);
        const f32 x0 = c.x - s * 0.27f;
        const f32 w[3] = {s * 0.54f, s * 0.32f, s * 0.44f};
        for (i32 i = 0; i < 3; ++i) {
            const f32 y = c.y + (static_cast<f32>(i) - 1.0f) * s * 0.20f;
            dl->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x0 + w[i], y + th * 0.5f), col,
                              th * 0.5f);
        }
        break;
    }
    }
}

// A square icon button — a real ImGui::Button (hover, active, keyboard nav, the
// theme's frame colours) with the glyph painted over its face. Square at the
// frame height so it lines up with the combos beside it, and so swapping
// play for pause cannot resize it and shuffle the toolbar sideways.
//
// @p active holds the button in its pressed colour, for the one that toggles a
// window rather than firing an action: with no words on the face, the colour is
// all that says the window is already open.
bool IconButton(const char* id, ToolbarIcon icon, const char* nameKey, const char* tipKey,
                bool active = false) {
    const f32 side = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (active)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(id, ImVec2(side, side));
    if (active)
        ImGui::PopStyleColor();
    DrawToolbarIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + side * 0.5f, p.y + side * 0.5f),
                    side, ImGui::GetColorU32(ImGuiCol_Text));
    // The face carries no words, so the tooltip names the button before it
    // explains it — that name is the only place the label text survives.
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", i18n::tr(nameKey), i18n::tr(tipKey));
    return clicked;
}

// ImGui writes a widget's label to its RIGHT. On a horizontal toolbar that
// reads backwards — you meet the value before you are told what it is — so
// every control here puts its caption in front by hand and carries a "##id"
// instead.
//
// The wider-than-usual gap in front is what keeps a caption attached to the
// control AFTER it rather than the one before: with uniform spacing the two
// are equidistant and the eye has to guess. Every caller is preceded by a
// SameLine (the transport group opens the row unconditionally), and a second
// SameLine simply re-places the cursor with the new spacing.
// AlignTextToFramePadding then drops the text onto the frame's baseline;
// without it the caption rides high against a taller neighbour.
void ToolbarLabel(const char* key) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr(key));
    ImGui::SameLine();
}

// Persists current host state to WhiteoutFlakes.ini. Called after every UI
// change that should survive a restart — same call shape as the old
// HandleSettingsMessage paths used.
void SaveIni(const ViewerApp& app) {
    SaveSettingsIni(app.Service(), app.LoopNonLoopingPolicy(), app.ForceHd(),
                    i18n::languageCode(i18n::Localizer::instance().current()));
}

constexpr std::array<const char*, 5> kLodLabels = {
    "Auto (screen size)", "Force LOD 0 (base)",   "Force LOD 1",
    "Force LOD 2",        "Force LOD 3 (lowest)",
};

constexpr std::array<const char*, 4> kIblLabels = {"Portrait", "Day/Night", "Dungeon", "Sunset"};
constexpr std::array<const char*, 3> kLightingLabels = {"InGame", "Glue", "Dynamic"};
constexpr std::array<const char*, 4> kShadowLabels = {"Off", "1 cascade", "2 cascades",
                                                      "3 cascades"};
constexpr std::array<const char*, 5> kBackendLabels = {"D3D11", "D3D12", "Vulkan", "WebGPU",
                                                       "Metal"};

// Parallel i18n key arrays for the visible label arrays above. Backend names
// are product/tech names and stay in English, so they get no key array.
// Every debug view, in menu order, with the label it wears in the PBR and the
// legacy menu. Which of the two a model gets is the renderer's call
// (DebugFamilyOf); which views a family lists is DebugViewInFamily.
struct DebugViewItem {
    DebugView view;
    const char* pbrKey;
    const char* legacyKey;
};
constexpr std::array<DebugViewItem, 20> kDebugViews = {{
    {DebugView::Off, "debugvis.off", "debugvis.off"},
    {DebugView::Albedo, "debugvis.albedo", "debugvis.diffuse"},
    {DebugView::Normal, "debugvis.world_normal", "debugvis.normal_mapped"},
    {DebugView::VertexNormal, "debugvis.vertex_normal", "debugvis.vertex_normal"},
    {DebugView::Roughness, "debugvis.roughness", "debugvis.roughness"},
    {DebugView::Metalness, "debugvis.metalness", "debugvis.metalness"},
    {DebugView::MaterialAo, "debugvis.material_ao", "debugvis.material_ao"},
    {DebugView::Specular, "debugvis.specular", "debugvis.specular"},
    {DebugView::Gloss, "debugvis.gloss", "debugvis.gloss"},
    {DebugView::Emissive, "debugvis.emissive", "debugvis.emissive"},
    {DebugView::TeamMask, "debugvis.team_mask", "debugvis.team_mask"},
    {DebugView::VertexColor, "debugvis.vertex_color", "debugvis.vertex_color"},
    {DebugView::Opacity, "debugvis.opacity", "debugvis.opacity"},
    {DebugView::TextureMip, "debugvis.lod_heatmap", "debugvis.lod_heatmap"},
    {DebugView::LightCount, "debugvis.light_count", "debugvis.light_count"},
    {DebugView::LightingWhite, "debugvis.shading_white", "debugvis.shading_white"},
    {DebugView::LightingGrey, "debugvis.shading_grey", "debugvis.shading_grey"},
    {DebugView::SpecularOnly, "debugvis.specular_only", "debugvis.specular_only"},
    {DebugView::NoOrm, "debugvis.no_orm", "debugvis.no_orm"},
    {DebugView::AoOnly, "debugvis.ao_only", "debugvis.ao_only"},
}};
constexpr std::array<const char*, 5> kLodKeys = {
    "lod.auto", "lod.0", "lod.1", "lod.2", "lod.3",
};
constexpr std::array<const char*, 4> kIblKeys = {"ibl.portrait", "ibl.daynight", "ibl.dungeon",
                                                 "ibl.sunset"};
constexpr std::array<const char*, 3> kLightingKeys = {"lighting.ingame", "lighting.glue",
                                                      "lighting.dynamic"};
constexpr std::array<const char*, 4> kShadowKeys = {"shadow.off", "shadow.1", "shadow.2",
                                                    "shadow.3"};
// Indexed by the shader's fog mode (bls::FogParams::mode).
constexpr std::array<const char*, 7> kFogModeKeys = {"fog.off",        "fog.linear", "fog.exp",
                                                     "fog.exp2",       "fog.volumetric",
                                                     "fog.exp_banded", "fog.exp2_banded"};

// The games the Settings window can configure, in left-panel order. Game
// names, so they are not localised — same rule the backend list follows.
// StarCraft II and Heroes of the Storm are one entry because they are one
// ProductId: they share a render profile, and the provider opens both.
struct SettingsProfile {
    ProductId product;
    const char* label;
};
constexpr std::array<SettingsProfile, 4> kSettingsProfiles = {{
    {ProductId::Wc3, "Warcraft III"},
    {ProductId::Sc2, "StarCraft II / Storm"},
    {ProductId::Wow, "World of Warcraft"},
    {ProductId::D3, "Diablo III"},
}};

// Games that never shipped an MPQ, so their settings page offers CASC roots
// and nothing else. Asking this rather than testing == Sc2 in five places is
// what keeps a fourth CASC-only product from needing five edits.
constexpr bool IsCascOnly(ProductId game) {
    return game == ProductId::Sc2 || game == ProductId::D3;
}

i32 BackendToIdx(gfx::GfxApi b) {
    switch (b) {
    case gfx::GfxApi::D3D11:
        return 0;
    case gfx::GfxApi::D3D12:
        return 1;
    case gfx::GfxApi::Vulkan:
        return 2;
    case gfx::GfxApi::WebGPU:
        return 3;
    case gfx::GfxApi::Metal:
        return 4;
    }
    return 1;
}
gfx::GfxApi IdxToBackend(i32 idx) {
    switch (idx) {
    case 0:
        return gfx::GfxApi::D3D11;
    case 2:
        return gfx::GfxApi::Vulkan;
    case 3:
        return gfx::GfxApi::WebGPU;
    case 4:
        return gfx::GfxApi::Metal;
    default:
        return gfx::GfxApi::D3D12;
    }
}

} // namespace

ViewerUI::ViewerUI(ViewerApp& app) : app_(app), exportWindow_(app) {
    // NFD's init / quit can be reference-counted; doing it once at first UI
    // construction matches its single-process expectations.
    NFD::Init();
}

void ViewerUI::BuildFrame() {
    BuildMenuBar();
    BuildToolbar();
    BuildTabBar();
    if (showViewCube_)
        BuildViewCubeWidget();
    if (settingsOpen_)
        BuildSettingsWindow();
    if (animWindowOpen_ && app_.CanAttachAnimations())
        BuildAnimationWindow();
    BuildSaveOptionsPopup();
    BuildMdxExportPopup();
    BuildM3ExportPopup();
    BuildGltfExportPopup();
    BuildM3SavePopup();
    BuildWemProfilePopup();
    exportWindow_.Build();
    app_.BuildStorageExplorerWindow();
    tools::LogConsole::Instance().DrawUi(&showLogConsole_);
    // Last, so it lands over everything else — which is the point of a modal
    // for work the rest of the UI cannot usefully be clicked during.
    tools::DrawProgressModal(app_.Tasks());
}

void ViewerUI::BuildViewCubeWidget() {
    // The view-cube is a pure host-side ImGui widget — the renderer has no
    // notion of it. See tools/common/imgui_viewcube.h. Offset it below our
    // toolbar (+ tab bar, when documents are open) so it isn't tucked under the
    // top strips — both are menuH+8 tall and sit stacked under the menu bar.
    const f32 stripH = ImGui::GetFrameHeight() + 8.0f;
    const f32 topOffset = stripH + (app_.DocumentCount() > 0 ? stripH : 0.0f);
    tools::DrawViewCube(app_.Service().Scene().Camera(), topOffset);
}

void ViewerUI::OpenFileDialog() {
    // Use the UTF-8 NFD entry points so the filter strings stay as plain
    // `char` literals on every platform — the native variant takes wchar_t
    // on Windows, which would break these inline string constants.
    NFD::UniquePathU8 outPath;
    nfdu8filteritem_t filter[5] = {{"All supported", kOpenAllExtensions},
                                   {"Warcraft III Model", "mdx,mdl"},
                                   {"PKB Effect", "pkb,pkfx"},
                                   {"WEM model", "wem"},
                                   {"Other Blizzard model", kForeignModelExtensions}};
    const nfdfiltersize_t nFilters = kHasForeignModelFilter ? 5 : 4;
    if (NFD::OpenDialog(outPath, filter, nFilters) != NFD_OKAY)
        return;

    std::filesystem::path p = io::FsPathFromUtf8(outPath.get());

    // A `.wem` does not open on the strength of its name: which profile it is
    // opened AS decides the materials, the render path and the game whose
    // storage its textures resolve against, and only the file can say what it
    // offers. So the pick comes first and the load waits for it.
    if (auto document = app_.PeekWemDocument(p)) {
        wemOpenDocument_ = std::move(document);
        wemOpenPath_ = p;
        wemOpenOptions_ = io::WemProfileOptions(wemOpenDocument_->document);
        wemOpenSelection_ = 0;
        const auto preferred = io::DefaultWemProfile(wemOpenDocument_->document);
        for (std::size_t i = 0; i < wemOpenOptions_.size(); ++i) {
            if (wemOpenOptions_[i].profile == preferred) {
                wemOpenSelection_ = static_cast<i32>(i);
                break;
            }
        }
        openWemProfilePopup_ = true;
        return;
    }

    // Async: an `.m2` or `.m3` picked here may need a game install that
    // is not open yet, and that open is seconds long. Dispatches
    // .pkb / .pkfx to the effect loader exactly as LoadModel does.
    app_.OpenModelAsync(p);
}

// The profile picker. Every row the file could be opened as, in the order
// io::WemProfileOptions ranks them: carried first, derives after, and the ones
// this build cannot open last and disabled — with the reason, because a file
// that says "Diablo III" and a dialog with no Diablo III row reads as a bug.
void ViewerUI::BuildWemProfilePopup() {
    if (openWemProfilePopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.wem.title"));
        openWemProfilePopup_ = false;
    }
    if (!wemOpenDocument_)
        return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.wem.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(io::PathToUtf8(wemOpenPath_.filename()).c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(i18n::tr("dialog.wem.prompt"));
    ImGui::Spacing();

    for (std::size_t i = 0; i < wemOpenOptions_.size(); ++i) {
        const io::WemProfileOption& option = wemOpenOptions_[i];
        ImGui::BeginDisabled(!option.supported);
        if (ImGui::RadioButton(option.displayName, wemOpenSelection_ == static_cast<i32>(i)))
            wemOpenSelection_ = static_cast<i32>(i);
        ImGui::EndDisabled();

        // What the row costs, said on the row. "Derived" is the load-bearing
        // one: it is always lossy (§6.6) and the alternative to saying so is a
        // model that quietly renders as an approximation of itself.
        ImGui::SameLine();
        if (!option.supported) {
            ImGui::TextDisabled("— %s", io::WemProfileUnsupportedReason(option.profile));
        } else if (option.carried) {
            ImGui::TextDisabled("— %s", i18n::tr(option.drawn ? "dialog.wem.carried"
                                                              : "dialog.wem.carried_undrawn"));
        } else {
            ImGui::TextDisabled("— %s", i18n::tr("dialog.wem.derived"));
        }
    }

    ImGui::Spacing();
    const bool canOpen = wemOpenSelection_ >= 0 &&
                         wemOpenSelection_ < static_cast<i32>(wemOpenOptions_.size()) &&
                         wemOpenOptions_[static_cast<std::size_t>(wemOpenSelection_)].supported;
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button(i18n::tr("dialog.wem.open"), ImVec2(120, 0))) {
        const auto profile = wemOpenOptions_[static_cast<std::size_t>(wemOpenSelection_)].profile;
        // Synchronous, unlike the OpenModelAsync path above: the file is
        // already parsed and its textures resolve against whichever install
        // the profile named, which OpenWemAs opens on the way through.
        app_.OpenWemAs(wemOpenPath_, wemOpenDocument_, profile);
        wemOpenDocument_.reset();
        wemOpenOptions_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(120, 0))) {
        wemOpenDocument_.reset();
        wemOpenOptions_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Export to MDX ---------------------------------------------------------
//
// Save As writes a Warcraft III model back in its own format. This writes a
// FOREIGN one — a `.m2`, a `.m3`, a Diablo III `.app` — as Warcraft III, which
// is the direction WEM exists for and which has choices Save As does not: the
// generation decides both the material vocabulary and the texture container,
// and the textures are only reachable while the model's own game is the one
// mounted. See tools/basic_viewer/mdx_export.h.

void ViewerUI::ExportMdxDialog() {
    nfdu8filteritem_t filter[1] = {{"Warcraft III model", "mdx"}};
    NFD::UniquePathU8 outPath;
    if (NFD::SaveDialog(outPath, filter, 1) != NFD_OKAY)
        return;
    pendingMdxPath_ = outPath.get();
    openMdxExportPopup_ = true;
}

void ViewerUI::BuildMdxExportPopup() {
    if (openMdxExportPopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.mdx.title"));
        openMdxExportPopup_ = false;
    }
    if (pendingMdxPath_.empty())
        return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.mdx.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(i18n::tr("dialog.mdx.prompt"));

    // Drawn, and drawn disabled. The file this writes IS generation-specific —
    // a v1000 container with PBR layers, or a v800 one with a blend stack — so
    // hiding the row would hide what the user is getting; and classic is not
    // offered because deriving that vocabulary has not been measured yet.
    ImGui::BeginDisabled(true);
    ImGui::RadioButton(i18n::tr("dialog.mdx.classic"), &mdxExportProfile_, 0);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::RadioButton(i18n::tr("dialog.mdx.reforged"), &mdxExportProfile_, 1);
    ImGui::TextDisabled("%s", i18n::tr("dialog.mdx.classic_soon"));

    ImGui::Separator();
    ImGui::Checkbox(i18n::tr("dialog.mdx.export_textures"), &mdxExportTextures_);
    const auto profile =
        (mdxExportProfile_ == 0) ? wem::ProfileId::Wc3Classic : wem::ProfileId::Wc3Reforged;
    ImGui::BeginDisabled(!mdxExportTextures_);
    // The format is not a choice: Reforged reads `.dds` and classic Warcraft III
    // reads BLP1, so saying which one is getting written is the whole of what
    // there is to say about it.
    ImGui::TextDisabled("%s: %s", i18n::tr("dialog.mdx.convert_to"), Wc3TextureExtension(profile));
    ImGui::TextDisabled("%s", i18n::tr("dialog.mdx.export_hint"));
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button(i18n::tr("dialog.mdx.export"), ImVec2(120, 0))) {
        app_.ExportMdx(io::FsPathFromUtf8(pendingMdxPath_), profile, mdxExportTextures_);
        pendingMdxPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0))) {
        pendingMdxPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Export to M3 ----------------------------------------------------------
//
// The MDX export one game over: a `.mdx` (either generation), a `.m2` or a
// Diablo III `.app` written as a StarCraft II `.m3` through WEM. See
// tools/basic_viewer/m3_export.h.

void ViewerUI::ExportM3Dialog() {
    nfdu8filteritem_t filter[1] = {{"StarCraft II model", "m3"}};
    NFD::UniquePathU8 outPath;
    if (NFD::SaveDialog(outPath, filter, 1) != NFD_OKAY)
        return;
    pendingM3Path_ = outPath.get();
    openM3ExportPopup_ = true;
}

void ViewerUI::BuildM3ExportPopup() {
    if (openM3ExportPopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.m3.title"));
        openM3ExportPopup_ = false;
    }
    if (pendingM3Path_.empty())
        return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.m3.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(i18n::tr("dialog.m3.prompt"));

    // Both rows live, unlike the MDX popup's generation row: the games share
    // the container and the version field is the whole of the difference
    // (v29 imports back as StarCraft II, v30 as Heroes of the Storm).
    ImGui::RadioButton(i18n::tr("dialog.m3.sc2"), &m3ExportProfile_, 0);
    ImGui::SameLine();
    ImGui::RadioButton(i18n::tr("dialog.m3.heroes"), &m3ExportProfile_, 1);

    // The checkbox reuses the MDX popup's string; the hint does not, because a
    // `.m3` names its textures by mod path and they go in a folder of their
    // own. No format row either: both profiles read `.dds`.
    ImGui::Separator();
    ImGui::Checkbox(i18n::tr("dialog.mdx.export_textures"), &m3ExportTextures_);
    ImGui::BeginDisabled(!m3ExportTextures_);
    ImGui::TextDisabled("%s: dds", i18n::tr("dialog.mdx.convert_to"));
    ImGui::TextDisabled("%s", i18n::tr("dialog.m3.export_hint"));
    ImGui::EndDisabled();

    // The Warcraft III knobs; inert for every other source. War3 (Mod) is
    // StarCraft II's, and it only saves writing textures.
    ImGui::Separator();
    ImGui::Checkbox(i18n::tr("dialog.m3.exact_passes"), &m3ExactPasses_);
    ImGui::Checkbox(i18n::tr("dialog.m3.sharpen_team_key"), &m3SharpenTeamKey_);
    ImGui::BeginDisabled(!m3ExportTextures_ || m3ExportProfile_ != 0);
    ImGui::Checkbox(i18n::tr("dialog.m3.war3_mod_textures"), &m3War3ModTextures_);
    ImGui::TextDisabled("%s", i18n::tr("dialog.m3.war3_mod_hint"));
    ImGui::EndDisabled();

    ImGui::Separator();
    const auto m3Profile =
        (m3ExportProfile_ == 0) ? wem::ProfileId::Sc2 : wem::ProfileId::Heroes;
    if (ImGui::Button(i18n::tr("dialog.mdx.export"), ImVec2(120, 0))) {
        app_.ExportM3(io::FsPathFromUtf8(pendingM3Path_), m3Profile, m3ExportTextures_,
                      m3ExactPasses_, m3SharpenTeamKey_,
                      m3War3ModTextures_ && m3ExportProfile_ == 0);
        pendingM3Path_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0))) {
        pendingM3Path_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Export to glTF ---------------------------------------------------------
//
// The export pointed out of the Blizzard family: any model WEM reads is
// lowered to metallic-roughness and written as `.glb` or `.gltf`, for Blender
// and everything else that speaks the interchange format the rest of the
// world settled on. See tools/basic_viewer/gltf_export.h and GLTF_DESIGN.md.

void ViewerUI::ExportGltfDialog() {
    nfdu8filteritem_t filter[2] = {{"glTF binary", "glb"}, {"glTF text", "gltf"}};
    NFD::UniquePathU8 outPath;
    if (NFD::SaveDialog(outPath, filter, 2) != NFD_OKAY)
        return;
    pendingGltfPath_ = outPath.get();
    openGltfExportPopup_ = true;
}

void ViewerUI::BuildGltfExportPopup() {
    if (openGltfExportPopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.gltf.title"));
        openGltfExportPopup_ = false;
    }
    if (pendingGltfPath_.empty())
        return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.gltf.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(i18n::tr("dialog.gltf.prompt"));

    // The container is the filename's own choice — `.glb` embeds everything in
    // one file, `.gltf` writes JSON + `.bin` + images for DCC editing — so the
    // dialog states it rather than asking twice.
    std::string ext = std::filesystem::path(io::FsPathFromUtf8(pendingGltfPath_))
                          .extension()
                          .string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool binary = ext != ".gltf";
    ImGui::TextDisabled("%s", i18n::tr(binary ? "dialog.gltf.glb" : "dialog.gltf.gltf"));

    ImGui::Separator();
    ImGui::Checkbox(i18n::tr("dialog.mdx.export_textures"), &gltfExportTextures_);
    ImGui::BeginDisabled(!gltfExportTextures_);
    ImGui::TextDisabled("%s: png", i18n::tr("dialog.mdx.convert_to"));
    ImGui::TextDisabled("%s", i18n::tr("dialog.mdx.export_hint"));
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button(i18n::tr("dialog.mdx.export"), ImVec2(120, 0))) {
        app_.ExportGltf(io::FsPathFromUtf8(pendingGltfPath_), binary, gltfExportTextures_);
        pendingGltfPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0))) {
        pendingGltfPath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Save As, for a model that already IS `.m3` -----------------------------
//
// Not a branch of the Save As above: that one writes Warcraft III, and this
// shares neither its writer nor one of its options. What it does share is the
// menu item, because from the user's side both are "write this model back in
// its own format" — the format just decides which questions come with it. See
// tools/basic_viewer/m3_save.h.

void ViewerUI::SaveM3Dialog() {
    nfdu8filteritem_t filter[1] = {{"StarCraft II model", "m3"}};
    NFD::UniquePathU8 outPath;
    if (NFD::SaveDialog(outPath, filter, 1) != NFD_OKAY)
        return;
    pendingM3SavePath_ = outPath.get();
    openM3SavePopup_ = true;
    m3SaveError_.clear();
}

void ViewerUI::BuildM3SavePopup() {
    if (openM3SavePopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.m3save.title"));
        openM3SavePopup_ = false;
    }
    if (pendingM3SavePath_.empty())
        return;

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.m3save.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextUnformatted(i18n::tr("dialog.m3save.prompt"));
    ImGui::Separator();

    // The merge has nothing to fold in until a file is attached, and the
    // Animation window is where that happens — so the row says how many are
    // waiting rather than offering an option that would do nothing.
    const std::size_t attached = app_.AttachedAnimations().size();
    ImGui::BeginDisabled(attached == 0);
    ImGui::Checkbox(i18n::tr("dialog.m3save.merge_anims"), &m3SaveMergeAnims_);
    ImGui::EndDisabled();
    if (attached == 0)
        ImGui::TextDisabled("%s", i18n::tr("dialog.m3save.merge_none"));
    else
        ImGui::TextDisabled(i18n::tr("dialog.m3save.merge_count"), static_cast<int>(attached));

    ImGui::Checkbox(i18n::tr("dialog.m3save.convert_sc2"), &m3SaveConvertSc2_);
    ImGui::TextDisabled("%s", i18n::tr("dialog.m3save.convert_hint"));

    // The Save As strings, reused for the M3 export popup's reason: they say
    // nothing MDX-specific. No format row — the bytes are copied as the
    // storage serves them, which for these games is `.dds` already.
    ImGui::Checkbox(i18n::tr("dialog.saveas.export_textures"), &m3SaveExportTextures_);
    ImGui::BeginDisabled(!m3SaveExportTextures_);
    ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.export_hint"));
    ImGui::EndDisabled();

    if (!m3SaveError_.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s: %s",
                           i18n::tr("dialog.m3save.failed"), m3SaveError_.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button(i18n::tr("app.save"), ImVec2(120, 0))) {
        // Stays open on failure. The one failure this has is the conversion
        // refusing a material, and the fix for it is a box in this modal.
        if (app_.SaveM3(io::FsPathFromUtf8(pendingM3SavePath_), m3SaveMergeAnims_ && attached > 0,
                        m3SaveConvertSc2_, m3SaveExportTextures_, &m3SaveError_)) {
            pendingM3SavePath_.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0))) {
        pendingM3SavePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ViewerUI::ExportWemDialog() {
    NFD::UniquePathU8 outPath;
    nfdu8filteritem_t filter[1] = {{"WEM model", "wem"}};
    if (NFD::SaveDialog(outPath, filter, 1) != NFD_OKAY)
        return;
    app_.ExportWem(io::FsPathFromUtf8(outPath.get()));
}

void ViewerUI::AttachAnimationDialog() {
    animAttachError_.clear();
    NFD::UniquePathSet outPaths;
    nfdu8filteritem_t filter[1] = {{"StarCraft II animations", "m3a"}};
    // Multi-select: a model's animations are routinely split across several
    // files (`*_RequiredAnims`, `*OptionalAnims`, `*_SwarmAnims`), and picking
    // them one dialog at a time is the same merge done slowly.
    if (NFD::OpenDialogMultiple(outPaths, filter, 1) != NFD_OKAY)
        return;
    nfdpathsetsize_t count = 0;
    if (NFD::PathSet::Count(outPaths, count) != NFD_OKAY)
        return;
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        NFD::UniquePathSetPathU8 path;
        if (NFD::PathSet::GetPath(outPaths, i, path) != NFD_OKAY)
            continue;
        const std::filesystem::path p = io::FsPathFromUtf8(path.get());
        // A pick can be refused — unparseable, no sequences, or a stem already
        // attached. Dropping that on the floor leaves the user staring at a
        // list their file did not appear in, so the popup keeps the last one.
        if (!app_.AttachAnimationFile(p))
            animAttachError_ = io::PathToUtf8(p.filename());
    }
}

namespace {

// Image formats the Save As "export textures" option can convert to — every
// WhiteoutLib image writer except GIF. `ext` includes the dot (it's the output
// extension and what the model's texture path is rewritten to); "" means keep
// the source format. `label` is the format name (not localized); the empty
// entry uses the localized "keep original" string at draw time.
struct TexExportFormat {
    const char* ext;
    const char* label;
};
constexpr TexExportFormat kExportFormats[] = {
    {"", ""},        {".blp", "BLP"}, {".png", "PNG"},  {".tga", "TGA"},
    {".dds", "DDS"}, {".bmp", "BMP"}, {".jpg", "JPEG"}, {".tif", "TIFF"},
};

// Re-encode a decoded texture into `ext`. Converts to RGBA8 first — accepted by
// every writer and the common denominator across formats (BCn/paletted sources
// are decoded). Returns nullopt on an unknown ext or a writer failure.
std::optional<std::vector<u8>> EncodeTextureAs(const whiteout::textures::Texture& src,
                                               const std::string& ext) {
    namespace tx = whiteout::textures;
    tx::Texture t = src;
    t.format(tx::PixelFormat::RGBA8);
    auto run = [&](tx::Writer& w) -> std::optional<std::vector<u8>> {
        try {
            std::vector<u8> bytes = w.write(t);
            if (bytes.empty())
                return std::nullopt;
            return bytes;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    if (ext == ".png") {
        tx::png::Writer w;
        return run(w);
    }
    if (ext == ".tga") {
        tx::tga::Writer w;
        return run(w);
    }
    if (ext == ".blp") {
        tx::blp::Writer w;
        return run(w);
    }
    if (ext == ".dds") {
        tx::dds::Writer w;
        return run(w);
    }
    if (ext == ".bmp") {
        tx::bmp::Writer w;
        return run(w);
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        tx::jpeg::Writer w;
        return run(w);
    }
    if (ext == ".tif" || ext == ".tiff") {
        tx::tiff::Writer w;
        return run(w);
    }
    return std::nullopt;
}

struct ExportStats {
    int exported = 0;
    int skipped = 0;
    int failed = 0;
};

// Writes the model's file-backed textures into `targetDir`, preserving each
// texture's relative path. A non-empty `formatExt` converts each texture to that
// format and rewrites the model's texture path to match (so the saved model
// references the exported files); "" keeps the referenced format (verbatim copy
// when the source bytes already match, else re-encoded to it — e.g. Reforged
// serves .dds for a .blp path). Textures already present at the target are left
// untouched. `model` is mutated (path rewrites); the caller writes it after.
ExportStats ExportModelTextures(ViewerApp& app, whiteout::mdx::Model& model,
                                const std::filesystem::path& targetDir,
                                const std::string& formatExt) {
    namespace fs = std::filesystem;
    ExportStats st;
    io::IContentProvider* provider = app.Service().Scene().ActiveContentProvider();

    auto lower = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    for (auto& tex : model.textures) {
        // replaceableId != 0 → runtime team-color/glow/tileset, no source file.
        if (tex.replaceableId != 0 || tex.fileName.empty())
            continue;

        const std::string origName = tex.fileName; // capture before any rewrite

        std::string relStr = origName;
        std::replace(relStr.begin(), relStr.end(), '\\', '/');
        const fs::path relPath(io::FsPathFromUtf8(relStr));
        const std::string srcExt = lower(relPath.extension().string());
        const std::string targetExt = formatExt.empty() ? srcExt : formatExt;

        fs::path outRel = relPath;
        outRel.replace_extension(targetExt);
        const fs::path outFile = targetDir / outRel;

        // Point the saved model at the exported file when the format changed
        // (WC3 texture paths use backslashes).
        if (targetExt != srcExt) {
            std::string nn = origName;
            if (const auto dot = nn.rfind('.'); dot != std::string::npos)
                nn.resize(dot);
            tex.fileName = nn + targetExt;
        }

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            st.skipped++;
            continue;
        }
        if (!provider) {
            st.failed++;
            continue;
        }

        std::string actualExt;
        std::optional<std::vector<u8>> bytes = provider->ReadFile(origName, &actualExt);
        if (!bytes || bytes->empty()) {
            std::fprintf(stderr, "[viewer] Export: source texture not found: %s\n",
                         origName.c_str());
            st.failed++;
            continue;
        }
        actualExt = lower(actualExt);

        std::vector<u8> outBytes;
        if (targetExt == actualExt) {
            outBytes = std::move(*bytes);
        } else {
            auto decoded = model::DispatchTextureParser(
                actualExt, [&](auto& p) { return p.parse(std::span<const u8>(*bytes)); });
            std::optional<std::vector<u8>> enc =
                decoded ? EncodeTextureAs(*decoded, targetExt) : std::nullopt;
            if (!enc) {
                std::fprintf(stderr, "[viewer] Export: convert failed %s -> %s\n", origName.c_str(),
                             targetExt.c_str());
                st.failed++;
                continue;
            }
            outBytes = std::move(*enc);
        }

        fs::create_directories(outFile.parent_path(), ec);
        std::ofstream f(outFile, std::ios::binary);
        if (f)
            f.write(reinterpret_cast<const char*>(outBytes.data()),
                    static_cast<std::streamsize>(outBytes.size()));
        if (!f) {
            st.failed++;
            continue;
        }
        st.exported++;
    }
    return st;
}

// Re-serialises the currently-loaded model to `outPath`. The Writer picks
// MDX-binary vs MDL-text from the file extension; `dialect` only matters for
// .mdl output. When `exportTextures`, the model's used textures are written next
// to it first (see ExportModelTextures) — `formatExt` optionally converts them.
// Returns false (and logs) when no model is loaded or the write throws.
bool WriteCurrentModel(ViewerApp& app, const std::string& outPath, whiteout::mdx::MdlFormat dialect,
                       bool exportTextures, const std::string& formatExt) {
    // The active document's actor holds a strong ref to its template — the most
    // reliable source. Fall back to the (weak) path-keyed cache if there's no
    // focused actor.
    std::shared_ptr<model::ModelTemplate> tmpl;
    model::Actor* focus = app.FocusActorPtr();
    if (focus)
        tmpl = focus->sourceTemplate;
    if (!tmpl || !tmpl->adapter)
        tmpl = app.Service().Scene().Templates().Lookup(io::PathToUtf8(app.CurrentModelPath()));

    // Save As writes an MDX/MDL through whiteout::mdx::Writer, so it needs the
    // parsed MDX model and not a format-neutral snapshot. Refuse a non-MDX
    // source rather than writing something that is not the model the user is
    // looking at.
    const auto* mdxAdapter =
        tmpl ? dynamic_cast<const io::MdxModelAdapter*>(tmpl->adapter.get()) : nullptr;
    if (!mdxAdapter && focus) {
        // No template at all: the actor was spawned from a live source. A
        // `.wem` opened as either Warcraft III profile is exactly that, and
        // writing it back out as MDX is the conversion the user came for.
        mdxAdapter = dynamic_cast<const io::MdxModelAdapter*>(focus->animation.Source().get());
    }
    if (!mdxAdapter) {
        std::fprintf(stderr, "[viewer] Save As: this model is not MDX; nothing to write\n");
        return false;
    }
    // Copy so texture-path rewrites during export don't touch the live template.
    whiteout::mdx::Model model = mdxAdapter->SourceModel();
    if (exportTextures) {
        const ExportStats st = ExportModelTextures(
            app, model, std::filesystem::path(io::FsPathFromUtf8(outPath)).parent_path(),
            formatExt);
        std::printf("[viewer] Textures: %d exported, %d skipped, %d failed\n", st.exported,
                    st.skipped, st.failed);
    }
    try {
        whiteout::mdx::Writer writer;
        writer.write(outPath, model, dialect);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewer] Save As FAILED '%s': %s\n", outPath.c_str(), e.what());
        return false;
    }
    std::printf("[viewer] Saved model: %s\n", outPath.c_str());
    return true;
}

// Save a standalone PopcornFX effect (.pkb / .pkfx). There's no editable model
// behind an effect — the viewer just plays it — so "save" writes the source
// bytes out verbatim.
//
// The source may not be a file on disk: documents opened from the Storage
// Explorer set CurrentModelPath() to an archive-relative path (CASC / MPQ), so
// only the on-disk case can be a plain copy. Everything else is read back
// through the document's own content provider — the same one the effect was
// loaded through — and written out.
bool WriteCurrentPkb(ViewerApp& app, const std::string& outPath) {
    const std::filesystem::path src = app.CurrentModelPath();

    std::error_code ec;
    if (std::filesystem::exists(src, ec) && !ec) {
        std::filesystem::copy_file(src, outPath, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (!ec) {
            std::printf("[viewer] Saved effect: %s\n", outPath.c_str());
            return true;
        }
    }

    const std::string rel = io::PathToUtf8(src);
    std::optional<std::vector<u8>> bytes;
    if (io::IContentProvider* provider = app.Service().Scene().ActiveContentProvider())
        bytes = provider->ReadFile(rel);
    if (!bytes) {
        std::fprintf(stderr, "[viewer] Save As FAILED: cannot read effect '%s'\n", rel.c_str());
        return false;
    }

    std::ofstream out(std::filesystem::path(outPath), std::ios::binary);
    if (out)
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
    if (!out) {
        std::fprintf(stderr, "[viewer] Save As FAILED: cannot write '%s'\n", outPath.c_str());
        return false;
    }
    std::printf("[viewer] Saved effect: %s (%zu bytes)\n", outPath.c_str(), bytes->size());
    return true;
}

} // namespace

void ViewerUI::SaveAsDialog() {
    // The output format is locked to the source's: a PopcornFX effect
    // (.pkb/.pkfx) is copied verbatim and can only be re-saved as the same
    // effect, a StarCraft II model goes back out as `.m3`, and everything else
    // that reaches here is Warcraft III and writes as MDX or MDL. Asked of the
    // source rather than the extension, for the menu gate's reason.
    if (app_.CanSaveM3()) {
        SaveM3Dialog();
        return;
    }

    std::string srcExt = app_.CurrentModelPath().extension().string();
    for (auto& c : srcExt)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    NFD::UniquePathU8 outPath;

    if (srcExt == ".pkb" || srcExt == ".pkfx") {
        // Effects are copied byte-for-byte, so the output keeps the source
        // extension (the bytes are that format).
        const char* effExt = (srcExt == ".pkfx") ? "pkfx" : "pkb";
        nfdu8filteritem_t effFilter[1] = {{"PopcornFX effect", effExt}};
        if (NFD::SaveDialog(outPath, effFilter, 1) != NFD_OKAY)
            return;
        WriteCurrentPkb(app_, outPath.get());
        return;
    }

    // Model: two separate filter entries (not "mdx,mdl") so NFD appends the
    // right extension for whichever the user selects — that extension is then
    // how we decide binary vs text.
    nfdu8filteritem_t filter[2] = {{"MDX model (binary)", "mdx"}, {"MDL model (text)", "mdl"}};
    if (NFD::SaveDialog(outPath, filter, 2) != NFD_OKAY)
        return;

    std::string path = outPath.get();
    std::string ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Both MDX and MDL route through the options modal next frame (texture
    // export + format); MDL additionally offers the dialect choice there.
    pendingSavePath_ = std::move(path);
    pendingSaveIsMdl_ = (ext == ".mdl");
    openSaveOptionsPopup_ = true;
}

void ViewerUI::BuildSaveOptionsPopup() {
    if (openSaveOptionsPopup_) {
        ImGui::OpenPopup(i18n::tr("dialog.saveas.title"));
        openSaveOptionsPopup_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(i18n::tr("dialog.saveas.title"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // ---- MDL dialect (text format only) ----
    if (pendingSaveIsMdl_) {
        ImGui::TextUnformatted(i18n::tr("dialog.saveas.prompt"));
        ImGui::RadioButton(i18n::tr("dialog.saveas.wc3"), &saveDialect_, 0);
        ImGui::SameLine();
        ImGui::RadioButton(i18n::tr("dialog.saveas.hive"), &saveDialect_, 1);
        ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.wc3_desc"));
        ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.hive_desc"));
        ImGui::Separator();
    }

    // ---- Texture export ----
    ImGui::Checkbox(i18n::tr("dialog.saveas.export_textures"), &saveExportTextures_);
    ImGui::BeginDisabled(!saveExportTextures_);
    ImGui::TextDisabled("%s", i18n::tr("dialog.saveas.export_hint"));
    saveTexFormatIdx_ =
        std::clamp(saveTexFormatIdx_, 0, static_cast<i32>(std::size(kExportFormats)) - 1);
    auto formatLabel = [&](i32 i) {
        return kExportFormats[i].ext[0] ? kExportFormats[i].label
                                        : i18n::tr("dialog.saveas.keep_original");
    };
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo(i18n::tr("dialog.saveas.convert_to"), formatLabel(saveTexFormatIdx_))) {
        for (i32 i = 0; i < static_cast<i32>(std::size(kExportFormats)); ++i) {
            const bool sel = (i == saveTexFormatIdx_);
            if (ImGui::Selectable(formatLabel(i), sel))
                saveTexFormatIdx_ = i;
            if (sel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button(i18n::tr("app.save"), ImVec2(120, 0))) {
        const auto dialect = (saveDialect_ == 1) ? whiteout::mdx::MdlFormat::Hiveworkshop
                                                 : whiteout::mdx::MdlFormat::WarcraftIII;
        WriteCurrentModel(app_, pendingSavePath_, dialect, saveExportTextures_,
                          kExportFormats[saveTexFormatIdx_].ext);
        pendingSavePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(i18n::tr("app.cancel"), ImVec2(80, 0))) {
        pendingSavePath_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---- Animation window ------------------------------------------------------
//
// StarCraft II does not play "a sequence". It plays a *bracket*, and one
// bracket expands into one player per sub-track container the sequence spans:
// the Marine's `Cover` is `Cover_full` plus `Cover_Shield`, and the shield is
// visible for as long as something holds that second container down. Several
// brackets run at once and blend against a weight budget. The sequence
// dropdown is one bracket; this window is the others, plus the ones the model
// starts by itself.

bool ViewerUI::BuildAnimTrackRow(std::size_t index) {
    const auto& tracks = app_.AnimTracks();
    if (index >= tracks.size())
        return true;
    ViewerApp::AnimTrackInfo t = tracks[index];
    const auto& seqs = app_.SequenceNames();
    bool changed = false;
    bool keep = true;

    ImGui::PushID(static_cast<int>(index));
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* preview = (t.sequence >= 0 && t.sequence < static_cast<i32>(seqs.size()))
                              ? seqs[t.sequence].c_str()
                              : "";
    if (ImGui::BeginCombo("##seq", preview)) {
        for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
            if (ImGui::Selectable(seqs[i].c_str(), i == t.sequence) && i != t.sequence) {
                t.sequence = i;
                // The old index named a container of the old sequence, and the
                // groups are not parallel. Back to the whole sequence.
                t.subtrack = -1;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    // Sub-track. A sequence with one container has nothing to choose, so the
    // cell names it instead of offering a combo with one entry.
    ImGui::TableNextColumn();
    const auto subs = app_.SubtracksOf(t.sequence);
    if (subs.size() <= 1) {
        ImGui::TextDisabled("%s", subs.empty() ? "-" : subs[0].name.c_str());
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* subPreview = i18n::tr("anim.track.all");
        if (t.subtrack >= 0 && t.subtrack < static_cast<i32>(subs.size()))
            subPreview = subs[t.subtrack].name.c_str();
        if (ImGui::BeginCombo("##sub", subPreview)) {
            if (ImGui::Selectable(i18n::tr("anim.track.all"), t.subtrack < 0) && t.subtrack >= 0) {
                t.subtrack = -1;
                changed = true;
            }
            for (i32 i = 0; i < static_cast<i32>(subs.size()); ++i) {
                if (ImGui::Selectable(subs[i].name.c_str(), i == t.subtrack) && i != t.subtrack) {
                    t.subtrack = i;
                    changed = true;
                }
                if (ImGui::IsItemHovered()) {
                    // Priority decides which container wins a property;
                    // concurrency decides whether it leaves the rest of the
                    // skeleton alone. Both belong on the choice.
                    ImGui::SetTooltip("%s %u  -  %s  -  %s %zu", i18n::tr("anim.track.priority"),
                                      static_cast<unsigned>(subs[i].priority),
                                      i18n::tr(subs[i].concurrent ? "anim.track.concurrent"
                                                                  : "anim.track.exclusive"),
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
        app_.RemoveAnimTrack(index);
        return false;
    }
    if (changed)
        app_.SetAnimTrack(index, t);
    return true;
}

void ViewerUI::BuildAnimationWindow() {
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(i18n::tr("anim.title"), &animWindowOpen_)) {
        ImGui::End();
        return;
    }

    // ---- Global loops ----
    const auto globals = app_.GlobalLoops();
    if (!globals.empty() &&
        ImGui::CollapsingHeader(i18n::tr("anim.globals"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", i18n::tr("anim.globals.help"));
        for (const auto& g : globals) {
            bool on = g.enabled;
            ImGui::PushID(g.sequence);
            if (ImGui::Checkbox(g.name.c_str(), &on))
                app_.SetGlobalLoopEnabled(g.sequence, on);
            const auto subs = app_.SubtracksOf(g.sequence);
            if (!subs.empty()) {
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
        const auto& tracks = app_.AnimTracks();
        if (!tracks.empty() &&
            ImGui::BeginTable("##tracks", 5, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(i18n::tr("anim.track.sequence"),
                                    ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableSetupColumn(i18n::tr("anim.track.subtrack"),
                                    ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableSetupColumn(i18n::tr("anim.track.weight"),
                                    ImGuiTableColumnFlags_WidthStretch, 0.18f);
            // Wide enough for the header text, not just the checkbox: a fixed
            // 22px column clipped "Loop" to "L...".
            ImGui::TableSetupColumn(i18n::tr("anim.track.loop"), ImGuiTableColumnFlags_WidthFixed,
                                    42.0f);
            ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableHeadersRow();
            // One removal per frame: a row that removed itself invalidated the
            // vector this loop is walking.
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                if (!BuildAnimTrackRow(i))
                    break;
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(i18n::tr("anim.tracks.add")))
            app_.AddAnimTrack();
        ImGui::Spacing();
    }

    // ---- Animation files ----
    if (ImGui::CollapsingHeader(i18n::tr("anim.files"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto attached = app_.AttachedAnimations();
        if (attached.empty()) {
            ImGui::TextDisabled("%s", i18n::tr("toolbar.animfiles.none"));
        } else {
            for (std::size_t i = 0; i < attached.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                // Detach first, then stop building this list - it is a snapshot
                // and the entries after the removed one shift.
                const bool drop = ImGui::SmallButton("x");
                ImGui::SameLine();
                ImGui::Text("%s", attached[i].label.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%zu)", attached[i].sequenceCount);
                ImGui::PopID();
                if (drop) {
                    app_.DetachAnimationFile(i);
                    break;
                }
            }
        }
        if (ImGui::Button(i18n::tr("toolbar.animfiles.add")))
            AttachAnimationDialog();
        if (!animAttachError_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s: %s",
                               i18n::tr("toolbar.animfiles.failed"), animAttachError_.c_str());
    }

    ImGui::End();
}

void ViewerUI::BuildMenuBar() {
    RenderService& svc = app_.Service();
    DisplayFlags df = svc.Settings().GetDisplayFlags();
    bool dfChanged = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(i18n::tr("menu.file"))) {
            if (ImGui::MenuItem(i18n::tr("menu.file.open"), "Ctrl+O"))
                OpenFileDialog();
            const bool hasModel = !app_.CurrentModelPath().empty();
            // Save As writes a model back in its own format: MDX/MDL, a `.m3`
            // through the M3 writer, or a .pkb copied verbatim. A `.m2` still
            // has no writer here, so the item greys out rather than opening a
            // dialog that can only fail at the end — and a `.wem` opened as any
            // of these is the same model, so it asks the SOURCE rather than the
            // extension.
            const bool canSaveMdx =
                hasModel && !app_.CurrentModelIsForeign() && app_.CanSaveAsMdx();
            const bool canSave = canSaveMdx || app_.CanSaveM3();
            if (ImGui::MenuItem(i18n::tr("menu.file.save_as"), "Ctrl+Shift+S", false, canSave))
                SaveAsDialog();
            // Separate from Save As, which writes the model back in its own
            // format: everything in here CONVERTS. Every model this build can
            // draw can be written to WEM — which is why the submenu is gated on
            // the model's SOURCE rather than on its extension — and the game
            // targets keep their own gates inside.
            if (ImGui::BeginMenu(i18n::tr("menu.file.export"), app_.CanExportWem())) {
                // The interchange format itself, always available in here.
                if (ImGui::MenuItem(i18n::tr("menu.file.export_wem"), nullptr, false,
                                    app_.CanExportWem()))
                    ExportWemDialog();
                // A World of Warcraft, StarCraft II or Diablo III model becomes
                // a Warcraft III one through WEM. A model that IS Warcraft III
                // uses Save As, which does not have to derive a material set it
                // already carries.
                if (ImGui::MenuItem(i18n::tr("menu.file.export_mdx"), nullptr, false,
                                    app_.CanExportMdx()))
                    ExportMdxDialog();
                // The same argument one game over: anything but a StarCraft II
                // model becomes a StarCraft II one.
                if (ImGui::MenuItem(i18n::tr("menu.file.export_m3"), nullptr, false,
                                    app_.CanExportM3()))
                    ExportM3Dialog();
                // And out of the family altogether: any model WEM reads
                // becomes glTF for Blender and everything else.
                if (ImGui::MenuItem(i18n::tr("menu.file.export_gltf"), nullptr, false,
                                    app_.CanExportGltf()))
                    ExportGltfDialog();
                ImGui::EndMenu();
            }
            const bool hasAnims = hasModel && !app_.SequenceNames().empty();
            if (ImGui::MenuItem(i18n::tr("menu.file.export_frames"), nullptr,
                                exportWindow_.IsOpen(), hasAnims)) {
                if (exportWindow_.IsOpen()) {
                    exportWindow_.Close();
                } else {
                    model::Actor* focus = app_.FocusActorPtr();
                    exportWindow_.Open(focus ? focus->animation.ActiveSequenceIndex() : 0);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(i18n::tr("menu.file.exit")))
                glfwSetWindowShouldClose(app_.Window(), GLFW_TRUE);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(i18n::tr("menu.view"))) {
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.grid"), nullptr, &df.showGrid);
            dfChanged |=
                ImGui::MenuItem(i18n::tr("menu.view.particles"), nullptr, &df.showParticles);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.ribbons"), nullptr, &df.showRibbons);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.view.events"), nullptr, &df.showEvents);
            ImGui::MenuItem(i18n::tr("menu.view.viewcube"), nullptr, &showViewCube_);

            ImGui::Separator();
            {
                // "Reforged Graphics" — force the HD pipeline for every model.
                bool reforged = app_.ForceHd();
                if (ImGui::MenuItem(i18n::tr("menu.view.reforged"), nullptr, &reforged)) {
                    app_.SetForceHd(reforged);
                    SaveIni(app_);
                }
            }
            // Which of Warcraft III's three CASC overlays a read resolves
            // through. Separate from the render mode above because 3.0.0 made
            // them separate questions: Reforged and Definitive are different
            // files drawn the same way. "Follow Render Mode" is the default and
            // the pre-3.0.0 behaviour (SD ⇒ Classic, HD ⇒ Reforged);
            // anything a storage browse opens still overrides this per model,
            // from the overlay the model itself came from.
            if (ImGui::BeginMenu(i18n::tr("menu.view.art_tier"))) {
                const std::optional<Wc3ArtTier> cur = svc.Settings().GetArtTier();
                struct Choice {
                    const char* key;
                    std::optional<Wc3ArtTier> tier;
                };
                const Choice choices[] = {
                    {"menu.view.art_tier_auto", std::nullopt},
                    {"menu.view.art_tier_classic", Wc3ArtTier::Classic},
                    {"menu.view.art_tier_reforged", Wc3ArtTier::Reforged},
                    {"menu.view.art_tier_definitive", Wc3ArtTier::Definitive},
                };
                for (const Choice& c : choices) {
                    if (ImGui::MenuItem(i18n::tr(c.key), nullptr, cur == c.tier) &&
                        cur != c.tier) {
                        app_.SetArtTier(c.tier);
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::BeginMenu(i18n::tr("menu.view.tileset"))) {
                const i32 n = static_cast<i32>(io::Tileset::Count);
                const i32 cur = static_cast<i32>(io::GetCurrentTileset());
                for (i32 i = 0; i < n; ++i) {
                    const bool sel = (i == cur);
                    if (ImGui::MenuItem(io::TilesetName(static_cast<io::Tileset>(i)), nullptr,
                                        sel)) {
                        svc.Replaceables().SetTileset(static_cast<io::Tileset>(i));
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(i18n::tr("menu.debug"))) {
            dfChanged |=
                ImGui::MenuItem(i18n::tr("menu.debug.collisions"), nullptr, &df.showCollisions);
            dfChanged |= ImGui::MenuItem(i18n::tr("menu.debug.lights"), nullptr, &df.showLights);

            if (ImGui::BeginMenu(i18n::tr("menu.debug.physics"))) {
                struct PhysicsToggle {
                    const char* key;
                    bool (RenderSettings::*get)() const;
                    void (RenderSettings::*set)(bool);
                };
                static constexpr PhysicsToggle kPhysicsToggles[] = {
                    {"menu.debug.physics.dynamic", &RenderSettings::ShowPhysicsDynamic,
                     &RenderSettings::SetShowPhysicsDynamic},
                    {"menu.debug.physics.kinematic", &RenderSettings::ShowPhysicsKinematic,
                     &RenderSettings::SetShowPhysicsKinematic},
                    {"menu.debug.physics.static", &RenderSettings::ShowPhysicsStatic,
                     &RenderSettings::SetShowPhysicsStatic},
                    {"menu.debug.physics.cloth", &RenderSettings::ShowPhysicsCloth,
                     &RenderSettings::SetShowPhysicsCloth},
                };
                for (const auto& t : kPhysicsToggles) {
                    const bool on = (svc.Settings().*t.get)();
                    if (ImGui::MenuItem(i18n::tr(t.key), nullptr, on)) {
                        (svc.Settings().*t.set)(!on);
                        SaveIni(app_);
                    }
                }

                // Not an overlay: this one changes the picture. A cloth solver
                // is only judgeable as a difference, so the host needs a way to
                // put the geoset back on its skinning without a rebuild.
                ImGui::Separator();
                {
                    const bool on = svc.Settings().ClothDeform();
                    if (ImGui::MenuItem(i18n::tr("menu.debug.physics.deform"), nullptr, on)) {
                        svc.Settings().SetClothDeform(!on);
                        SaveIni(app_);
                    }
                }

                // Everything above draws; this one *runs*. D3 builds a ragdoll
                // on a gameplay event no model file carries, so the host is the
                // only place it can come from. Per-actor, hence not saved to
                // the ini, and greyed rather than hidden so a model without a
                // rig still shows that the viewer has one.
                ImGui::Separator();
                const bool hasRig = app_.HasD3Ragdoll();
                if (ImGui::MenuItem(i18n::tr("menu.debug.physics.ragdoll"), nullptr,
                                    hasRig && app_.D3Ragdoll(), hasRig))
                    app_.SetD3Ragdoll(!app_.D3Ragdoll());
                ImGui::EndMenu();
            }
            ImGui::Separator();

            if (ImGui::BeginMenu(i18n::tr("menu.debug.debugview"))) {
                // The views the focused model's materials can answer: a
                // Reforged model drawn in HD gets the PBR set, everything
                // else the legacy one.
                const DebugViewFamily family = svc.DebugFamilyOf(app_.FocusActor());
                ImGui::TextDisabled("%s", i18n::tr(family == DebugViewFamily::Pbr
                                                        ? "debugvis.family_pbr"
                                                        : "debugvis.family_legacy"));
                ImGui::Separator();
                const DebugView cur = svc.Settings().GetDebugView();
                for (const auto& item : kDebugViews) {
                    if (!DebugViewInFamily(item.view, family))
                        continue;
                    const char* key =
                        family == DebugViewFamily::Pbr ? item.pbrKey : item.legacyKey;
                    if (ImGui::MenuItem(i18n::tr(key), nullptr, item.view == cur)) {
                        svc.Settings().SetDebugView(item.view);
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu(i18n::tr("menu.debug.lod"))) {
                const i32 cur = svc.Settings().LodOverride();
                const i32 curIdx = (cur < 0) ? 0 : (1 + std::clamp(cur, 0, 3));
                for (i32 i = 0; i < static_cast<i32>(kLodLabels.size()); ++i) {
                    if (ImGui::MenuItem(i18n::tr(kLodKeys[i]), nullptr, i == curIdx)) {
                        svc.Settings().SetLodOverride(i == 0 ? -1 : (i - 1));
                        SaveIni(app_);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::MenuItem(i18n::tr("menu.debug.log_console"), nullptr, &showLogConsole_);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(i18n::tr("menu.tools"))) {
            bool seOpen = app_.StorageExplorerOpen();
            if (ImGui::MenuItem(i18n::tr("menu.tools.storage_explorer"), nullptr, &seOpen))
                app_.SetStorageExplorerOpen(seOpen);
            ImGui::EndMenu();
        }

        // Language picker — endonyms are shown in their own script (not
        // translated); the bundled Noto fonts cover every entry. Switching only
        // swaps the in-memory catalog, so the whole UI re-localizes next frame.
        if (ImGui::BeginMenu(i18n::tr("menu.language"))) {
            const i18n::Language cur = i18n::Localizer::instance().current();
            for (const auto& e : i18n::languages()) {
                if (ImGui::MenuItem(e.endonym, nullptr, e.lang == cur)) {
                    i18n::Localizer::instance().setLanguage(e.lang);
                    SaveIni(app_);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(i18n::tr("menu.settings")))
            settingsOpen_ = true;

        ImGui::EndMainMenuBar();
    }

    if (dfChanged) {
        svc.Settings().SetDisplayFlags(df);
        SaveIni(app_);
    }
}

void ViewerUI::BuildToolbar() {
    // Anchor the toolbar just below the main menu bar; sized to the
    // viewport width, fixed-height. No close / collapse decorations.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menuH = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, menuH + 8.0f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##toolbar", nullptr, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    RenderService& svc = app_.Service();

    // ---- Transport ----
    //
    // First on the row and never hidden. The scene clock is what every profile
    // advances on — animation, particles, ribbons and corn-fx alike — so one
    // pair of buttons covers a Warcraft III `.mdx`, a WoW `.m2`, a StarCraft II
    // `.m3`, a Diablo III actor and a bare `.pkb` effect, including the ones
    // with no sequence list to sit a dropdown next to.
    {
        const bool paused = app_.IsPaused();
        if (IconButton("##transport.toggle", paused ? ToolbarIcon::Play : ToolbarIcon::Pause,
                       paused ? "toolbar.play" : "toolbar.pause",
                       paused ? "toolbar.play.tip" : "toolbar.pause.tip"))
            app_.SetPaused(!paused);
        ImGui::SameLine();
        if (IconButton("##transport.restart", ToolbarIcon::Restart, "toolbar.restart",
                       "toolbar.restart.tip"))
            app_.RestartPlayback();
        ImGui::SameLine();
    }

    // ---- Animation sequence ----
    const auto& seqs = app_.SequenceNames();
    if (!seqs.empty()) {
        model::Actor* focus = app_.FocusActorPtr();
        i32 sel = focus ? focus->animation.ActiveSequenceIndex() : 0;
        ToolbarLabel("toolbar.animation");
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##animation",
                              seqs[std::clamp(sel, 0, (i32)seqs.size() - 1)].c_str())) {
            for (i32 i = 0; i < static_cast<i32>(seqs.size()); ++i) {
                const bool isSel = (i == sel);
                if (ImGui::Selectable(seqs[i].c_str(), isSel)) {
                    if (focus) {
                        const i32 prev = focus->animation.ActiveSequenceIndex();
                        focus->animation.SetActiveSequenceIndex(i);
                        if (i != prev) {
                            const std::string& name = seqs[i];
                            const bool keep = (name.find("decay") != std::string::npos) ||
                                              (name.find("dissipate") != std::string::npos);
                            if (!keep)
                                svc.Splats().Clear();
                        }
                    }
                }
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Animation window (StarCraft II only) ----
    //
    // Next to the sequence dropdown because it is the rest of that control:
    // the dropdown picks one sequence, and an `.m3` needs several plays at
    // once plus the global loops it starts on its own. Hidden for every model
    // that has none of those concepts.
    if (app_.CanAttachAnimations()) {
        if (IconButton("##animfiles", ToolbarIcon::Tracks, "toolbar.animfiles",
                       "toolbar.animfiles.tip", animWindowOpen_))
            animWindowOpen_ = !animWindowOpen_;
        ImGui::SameLine();
    }

    // ---- Camera preset ----
    {
        const auto& presetNames = app_.CameraPresetNamesUtf8();
        const i32 active = app_.ActiveCameraPresetIdx();
        const char* preview = (active < 0 || active >= static_cast<i32>(presetNames.size()))
                                  ? i18n::tr("toolbar.camera.free")
                                  : presetNames[active].c_str();
        ToolbarLabel("toolbar.camera");
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("##camera", preview)) {
            if (ImGui::Selectable(i18n::tr("toolbar.camera.free"), active < 0))
                app_.ActivateCameraPreset(-1);
            for (i32 i = 0; i < static_cast<i32>(presetNames.size()); ++i) {
                const bool isSel = (i == active);
                if (ImGui::Selectable(presetNames[i].c_str(), isSel))
                    app_.ActivateCameraPreset(i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Team colour ----
    {
        model::Actor* focus = app_.FocusActorPtr();
        u32 tcRaw = focus ? (focus->teamColor & 0x00FFFFFFu) : 0x000000FFu;
        f32 col[3] = {
            static_cast<f32>(tcRaw & 0xFFu) / 255.0f,
            static_cast<f32>((tcRaw >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((tcRaw >> 16) & 0xFFu) / 255.0f,
        };
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel;
        ToolbarLabel("toolbar.team");
        if (ImGui::ColorEdit3("##team", col, flags)) {
            if (focus) {
                focus->SetTeamColor(static_cast<u8>(col[0] * 255.0f),
                                    static_cast<u8>(col[1] * 255.0f),
                                    static_cast<u8>(col[2] * 255.0f));
            }
        }
        ImGui::SameLine();
    }

    // ---- Creature skin (`.m2` only) ----
    // A creature model leaves its skin blank for the game to fill; this is
    // which fill. Absent for every other model, which is most of them.
    if (const auto skins = app_.WowSkinNames(); !skins.empty()) {
        const u32 sel = app_.WowSkin() % static_cast<u32>(skins.size());
        ToolbarLabel("toolbar.skin");
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("##skin", skins[sel].c_str())) {
            for (u32 i = 0; i < static_cast<u32>(skins.size()); ++i) {
                const bool isSel = (i == sel);
                if (ImGui::Selectable(skins[i].c_str(), isSel))
                    app_.SetWowSkin(i);
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }

    // ---- Character customisation (`.m2` only) ----
    // A dozen options behind one button rather than a dozen combos: a character
    // model offers skin, face, hair, beard, eyes and more, and the toolbar has
    // room for none of that. Absent for every model that is not a character.
    if (const auto options = app_.WowCharacterOptions(); !options.empty()) {
        if (ImGui::Button(i18n::tr("toolbar.customize")))
            ImGui::OpenPopup("##customize");
        if (ImGui::BeginPopup("##customize")) {
            for (const auto& opt : options) {
                if (opt.choiceCount == 0)
                    continue;
                char label[128];
                std::snprintf(label, sizeof(label), "%s##opt%u", opt.name.c_str(), opt.optionId);
                i32 sel = static_cast<i32>(opt.selected);
                ImGui::SetNextItemWidth(140);
                // The choices are mostly unnamed — a skin swatch has a colour,
                // not a name — so they are numbered rather than labelled.
                if (ImGui::SliderInt(label, &sel, 0, static_cast<i32>(opt.choiceCount) - 1))
                    app_.SetWowCharacterChoice(opt.optionId, static_cast<u32>(sel));
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }

    // ---- Character equipment (`.acr` / `.app` only) ----
    // The Diablo III half of the same idea, and the opposite problem: a `.m2`
    // character leaves its geosets blank for the game to fill, while a `.app`
    // ships every armour variant at once for the game to pick between. So this
    // offers pieces out of a wardrobe rather than choices out of a database.
    // Absent for every model that is not a player character.
    if (const auto slots = app_.D3CharacterSlots(); !slots.empty()) {
        // Asking kicks the registry build as a background task, so it runs
        // while the user is still looking at the model and the popup usually
        // opens ready instead of opening onto its progress bar.
        app_.D3ItemRegistryReady();
        if (ImGui::Button(i18n::tr("toolbar.equip")))
            ImGui::OpenPopup("##d3equip");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i18n::tr("toolbar.equip.tip"));
        if (ImGui::BeginPopup("##d3equip")) {
            // ---- Items: dressing by in-game name ----
            // The registry half of the dressing room. The first frame of the
            // popup kicks the registry build as a background task (~3.4k
            // Actor reads); a progress bar stands in until it lands. Once
            // ready, these rows ALSO replace the per-slot wardrobe below —
            // the registry drives the same look tags — so only Hair and the
            // Extras survive of the manual controls.
            const bool registryReady = app_.D3ItemRegistryReady();
            const bool registryBuilding = app_.D3ItemRegistryBuilding();
            if (registryBuilding) {
                // The runner is one task at a time, so the snapshot may
                // belong to whatever the build is queued behind — trust its
                // numbers only when the title says it is ours.
                const io::ProgressSnapshot snap = app_.Tasks().Poll();
                const bool ours = snap.title == "Building item registry";
                ImGui::TextUnformatted(ours && !snap.stage.empty()
                                           ? snap.stage.c_str()
                                           : i18n::tr("toolbar.equip.building"));
                char counts[64] = "";
                if (ours && snap.total != 0)
                    std::snprintf(counts, sizeof(counts), "%llu / %llu",
                                  static_cast<unsigned long long>(snap.current),
                                  static_cast<unsigned long long>(snap.total));
                const float fraction = (ours && !snap.indeterminate)
                                           ? snap.fraction
                                           : -1.0f * static_cast<float>(ImGui::GetTime());
                ImGui::ProgressBar(fraction, ImVec2(320, 0.0f), counts[0] ? counts : nullptr);
            } else if (!registryReady) {
                ImGui::TextDisabled("%s", i18n::tr("toolbar.equip.noregistry"));
            } else if (const auto rows = app_.D3OutfitSlots(); !rows.empty()) {
                // One filter shared by all the item combos: only one popup is
                // ever open, and keeping the text across rows is a feature —
                // a set is usually searched for once and equipped piecewise.
                static char itemFilter[64] = "";
                // Presets: a whole outfit by name. Saved beside the viewer
                // settings as item NAMES, so a preset survives a re-parse of
                // the item tables.
                static char presetName[48] = "";
                ImGui::SetNextItemWidth(150);
                if (ImGui::BeginCombo("##d3preset", i18n::tr("toolbar.equip.preset"))) {
                    for (const auto& p : app_.D3OutfitPresetNames()) {
                        if (ImGui::Selectable(p.c_str(), false))
                            app_.LoadD3OutfitPreset(p);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::InputTextWithHint("##d3presetname", i18n::tr("toolbar.equip.preset.name"),
                                         presetName, sizeof(presetName));
                ImGui::SameLine();
                if (ImGui::Button(i18n::tr("toolbar.equip.preset.save")) && presetName[0])
                    app_.SaveD3OutfitPreset(presetName);

                bool sheathed = app_.D3OutfitSheathed();
                if (ImGui::Checkbox(i18n::tr("toolbar.equip.sheathe"), &sheathed))
                    app_.SetD3OutfitSheathed(sheathed);
                // The class gate: the offer is what the focused character can
                // wear (class-neutral always shows); this widens it to the
                // whole registry.
                static bool allClasses = false;
                ImGui::SameLine();
                ImGui::Checkbox(i18n::tr("toolbar.equip.allclasses"), &allClasses);
                // A whole set in one click. Same class gate as the items.
                ImGui::SetNextItemWidth(230);
                if (ImGui::BeginCombo("##d3set", i18n::tr("toolbar.equip.equipset"))) {
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputTextWithHint("##d3setfilter", i18n::tr("toolbar.equip.item.search"),
                                             itemFilter, sizeof(itemFilter));
                    for (const auto& set : app_.D3OutfitSetEntries(itemFilter, allClasses)) {
                        char setId[96];
                        std::snprintf(setId, sizeof(setId), "%s (%d)##%s", set.label.c_str(),
                                      static_cast<int>(set.pieces), set.key.c_str());
                        if (ImGui::Selectable(setId, false))
                            app_.EquipD3OutfitSet(set.key);
                    }
                    ImGui::EndCombo();
                }
                for (const auto& row : rows) {
                    char id[64];
                    std::snprintf(id, sizeof(id), "%s##d3out%d", row.name.c_str(), row.visualSlot);
                    const char* preview = row.equipped.empty() ? i18n::tr("toolbar.equip.item.none")
                                                               : row.equippedLabel.c_str();
                    ImGui::SetNextItemWidth(230);
                    if (ImGui::BeginCombo(id, preview)) {
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::InputTextWithHint("##d3itemfilter",
                                                 i18n::tr("toolbar.equip.item.search"), itemFilter,
                                                 sizeof(itemFilter));
                        if (ImGui::Selectable(i18n::tr("toolbar.equip.item.none"),
                                              row.equipped.empty()))
                            app_.SetD3OutfitItem(row.visualSlot, "");
                        for (const auto& entry : app_.D3OutfitItemEntries(
                                 row.visualSlot, itemFilter, 200, allClasses)) {
                            // Display names collide (the art-test dupes); the
                            // stem keeps every row a distinct widget.
                            char rowId[160];
                            std::snprintf(rowId, sizeof(rowId), "%s##%s", entry.label.c_str(),
                                          entry.stem.c_str());
                            if (ImGui::Selectable(rowId, entry.stem == row.equipped))
                                app_.SetD3OutfitItem(row.visualSlot, entry.stem);
                            if (ImGui::IsItemHovered()) {
                                const std::string tip = app_.D3OutfitItemTip(entry.stem);
                                if (!tip.empty())
                                    ImGui::SetTooltip("%s", tip.c_str());
                            }
                        }
                        ImGui::EndCombo();
                    }
                    // The dye. One control for the whole value space: 0 is
                    // undyed, 1 is the engine's HIDDEN (naked armour, a
                    // despawned attachment), 2..22 the dye_ramp rows.
                    ImGui::SameLine();
                    std::snprintf(id, sizeof(id), "##d3dye%d", row.visualSlot);
                    const char* dyeLabel;
                    char dyeBuf[16];
                    if (row.dye == 1) {
                        dyeLabel = i18n::tr("toolbar.equip.item.hide");
                    } else if (row.dye >= 2) {
                        std::snprintf(dyeBuf, sizeof(dyeBuf), "Dye %d", row.dye);
                        dyeLabel = dyeBuf;
                    } else {
                        dyeLabel = i18n::tr("toolbar.equip.item.undyed");
                    }
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::BeginCombo(id, dyeLabel)) {
                        if (ImGui::Selectable(i18n::tr("toolbar.equip.item.undyed"), row.dye == 0))
                            app_.SetD3OutfitDye(row.visualSlot, 0);
                        if (ImGui::Selectable(i18n::tr("toolbar.equip.item.hide"), row.dye == 1))
                            app_.SetD3OutfitDye(row.visualSlot, 1);
                        for (i32 d = 2; d <= 22; ++d) {
                            char dyeItem[24];
                            std::snprintf(dyeItem, sizeof(dyeItem), "Dye %d##d3dyev%d", d,
                                          row.visualSlot);
                            if (ImGui::Selectable(dyeItem, row.dye == d))
                                app_.SetD3OutfitDye(row.visualSlot, d);
                        }
                        ImGui::EndCombo();
                    }
                }
                ImGui::Separator();
            }

            // Read once: an appearance carries up to ninety-odd looks and the
            // list is the same for every row below it.
            const auto looks = app_.D3LookNames();

            // The manual wardrobe below is the fallback for a storage with no
            // item tables; with the registry up (or on its way) it would just
            // restate the item rows in engine-internal words, so all that
            // survives of it is Hair — the one slot no item dresses — and the
            // Extras. `registryBuilding` counts so the redundant rows do not
            // flash for the seconds the build takes and then vanish.
            const bool manualWardrobe = !registryReady && !registryBuilding;

            // The set first, because putting one material set on every slot is
            // what wearing a set *is* — the per-slot rows underneath are for
            // mixing pieces from two of them.
            if (manualWardrobe && !looks.empty()) {
                const u32 shared = slots[0].lookIndex;
                bool uniform = true;
                for (const auto& s2 : slots)
                    uniform &= (s2.lookIndex == shared);
                const char* label = (uniform && shared < looks.size()) ? looks[shared].c_str() : "";
                ImGui::SetNextItemWidth(180);
                if (ImGui::BeginCombo(i18n::tr("toolbar.equip.set"), label)) {
                    for (u32 i = 0; i < static_cast<u32>(looks.size()); ++i) {
                        const bool isSel = uniform && (i == shared);
                        if (ImGui::Selectable(looks[i].c_str(), isSel))
                            app_.SetD3CharacterLookForAll(i);
                        if (isSel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", i18n::tr("toolbar.equip.set.tip"));
                ImGui::Separator();
            }

            for (const auto& slot : slots) {
                if (slot.items.empty())
                    continue;
                if (!manualWardrobe && slot.registryDriven)
                    continue; // the outfit rows above drive this slot now
                const u32 sel =
                    std::min<u32>(slot.selectedItem, static_cast<u32>(slot.items.size()) - 1);
                char id[64];
                std::snprintf(id, sizeof(id), "%s##d3item%d", slot.name.c_str(), slot.slot);
                ImGui::SetNextItemWidth(130);
                if (ImGui::BeginCombo(id, slot.items[sel].c_str())) {
                    for (u32 i = 0; i < static_cast<u32>(slot.items.size()); ++i) {
                        const bool isSel = (i == sel);
                        if (ImGui::Selectable(slot.items[i].c_str(), isSel))
                            app_.SetD3CharacterItem(slot.slot, i);
                        if (isSel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                // The material this one slot wears. Per slot and not per model
                // because the original reads a look name off each equipped
                // item — a heavy chest and heavy boots from two sets share one
                // material read at two variant indices.
                if (!looks.empty()) {
                    ImGui::SameLine();
                    std::snprintf(id, sizeof(id), "##d3look%d", slot.slot);
                    const u32 li =
                        std::min<u32>(slot.lookIndex, static_cast<u32>(looks.size()) - 1);
                    ImGui::SetNextItemWidth(150);
                    if (ImGui::BeginCombo(id, looks[li].c_str())) {
                        for (u32 i = 0; i < static_cast<u32>(looks.size()); ++i) {
                            const bool isSel = (i == li);
                            if (ImGui::Selectable(looks[i].c_str(), isSel))
                                app_.SetD3CharacterSlotLook(slot.slot, i);
                            if (isSel)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }

            // Everything no slot claims. Nothing in ActorModel_ApplyLook
            // switches these — a decapitated body is gameplay, not equipment —
            // so they are off until asked for rather than picked between.
            if (const auto extras = app_.D3CharacterExtras(); !extras.empty()) {
                ImGui::Separator();
                if (ImGui::TreeNode(i18n::tr("toolbar.equip.extras"))) {
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", i18n::tr("toolbar.equip.extras.tip"));
                    for (const auto& ex : extras) {
                        bool on = ex.shown;
                        char exid[160];
                        std::snprintf(exid, sizeof(exid), "%s##d3x%u", ex.name.c_str(), ex.geoset);
                        if (ImGui::Checkbox(exid, &on))
                            app_.SetD3CharacterExtra(ex.geoset, on);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }

    // ---- Lighting mode ----
    {
        i32 sel = static_cast<i32>(svc.Settings().GetLightingMode());
        ToolbarLabel("toolbar.lighting");
        ImGui::SetNextItemWidth(120);
        const char* lightingItems[3];
        for (i32 i = 0; i < static_cast<i32>(kLightingKeys.size()); ++i)
            lightingItems[i] = i18n::tr(kLightingKeys[i]);
        if (ImGui::Combo("##lighting", &sel, lightingItems,
                         static_cast<i32>(kLightingLabels.size()))) {
            svc.Settings().SetLightingMode(static_cast<LightingMode>(sel));
            SaveIni(app_);
        }
    }

    // Background work nobody asked for — the client-database prewarm — reports
    // here instead of taking the screen. Draws nothing when idle or when the
    // running task is one the modal is already showing.
    tools::DrawProgressStatus(app_.Tasks());

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void ViewerUI::BuildTabBar() {
    const i32 count = app_.DocumentCount();
    if (count <= 0)
        return; // nothing open — no strip

    // Anchor a thin strip directly beneath the toolbar (which is menuH + 8 tall
    // and sits at WorkPos.y). The tab bar draws over the top of the 3D view.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menuH = ImGui::GetFrameHeight();
    const f32 toolbarH = menuH + 8.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + toolbarH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, menuH + 8.0f));
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##tabbar", nullptr, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    ImGuiTabBarFlags tbFlags = ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_Reorderable |
                               ImGuiTabBarFlags_FittingPolicyScroll;
    if (ImGui::BeginTabBar("##documents", tbFlags)) {
        // After an app-driven active change (CLI bulk-load, File > Open, a
        // close handing focus to a neighbour) the tab bar would otherwise
        // default to its first tab. Force-select the app's active tab for that
        // one frame, and skip the "follow ImGui's selection" logic so a
        // transient first-tab selection can't snap the active document back.
        const i32 forceSelect = app_.ConsumePendingTabSelect();
        i32 toClose = -1;
        for (i32 i = 0; i < app_.DocumentCount(); ++i) {
            bool open = true;
            const ImGuiTabItemFlags flags = (i == forceSelect) ? ImGuiTabItemFlags_SetSelected : 0;
            // PushID disambiguates tabs whose labels (file stems) collide; the
            // visible label is still the file name.
            ImGui::PushID(i);
            if (ImGui::BeginTabItem(app_.DocumentTitle(i).c_str(), &open, flags)) {
                // BeginTabItem returns true for the selected tab — follow the
                // user's click by activating that document (but not on a
                // force-select frame, where ImGui's selection is still settling).
                if (forceSelect < 0 && app_.ActiveDocumentIndex() != i)
                    app_.SetActiveDocument(i);
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open)
                toClose = i; // the (x) was clicked
        }
        ImGui::EndTabBar();
        if (toClose >= 0)
            app_.CloseDocument(toClose);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void ViewerUI::BuildSettingsWindow() {
    ImGui::SetNextWindowSize(ImVec2(660, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(i18n::tr("settings.title"), &settingsOpen_)) {
        ImGui::End();
        return;
    }

    // The profile being EDITED and the product the provider is SERVING are two
    // facts. They agree most of the time, and clicking a row moves only the
    // first — configuring a game is not a reason to go read it.
    auto& provider = app_.Service().DefaultScene().GetContentProvider();
    const ProductId game = app_.SettingsProfile();
    const ProductId serving = provider.Game();

    ImGui::BeginChild("##profiles", ImVec2(170.0f, 0.0f), ImGuiChildFlags_Borders);
    // Above the games, and not one of them: the background colour, the frame's
    // post chain and the backend hold whatever is loaded.
    if (ImGui::Selectable(i18n::tr("settings.profile.general"), settingsGlobalPage_))
        settingsGlobalPage_ = true;
    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("settings.profile.header"));
    ImGui::Separator();
    for (const auto& p : kSettingsProfiles) {
        const bool picked = !settingsGlobalPage_ && p.product == game;
        if (ImGui::Selectable(p.label, picked) && !picked) {
            settingsGlobalPage_ = false;
            SelectSettingsProfile(p.product);
        }
        // A dot on the one whose storage is actually in use, so a page showing
        // "nothing is open" is legible rather than alarming.
        if (p.product == serving) {
            ImGui::SameLine();
            ImGui::TextDisabled("*");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##profilebody", ImVec2(0.0f, 0.0f));
    if (settingsGlobalPage_) {
        // No tab bar: the shared page has no second page to sit beside. IO is
        // a game's install, so it belongs to a game's row.
        BuildSettingsGeneralPage();
    } else if (ImGui::BeginTabBar("##SettingsTabs")) {
        if (ImGui::BeginTabItem(i18n::tr("settings.tab.general"))) {
            BuildSettingsGeneralTab(game);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(i18n::tr("settings.tab.io"))) {
            BuildSettingsIoTab(provider, game);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::End();
}

void ViewerUI::SelectSettingsProfile(ProductId game) {
    // The whole of it. Picking a profile says which settings to show and which
    // ini section to write; it does not repoint the provider, does not touch a
    // storage, and does not retry a single asset. A profile's CASC opens when
    // content from that game is loaded — see ViewerApp::FollowModelGame.
    app_.SetSettingsProfile(game);
}

void ViewerUI::BuildSettingsGeneralTab(ProductId game) {
    RenderService& svc = app_.Service();
    if (game == ProductId::Wow) {
        // ---- Lazy `.anim` loading ----
        // Applies to the next model loaded, not to the ones already in the
        // scene: the choice is made while parsing.
        {
            bool on = svc.Settings().M2LazyAnimations();
            if (ImGui::Checkbox(i18n::tr("settings.general.m2_lazy_anim"), &on)) {
                svc.Settings().SetM2LazyAnimations(on);
                SaveIni(app_);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i18n::tr("settings.general.m2_lazy_anim.tip"));
        }

        // ---- Transparent geometry ordering ----
        {
            bool on = svc.Settings().M2DistanceSortGeometry();
            if (ImGui::Checkbox(i18n::tr("settings.general.m2_dist_sort"), &on)) {
                svc.Settings().SetM2DistanceSortGeometry(on);
                SaveIni(app_);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i18n::tr("settings.general.m2_dist_sort.tip"));
        }

        // ---- Embedded lights ----
        {
            bool on = svc.Settings().M2ModelLights();
            if (ImGui::Checkbox(i18n::tr("settings.general.m2_model_lights"), &on)) {
                svc.Settings().SetM2ModelLights(on);
                SaveIni(app_);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i18n::tr("settings.general.m2_model_lights.tip"));
        }
        return;
    }
    if (game == ProductId::D3) {
        // ---- Lazy clip loading ----
        // On by default, unlike the `.m2` twin above: one character AnimSet
        // names 259 clips and 5.8 MB of keys to play one idle, and D3 has no
        // byte-identical gate recorded against an eager parse to protect.
        bool on = svc.Settings().D3LazyAnimations();
        if (ImGui::Checkbox(i18n::tr("settings.general.d3_lazy_anim"), &on)) {
            svc.Settings().SetD3LazyAnimations(on);
            SaveIni(app_);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i18n::tr("settings.general.d3_lazy_anim.tip"));
        return;
    }
    if (game == ProductId::Sc2) {
        // ---- Physics substepping ----
        // On by default: it needs nothing from the host, and the cadence it
        // replaces is visibly wrong on shipped content.
        {
            bool on = svc.Settings().PhysicsSubstepping();
            if (ImGui::Checkbox(i18n::tr("settings.general.physics_substep"), &on)) {
                svc.Settings().SetPhysicsSubstepping(on);
                SaveIni(app_);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i18n::tr("settings.general.physics_substep.tip"));
        }
        return;
    }
    if (game != ProductId::Wc3) {
        // Nothing here yet: this page is only what one game's data means, and
        // the settings that hold for all of them are the General row's.
        ImGui::TextDisabled("%s", i18n::tr("settings.general.none_for_profile"));
        return;
    }

    // What is left on this page is Warcraft III's, and only its: the day/night
    // cycle is a rig the game ships, the IBL probes are its Reforged
    // environment maps, the cascades are the only shadow pass any profile
    // registers, and the depth of field is where WC3 runs its own.

    // ---- Time of day ----
    if (auto* dnc = svc.GetDncService()) {
        const f32 hpd = dnc->GetHoursPerDay();
        f32 tod = dnc->GetTimeOfDay();
        if (ImGui::SliderFloat(i18n::tr("settings.general.time_of_day"), &tod, 0.0f, hpd,
                               "%.2f h")) {
            dnc->SetTimeOfDay(tod);
            SaveIni(app_);
        }
        bool animating = dnc->GetTodScale() > 0.0f;
        if (ImGui::Checkbox(i18n::tr("settings.general.animate_tod"), &animating)) {
            dnc->SetTodScale(animating ? 1.0f : 0.0f);
            SaveIni(app_);
        }
    }

    ImGui::Separator();

    // ---- IBL mode ----
    {
        i32 sel = static_cast<i32>(svc.Settings().GetIblMode());
        const char* iblItems[4];
        for (i32 i = 0; i < static_cast<i32>(kIblKeys.size()); ++i)
            iblItems[i] = i18n::tr(kIblKeys[i]);
        if (ImGui::Combo(i18n::tr("settings.general.ibl"), &sel, iblItems,
                         static_cast<i32>(kIblLabels.size()))) {
            svc.Settings().SetIblMode(static_cast<IblMode>(sel));
            SaveIni(app_);
        }
    }

    // ---- Shadows ----
    {
        i32 sel = 0;
        if (auto* shadow = svc.GetShadowService()) {
            sel = shadow->IsEnabled() ? std::clamp(shadow->Params().cascadeCount, 0, 3) : 0;
        }
        const char* shadowItems[4];
        for (i32 i = 0; i < static_cast<i32>(kShadowKeys.size()); ++i)
            shadowItems[i] = i18n::tr(kShadowKeys[i]);
        if (ImGui::Combo(i18n::tr("settings.general.shadows"), &sel, shadowItems,
                         static_cast<i32>(kShadowLabels.size()))) {
            if (auto* shadow = svc.GetShadowService()) {
                shadow::ShadowParams p = shadow->Params();
                p.enabled = (sel > 0);
                p.cascadeCount = (sel > 0) ? sel : 1;
                shadow->SetParams(p);
                SaveIni(app_);
            }
        }
    }

    // ---- World fog ----
    // The game reads fog from the map; a viewer has none, so this is the only
    // source. Distances are world units, the same scale as the camera distance.
    if (ImGui::CollapsingHeader(i18n::tr("settings.fog.header"))) {
        RenderSettings::WorldFog fog = svc.Settings().GetWorldFog();
        bool changed = false;
        std::array<const char*, kFogModeKeys.size()> modeItems{};
        for (usize i = 0; i < kFogModeKeys.size(); ++i)
            modeItems[i] = i18n::tr(kFogModeKeys[i]);
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::Combo(i18n::tr("settings.fog.mode"), &fog.mode, modeItems.data(),
                                static_cast<i32>(kFogModeKeys.size()));
        f32 rgb[3] = {fog.color[0] / 255.0f, fog.color[1] / 255.0f, fog.color[2] / 255.0f};
        if (ImGui::ColorEdit3(i18n::tr("settings.fog.color"), rgb)) {
            for (i32 c = 0; c < 3; ++c)
                fog.color[c] = static_cast<u8>(std::clamp(rgb[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            changed = true;
        }
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::DragFloat(i18n::tr("settings.fog.start"), &fog.start, 5.0f, 0.0f,
                                   20000.0f, "%.0f");
        ImGui::SetNextItemWidth(180.0f);
        changed |= ImGui::DragFloat(i18n::tr("settings.fog.end"), &fog.end, 5.0f, 0.0f, 20000.0f,
                                   "%.0f");
        if (fog.mode == 2 || fog.mode == 3 || fog.mode == 5 || fog.mode == 6) {
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.density"), &fog.density, 0.0001f,
                                       0.0f, 1.0f, "%.4f");
        }
        if (fog.mode == 4) {
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.height_top"), &fog.heightTop, 1.0f,
                                       -5000.0f, 5000.0f, "%.0f");
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.height_bottom"), &fog.heightBottom,
                                       1.0f, -5000.0f, 5000.0f, "%.0f");
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.radial_inner"), &fog.radialInner,
                                       5.0f, 0.0f, 20000.0f, "%.0f");
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.radial_outer"), &fog.radialOuter,
                                       5.0f, 0.0f, 20000.0f, "%.0f");
            ImGui::SetNextItemWidth(180.0f);
            changed |= ImGui::SliderFloat(i18n::tr("settings.fog.radial_strength"),
                                         &fog.radialStrength, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::Checkbox(i18n::tr("settings.fog.everywhere"), &fog.everywhere);
        }
        if (changed) {
            svc.Settings().SetWorldFog(fog);
            SaveIni(app_);
        }
    }

    ImGui::Separator();

    // ---- DNC model ----
    // The stock DNC set is small and fully enumerable (dnc_catalog.h), so
    // this is two combos instead of a free-text path: which light rig, and
    // which mod layer to read it from. A path the catalog doesn't know —
    // an older ini, or a hand-edited one — still shows and still loads.
    if (auto* dnc = svc.GetDncService()) {
        const auto catalog = dnc::DncCatalog();
        const std::string current = dnc->UnitMdlPath();
        const i32 sel = dnc::DncCatalogIndexOf(current);
        const dnc::DncVariant variant = dnc::DncVariantOf(current);

        ImGui::SetNextItemWidth(220.0f);
        const std::string preview = (sel >= 0) ? dnc::DncEntryLabel(catalog[sel]) : current;
        if (ImGui::BeginCombo(i18n::tr("settings.general.dnc_model"), preview.c_str())) {
            for (usize i = 0; i < catalog.size(); ++i) {
                const auto& e = catalog[i];
                if (ImGui::Selectable(dnc::DncEntryLabel(e).c_str(), static_cast<i32>(i) == sel)) {
                    dnc->SetUnitMdl(dnc::DncPathForVariant(e.path, variant));
                    SaveIni(app_);
                }
                if (ImGui::IsItemHovered()) {
                    std::string tilesets;
                    for (const auto& ts : dnc::DncTilesets()) {
                        if (ts.family != e.family)
                            continue;
                        if (!tilesets.empty())
                            tilesets += ", ";
                        tilesets += std::string(ts.name);
                    }
                    ImGui::SetTooltip("%.*s\n%s %s", static_cast<i32>(e.path.size()), e.path.data(),
                                      i18n::tr("settings.general.dnc_tilesets"), tilesets.c_str());
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.general.dnc_reset"))) {
            dnc->SetUnitMdl(dnc::DncService::kDefaultUnitMdl);
            SaveIni(app_);
        }

        // Auto follows the scene's art tier; SD/HD/DE pin the path to one
        // layer. Only Lordaeron's legacy target rig is SD-only, so the
        // overlay entries are greyed out rather than hidden.
        const bool hasSd = sel < 0 || catalog[sel].hasSd;
        const bool hasHd = sel < 0 || catalog[sel].hasHd;
        const bool hasDe = sel < 0 || catalog[sel].hasDe;
        const char* variantLabels[] = {i18n::tr("settings.general.dnc_variant_auto"), "SD", "HD",
                                       "DE"};
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo(i18n::tr("settings.general.dnc_variant"),
                              variantLabels[static_cast<usize>(variant)])) {
            const bool enabled[] = {true, hasSd, hasHd, hasDe};
            for (usize i = 0; i < std::size(variantLabels); ++i) {
                ImGui::BeginDisabled(!enabled[i]);
                if (ImGui::Selectable(variantLabels[i], i == static_cast<usize>(variant))) {
                    dnc->SetUnitMdl(
                        dnc::DncPathForVariant(current, static_cast<dnc::DncVariant>(i)));
                    SaveIni(app_);
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // ---- Depth of Field (HD-only) ----
    // Runs the shipped depthoffield.bls. The pass self-disables until a
    // focal distance > 0 is set, so enabling with a zero distance seeds a
    // sensible default — otherwise the checkbox would appear to do nothing.
    if (ImGui::CollapsingHeader(i18n::tr("settings.dof.header"))) {
        // Focus on the subject: the camera→target distance is the model
        // centre's view-space depth, which is what `linearDepth` carries.
        // The CoC is hyperbolic — (1/focus − 1/depth)·focusScale — so at
        // view-space depths (hundreds) focusScale needs to be ~tens-hundreds
        // for visible blur, not the ~1 a normalised-depth pass would use.
        const f32 camDist = svc.Scene().Camera().GetDistance();
        const f32 autoFocus = camDist > 0.0f ? camDist : 600.0f;
        bool dof = svc.Settings().DofEnabled();
        if (ImGui::Checkbox(i18n::tr("settings.dof.enabled"), &dof)) {
            svc.Settings().SetDofEnabled(dof);
            if (dof) {
                // Auto-focus on the model and seed a visible strength if the
                // current values would produce no perceptible blur.
                if (svc.Settings().DofFocusDistance() <= 0.0f)
                    svc.Settings().SetDofFocusDistance(autoFocus);
                if (svc.Settings().DofFocusScale() < 5.0f)
                    svc.Settings().SetDofFocusScale(50.0f);
            }
            SaveIni(app_);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(i18n::tr("settings.dof.reset"))) {
            svc.Settings().SetDofFocusDistance(autoFocus);
            svc.Settings().SetDofFocusScale(50.0f);
            svc.Settings().SetDofMaxBlurSize(20.0f);
            svc.Settings().SetDofRadiusScale(1.0f);
            svc.Settings().SetDofFarFieldOnly(false);
            SaveIni(app_);
        }
        f32 focusDist = svc.Settings().DofFocusDistance();
        f32 focusScale = svc.Settings().DofFocusScale();
        f32 maxBlur = svc.Settings().DofMaxBlurSize();
        f32 radius = svc.Settings().DofRadiusScale();
        bool farOnly = svc.Settings().DofFarFieldOnly();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.dof.focus_dist"), &focusDist, 0.0f, 3000.0f,
                               "%.0f")) {
            svc.Settings().SetDofFocusDistance(focusDist);
            SaveIni(app_);
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.dof.focus_scale"), &focusScale, 0.0f, 200.0f,
                               "%.1f")) {
            svc.Settings().SetDofFocusScale(focusScale);
            SaveIni(app_);
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.dof.max_blur"), &maxBlur, 1.0f, 40.0f, "%.1f")) {
            svc.Settings().SetDofMaxBlurSize(maxBlur);
            SaveIni(app_);
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.dof.sample_density"), &radius, 0.25f, 4.0f,
                               "%.2f")) {
            svc.Settings().SetDofRadiusScale(radius);
            SaveIni(app_);
        }
        if (ImGui::Checkbox(i18n::tr("settings.dof.far_field_only"), &farOnly)) {
            svc.Settings().SetDofFarFieldOnly(farOnly);
            SaveIni(app_);
        }
    }
}

void ViewerUI::BuildSettingsGeneralPage() {
    RenderService& svc = app_.Service();

    // ---- Background colour ----
    {
        const u32 bg = svc.Settings().BackgroundColorRaw();
        f32 col[3] = {
            static_cast<f32>(bg & 0xFFu) / 255.0f,
            static_cast<f32>((bg >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((bg >> 16) & 0xFFu) / 255.0f,
        };
        if (ImGui::ColorEdit3(i18n::tr("settings.general.background"), col)) {
            svc.Settings().SetBackgroundColor(static_cast<u8>(col[0] * 255.0f),
                                              static_cast<u8>(col[1] * 255.0f),
                                              static_cast<u8>(col[2] * 255.0f));
            SaveIni(app_);
        }
    }

    // ---- Exposure ----
    {
        f32 exposure = svc.Settings().GetTonemapExposure();
        if (ImGui::SliderFloat(i18n::tr("settings.general.exposure"), &exposure, 0.0f, 3.0f,
                               "%.2f")) {
            svc.Settings().SetTonemapExposure(exposure);
            SaveIni(app_);
        }
    }

    // ---- Sound volume ----
    {
        f32 vol = svc.Sound().GetVolume();
        if (ImGui::SliderFloat(i18n::tr("settings.general.snd_volume"), &vol, 0.0f, 1.0f, "%.2f")) {
            svc.Sound().SetVolume(vol);
            SaveIni(app_);
        }
    }

    // ---- Loop non-looping ----
    {
        bool on = app_.LoopNonLoopingPolicy();
        if (ImGui::Checkbox(i18n::tr("settings.general.loop_nonlooping"), &on)) {
            app_.SetLoopNonLoopingPolicy(on);
            SaveIni(app_);
        }
    }

    ImGui::Separator();

    // ---- Ambient occlusion (GTAO) ----
    {
        bool ao = svc.Settings().AoEnabled();
        if (ImGui::Checkbox(i18n::tr("settings.general.ao"), &ao)) {
            svc.Settings().SetAoEnabled(ao);
            SaveIni(app_);
        }

        static constexpr std::array<const char*, 3> kAoQualityLabels = {"Low", "Medium", "High"};
        static constexpr std::array<const char*, 3> kAoQualityKeys = {
            "aoquality.low", "aoquality.medium", "aoquality.high"};
        i32 q = static_cast<i32>(svc.Settings().AoQuality());
        if (q < 0 || q >= static_cast<i32>(kAoQualityLabels.size()))
            q = 1;
        ImGui::SetNextItemWidth(180.0f);
        const char* aoItems[3];
        for (i32 i = 0; i < static_cast<i32>(kAoQualityKeys.size()); ++i)
            aoItems[i] = i18n::tr(kAoQualityKeys[i]);
        if (ImGui::Combo(i18n::tr("settings.general.ao_quality"), &q, aoItems,
                         static_cast<i32>(kAoQualityLabels.size()))) {
            svc.Settings().SetAoQuality(static_cast<u32>(q));
            SaveIni(app_);
        }

        f32 boost = svc.Settings().AoBentBoost();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.general.ao_bent_boost"), &boost, 0.0f, 0.5f,
                               "%.3f")) {
            svc.Settings().SetAoBentBoost(boost);
            SaveIni(app_);
        }
    }

    // ---- Bloom (HD-only) ----
    // CollapsingHeader keeps three sliders + a reset button from
    // crowding the main settings list when bloom is off. Defaults
    // mirror the engine's RegisterBloom (BL_BLOOM_D=off,
    // threshold=1.0, intensity=1.25, saturation=1.0).
    if (ImGui::CollapsingHeader(i18n::tr("settings.bloom.header"))) {
        bool bloom = svc.Settings().BloomEnabled();
        if (ImGui::Checkbox(i18n::tr("settings.bloom.enabled"), &bloom)) {
            svc.Settings().SetBloomEnabled(bloom);
            SaveIni(app_);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(i18n::tr("settings.bloom.reset"))) {
            svc.Settings().SetBloomThreshold(1.0f);
            svc.Settings().SetBloomIntensity(1.25f);
            svc.Settings().SetBloomSaturation(1.0f);
            SaveIni(app_);
        }
        f32 threshold = svc.Settings().BloomThreshold();
        f32 intensity = svc.Settings().BloomIntensity();
        f32 saturation = svc.Settings().BloomSaturation();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.bloom.threshold"), &threshold, 0.0f, 4.0f,
                               "%.2f")) {
            svc.Settings().SetBloomThreshold(threshold);
            SaveIni(app_);
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.bloom.intensity"), &intensity, 0.0f, 4.0f,
                               "%.2f")) {
            svc.Settings().SetBloomIntensity(intensity);
            SaveIni(app_);
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat(i18n::tr("settings.bloom.saturation"), &saturation, 0.0f, 4.0f,
                               "%.2f")) {
            svc.Settings().SetBloomSaturation(saturation);
            SaveIni(app_);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled(i18n::tr("settings.general.startup_note"));

    // ---- Default backend ----
    // Platform availability:
    //   Windows: D3D11, D3D12, Vulkan, WebGPU (when WDX_HAS_WEBGPU)
    //   macOS:   Vulkan, WebGPU (when WDX_HAS_WEBGPU) — D3D11/D3D12 are
    //            WIN32-only via CMake + gfx_factory
    //   Linux:   Vulkan only — D3D11/D3D12 WIN32-only, WebGPU/Dawn isn't
    //            wired into Linux builds.
#if defined(_WIN32)
    {
        // Metal is Apple-only, so it's excluded from the Windows list. It's
        // the trailing entry in kBackendLabels (index 4), so the windowed
        // set is just the first four: D3D11, D3D12, Vulkan, WebGPU.
        constexpr i32 kWinBackendCount = static_cast<i32>(kBackendLabels.size()) - 1;
        i32 sel = BackendToIdx(svc.Settings().DefaultBackend());
        if (sel >= kWinBackendCount)
            sel = BackendToIdx(gfx::GfxApi::D3D12); // clamp a stale Metal selection
        if (ImGui::Combo(i18n::tr("settings.general.backend"), &sel, kBackendLabels.data(),
                         kWinBackendCount)) {
            svc.Settings().SetDefaultBackend(IdxToBackend(sel));
            SaveIni(app_);
        }
    }
#elif defined(__APPLE__)
    {
#if WDX_HAS_WEBGPU
        const char* macLabels[] = {"Metal", "Vulkan", "WebGPU"};
        const gfx::GfxApi macApis[] = {gfx::GfxApi::Metal, gfx::GfxApi::Vulkan,
                                       gfx::GfxApi::WebGPU};
#else
        const char* macLabels[] = {"Metal", "Vulkan"};
        const gfx::GfxApi macApis[] = {gfx::GfxApi::Metal, gfx::GfxApi::Vulkan};
#endif
        // Find the index of the currently-selected backend; fall back to
        // Metal (entry 0) if the saved value is something this build
        // doesn't expose.
        const auto cur = svc.Settings().DefaultBackend();
        i32 sel = 0;
        for (i32 i = 0; i < static_cast<i32>(std::size(macApis)); ++i) {
            if (macApis[i] == cur) {
                sel = i;
                break;
            }
        }
        if (ImGui::Combo(i18n::tr("settings.general.backend"), &sel, macLabels,
                         static_cast<i32>(std::size(macLabels)))) {
            svc.Settings().SetDefaultBackend(macApis[sel]);
            SaveIni(app_);
        }
    }
#else
    {
        ImGui::BeginDisabled();
        i32 sel = 0;
        const char* vkOnly[] = {"Vulkan"};
        ImGui::Combo(i18n::tr("settings.general.backend"), &sel, vkOnly, 1);
        ImGui::EndDisabled();
    }
#endif

    // ---- Preferred device ----
    {
        static std::vector<std::string> devices;
        static i32 lastBackendIdx = -1;
        const i32 curBackendIdx = BackendToIdx(svc.Settings().DefaultBackend());
        if (curBackendIdx != lastBackendIdx) {
            devices = gfx::EnumerateDevices(svc.Settings().DefaultBackend());
            lastBackendIdx = curBackendIdx;
        }
        const std::string& cur = svc.Settings().PreferredDevice();
        const char* preview = cur.empty() ? i18n::tr("settings.general.device_auto") : cur.c_str();
        if (ImGui::BeginCombo(i18n::tr("settings.general.device"), preview)) {
            if (ImGui::Selectable(i18n::tr("settings.general.device_auto"), cur.empty())) {
                svc.Settings().SetPreferredDevice("");
                SaveIni(app_);
            }
            for (const auto& n : devices) {
                const bool isSel = (n == cur);
                if (ImGui::Selectable(n.c_str(), isSel)) {
                    svc.Settings().SetPreferredDevice(n);
                    SaveIni(app_);
                }
                if (isSel)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // ---- Graphics debug ----
    {
        bool on = svc.Settings().GraphicsDebug();
        if (ImGui::Checkbox(i18n::tr("settings.general.graphics_debug"), &on)) {
            svc.Settings().SetGraphicsDebug(on);
            SaveIni(app_);
        }
    }
}

// ---- IO pages ----
// These edit a PROFILE, not "the provider". A profile is an ini section plus,
// for whichever product the provider is currently serving, a live slot inside
// it. Every edit commits to the ini, because that is what a profile is; it
// reaches the provider only when the profile is the active one, and then its
// storage genuinely has to be reopened, since where it reads from just moved.
// A profile that is not active has nothing open to reopen: it picks the new
// settings up through ApplyIoPathOverrides at the moment content first needs
// it, which is the only moment its CASC should open.

void ViewerUI::SeedIoBuffers(io::FileContentProvider& provider, ProductId game) {
    // The provider's getters answer for its ACTIVE slot, so they are the truth
    // for exactly one profile and the wrong product's answer for the others.
    if (game == provider.Game()) {
        installPathBuf_ = provider.InstallPath();
        hotsPathBuf_ = provider.HotsInstallPath();
        listfileBuf_ = provider.ListfilePath();
        tactKeyBuf_ = provider.TactKeyPath();
        ioIgnoreCascBuf_ = provider.IgnoreCasc();
        ioIgnoreMpqBuf_ = provider.IgnoreMpq();
        ioMpqListBuf_ = provider.MpqList();
    } else {
        const IoPathOverrides o = LoadIoPathOverrides(game);
        installPathBuf_ = o.installPath.empty() ? provider.GamePath(game) : o.installPath;
        hotsPathBuf_ = o.hotsInstallPath.empty() ? provider.HotsPath() : o.hotsInstallPath;
        listfileBuf_ = o.listfilePath;
        tactKeyBuf_ = o.tactKeyPath;
        ioIgnoreCascBuf_ = o.ignoreCasc;
        ioIgnoreMpqBuf_ = o.ignoreMpq;
        // No saved order means the same answer the provider would have reached:
        // what is actually on disk for this game. Reading a directory is not
        // opening a storage.
        ioMpqListBuf_ = o.mpqListSet ? o.mpqList : io::ScanArchives(game, installPathBuf_);
    }
    newMpqEntryBuf_.clear();
}

void ViewerUI::CommitIoProfile(io::FileContentProvider& provider, ProductId game) {
    IoPathOverrides o;
    // "Same as auto-detected" is stored as no override, so a later reinstall
    // elsewhere is picked up instead of pinned to a stale path.
    o.installPath = (installPathBuf_ == provider.GamePath(game)) ? std::string{} : installPathBuf_;
    o.ignoreCasc = ioIgnoreCascBuf_;
    o.ignoreMpq = ioIgnoreMpqBuf_;
    o.listfilePath = listfileBuf_;
    o.tactKeyPath = tactKeyBuf_;
    if (game == ProductId::Sc2)
        o.hotsInstallPath = (hotsPathBuf_ == provider.HotsPath()) ? std::string{} : hotsPathBuf_;
    if (!IsCascOnly(game)) {
        o.mpqListSet = true;
        o.mpqList = ioMpqListBuf_;
    }
    SaveIoPathOverrides(game, o);

    if (game != provider.Game())
        return; // not the active profile: nothing is open, so nothing reopens

    // It IS the active profile, so its storage is in use and now points
    // somewhere else. ApplyIoPathOverrides invalidates the slot; the retry's
    // reads are what rebuild it — which for the game being read is the point,
    // not a cost.
    app_.ApplyProfile(game, /*force=*/true);
    // The two roots ApplyIoPathOverrides deliberately skips when empty: at
    // startup "no override" means "leave the detected path alone", but here it
    // means the user pressed Reset, and the provider is still holding the
    // override they just cleared.
    provider.SetInstallPath(o.installPath);
    if (game == ProductId::Sc2)
        provider.SetHotsInstallPath(o.hotsInstallPath);
    // Opened here, deliberately, rather than left to the next read. This is the
    // commit that used to freeze the window for the length of a retail World of
    // Warcraft open — index files, encoding table, ~870 VFS manifests and a
    // 90 MB listfile — because whichever thread read next paid for all of it.
    // Asking now puts it on the task thread with a bar in front of it.
    // RetryUnloadedAssets moves into the completion: retrying against a storage
    // that is still opening just misses again.
    app_.OpenStoragesAsync();
}

void ViewerUI::BuildSettingsIoTab(io::FileContentProvider& provider, ProductId game) {
    // Re-seed on a change of EITHER: a different profile is being edited, or
    // the provider moved onto a different product — which changes where this
    // profile's truth lives (its live slot vs its ini section), and can bring
    // settings the ini never saw, like keys adopted beside a loose model.
    if (!ioBufsInitialised_ || ioBufsGame_ != game || ioBufsServing_ != provider.Game()) {
        SeedIoBuffers(provider, game);
        ioBufsInitialised_ = true;
        ioBufsGame_ = game;
        ioBufsServing_ = provider.Game();
    }
    if (IsCascOnly(game))
        BuildIoCascPage(provider, game);
    else
        BuildIoArchivePage(provider, game);
    BuildIoStorageStatus(provider, game);
}

// Live storage state, and only for the profile that has any. For the others the
// honest answer is that nothing is open — saying "not loaded" would read as a
// failure when it is the design.
void ViewerUI::BuildIoStorageStatus(io::FileContentProvider& provider, ProductId game) {
    ImGui::Spacing();
    ImGui::Separator();
    if (game != provider.Game()) {
        ImGui::TextDisabled("%s", i18n::tr("settings.io.inactive_profile"));
        return;
    }
    // Pending is checked first on purpose: storages open on demand, and
    // HasCasc() is a demand. Reading the status must not be what triggers the
    // open it is reporting on.
    // Opening is checked FIRST and answered from an atomic. Every call below
    // this point takes the storage lock, which an open holds for its entire
    // duration — so asking one of them here would block the UI thread on the
    // very operation the status line is trying to describe.
    if (provider.StoragesOpening()) {
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"), "opening...");
        return;
    }
    if (provider.StoragesState() == io::StorageState::Cancelled) {
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"), "cancelled");
        if (ImGui::SmallButton("Retry"))
            app_.OpenStoragesAsync();
        return;
    }
    if (provider.StoragesPending()) {
        ImGui::TextDisabled(i18n::tr("settings.io.casc_status"), i18n::tr("settings.io.pending"));
        if (!IsCascOnly(game))
            ImGui::TextDisabled(i18n::tr("settings.io.mpq_status"),
                                i18n::tr("settings.io.pending"));
        return;
    }
    // Which roots opened, not just whether any did: StarCraft II offers two and
    // either can fail on its own, which a single "CASC: open" would hide.
    const auto roots = provider.OpenCascRoots();
    ImGui::TextDisabled(i18n::tr("settings.io.casc_status"),
                        roots.empty() ? i18n::tr("settings.io.not_loaded")
                                      : i18n::tr("settings.io.open"));
    for (const auto& r : roots)
        ImGui::TextDisabled("    %s", r.c_str());
    if (!IsCascOnly(game))
        ImGui::TextDisabled(i18n::tr("settings.io.mpq_status"),
                            provider.HasMpq() ? i18n::tr("app.yes") : i18n::tr("app.no"));
}

void ViewerUI::BuildIoArchivePage(io::FileContentProvider& provider, ProductId game) {
    const std::string autoDetected = provider.GamePath(game);
    if (autoDetected.empty())
        ImGui::TextDisabled(i18n::tr(game == ProductId::Wow ? "settings.io.wow_not_detected"
                                                            : "settings.io.not_detected"));
    else
        ImGui::TextDisabled(i18n::tr("settings.io.auto_detected"), autoDetected.c_str());
    ImGui::Spacing();

    auto commit = [&] { CommitIoProfile(provider, game); };

    // ---- Install path row ----
    {
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", installPathBuf_.c_str());
        ImGui::SetNextItemWidth(-180.0f);
        if (ImGui::InputText("##install", tmp, sizeof(tmp)))
            installPathBuf_ = tmp;
        if (ImGui::IsItemDeactivatedAfterEdit())
            commit();
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.browse_install"))) {
            NFD::UniquePathU8 outPath;
            if (NFD::PickFolder(outPath) == NFD_OKAY) {
                installPathBuf_ = outPath.get();
                commit();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.reset_install"))) {
            installPathBuf_ = autoDetected;
            commit();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(i18n::tr("settings.io.install_path"));
    }

    // ---- Listfile row (World of Warcraft only) ----
    // A WoW CASC root stores fileDataIDs and name hashes, not paths, so
    // nothing can browse or path-read it without a community `id;path`
    // CSV. Reads by id work regardless — this is what makes the Storage
    // Explorer and by-path loads possible.
    if (game == ProductId::Wow) {
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", listfileBuf_.c_str());
        ImGui::SetNextItemWidth(-180.0f);
        if (ImGui::InputText("##listfile", tmp, sizeof(tmp)))
            listfileBuf_ = tmp;
        if (ImGui::IsItemDeactivatedAfterEdit())
            commit();
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.browse_listfile"))) {
            NFD::UniquePathU8 outPath;
            nfdu8filteritem_t filter[1] = {{"Listfile", "csv,txt"}};
            if (NFD::OpenDialog(outPath, filter, 1) == NFD_OKAY) {
                listfileBuf_ = outPath.get();
                commit();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.clear_listfile"))) {
            listfileBuf_.clear();
            commit();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(i18n::tr("settings.io.listfile"));
        // "A path is set" and "it loaded" are different facts, and only the
        // second one makes the root browsable. The listfile is read as part of
        // opening the storage, so before that it is pending like the rest —
        // and for a profile that is not the active one, nothing has read it.
        ImGui::TextDisabled(i18n::tr("settings.io.listfile_status"),
                            (game != provider.Game() || provider.StoragesPending())
                                ? i18n::tr("settings.io.pending")
                            : provider.HasListfile() ? i18n::tr("settings.io.loaded")
                                                     : i18n::tr("settings.io.not_loaded"));

        // ---- TACT key row ----
        // The listfile's twin one layer down. Blizzard encrypts individual
        // frames of shipped files, and a file holding one reads back as
        // *missing* rather than as an error — so without a key list part of the
        // install is simply invisible, with nothing to say why.
        char keyTmp[1024];
        std::snprintf(keyTmp, sizeof(keyTmp), "%s", tactKeyBuf_.c_str());
        ImGui::SetNextItemWidth(-180.0f);
        if (ImGui::InputText("##tactkeys", keyTmp, sizeof(keyTmp)))
            tactKeyBuf_ = keyTmp;
        if (ImGui::IsItemDeactivatedAfterEdit())
            commit();
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.browse_tactkeys"))) {
            NFD::UniquePathU8 outPath;
            nfdu8filteritem_t filter[1] = {{"TACT keys", "txt,csv"}};
            if (NFD::OpenDialog(outPath, filter, 1) == NFD_OKAY) {
                tactKeyBuf_ = outPath.get();
                commit();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.clear_tactkeys"))) {
            tactKeyBuf_.clear();
            commit();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(i18n::tr("settings.io.tactkeys"));
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- Ignore flags ----
    {
        if (ImGui::Checkbox(i18n::tr("settings.io.ignore_casc"), &ioIgnoreCascBuf_))
            commit();
        if (ImGui::Checkbox(i18n::tr("settings.io.ignore_mpq"), &ioIgnoreMpqBuf_))
            commit();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- MPQ load list ----
    // Earlier entries win. The buttons edit the buffer and commit, which for
    // the active profile reopens its storages each time — fine for a settings
    // dialog (low-frequency edits), and free for any other profile.
    ImGui::TextUnformatted(i18n::tr("settings.io.mpq_header"));
    ImGui::BeginDisabled(ioIgnoreMpqBuf_);

    bool mpqsDirty = false;
    i32 swapWith = -1; // [i, i+1] to swap when set
    i32 removeAt = -1;
    for (usize i = 0; i < ioMpqListBuf_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool isFirst = (i == 0);
        const bool isLast = (i + 1 == ioMpqListBuf_.size());
        ImGui::BeginDisabled(isFirst);
        if (ImGui::ArrowButton("up", ImGuiDir_Up))
            swapWith = static_cast<i32>(i) - 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(isLast);
        if (ImGui::ArrowButton("down", ImGuiDir_Down))
            swapWith = static_cast<i32>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("X"))
            removeAt = static_cast<i32>(i);
        ImGui::SameLine();
        ImGui::TextUnformatted(ioMpqListBuf_[i].c_str());
        ImGui::PopID();
    }
    if (swapWith >= 0 && swapWith + 1 < static_cast<i32>(ioMpqListBuf_.size())) {
        std::swap(ioMpqListBuf_[swapWith], ioMpqListBuf_[swapWith + 1]);
        mpqsDirty = true;
    }
    if (removeAt >= 0 && removeAt < static_cast<i32>(ioMpqListBuf_.size())) {
        ioMpqListBuf_.erase(ioMpqListBuf_.begin() + removeAt);
        mpqsDirty = true;
    }

    // Add-new row.
    {
        char tmp[256];
        std::snprintf(tmp, sizeof(tmp), "%s", newMpqEntryBuf_.c_str());
        ImGui::SetNextItemWidth(-140.0f);
        if (ImGui::InputText("##newmpq", tmp, sizeof(tmp)))
            newMpqEntryBuf_ = tmp;
        ImGui::SameLine();
        const bool canAdd = !newMpqEntryBuf_.empty();
        ImGui::BeginDisabled(!canAdd);
        if (ImGui::Button(i18n::tr("settings.io.add_mpq"))) {
            ioMpqListBuf_.push_back(newMpqEntryBuf_);
            newMpqEntryBuf_.clear();
            mpqsDirty = true;
        }
        ImGui::EndDisabled();
    }

    // "Defaults" means the fixed three for Warcraft III, but for WoW it
    // means what is actually in Data/ — its archive names changed twice
    // across the MPQ era, so a static list is wrong for most installs.
    if (ImGui::SmallButton(i18n::tr("settings.io.reset_defaults"))) {
        ioMpqListBuf_ = io::ScanArchives(game, installPathBuf_);
        mpqsDirty = true;
    }

    ImGui::EndDisabled(); // IgnoreMpq guard around the list controls

    if (mpqsDirty)
        commit();
}

// The CASC-only products: no archives, because neither StarCraft II nor
// Diablo III ever shipped an MPQ. StarCraft II is the one with *two* roots —
// it and Heroes of the Storm are two installs behind one product, because they
// share a render profile — so the second row is its alone.
void ViewerUI::BuildIoCascPage(io::FileContentProvider& provider, ProductId game) {
    auto commit = [&] { CommitIoProfile(provider, game); };

    // One row per game: the text field, a folder picker, a reset to the
    // discovered path, and a label. The two rows differ only in the buffer.
    auto rootRow = [&](const char* id, std::string& buf, const std::string& discovered,
                       const char* label) {
        ImGui::PushID(id);
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", buf.c_str());
        ImGui::SetNextItemWidth(-180.0f);
        if (ImGui::InputText("##root", tmp, sizeof(tmp)))
            buf = tmp;
        if (ImGui::IsItemDeactivatedAfterEdit())
            commit();
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.browse_install"))) {
            NFD::UniquePathU8 outPath;
            if (NFD::PickFolder(outPath) == NFD_OKAY) {
                buf = outPath.get();
                commit();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("settings.io.reset_install"))) {
            buf = discovered;
            commit();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        if (discovered.empty())
            ImGui::TextDisabled(i18n::tr("settings.io.casc_not_detected"), label);
        ImGui::PopID();
    };

    if (game == ProductId::D3) {
        rootRow("d3", installPathBuf_, provider.GamePath(ProductId::D3), "Diablo III");
    } else {
        rootRow("sc2", installPathBuf_, provider.GamePath(ProductId::Sc2), "StarCraft II");
        ImGui::Spacing();
        rootRow("hots", hotsPathBuf_, provider.HotsPath(), "Heroes of the Storm");
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Checkbox(i18n::tr("settings.io.ignore_casc"), &ioIgnoreCascBuf_))
        commit();
}

} // namespace whiteout::flakes
