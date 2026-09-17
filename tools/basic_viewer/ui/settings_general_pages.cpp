// The Settings window's General page and each game's own page.

#include "ui/settings_window.h"

#include "app/viewer_app.h"
#include "backend_names.h"
#include "color_pack.h"
#include "localization.h"
#include "renderer/dnc/dnc_catalog.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "ui/ui_context.h"
#include "ui/ui_metrics.h"
#include "ui/setting_rows.h"
#include "ui/widgets.h"
#include "whiteout/flakes/sound_emitter.h"

#include "gfx/gfx.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <span>

namespace whiteout::flakes {

namespace dnc = renderer::dnc;
namespace shadow = renderer::shadow;

namespace {

using renderer::RenderSettings;

constexpr std::array<const char*, 4> kIblKeys = {"ibl.portrait", "ibl.daynight", "ibl.dungeon", "ibl.sunset"};
/// Off, then one to three cascades.
constexpr std::array<const char*, 4> kShadowKeys = {"shadow.off", "shadow.1", "shadow.2", "shadow.3"};
constexpr i32 kMaxShadowCascades = 3;
/// Indexed by the shader's fog mode (bls::FogParams::mode).
constexpr std::array<const char*, 7> kFogModeKeys = {"fog.off",      "fog.linear",     "fog.exp",
                                                     "fog.exp2",     "fog.volumetric", "fog.exp_banded",
                                                     "fog.exp2_banded"};
constexpr std::array<const char*, 3> kAoQualityKeys = {"aoquality.low", "aoquality.medium", "aoquality.high"};
constexpr i32 kDefaultAoQuality = 1;

} // namespace

void SettingsWindow::BuildProfilePage(ProductId game) {
    struct ProfilePage {
        ProductId product;
        void (SettingsWindow::*build)();
    };
    static constexpr ProfilePage kProfilePages[] = {
        {ProductId::Wc3, &SettingsWindow::BuildWc3Page},
        {ProductId::Wow, &SettingsWindow::BuildWowPage},
        {ProductId::Sc2, &SettingsWindow::BuildSc2Page},
        {ProductId::D3, &SettingsWindow::BuildD3Page},
    };
    for (const ProfilePage& page : kProfilePages) {
        if (page.product == game) {
            (this->*page.build)();
            return;
        }
    }
    // This page is only what one game's data means, and the settings that hold
    // for all of them are the General row's.
    ImGui::TextDisabled("%s", i18n::tr("settings.general.none_for_profile"));
}

void SettingsWindow::BuildWowPage() {
    RenderSettings& render = ctx_.app.Service().Settings();
    // Lazy `.anim` loading applies to the next model loaded, not to the ones in
    // the scene: the choice is made while parsing.
    for (const settings::BoolSetting* row : settings::kWowPage) {
        if (ui::SettingCheckbox(render, *row))
            ctx_.MarkSettingsDirty();
    }
}

void SettingsWindow::BuildD3Page() {
    // Lazy clip loading is on by default, unlike the `.m2` twin: one character
    // AnimSet names 259 clips and 5.8 MB of keys to play one idle, and D3 has no
    // byte-identical gate recorded against an eager parse to protect.
    RenderSettings& render = ctx_.app.Service().Settings();
    if (ui::SettingCheckbox(render, settings::kD3LazyAnimations))
        ctx_.MarkSettingsDirty();
}

void SettingsWindow::BuildSc2Page() {
    // Physics substepping is on by default: it needs nothing from the host, and
    // the cadence it replaces is visibly wrong on shipped content.
    RenderSettings& render = ctx_.app.Service().Settings();
    if (ui::SettingCheckbox(render, settings::kPhysicsSubstepping))
        ctx_.MarkSettingsDirty();
}

// Warcraft III's, and only its: the day/night cycle is a rig the game ships, the
// IBL probes are its Reforged environment maps, the cascades are the only shadow
// pass any profile registers, and the depth of field is where WC3 runs its own.
void SettingsWindow::BuildWc3Page() {
    renderer::RenderService& svc = ctx_.app.Service();
    RenderSettings& render = svc.Settings();

    // ---- Time of day ----
    if (auto* dnc = svc.GetDncService()) {
        const f32 hoursPerDay = dnc->GetHoursPerDay();
        f32 tod = dnc->GetTimeOfDay();
        if (ImGui::SliderFloat(i18n::tr("settings.general.time_of_day"), &tod, 0.0f, hoursPerDay, "%.2f h")) {
            dnc->SetTimeOfDay(tod);
            ctx_.MarkSettingsDirty();
        }
        bool animating = dnc->GetTodScale() > 0.0f;
        if (ImGui::Checkbox(i18n::tr("settings.general.animate_tod"), &animating)) {
            dnc->SetTodScale(animating ? 1.0f : 0.0f);
            ctx_.MarkSettingsDirty();
        }
    }

    ImGui::Separator();

    // ---- IBL mode ----
    {
        i32 sel = static_cast<i32>(render.GetIblMode());
        if (ui::KeyCombo(i18n::tr("settings.general.ibl"), sel, kIblKeys)) {
            render.SetIblMode(static_cast<IblMode>(sel));
            ctx_.MarkSettingsDirty();
        }
    }

    // ---- Shadows ----
    {
        auto* shadow = svc.GetShadowService();
        i32 sel = (shadow && shadow->IsEnabled()) ? std::clamp(shadow->Params().cascadeCount, 0, kMaxShadowCascades) : 0;
        if (ui::KeyCombo(i18n::tr("settings.general.shadows"), sel, kShadowKeys) && shadow) {
            shadow::ShadowParams p = shadow->Params();
            p.enabled = sel > 0;
            p.cascadeCount = sel > 0 ? sel : 1;
            shadow->SetParams(p);
            ctx_.MarkSettingsDirty();
        }
    }

    // ---- World fog ----
    // The game reads fog from the map; a viewer has none, so this is the only
    // source. Distances are world units, the camera distance's scale.
    if (ImGui::CollapsingHeader(i18n::tr("settings.fog.header"))) {
        RenderSettings::WorldFog fog = render.GetWorldFog();
        bool changed = false;
        ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
        changed |= ui::KeyCombo(i18n::tr("settings.fog.mode"), fog.mode, kFogModeKeys);
        tools::Rgb8 color{fog.color[0], fog.color[1], fog.color[2]};
        if (ui::ColorEditRgb8(i18n::tr("settings.fog.color"), color, 0, /*round=*/true)) {
            fog.color[0] = color.r;
            fog.color[1] = color.g;
            fog.color[2] = color.b;
            changed = true;
        }
        ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
        changed |= ImGui::DragFloat(i18n::tr("settings.fog.start"), &fog.start, 5.0f, 0.0f, 20000.0f, "%.0f");
        ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
        changed |= ImGui::DragFloat(i18n::tr("settings.fog.end"), &fog.end, 5.0f, 0.0f, 20000.0f, "%.0f");
        // The exponential modes, plain and banded, read a density.
        if (fog.mode == 2 || fog.mode == 3 || fog.mode == 5 || fog.mode == 6) {
            ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.density"), &fog.density, 0.0001f, 0.0f, 1.0f, "%.4f");
        }
        // Volumetric reads a height band and a radial falloff.
        if (fog.mode == 4) {
            ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.height_top"), &fog.heightTop, 1.0f, -5000.0f, 5000.0f,
                                       "%.0f");
            ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.height_bottom"), &fog.heightBottom, 1.0f, -5000.0f,
                                       5000.0f, "%.0f");
            ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.radial_inner"), &fog.radialInner, 5.0f, 0.0f,
                                       20000.0f, "%.0f");
            ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
            changed |= ImGui::DragFloat(i18n::tr("settings.fog.radial_outer"), &fog.radialOuter, 5.0f, 0.0f,
                                       20000.0f, "%.0f");
            ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
            changed |= ImGui::SliderFloat(i18n::tr("settings.fog.radial_strength"), &fog.radialStrength, 0.0f, 1.0f,
                                         "%.2f");
            changed |= ImGui::Checkbox(i18n::tr("settings.fog.everywhere"), &fog.everywhere);
        }
        if (changed) {
            render.SetWorldFog(fog);
            ctx_.MarkSettingsDirty();
        }
    }

    ImGui::Separator();

    // ---- DNC model ----
    // The stock DNC set is small and fully enumerable (dnc_catalog.h), so this
    // is two combos instead of a free-text path: which light rig, and which mod
    // layer to read it from. A path the catalog does not know — an older ini, a
    // hand-edited one — still shows and still loads.
    if (auto* dnc = svc.GetDncService()) {
        const auto catalog = dnc::DncCatalog();
        const std::string current = dnc->UnitMdlPath();
        const i32 sel = dnc::DncCatalogIndexOf(current);
        const dnc::DncVariant variant = dnc::DncVariantOf(current);

        ImGui::SetNextItemWidth(ui::kSettingsWideFieldWidth);
        const std::string preview = (sel >= 0) ? dnc::DncEntryLabel(catalog[sel]) : current;
        if (ImGui::BeginCombo(i18n::tr("settings.general.dnc_model"), preview.c_str())) {
            for (usize i = 0; i < catalog.size(); ++i) {
                const auto& e = catalog[i];
                if (ImGui::Selectable(dnc::DncEntryLabel(e).c_str(), static_cast<i32>(i) == sel)) {
                    dnc->SetUnitMdl(dnc::DncPathForVariant(e.path, variant));
                    ctx_.MarkSettingsDirty();
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
            ctx_.MarkSettingsDirty();
        }

        // Auto follows the scene's art tier; SD/HD/DE pin the path to one layer.
        // Only Lordaeron's legacy target rig is SD-only, so the overlay entries
        // are greyed out rather than hidden.
        const bool hasSd = sel < 0 || catalog[sel].hasSd;
        const bool hasHd = sel < 0 || catalog[sel].hasHd;
        const bool hasDe = sel < 0 || catalog[sel].hasDe;
        const char* variantLabels[] = {i18n::tr("settings.general.dnc_variant_auto"), "SD", "HD", "DE"};
        ImGui::SetNextItemWidth(ui::kSettingsWideFieldWidth);
        if (ImGui::BeginCombo(i18n::tr("settings.general.dnc_variant"), variantLabels[static_cast<usize>(variant)])) {
            const bool enabled[] = {true, hasSd, hasHd, hasDe};
            for (usize i = 0; i < std::size(variantLabels); ++i) {
                ImGui::BeginDisabled(!enabled[i]);
                if (ImGui::Selectable(variantLabels[i], i == static_cast<usize>(variant))) {
                    dnc->SetUnitMdl(dnc::DncPathForVariant(current, static_cast<dnc::DncVariant>(i)));
                    ctx_.MarkSettingsDirty();
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // ---- Depth of Field (HD-only) ----
    // Runs the shipped depthoffield.bls. The pass self-disables until a focal
    // distance > 0 is set, so enabling with a zero distance seeds a sensible
    // default — otherwise the checkbox would appear to do nothing.
    if (ImGui::CollapsingHeader(i18n::tr("settings.dof.header"))) {
        // Focus on the subject: the camera→target distance is the model centre's
        // view-space depth, which is what `linearDepth` carries. The CoC is
        // hyperbolic — (1/focus − 1/depth)·focusScale — so at view-space depths
        // (hundreds) focusScale needs to be tens to hundreds for visible blur.
        const f32 camDist = svc.Scene().Camera().GetDistance();
        const f32 autoFocus = camDist > 0.0f ? camDist : settings::kDofFocusDistance.resetValue;
        if (ui::SettingCheckbox(render, settings::kDofEnabled)) {
            if (render.DofEnabled()) {
                // Seed a visible strength when the current values would blur nothing.
                if (render.DofFocusDistance() <= 0.0f)
                    render.SetDofFocusDistance(autoFocus);
                if (render.DofFocusScale() < settings::kDofVisibleFocusScale)
                    settings::Reset(render, settings::kDofFocusScale);
            }
            ctx_.MarkSettingsDirty();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(i18n::tr("settings.dof.reset"))) {
            render.SetDofFocusDistance(autoFocus);
            settings::Reset(render, settings::kDofFocusScale);
            settings::Reset(render, settings::kDofMaxBlurSize);
            settings::Reset(render, settings::kDofRadiusScale);
            render.SetDofFarFieldOnly(false);
            ctx_.MarkSettingsDirty();
        }
        for (const settings::FloatSetting* level : settings::kDofLevels) {
            if (ui::SettingSlider(render, *level))
                ctx_.MarkSettingsDirty();
        }
        if (ui::SettingCheckbox(render, settings::kDofFarFieldOnly))
            ctx_.MarkSettingsDirty();
    }
}

void SettingsWindow::BuildGeneralPage() {
    renderer::RenderService& svc = ctx_.app.Service();
    RenderSettings& render = svc.Settings();

    // ---- Background colour ----
    {
        tools::Rgb8 background = tools::UnpackBgr(render.BackgroundColorRaw());
        if (ui::ColorEditRgb8(i18n::tr("settings.general.background"), background)) {
            render.SetBackgroundColor(background.r, background.g, background.b);
            ctx_.MarkSettingsDirty();
        }
    }

    // ---- Exposure ----
    if (ui::SettingSlider(render, settings::kExposure))
        ctx_.MarkSettingsDirty();

    // ---- Sound volume ----
    {
        f32 volume = svc.Sound().GetVolume();
        if (ImGui::SliderFloat(i18n::tr("settings.general.snd_volume"), &volume, 0.0f, 1.0f, "%.2f")) {
            svc.Sound().SetVolume(volume);
            ctx_.MarkSettingsDirty();
        }
    }

    // ---- Loop non-looping ----
    {
        bool on = ctx_.app.Playback().LoopNonLooping();
        if (ImGui::Checkbox(i18n::tr("settings.general.loop_nonlooping"), &on)) {
            ctx_.app.Playback().SetLoopNonLooping(on);
            ctx_.MarkSettingsDirty();
        }
    }

    ImGui::Separator();

    // ---- Ambient occlusion (GTAO) ----
    {
        if (ui::SettingCheckbox(render, settings::kAoEnabled))
            ctx_.MarkSettingsDirty();

        i32 quality = static_cast<i32>(render.AoQuality());
        if (quality < 0 || quality >= static_cast<i32>(kAoQualityKeys.size()))
            quality = kDefaultAoQuality;
        ImGui::SetNextItemWidth(ui::kSettingsFieldWidth);
        if (ui::KeyCombo(i18n::tr("settings.general.ao_quality"), quality, kAoQualityKeys)) {
            render.SetAoQuality(static_cast<u32>(quality));
            ctx_.MarkSettingsDirty();
        }

        if (ui::SettingSlider(render, settings::kAoBentBoost))
            ctx_.MarkSettingsDirty();
    }

    // ---- Bloom (HD-only) ----
    // A collapsing header keeps three sliders and a reset button from crowding
    // the page while bloom is off. The reset values mirror the engine's
    // RegisterBloom (threshold 1.0, intensity 1.25, saturation 1.0).
    if (ImGui::CollapsingHeader(i18n::tr("settings.bloom.header"))) {
        if (ui::SettingCheckbox(render, settings::kBloomEnabled))
            ctx_.MarkSettingsDirty();
        ImGui::SameLine();
        if (ImGui::SmallButton(i18n::tr("settings.bloom.reset"))) {
            for (const settings::FloatSetting* level : settings::kBloomLevels)
                settings::Reset(render, *level);
            ctx_.MarkSettingsDirty();
        }
        for (const settings::FloatSetting* level : settings::kBloomLevels) {
            if (ui::SettingSlider(render, *level))
                ctx_.MarkSettingsDirty();
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", i18n::tr("settings.general.startup_note"));
    BuildBackendChoice();
    BuildDeviceChoice();

    if (ui::SettingCheckbox(render, settings::kGraphicsDebug))
        ctx_.MarkSettingsDirty();
}

// What this platform builds, in the order it lists them (backend_names.h):
// Windows D3D11, D3D12, Vulkan and WebGPU; macOS Vulkan and WebGPU; Linux
// Vulkan alone, so the combo shows it and offers nothing else.
void SettingsWindow::BuildBackendChoice() {
    RenderSettings& render = ctx_.app.Service().Settings();
    const std::span<const gfx::GfxApi> apis = tools::kSettingsBackends;
    std::vector<const char*> labels;
    for (const gfx::GfxApi api : apis)
        labels.push_back(tools::BackendNameOf(api).label.data());
    const gfx::GfxApi current = render.DefaultBackend();
    i32 sel = -1;
    for (i32 i = 0; i < static_cast<i32>(apis.size()); ++i) {
        if (apis[i] == current)
            sel = i;
    }
    if (apis.empty()) {
        ImGui::BeginDisabled();
        i32 only = 0;
        const char* vulkan = tools::BackendNameOf(gfx::GfxApi::Vulkan).label.data();
        ImGui::Combo(i18n::tr("settings.general.backend"), &only, &vulkan, 1);
        ImGui::EndDisabled();
        return;
    }
    if (sel < 0) {
        // A stored backend this platform does not list, e.g. Metal carried over
        // from a Mac settings file.
        for (i32 i = 0; i < static_cast<i32>(apis.size()); ++i) {
            if (apis[i] == tools::kSettingsBackendFallback)
                sel = i;
        }
    }
    if (ImGui::Combo(i18n::tr("settings.general.backend"), &sel, labels.data(), static_cast<i32>(labels.size()))) {
        render.SetDefaultBackend(apis[static_cast<usize>(sel)]);
        ctx_.MarkSettingsDirty();
    }
}

void SettingsWindow::BuildDeviceChoice() {
    RenderSettings& render = ctx_.app.Service().Settings();
    const i32 backend = static_cast<i32>(render.DefaultBackend());
    if (backend != devicesBackend_) {
        devices_ = gfx::EnumerateDevices(render.DefaultBackend());
        devicesBackend_ = backend;
    }
    const std::string& cur = render.PreferredDevice();
    const char* preview = cur.empty() ? i18n::tr("settings.general.device_auto") : cur.c_str();
    if (ImGui::BeginCombo(i18n::tr("settings.general.device"), preview)) {
        if (ImGui::Selectable(i18n::tr("settings.general.device_auto"), cur.empty())) {
            render.SetPreferredDevice("");
            ctx_.MarkSettingsDirty();
        }
        for (const auto& name : devices_) {
            const bool isSel = (name == cur);
            if (ImGui::Selectable(name.c_str(), isSel)) {
                render.SetPreferredDevice(name);
                ctx_.MarkSettingsDirty();
            }
            if (isSel)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

} // namespace whiteout::flakes
