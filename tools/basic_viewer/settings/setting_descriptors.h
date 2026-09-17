#pragma once

// ============================================================================
// The scalar settings the viewer persists, one descriptor each: the ini key,
// how the value is read back, the Settings page row that edits it, and what its
// Reset button restores. One descriptor drives the ini load, the ini save, the
// row, and the reset — so a new setting is one row here.
//
// The structured settings (display flags, fog, backend, shadows, the day/night
// rig, the IO overrides) stay hand-written in settings_ini.cpp and settings_io.cpp,
// where a table would be contortion. Device-free and ImGui-free.
// ============================================================================

#include "ini_file.h"
#include "renderer/render_settings.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <span>
#include <string_view>

namespace whiteout::flakes::settings {

using renderer::RenderSettings;

struct BoolSetting {
    /// Empty: edited on a page but not persisted.
    std::string_view iniKey;
    const char* labelKey;
    const char* tipKey;
    bool (RenderSettings::*get)() const;
    void (RenderSettings::*set)(bool);
    /// The physics toggles only ever read back "1" as on; the rest take
    /// ini::ParseBool's spellings. Both write "1" / "0".
    bool strictOne = false;
};

struct FloatSetting {
    std::string_view iniKey;
    const char* labelKey;
    f32 min;
    f32 max;
    /// What the page's Reset button puts back.
    f32 resetValue;
    const char* format;
    /// The row takes the settings field width rather than the window's.
    bool fieldWidth;
    /// Clamp a stored value to [min, max] on load.
    bool clampOnLoad;
    f32 (RenderSettings::*get)() const;
    void (RenderSettings::*set)(f32);
};

// ---- Debug ▸ Physics ----
inline constexpr BoolSetting kShowPhysicsDynamic = {"ShowPhysicsDynamic", "menu.debug.physics.dynamic", nullptr,
                                                    &RenderSettings::ShowPhysicsDynamic,
                                                    &RenderSettings::SetShowPhysicsDynamic, true};
inline constexpr BoolSetting kShowPhysicsKinematic = {"ShowPhysicsKinematic", "menu.debug.physics.kinematic", nullptr,
                                                      &RenderSettings::ShowPhysicsKinematic,
                                                      &RenderSettings::SetShowPhysicsKinematic, true};
inline constexpr BoolSetting kShowPhysicsStatic = {"ShowPhysicsStatic", "menu.debug.physics.static", nullptr,
                                                   &RenderSettings::ShowPhysicsStatic,
                                                   &RenderSettings::SetShowPhysicsStatic, true};
inline constexpr BoolSetting kShowPhysicsCloth = {"ShowPhysicsCloth", "menu.debug.physics.cloth", nullptr,
                                                  &RenderSettings::ShowPhysicsCloth,
                                                  &RenderSettings::SetShowPhysicsCloth, true};
/// Not an overlay: this one changes the picture.
inline constexpr BoolSetting kClothDeform = {"ClothDeform", "menu.debug.physics.deform", nullptr,
                                             &RenderSettings::ClothDeform, &RenderSettings::SetClothDeform, true};
/// The overlays, in menu order.
inline constexpr std::array<const BoolSetting*, 4> kPhysicsOverlays = {&kShowPhysicsDynamic, &kShowPhysicsKinematic,
                                                                      &kShowPhysicsStatic, &kShowPhysicsCloth};

// ---- StarCraft II page ----
inline constexpr BoolSetting kPhysicsSubstepping = {
    "PhysicsSubstepping", "settings.general.physics_substep", "settings.general.physics_substep.tip",
    &RenderSettings::PhysicsSubstepping, &RenderSettings::SetPhysicsSubstepping, true};

// ---- General page ----
inline constexpr FloatSetting kExposure = {
    "Exposure", "settings.general.exposure", 0.0f, 3.0f, 1.0f, "%.2f", false, true,
    &RenderSettings::GetTonemapExposure, &RenderSettings::SetTonemapExposure};
inline constexpr BoolSetting kGraphicsDebug = {"GraphicsDebug", "settings.general.graphics_debug", nullptr,
                                               &RenderSettings::GraphicsDebug, &RenderSettings::SetGraphicsDebug};

// ---- World of Warcraft page ----
inline constexpr BoolSetting kM2LazyAnimations = {"M2LazyAnimations", "settings.general.m2_lazy_anim",
                                                  "settings.general.m2_lazy_anim.tip",
                                                  &RenderSettings::M2LazyAnimations,
                                                  &RenderSettings::SetM2LazyAnimations};
inline constexpr BoolSetting kM2DistanceSortGeometry = {"M2DistanceSortGeometry", "settings.general.m2_dist_sort",
                                                        "settings.general.m2_dist_sort.tip",
                                                        &RenderSettings::M2DistanceSortGeometry,
                                                        &RenderSettings::SetM2DistanceSortGeometry};
inline constexpr BoolSetting kM2ModelLights = {"M2ModelLights", "settings.general.m2_model_lights",
                                               "settings.general.m2_model_lights.tip", &RenderSettings::M2ModelLights,
                                               &RenderSettings::SetM2ModelLights};
/// The page's rows, in order.
inline constexpr std::array<const BoolSetting*, 3> kWowPage = {&kM2LazyAnimations, &kM2DistanceSortGeometry,
                                                              &kM2ModelLights};

// ---- Diablo III page ----
/// Not in the ini: it has only ever lasted the session.
inline constexpr BoolSetting kD3LazyAnimations = {{}, "settings.general.d3_lazy_anim",
                                                  "settings.general.d3_lazy_anim.tip",
                                                  &RenderSettings::D3LazyAnimations,
                                                  &RenderSettings::SetD3LazyAnimations};

// ---- Ambient occlusion ----
inline constexpr BoolSetting kAoEnabled = {"AoEnabled", "settings.general.ao", nullptr, &RenderSettings::AoEnabled,
                                           &RenderSettings::SetAoEnabled};
inline constexpr FloatSetting kAoBentBoost = {
    "AoBentBoost", "settings.general.ao_bent_boost", 0.0f, 0.5f, 0.0f, "%.3f", true, false,
    &RenderSettings::AoBentBoost, &RenderSettings::SetAoBentBoost};

// ---- Bloom (HD only) ----
// The reset values mirror the engine's RegisterBloom.
inline constexpr BoolSetting kBloomEnabled = {"BloomEnabled", "settings.bloom.enabled", nullptr,
                                              &RenderSettings::BloomEnabled, &RenderSettings::SetBloomEnabled};
inline constexpr FloatSetting kBloomThreshold = {"BloomThreshold", "settings.bloom.threshold", 0.0f, 4.0f, 1.0f,
                                                 "%.2f", true, false, &RenderSettings::BloomThreshold,
                                                 &RenderSettings::SetBloomThreshold};
inline constexpr FloatSetting kBloomIntensity = {"BloomIntensity", "settings.bloom.intensity", 0.0f, 4.0f, 1.25f,
                                                 "%.2f", true, false, &RenderSettings::BloomIntensity,
                                                 &RenderSettings::SetBloomIntensity};
inline constexpr FloatSetting kBloomSaturation = {"BloomSaturation", "settings.bloom.saturation", 0.0f, 4.0f, 1.0f,
                                                  "%.2f", true, false, &RenderSettings::BloomSaturation,
                                                  &RenderSettings::SetBloomSaturation};
/// The three sliders, in page order.
inline constexpr std::array<const FloatSetting*, 3> kBloomLevels = {&kBloomThreshold, &kBloomIntensity,
                                                                   &kBloomSaturation};

// ---- Depth of field (HD only) ----
inline constexpr BoolSetting kDofEnabled = {"DofEnabled", "settings.dof.enabled", nullptr,
                                            &RenderSettings::DofEnabled, &RenderSettings::SetDofEnabled};
/// Its reset is the camera's distance to the model, so the value here is the
/// fallback when the camera has none.
inline constexpr FloatSetting kDofFocusDistance = {
    "DofFocusDistance", "settings.dof.focus_dist", 0.0f, 3000.0f, 600.0f, "%.0f", true, false,
    &RenderSettings::DofFocusDistance, &RenderSettings::SetDofFocusDistance};
/// The CoC is hyperbolic — (1/focus − 1/depth)·focusScale — so at view-space
/// depths (hundreds) the scale needs tens to hundreds for visible blur.
inline constexpr FloatSetting kDofFocusScale = {
    "DofFocusScale", "settings.dof.focus_scale", 0.0f, 200.0f, 50.0f, "%.1f", true, false,
    &RenderSettings::DofFocusScale, &RenderSettings::SetDofFocusScale};
/// Below this a freshly enabled depth of field blurs nothing visible, so
/// enabling it seeds the reset scale.
inline constexpr f32 kDofVisibleFocusScale = 5.0f;
inline constexpr FloatSetting kDofMaxBlurSize = {
    "DofMaxBlurSize", "settings.dof.max_blur", 1.0f, 40.0f, 20.0f, "%.1f", true, false,
    &RenderSettings::DofMaxBlurSize, &RenderSettings::SetDofMaxBlurSize};
inline constexpr FloatSetting kDofRadiusScale = {
    "DofRadiusScale", "settings.dof.sample_density", 0.25f, 4.0f, 1.0f, "%.2f", true, false,
    &RenderSettings::DofRadiusScale, &RenderSettings::SetDofRadiusScale};
/// The four sliders, in page order.
inline constexpr std::array<const FloatSetting*, 4> kDofLevels = {&kDofFocusDistance, &kDofFocusScale,
                                                                 &kDofMaxBlurSize, &kDofRadiusScale};
inline constexpr BoolSetting kDofFarFieldOnly = {"DofFarFieldOnly", "settings.dof.far_field_only", nullptr,
                                                 &RenderSettings::DofFarFieldOnly,
                                                 &RenderSettings::SetDofFarFieldOnly};

// ---- Load and save ----
// The ini section every descriptor above lives in.
inline constexpr std::string_view kDisplaySection = "Display";

void Load(RenderSettings& settings, const ini::IniMap& ini, const BoolSetting& setting);
void Load(RenderSettings& settings, const ini::IniMap& ini, const FloatSetting& setting);
void Save(const RenderSettings& settings, ini::IniMap& ini, const BoolSetting& setting);
void Save(const RenderSettings& settings, ini::IniMap& ini, const FloatSetting& setting);

void Load(RenderSettings& settings, const ini::IniMap& ini, std::span<const BoolSetting* const> group);
void Load(RenderSettings& settings, const ini::IniMap& ini, std::span<const FloatSetting* const> group);
void Save(const RenderSettings& settings, ini::IniMap& ini, std::span<const BoolSetting* const> group);
void Save(const RenderSettings& settings, ini::IniMap& ini, std::span<const FloatSetting* const> group);

/// Put @p setting back to its reset value.
void Reset(RenderSettings& settings, const FloatSetting& setting);

} // namespace whiteout::flakes::settings
