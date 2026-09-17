// ============================================================================
// The display half of WhiteoutFlakes.ini — everything that lives on
// RenderService. The content-provider half is in settings_io.cpp, kept apart
// so that reading it does not link the renderer.
// ============================================================================

#include "settings_ini.h"

#include "backend_names.h"
#include "color_pack.h"
#include "ini_file.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/render_service.h"
#include "settings/setting_descriptors.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::assets;

using ini::FloatToString;
using ini::IniMap;
using ini::ParseBool;
using ini::ParseFloat;
using ini::ParseInt;
using ini::ToString;

namespace {

namespace fs = std::filesystem;

constexpr const char* kSection = "Display";

std::string KeyOf(const char* k) {
    return std::string(kSection) + "." + k;
}

} // namespace

void LoadStartupSettingsFromIni(RenderService& service) {
    IniMap ini;
    ini.Load(SettingsIniPath());

    settings::Load(service.Settings(), ini, settings::kGraphicsDebug);
    // A name this platform does not build (Metal off Apple) is ignored rather
    // than selecting a backend that cannot start.
    if (auto* s = ini.Get(KeyOf("DefaultBackend"))) {
        if (const auto api = tools::BackendFromIniName(*s))
            service.Settings().SetDefaultBackend(*api);
    }
    if (auto* s = ini.Get(KeyOf("PreferredDevice")); s && !s->empty()) {
        service.Settings().SetPreferredDevice(*s);
    }
}

