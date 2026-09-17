#pragma once

// ============================================================================
// The graphics backends by name: what `--backend` accepts, what the settings
// file stores, what the log and the Settings combo call each one, and which of
// them this platform builds. One table, so the CLI, the ini reader, the combo
// and the startup line cannot disagree about a name again.
// ============================================================================

#include "string_util.h"
#include "whiteout/flakes/gfx_types.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace whiteout::flakes::tools {

struct BackendName {
    gfx::GfxApi api;
    std::string_view label;   ///< "D3D12": the startup log and the Settings combo.
    std::string_view iniName; ///< "d3d12": what DefaultBackend stores.
    std::string_view alias;   ///< "dx12": `--backend`'s second spelling.
    bool cli;                 ///< `--backend` accepts it on this platform.
    bool listed;              ///< Named in the CLI's "valid on this platform" message.
    bool ini;                 ///< A stored DefaultBackend of this name is honoured.
};

#if defined(_WIN32)
constexpr bool kBackendIsWindows = true;
#else
constexpr bool kBackendIsWindows = false;
#endif
#if defined(__APPLE__)
constexpr bool kBackendIsApple = true;
#else
constexpr bool kBackendIsApple = false;
#endif

// Indexed by gfx::GfxApi. WebGPU is accepted everywhere and listed nowhere:
// Dawn is optional, and a platform without it coerces the choice afterwards.
inline constexpr std::array<BackendName, 5> kBackendNames = {{
    {gfx::GfxApi::D3D11, "D3D11", "d3d11", "dx11", kBackendIsWindows, kBackendIsWindows, true},
    {gfx::GfxApi::D3D12, "D3D12", "d3d12", "dx12", kBackendIsWindows, kBackendIsWindows, true},
    {gfx::GfxApi::Vulkan, "Vulkan", "vulkan", "vk", true, true, true},
    {gfx::GfxApi::WebGPU, "WebGPU", "webgpu", "wgpu", true, false, true},
    {gfx::GfxApi::Metal, "Metal", "metal", "mtl", kBackendIsApple, kBackendIsApple,
     kBackendIsApple},
}};

inline const BackendName& BackendNameOf(gfx::GfxApi api) {
    return kBackendNames[static_cast<std::size_t>(api)];
}

/// `--backend`: the name or its alias, any case.
inline std::optional<gfx::GfxApi> BackendFromCliName(std::string_view name) {
    for (const BackendName& b : kBackendNames)
        if (b.cli && (EqualsIgnoreCase(name, b.iniName) || EqualsIgnoreCase(name, b.alias)))
            return b.api;
    return std::nullopt;
}

/// DefaultBackend: the stored spelling, the label, or the stored spelling in
/// capitals — the three forms the settings file has ever been written with.
inline std::optional<gfx::GfxApi> BackendFromIniName(std::string_view name) {
    for (const BackendName& b : kBackendNames) {
        if (!b.ini)
            continue;
        std::string upper(b.iniName);
        for (char& c : upper)
            c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
        if (name == b.iniName || name == b.label || name == upper)
            return b.api;
    }
    return std::nullopt;
}

/// "d3d11, d3d12, vulkan": the names `--backend` lists for this platform.
inline std::string BackendCliList() {
    std::string out;
    for (const BackendName& b : kBackendNames) {
        if (!b.listed)
            continue;
        if (!out.empty())
            out += ", ";
        out += b.iniName;
    }
    return out;
}

/// The Settings combo's entries on this platform, in the order it lists them.
/// Empty where there is nothing to choose (Linux ships Vulkan alone).
#if defined(_WIN32)
inline constexpr std::array kSettingsBackends = {gfx::GfxApi::D3D11, gfx::GfxApi::D3D12,
                                                 gfx::GfxApi::Vulkan, gfx::GfxApi::WebGPU};
/// What the combo shows for a stored backend it does not list.
inline constexpr gfx::GfxApi kSettingsBackendFallback = gfx::GfxApi::D3D12;
#elif defined(__APPLE__)
#if WDX_HAS_WEBGPU
inline constexpr std::array kSettingsBackends = {gfx::GfxApi::Metal, gfx::GfxApi::Vulkan,
                                                 gfx::GfxApi::WebGPU};
#else
inline constexpr std::array kSettingsBackends = {gfx::GfxApi::Metal, gfx::GfxApi::Vulkan};
#endif
inline constexpr gfx::GfxApi kSettingsBackendFallback = gfx::GfxApi::Metal;
#else
inline constexpr std::array<gfx::GfxApi, 0> kSettingsBackends = {};
inline constexpr gfx::GfxApi kSettingsBackendFallback = gfx::GfxApi::Vulkan;
#endif

} // namespace whiteout::flakes::tools
