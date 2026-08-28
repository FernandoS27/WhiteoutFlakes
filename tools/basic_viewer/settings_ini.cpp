// ============================================================================
// The display half of WhiteoutFlakes.ini — everything that lives on
// RenderService. The content-provider half is in settings_io.cpp, kept apart
// so that reading it does not link the renderer.
// ============================================================================

#include "settings_ini.h"

#include "ini_file.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/render_service.h"
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

    if (auto* s = ini.Get(KeyOf("GraphicsDebug"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetGraphicsDebug(v);
    }
    if (auto* s = ini.Get(KeyOf("DefaultBackend"))) {
        using gfx::GfxApi;
        if (*s == "d3d11" || *s == "D3D11")
            service.Settings().SetDefaultBackend(GfxApi::D3D11);
        else if (*s == "d3d12" || *s == "D3D12")
            service.Settings().SetDefaultBackend(GfxApi::D3D12);
        else if (*s == "vulkan" || *s == "Vulkan" || *s == "VULKAN")
            service.Settings().SetDefaultBackend(GfxApi::Vulkan);
        else if (*s == "webgpu" || *s == "WebGPU" || *s == "WEBGPU")
            service.Settings().SetDefaultBackend(GfxApi::WebGPU);
#if defined(__APPLE__)
        // Metal is Apple-only; ignore a stale / hand-edited "metal" value on
        // other platforms so it can't select an unavailable backend (the UI
        // doesn't offer it there either).
        else if (*s == "metal" || *s == "Metal" || *s == "METAL")
            service.Settings().SetDefaultBackend(GfxApi::Metal);
#endif
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
            const u32 c = static_cast<u32>(v);
            service.Settings().SetBackgroundColor(static_cast<u8>(c & 0xFF),
                                                  static_cast<u8>((c >> 8) & 0xFF),
                                                  static_cast<u8>((c >> 16) & 0xFF));
        }
    }
    if (auto* s = ini.Get(KeyOf("Exposure"))) {
        f32 v = 0;
        if (ParseFloat(*s, v))
            service.Settings().SetTonemapExposure(std::clamp(v, 0.0f, 3.0f));
    }
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

    {
        auto loadPhysFlag = [&](const char* key, void (RenderSettings::*set)(bool)) {
            if (auto* s = ini.Get(KeyOf(key)))
                (service.Settings().*set)(*s == "1");
        };
        loadPhysFlag("ShowPhysicsDynamic", &RenderSettings::SetShowPhysicsDynamic);
        loadPhysFlag("ShowPhysicsKinematic", &RenderSettings::SetShowPhysicsKinematic);
        loadPhysFlag("ShowPhysicsStatic", &RenderSettings::SetShowPhysicsStatic);
        loadPhysFlag("ShowPhysicsCloth", &RenderSettings::SetShowPhysicsCloth);
        loadPhysFlag("ClothDeform", &RenderSettings::SetClothDeform);
        loadPhysFlag("PhysicsSubstepping", &RenderSettings::SetPhysicsSubstepping);
    }

    if (auto* s = ini.Get(KeyOf("LightingMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 2)
            service.Settings().SetLightingMode(static_cast<LightingMode>(v));
    }
    if (auto* s = ini.Get(KeyOf("HdDebugMode"))) {
        i32 v = 0;
        if (ParseInt(*s, v) && v >= 0 && v <= 7)
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
    if (auto* s = ini.Get(KeyOf("M2LazyAnimations"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetM2LazyAnimations(v);
    }
    if (auto* s = ini.Get(KeyOf("M2DistanceSortGeometry"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetM2DistanceSortGeometry(v);
    }
    if (auto* s = ini.Get(KeyOf("M2ModelLights"))) {
        bool v = true;
        if (ParseBool(*s, v))
            service.Settings().SetM2ModelLights(v);
    }
    if (auto* s = ini.Get(KeyOf("AoEnabled"))) {
        bool v = true;
        if (ParseBool(*s, v))
            service.Settings().SetAoEnabled(v);
    }
    if (auto* s = ini.Get(KeyOf("AoQuality"))) {
        const i32 v = std::atoi(s->c_str());
        if (v >= 0 && v <= 2)
            service.Settings().SetAoQuality(static_cast<u32>(v));
    }
    if (auto* s = ini.Get(KeyOf("AoBentBoost"))) {
        f32 v = 0.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetAoBentBoost(v);
    }
    if (auto* s = ini.Get(KeyOf("BloomEnabled"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetBloomEnabled(v);
    }
    if (auto* s = ini.Get(KeyOf("BloomThreshold"))) {
        f32 v = 1.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetBloomThreshold(v);
    }
    if (auto* s = ini.Get(KeyOf("BloomIntensity"))) {
        f32 v = 1.25f;
        if (ParseFloat(*s, v))
            service.Settings().SetBloomIntensity(v);
    }
    if (auto* s = ini.Get(KeyOf("BloomSaturation"))) {
        f32 v = 1.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetBloomSaturation(v);
    }
    if (auto* s = ini.Get(KeyOf("DofEnabled"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetDofEnabled(v);
    }
    if (auto* s = ini.Get(KeyOf("DofFocusDistance"))) {
        f32 v = 0.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetDofFocusDistance(v);
    }
    if (auto* s = ini.Get(KeyOf("DofFocusScale"))) {
        f32 v = 1.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetDofFocusScale(v);
    }
    if (auto* s = ini.Get(KeyOf("DofMaxBlurSize"))) {
        f32 v = 10.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetDofMaxBlurSize(v);
    }
    if (auto* s = ini.Get(KeyOf("DofRadiusScale"))) {
        f32 v = 1.0f;
        if (ParseFloat(*s, v))
            service.Settings().SetDofRadiusScale(v);
    }
    if (auto* s = ini.Get(KeyOf("DofFarFieldOnly"))) {
        bool v = false;
        if (ParseBool(*s, v))
            service.Settings().SetDofFarFieldOnly(v);
    }
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
    ini.Set(KeyOf("Exposure"), FloatToString(service.Settings().GetTonemapExposure()));
    ini.Set(KeyOf("SoundVolume"), FloatToString(service.Sound().GetVolume()));
    ini.Set(KeyOf("LoopNonLooping"), loopNonLoopingPolicy ? "1" : "0");
    ini.Set(KeyOf("ReforgedGraphics"), forceHd ? "1" : "0");
    ini.Set(KeyOf("Language"), languageCode);
    ini.Set(KeyOf("GraphicsDebug"), service.Settings().GraphicsDebug() ? "1" : "0");

    {
        const char* name = "d3d12";
        switch (service.Settings().DefaultBackend()) {
        case gfx::GfxApi::D3D11:
            name = "d3d11";
            break;
        case gfx::GfxApi::D3D12:
            name = "d3d12";
            break;
        case gfx::GfxApi::Vulkan:
            name = "vulkan";
            break;
        case gfx::GfxApi::WebGPU:
            name = "webgpu";
            break;
        case gfx::GfxApi::Metal:
            name = "metal";
            break;
        }
        ini.Set(KeyOf("DefaultBackend"), name);
    }
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
        saveFlag("ShowPhysicsDynamic", service.Settings().ShowPhysicsDynamic());
        saveFlag("ShowPhysicsKinematic", service.Settings().ShowPhysicsKinematic());
        saveFlag("ShowPhysicsStatic", service.Settings().ShowPhysicsStatic());
        saveFlag("ShowPhysicsCloth", service.Settings().ShowPhysicsCloth());
        saveFlag("ClothDeform", service.Settings().ClothDeform());
        saveFlag("PhysicsSubstepping", service.Settings().PhysicsSubstepping());
    }
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
    ini.Set(KeyOf("M2LazyAnimations"), service.Settings().M2LazyAnimations() ? "1" : "0");
    ini.Set(KeyOf("M2DistanceSortGeometry"),
            service.Settings().M2DistanceSortGeometry() ? "1" : "0");
    ini.Set(KeyOf("M2ModelLights"), service.Settings().M2ModelLights() ? "1" : "0");
    ini.Set(KeyOf("AoEnabled"), service.Settings().AoEnabled() ? "1" : "0");
    ini.Set(KeyOf("AoQuality"), ToString(static_cast<i32>(service.Settings().AoQuality())));
    ini.Set(KeyOf("AoBentBoost"), FloatToString(service.Settings().AoBentBoost()));
    ini.Set(KeyOf("BloomEnabled"), service.Settings().BloomEnabled() ? "1" : "0");
    ini.Set(KeyOf("BloomThreshold"), FloatToString(service.Settings().BloomThreshold()));
    ini.Set(KeyOf("BloomIntensity"), FloatToString(service.Settings().BloomIntensity()));
    ini.Set(KeyOf("BloomSaturation"), FloatToString(service.Settings().BloomSaturation()));
    ini.Set(KeyOf("DofEnabled"), service.Settings().DofEnabled() ? "1" : "0");
    ini.Set(KeyOf("DofFocusDistance"), FloatToString(service.Settings().DofFocusDistance()));
    ini.Set(KeyOf("DofFocusScale"), FloatToString(service.Settings().DofFocusScale()));
    ini.Set(KeyOf("DofMaxBlurSize"), FloatToString(service.Settings().DofMaxBlurSize()));
    ini.Set(KeyOf("DofRadiusScale"), FloatToString(service.Settings().DofRadiusScale()));
    ini.Set(KeyOf("DofFarFieldOnly"), service.Settings().DofFarFieldOnly() ? "1" : "0");
    if (const auto* dnc = service.GetDncService()) {
        ini.Set(KeyOf("TimeOfDay"), FloatToString(dnc->GetTimeOfDay()));
        ini.Set(KeyOf("AnimateTod"), dnc->GetTodScale() > 0.0f ? "1" : "0");
        ini.Set(KeyOf("DncModel"), dnc->UnitMdlPath());
    }

    ini.Save(path);
}

} // namespace whiteout::flakes
