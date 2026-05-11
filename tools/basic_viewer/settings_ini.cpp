#include "settings_ini.h"

#include "whiteout/flakes/types.h"
#include "renderer/render_service.h"
#include "renderer/assets/replaceable_texture_manager.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::assets;

namespace {

std::wstring SettingsIniPath() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"WhiteoutFlakes.ini";
    std::filesystem::path p(exePath);
    p.replace_filename(L"WhiteoutFlakes.ini");
    return p.wstring();
}

constexpr const wchar_t* kSection = L"Display";

}

void LoadStartupSettingsFromIni(RenderService& service) {
    const std::wstring iniPath = SettingsIniPath();
    {
        const i32 v = ::GetPrivateProfileIntW(kSection, L"GraphicsDebug",
                                               -1, iniPath.c_str());
        if (v == 0 || v == 1) service.Settings().SetGraphicsDebug(v != 0);
    }
    {
        wchar_t buf[16] = {};
        ::GetPrivateProfileStringW(kSection, L"DefaultBackend", L"",
                                   buf, 16, iniPath.c_str());
        if (buf[0]) {
            using gfx::GfxApi;
            if      (_wcsicmp(buf, L"d3d11")  == 0)
                service.Settings().SetDefaultBackend(GfxApi::D3D11);
            else if (_wcsicmp(buf, L"d3d12")  == 0)
                service.Settings().SetDefaultBackend(GfxApi::D3D12);
            else if (_wcsicmp(buf, L"vulkan") == 0)
                service.Settings().SetDefaultBackend(GfxApi::Vulkan);
        }
    }
    {
        // PreferredDevice is a verbatim adapter name (UTF-8 once
        // converted from the INI's UTF-16 storage). Empty = "let the
        // backend pick" — the same behaviour as before this knob
        // existed.
        wchar_t buf[256] = {};
        ::GetPrivateProfileStringW(kSection, L"PreferredDevice", L"",
                                   buf, 256, iniPath.c_str());
        if (buf[0]) {
            const i32 len = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                std::string utf8(len - 1, '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                      utf8.data(), len, nullptr, nullptr);
                service.Settings().SetPreferredDevice(std::move(utf8));
            }
        }
    }
}

