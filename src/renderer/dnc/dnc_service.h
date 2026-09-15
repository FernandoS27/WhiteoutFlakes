#pragma once

#include "whiteout/flakes/enums.h" // Wc3ArtTier
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
    // Raw KLBC colour and KLBI intensity. The SD palette uses `ambient` (the
    // intensity alone); the 3.0.0 HD main light block multiplies the two.
    whiteout::Vector3f ambientColor{0, 0, 0};
    f32 ambientIntensity = 0.0f;
    // The rig light's static ShadowIntensity (MDLLIGHT+488). HD writes it to PS
    // cb2[28].w, where it scales the image-based lighting.
    f32 shadowIntensity = 0.0f;
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
    /// An unpinned path names one file but reaches three — `war3.w3mod:` for
    /// Classic, `_hd.w3mod:` for Reforged, `_de.w3mod:` for Definitive — and
    /// the overlay rigs carry a very different curve (ambientIntensity 0 vs
    /// 0.3). This is per-scene state rather than a read of the provider's
    /// tier, because the host may point several scenes at one shared provider;
    /// resolving through a mod-pinned path keeps each scene on its own variant
    /// regardless. A no-op if unchanged.
    void SetArtTierPreference(Wc3ArtTier tier);
    Wc3ArtTier ArtTierPreference() const {
        return tierPreference_;
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

    /// @brief Load the rig now, and keep it loaded across later path / HD
    ///        changes.
    ///
    /// The rig is a Warcraft III file, so reading it is what opens that
    /// game's install — and the service is constructed at device init, for
    /// every session, including ones that only ever show a WoW or StarCraft II
    /// model. So the first acquire waits until something that knows this
    /// session loads Warcraft III content says so; RenderService::
    /// EnsureWc3GameData is that caller. Until then HasAsset() answers false
    /// and SampleNow() returns an invalid sample, which is exactly what they
    /// answer for a rig that failed to load.
    void RealiseAsset();

private:
    void ReacquireAsset();
    void AcquireNow();

    io::IContentProvider* contentProvider_ = nullptr;
    std::unique_ptr<DncCache> cache_;
    DncAsset* unitAsset_ = nullptr;
    std::string unitPath_;
    Wc3ArtTier tierPreference_ = Wc3ArtTier::Classic;
    // RealiseAsset has been called: this session wants the rig.
    bool wanted_ = false;
    // The resolved path has changed (or was never loaded) since the last
    // acquire.
    bool dirty_ = true;
    // A failed acquire has already been reported. Cleared by the next one that
    // works, so a rig that comes and goes says so each time.
    bool warnedMissing_ = false;

    std::atomic<f32> tod_{12.0f};
    f32 hoursPerDay_ = 24.0f;
    f32 secondsPerDay_ = 480.0f;
    f32 dawnHours_ = 6.0f;
    f32 duskHours_ = 18.0f;
    f32 todScale_ = 0.0f;
    bool suspended_ = false;
};

} // namespace whiteout::flakes::renderer::dnc