void LoadSettingsIni(RenderService& service, bool& loopNonLoopingPolicy, bool& forceHd,
                     std::string& languageCode) {
    IniMap ini;
    ini.Load(SettingsIniPath());

    if (auto* s = ini.Get(KeyOf("BackgroundColor"))) {
        i32 v = 0;
        if (ParseInt(*s, v)) {
            const tools::Rgb8 c = tools::UnpackBgr(static_cast<u32>(v));
            service.Settings().SetBackgroundColor(c.r, c.g, c.b);
        }
    }
    settings::Load(service.Settings(), ini, settings::kExposure);
    if (auto* s = ini.Get(KeyOf("SoundVolume"))) {
        f32 v = 0;
        if (ParseFloat(*s, v))
            service.Sound().SetVolume(std::clamp(v, 0.0f, 1.0f));
    }
    if (auto* s = ini.Get(KeyOf("LoopNonLooping"))) {
        bool v = false;
        if (ParseBool(*s, v))
            loopNonLoopingPolicy = v;
    }
    if (auto* s = ini.Get(KeyOf("Language")); s && !s->empty())
        languageCode = *s;
    if (auto* s = ini.Get(KeyOf("ReforgedGraphics"))) {
        bool v = false;
        if (ParseBool(*s, v))
            forceHd = v;
    }
    // Warcraft III's art tier, spelled the way the game's own launch flag
    // does: 0 Classic, 1 Reforged, 2 Definitive. Absent or out of range means
    // "follow the render mode", which is both the default and what every
    // settings file written before 3.0.0 says by omission.
    if (auto* s = ini.Get(KeyOf("Wc3ArtTier")); s && !s->empty()) {
        const int v = std::atoi(s->c_str());
        if (v >= 0 && v <= static_cast<int>(Wc3ArtTier::Definitive))
            service.Settings().SetArtTier(static_cast<Wc3ArtTier>(v));
    }

    {
        DisplayFlags df = service.Settings().GetDisplayFlags();
        bool dirty = false;
        auto loadFlag = [&](const char* key, bool& field) {
            if (auto* s = ini.Get(KeyOf(key))) {
                bool v = false;
                if (ParseBool(*s, v)) {
                    field = v;
                    dirty = true;
                }
            }
        };
        loadFlag("ShowGrid", df.showGrid);
        loadFlag("ShowParticles", df.showParticles);
        loadFlag("ShowRibbons", df.showRibbons);
        loadFlag("ShowEvents", df.showEvents);
        loadFlag("ShowCollisions", df.showCollisions);
        loadFlag("ShowLights", df.showLights);
        if (dirty)
            service.Settings().SetDisplayFlags(df);
    }

    settings::Load(service.Settings(), ini, settings::kPhysicsOverlays);
    settings::Load(service.Settings(), ini, settings::kClothDeform);
    settings::Load(service.Settings(), ini, settings::kPhysicsSubstepping);

    if (auto* s = ini.Get(KeyOf("LightingMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 2)
            service.Settings().SetLightingMode(static_cast<LightingMode>(v));
    }
    // The key predates DebugView and keeps its name: the values are the same
    // integers, and one that is not a view turns the debug view off.
    if (auto* s = ini.Get(KeyOf("HdDebugMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v))
            service.Settings().SetHdDebugMode(v);
    }
    if (auto* s = ini.Get(KeyOf("LodOverride"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= -1 && v <= 3)
            service.Settings().SetLodOverride(v);
    }
    if (auto* s = ini.Get(KeyOf("Tileset"))) {
        i32 v = 0;
        const i32 n = static_cast<i32>(io::Tileset::Count);
        if (ParseInt(*s, v) && v >= 0 && v < n)
            service.Replaceables().SetTileset(static_cast<io::Tileset>(v));
    }
    if (auto* s = ini.Get(KeyOf("IblMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= static_cast<i32>(IblMode::Sunset) &&
            static_cast<IblMode>(v) != service.Settings().GetIblMode()) {
            service.Settings().SetIblMode(static_cast<IblMode>(v));
        }
    }
    if (auto* s = ini.Get(KeyOf("ShadowCascades"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 3) {
            if (auto* shadow = service.GetShadowService()) {
                shadow::ShadowParams p = shadow->Params();
                p.enabled = (v > 0);
                p.cascadeCount = (v > 0) ? v : 1;
                shadow->SetParams(p);
            }
        }
    }
    {
        RenderSettings::WorldFog fog = service.Settings().GetWorldFog();
        if (auto* s = ini.Get(KeyOf("FogMode"))) {
            i32 v = 0;
            if (ParseInt(*s, v) && v >= 0 && v <= 6)
                fog.mode = v;
        }
        if (auto* s = ini.Get(KeyOf("FogColor"))) {
            i32 v = 0;
            if (ParseInt(*s, v) && v >= 0 && v <= 0xFFFFFF) {
                const tools::Rgb8 c = tools::UnpackRgb(static_cast<u32>(v));
                fog.color[0] = c.r;
                fog.color[1] = c.g;
                fog.color[2] = c.b;
            }
        }
        auto loadFogFloat = [&](const char* key, f32& out) {
            if (auto* s = ini.Get(KeyOf(key))) {
                f32 v = 0.0f;
                if (ParseFloat(*s, v))
                    out = v;
            }
        };
        loadFogFloat("FogStart", fog.start);
        loadFogFloat("FogEnd", fog.end);
        loadFogFloat("FogDensity", fog.density);
        loadFogFloat("FogHeightTop", fog.heightTop);
        loadFogFloat("FogHeightBottom", fog.heightBottom);
        loadFogFloat("FogRadialInner", fog.radialInner);
        loadFogFloat("FogRadialOuter", fog.radialOuter);
        loadFogFloat("FogRadialStrength", fog.radialStrength);
        if (auto* s = ini.Get(KeyOf("FogEverywhere"))) {
            bool v = false;
            if (ParseBool(*s, v))
                fog.everywhere = v;
        }
        service.Settings().SetWorldFog(fog);
    }
    settings::Load(service.Settings(), ini, settings::kWowPage);
    settings::Load(service.Settings(), ini, settings::kAoEnabled);
    if (auto* s = ini.Get(KeyOf("AoQuality"))) {
        const i32 v = std::atoi(s->c_str());
        if (v >= 0 && v <= 2)
            service.Settings().SetAoQuality(static_cast<u32>(v));
    }
    settings::Load(service.Settings(), ini, settings::kAoBentBoost);
    settings::Load(service.Settings(), ini, settings::kBloomEnabled);
    settings::Load(service.Settings(), ini, settings::kBloomLevels);
    settings::Load(service.Settings(), ini, settings::kDofEnabled);
    settings::Load(service.Settings(), ini, settings::kDofLevels);
    settings::Load(service.Settings(), ini, settings::kDofFarFieldOnly);
    if (auto* dnc = service.GetDncService()) {
        if (auto* s = ini.Get(KeyOf("TimeOfDay"))) {
            f32 v = 0;
            if (ParseFloat(*s, v))
                dnc->SetTimeOfDay(v);
        }
        if (auto* s = ini.Get(KeyOf("AnimateTod"))) {
            bool v = false;
            if (ParseBool(*s, v))
                dnc->SetTodScale(v ? 1.0f : 0.0f);
        }
        if (auto* s = ini.Get(KeyOf("DncModel")); s && !s->empty() && *s != dnc->UnitMdlPath()) {
            dnc->SetUnitMdl(*s);
        }
    }
}

void SaveSettingsIni(const RenderService& service, bool loopNonLoopingPolicy, bool forceHd,
                     const std::string& languageCode) {
    const fs::path path = SettingsIniPath();
    // Round-trip: load existing values first so unrelated keys (older
    // settings, future additions) don't get nuked when a single setting
    // changes.
    IniMap ini;
    ini.Load(path);

    {
        // BackgroundColor stays in `0xRRGGBB` hex via snprintf — printf's
        // %x is locale-neutral (no decimals involved).
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X",
                      static_cast<u32>(service.Settings().BackgroundColorRaw()));
        ini.Set(KeyOf("BackgroundColor"), buf);
    }
    settings::Save(service.Settings(), ini, settings::kExposure);
    ini.Set(KeyOf("SoundVolume"), FloatToString(service.Sound().GetVolume()));
    ini.Set(KeyOf("LoopNonLooping"), loopNonLoopingPolicy ? "1" : "0");
    ini.Set(KeyOf("ReforgedGraphics"), forceHd ? "1" : "0");
    // Written only when pinned: an absent key is the honest spelling of
    // "follow the render mode", and writing a number for it would turn the
    // default into a choice nobody made.
    if (const auto tier = service.Settings().GetArtTier())
        ini.Set(KeyOf("Wc3ArtTier"), std::to_string(static_cast<int>(*tier)));
    else
        ini.Remove(KeyOf("Wc3ArtTier"));
    ini.Set(KeyOf("Language"), languageCode);
    settings::Save(service.Settings(), ini, settings::kGraphicsDebug);

    ini.Set(KeyOf("DefaultBackend"),
            std::string(tools::BackendNameOf(service.Settings().DefaultBackend()).iniName));
    ini.Set(KeyOf("PreferredDevice"), service.Settings().PreferredDevice());

    {
        const DisplayFlags df = service.Settings().GetDisplayFlags();
        auto saveFlag = [&](const char* key, bool v) { ini.Set(KeyOf(key), v ? "1" : "0"); };
        saveFlag("ShowGrid", df.showGrid);
        saveFlag("ShowParticles", df.showParticles);
        saveFlag("ShowRibbons", df.showRibbons);
        saveFlag("ShowEvents", df.showEvents);
        saveFlag("ShowCollisions", df.showCollisions);
        saveFlag("ShowLights", df.showLights);
    }
    settings::Save(service.Settings(), ini, settings::kPhysicsOverlays);
    settings::Save(service.Settings(), ini, settings::kClothDeform);
    settings::Save(service.Settings(), ini, settings::kPhysicsSubstepping);
    ini.Set(KeyOf("LightingMode"),
            ToString(static_cast<u32>(service.Settings().GetLightingMode())));
    ini.Set(KeyOf("HdDebugMode"), ToString(service.Settings().HdDebugMode()));
    ini.Set(KeyOf("LodOverride"), ToString(service.Settings().LodOverride()));
    ini.Set(KeyOf("Tileset"), ToString(static_cast<u32>(io::GetCurrentTileset())));
    ini.Set(KeyOf("IblMode"), ToString(static_cast<u32>(service.Settings().GetIblMode())));
    if (const auto* shadow = service.GetShadowService()) {
        const i32 cascades =
            shadow->IsEnabled() ? std::clamp(shadow->Params().cascadeCount, 1, 3) : 0;
        ini.Set(KeyOf("ShadowCascades"), ToString(cascades));
    }
    {
        const RenderSettings::WorldFog& fog = service.Settings().GetWorldFog();
        ini.Set(KeyOf("FogMode"), ToString(fog.mode));
        ini.Set(KeyOf("FogColor"), ToString(static_cast<i32>(tools::PackRgb(
                                       {fog.color[0], fog.color[1], fog.color[2]}))));
        ini.Set(KeyOf("FogStart"), FloatToString(fog.start));
        ini.Set(KeyOf("FogEnd"), FloatToString(fog.end));
        ini.Set(KeyOf("FogDensity"), FloatToString(fog.density));
        ini.Set(KeyOf("FogHeightTop"), FloatToString(fog.heightTop));
        ini.Set(KeyOf("FogHeightBottom"), FloatToString(fog.heightBottom));
        ini.Set(KeyOf("FogRadialInner"), FloatToString(fog.radialInner));
        ini.Set(KeyOf("FogRadialOuter"), FloatToString(fog.radialOuter));
        ini.Set(KeyOf("FogRadialStrength"), FloatToString(fog.radialStrength));
        ini.Set(KeyOf("FogEverywhere"), fog.everywhere ? "1" : "0");
    }
    settings::Save(service.Settings(), ini, settings::kWowPage);
    settings::Save(service.Settings(), ini, settings::kAoEnabled);
    ini.Set(KeyOf("AoQuality"), ToString(static_cast<i32>(service.Settings().AoQuality())));
    settings::Save(service.Settings(), ini, settings::kAoBentBoost);
    settings::Save(service.Settings(), ini, settings::kBloomEnabled);
    settings::Save(service.Settings(), ini, settings::kBloomLevels);
    settings::Save(service.Settings(), ini, settings::kDofEnabled);
    settings::Save(service.Settings(), ini, settings::kDofLevels);
    settings::Save(service.Settings(), ini, settings::kDofFarFieldOnly);
    if (const auto* dnc = service.GetDncService()) {
        ini.Set(KeyOf("TimeOfDay"), FloatToString(dnc->GetTimeOfDay()));
        ini.Set(KeyOf("AnimateTod"), dnc->GetTodScale() > 0.0f ? "1" : "0");
        ini.Set(KeyOf("DncModel"), dnc->UnitMdlPath());
    }

    ini.Save(path);
}

} // namespace whiteout::flakes