void LoadSettingsIni(RenderService& service, bool& loopNonLoopingPolicy) {
    const std::wstring iniPath = SettingsIniPath();

    {
        wchar_t buf[32] = {};
        ::GetPrivateProfileStringW(kSection, L"BackgroundColor", L"",
                                   buf, 32, iniPath.c_str());
        if (buf[0]) {
            wchar_t* endptr = nullptr;
            const unsigned long val = std::wcstoul(buf, &endptr, 16);
            if (endptr != buf) {
                const u32 v = static_cast<u32>(val);

                service.Settings().SetBackgroundColor(
                    static_cast<u8>(v        & 0xFF),
                    static_cast<u8>((v >> 8) & 0xFF),
                    static_cast<u8>((v >> 16) & 0xFF));
            }
        }
    }

    {
        wchar_t buf[32] = {};
        ::GetPrivateProfileStringW(kSection, L"Exposure", L"",
                                   buf, 32, iniPath.c_str());
        if (buf[0]) {
            wchar_t* endptr = nullptr;
            const f64 val = std::wcstod(buf, &endptr);
            if (endptr != buf) {

                f32 clamped = static_cast<f32>(val);
                if (clamped < 0.0f) clamped = 0.0f;
                if (clamped > 3.0f) clamped = 3.0f;
                service.Settings().SetTonemapExposure(clamped);
            }
        }
    }

    {
        wchar_t buf[32] = {};
        ::GetPrivateProfileStringW(kSection, L"SoundVolume", L"",
                                   buf, 32, iniPath.c_str());
        if (buf[0]) {
            wchar_t* endptr = nullptr;
            const f64 val = std::wcstod(buf, &endptr);
            if (endptr != buf) {
                f32 clamped = static_cast<f32>(val);
                if (clamped < 0.0f) clamped = 0.0f;
                if (clamped > 1.0f) clamped = 1.0f;
                service.Sound().SetVolume(clamped);
            }
        }
    }

    {
        const i32 v = ::GetPrivateProfileIntW(kSection, L"LoopNonLooping",
                                              -1, iniPath.c_str());
        if (v == 0 || v == 1) loopNonLoopingPolicy = (v != 0);
    }

    {
        DisplayFlags df = service.Settings().GetDisplayFlags();
        bool dirty = false;
        auto loadFlag = [&](const wchar_t* key, bool& field) {
            const i32 v = ::GetPrivateProfileIntW(kSection, key, -1,
                                                  iniPath.c_str());
            if (v == 0 || v == 1) {
                field = (v != 0);
                dirty = true;
            }
        };
        loadFlag(L"ShowGrid",      df.showGrid);
        loadFlag(L"ShowParticles", df.showParticles);
        loadFlag(L"ShowRibbons",   df.showRibbons);
        loadFlag(L"ShowEvents",    df.showEvents);
        if (dirty) service.Settings().SetDisplayFlags(df);
    }

    {
        const i32 v = ::GetPrivateProfileIntW(kSection, L"Tileset", -1,
                                              iniPath.c_str());
        const i32 n = static_cast<i32>(io::Tileset::Count);
        if (v >= 0 && v < n) service.Replaceables().SetTileset(static_cast<io::Tileset>(v));
    }

    {
        const i32 v = ::GetPrivateProfileIntW(kSection, L"IblMode",
                                              -1, iniPath.c_str());
        if (v >= 0 && v <= static_cast<i32>(IblMode::Sunset)
            && static_cast<IblMode>(v) != service.Settings().GetIblMode()) {
            service.Settings().SetIblMode(static_cast<IblMode>(v));
        }
    }

    {
        const i32 v = ::GetPrivateProfileIntW(kSection, L"ShadowCascades",
                                              -1, iniPath.c_str());
        (void)v;
    }

    if (auto* dnc = service.GetDncService()) {
        wchar_t buf[32] = {};
        ::GetPrivateProfileStringW(kSection, L"TimeOfDay", L"",
                                   buf, 32, iniPath.c_str());
        if (buf[0]) {
            wchar_t* endptr = nullptr;
            const f64 v = std::wcstod(buf, &endptr);
            if (endptr != buf) dnc->SetTimeOfDay(static_cast<f32>(v));
        }
        const i32 animate = ::GetPrivateProfileIntW(kSection, L"AnimateTod",
                                                    -1, iniPath.c_str());
        if (animate == 0 || animate == 1) {
            dnc->SetTodScale(animate ? 1.0f : 0.0f);
        }

        wchar_t pathBuf[512] = {};
        ::GetPrivateProfileStringW(kSection, L"DncModel", L"",
                                   pathBuf, 512, iniPath.c_str());
        if (pathBuf[0]) {
            const i32 u8len = ::WideCharToMultiByte(CP_UTF8, 0, pathBuf, -1,
                                                    nullptr, 0, nullptr, nullptr);
            if (u8len > 1) {
                std::string utf8(u8len - 1, '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, pathBuf, -1,
                                      utf8.data(), u8len, nullptr, nullptr);
                if (utf8 != dnc->UnitMdlPath()) dnc->SetUnitMdl(utf8);
            }
        }
    }
}

