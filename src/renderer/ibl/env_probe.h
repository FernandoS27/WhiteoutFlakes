#pragma once

#include "whiteout/flakes/types.h"
#include "gfx/gfx.h"

#include <string>

namespace whiteout::flakes::io { class IContentProvider; }

namespace whiteout::flakes::renderer::ibl {

inline constexpr const char* kDayIblPath   =
    "Environment/EnvironmentMap/LordaeronSummer/Day_IBL.dds";
inline constexpr const char* kNightIblPath =
    "Environment/EnvironmentMap/LordaeronSummer/Night_IBL.dds";

inline constexpr const char* kPortraitIblPath =
    "Environment/EnvironmentMap/Portraits/PortraitDefault_IBL.dds";

inline constexpr const char* kDungeonIblPath =
    "Environment/EnvironmentMap/Dungeon/Night_IBL.dds";
inline constexpr const char* kSunsetIblPath =
    "Environment/EnvironmentMap/Northrend/Sunset_IBL.dds";

struct LoadedEnvProbe {
    gfx::TextureHandle handle   = gfx::TextureHandle::Invalid;
    i32                mipCount = 0;
};

LoadedEnvProbe LoadEnvProbe(gfx::IGFXDevice&             gfx,
                            const io::IContentProvider&  content,
                            const std::string&           relPath);

LoadedEnvProbe LoadEnvProbeFromFile(gfx::IGFXDevice&   gfx,
                                     const std::string& absPath,
                                     bool applyBlizzardFaceRemap = false);

constexpr i32 kEnvProbeSize       = 16;
constexpr i32 kEnvProbeMipLevels  = 5;

gfx::TextureHandle CreateDefaultEnvProbe(gfx::IGFXDevice& gfx);

bool WriteDebugFacesDds(const std::string& outPath);

gfx::TextureHandle CreateDebugFacesEnvProbe(gfx::IGFXDevice& gfx);

gfx::TextureHandle CreateStudioEnvProbe(gfx::IGFXDevice& gfx);

inline f32 DefaultEnvProbeEndMip() {
    return static_cast<f32>(kEnvProbeMipLevels - 1);
}

}
