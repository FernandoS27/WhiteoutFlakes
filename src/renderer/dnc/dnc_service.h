#pragma once

#include "whiteout/flakes/types.h"

#include <whiteout/vector_types.h>

#include <atomic>
#include <memory>
#include <string>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::renderer::dnc {

class DncCache;
struct DncAsset;

struct DncSample {
    whiteout::Vector3f ambient{0, 0, 0};
    whiteout::Vector3f diffuse{0, 0, 0};
    // Raw KLBC colour, kept separate from `ambient` because only the SD-on-HD
    // path folds it in (CGxLightToShaderLight's ambLightModifier).
    whiteout::Vector3f ambientColor{0, 0, 0};
    whiteout::Vector3f worldDir{0, 0, -1};
    bool valid = false;
};

class DncService {
public:
    explicit DncService(io::IContentProvider* contentProvider);
    ~DncService();

    DncService(const DncService&) = delete;
    DncService& operator=(const DncService&) = delete;

    static constexpr const char* kDefaultUnitMdl =
        "Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdl";
    static constexpr const char* kDefaultTerrainMdl =
        "Environment/DNC/DNCLordaeron/DNCLordaeronTerrain/DNCLordaeronTerrain.mdl";
    static constexpr const char* kDefaultPortraitMdl =
        "Environment/DNC/DNCLordaeron/DNCLordaeronPortrait/DNCLordaeronPortrait.mdl";

    void SetUnitMdl(const std::string& path);
    const std::string& UnitMdlPath() const {
        return unitPath_;
    }

    /// @brief Re-point at another provider (the owning scene was given a new
    ///        one). Reloads the rig through it; a no-op if unchanged.
    void SetContentProvider(io::IContentProvider* contentProvider);

    /// @brief Which mod layer an unpinned ("Auto") path resolves from.
    ///
    /// An unpinned path names one file but reaches two — `war3.w3mod:` in SD,
    /// `war3.w3mod:_hd.w3mod:` in HD — and the HD rigs carry a very different
    /// curve (ambientIntensity 0 vs 0.3). This is per-scene state rather than
    /// a read of the provider's HD flag, because the host may point several
    /// scenes at one shared provider; resolving through a mod-pinned path
    /// keeps each scene on its own variant regardless. A no-op if unchanged.
    void SetHdPreference(bool hd);
    bool HdPreference() const {
        return hdPreference_;
    }

    bool HasAsset() const;

    void SetTimeOfDay(f32 hours);
    f32 GetTimeOfDay() const {
        return tod_.load(std::memory_order_relaxed);
    }

    void SetHoursPerDay(f32 h) {
        hoursPerDay_ = (h > 0.0f) ? h : 24.0f;
    }
    f32 GetHoursPerDay() const {
        return hoursPerDay_;
    }

    void SetDayLengthSeconds(f32 s) {
        secondsPerDay_ = (s > 0.0f) ? s : 480.0f;
    }
    f32 GetDayLengthSeconds() const {
        return secondsPerDay_;
    }

    void SetDawnHours(f32 h) {
        dawnHours_ = h;
    }
    f32 GetDawnHours() const {
        return dawnHours_;
    }

    void SetDuskHours(f32 h) {
        duskHours_ = h;
    }
    f32 GetDuskHours() const {
        return duskHours_;
    }

    void SetTodScale(f32 s) {
        todScale_ = s;
    }
    f32 GetTodScale() const {
        return todScale_;
    }

    void Suspend(bool s) {
        suspended_ = s;
    }
    bool IsSuspended() const {
        return suspended_;
    }

    void Advance(f32 dtSec);

    DncSample SampleNow() const;

    DncSample SampleAt(f32 todHours) const;

    struct EnvMapBlend {
        bool isDaytime = true;
        f32 transitionT = 0.0f;
    };
    EnvMapBlend ComputeEnvMapBlend() const;

private:
    void ReacquireAsset();

    io::IContentProvider* contentProvider_ = nullptr;
    std::unique_ptr<DncCache> cache_;
    DncAsset* unitAsset_ = nullptr;
    std::string unitPath_;
    bool hdPreference_ = false;

    std::atomic<f32> tod_{12.0f};
    f32 hoursPerDay_ = 24.0f;
    f32 secondsPerDay_ = 480.0f;
    f32 dawnHours_ = 6.0f;
    f32 duskHours_ = 18.0f;
    f32 todScale_ = 0.0f;
    bool suspended_ = false;
};

} // namespace whiteout::flakes::renderer::dnc