void SaveSettingsIni(const RenderService& service, bool loopNonLoopingPolicy) {
    const std::wstring iniPath = SettingsIniPath();

    {
        wchar_t buf[32] = {};
        ::swprintf_s(buf, L"0x%08X",
                     static_cast<u32>(service.Settings().BackgroundColorRaw()));
        ::WritePrivateProfileStringW(kSection, L"BackgroundColor",
                                     buf, iniPath.c_str());
    }
    {
        wchar_t buf[32] = {};
        ::swprintf_s(buf, L"%.3f",
                     static_cast<f64>(service.Settings().GetTonemapExposure()));
        ::WritePrivateProfileStringW(kSection, L"Exposure",
                                     buf, iniPath.c_str());
    }
    {
        wchar_t buf[32] = {};
        ::swprintf_s(buf, L"%.3f",
                     static_cast<f64>(service.Sound().GetVolume()));
        ::WritePrivateProfileStringW(kSection, L"SoundVolume",
                                     buf, iniPath.c_str());
    }
    {
        ::WritePrivateProfileStringW(kSection, L"LoopNonLooping",
                                     loopNonLoopingPolicy ? L"1" : L"0",
                                     iniPath.c_str());
    }
    {
        ::WritePrivateProfileStringW(kSection, L"GraphicsDebug",
                                     service.Settings().GraphicsDebug() ? L"1" : L"0",
                                     iniPath.c_str());
    }
    {
        const wchar_t* name = L"d3d12";
        switch (service.Settings().DefaultBackend()) {
            case gfx::GfxApi::D3D11:  name = L"d3d11";  break;
            case gfx::GfxApi::D3D12:  name = L"d3d12";  break;
            case gfx::GfxApi::Vulkan: name = L"vulkan"; break;
        }
        ::WritePrivateProfileStringW(kSection, L"DefaultBackend",
                                     name, iniPath.c_str());
    }
    {
        const std::string& pref = service.Settings().PreferredDevice();
        if (pref.empty()) {
            ::WritePrivateProfileStringW(kSection, L"PreferredDevice",
                                         L"", iniPath.c_str());
        } else {
            const i32 wlen = ::MultiByteToWideChar(CP_UTF8, 0,
                                                   pref.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::wstring wname(wlen, L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, pref.c_str(), -1,
                                      wname.data(), wlen);
                ::WritePrivateProfileStringW(kSection, L"PreferredDevice",
                                             wname.c_str(), iniPath.c_str());
            }
        }
    }
    {
        const DisplayFlags df = service.Settings().GetDisplayFlags();
        auto saveFlag = [&](const wchar_t* key, bool v) {
            ::WritePrivateProfileStringW(kSection, key, v ? L"1" : L"0",
                                         iniPath.c_str());
        };
        saveFlag(L"ShowGrid",      df.showGrid);
        saveFlag(L"ShowParticles", df.showParticles);
        saveFlag(L"ShowRibbons",   df.showRibbons);
        saveFlag(L"ShowEvents",    df.showEvents);
    }
    {
        wchar_t buf[8] = {};
        ::swprintf_s(buf, L"%u",
                     static_cast<u32>(io::GetCurrentTileset()));
        ::WritePrivateProfileStringW(kSection, L"Tileset", buf, iniPath.c_str());
    }
    {
        wchar_t buf[8] = {};
        ::swprintf_s(buf, L"%u",
                     static_cast<u32>(service.Settings().GetIblMode()));
        ::WritePrivateProfileStringW(kSection, L"IblMode", buf, iniPath.c_str());
    }
    if (const auto* shadow = service.GetShadowService()) {
        const i32 cascades = shadow->IsEnabled()
                                 ? std::clamp(shadow->Params().cascadeCount, 1, 3)
                                 : 0;
        wchar_t buf[8] = {};
        ::swprintf_s(buf, L"%d", cascades);
        ::WritePrivateProfileStringW(kSection, L"ShadowCascades", buf, iniPath.c_str());
    }
    if (const auto* dnc = service.GetDncService()) {
        wchar_t buf[32] = {};
        ::swprintf_s(buf, L"%.3f", static_cast<f64>(dnc->GetTimeOfDay()));
        ::WritePrivateProfileStringW(kSection, L"TimeOfDay", buf, iniPath.c_str());
        ::WritePrivateProfileStringW(kSection, L"AnimateTod",
                                     dnc->GetTodScale() > 0.0f ? L"1" : L"0",
                                     iniPath.c_str());

        const std::string& path = dnc->UnitMdlPath();
        const i32 wlen = ::MultiByteToWideChar(CP_UTF8, 0,
                                               path.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring wpath(wlen, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1,
                                  wpath.data(), wlen);
            ::WritePrivateProfileStringW(kSection, L"DncModel",
                                         wpath.c_str(), iniPath.c_str());
        }
    }
}

}
