#pragma once

#include "whiteout/flakes/types.h"

#include <whiteout/vector_types.h>

#include <atomic>
#include <memory>
#include <string>

namespace whiteout::flakes::io { class IContentProvider; }

namespace whiteout::flakes::renderer::dnc {

class  DncCache;
struct DncAsset;

struct DncSample {
    whiteout::Vector3f ambient   {0, 0, 0};
    whiteout::Vector3f diffuse   {0, 0, 0};
    whiteout::Vector3f worldDir  {0, 0, -1};
    bool               valid     = false;
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
    const std::string& UnitMdlPath() const { return unitPath_; }

    bool HasAsset() const;

    void SetTimeOfDay(f32 hours);
    f32  GetTimeOfDay() const  { return tod_.load(std::memory_order_relaxed); }

    void SetHoursPerDay(f32 h)         { hoursPerDay_   = (h > 0.0f) ? h : 24.0f; }
    f32  GetHoursPerDay() const        { return hoursPerDay_; }

    void SetDayLengthSeconds(f32 s)    { secondsPerDay_ = (s > 0.0f) ? s : 480.0f; }
    f32  GetDayLengthSeconds() const   { return secondsPerDay_; }

    void SetDawnHours(f32 h)           { dawnHours_ = h; }
    f32  GetDawnHours() const          { return dawnHours_; }

    void SetDuskHours(f32 h)           { duskHours_ = h; }
    f32  GetDuskHours() const          { return duskHours_; }

    void SetTodScale(f32 s)            { todScale_ = s; }
    f32  GetTodScale() const           { return todScale_; }

    void Suspend(bool s)               { suspended_ = s; }
    bool IsSuspended() const           { return suspended_; }

    void Advance(f32 dtSec);

    DncSample SampleNow() const;

    DncSample SampleAt(f32 todHours) const;

    struct EnvMapBlend {
        bool isDaytime   = true;
        f32  transitionT = 0.0f;
    };
    EnvMapBlend ComputeEnvMapBlend() const;

private:
    io::IContentProvider*           contentProvider_ = nullptr;
    std::unique_ptr<DncCache>   cache_;
    DncAsset*                   unitAsset_       = nullptr;
    std::string                 unitPath_;

    std::atomic<f32>            tod_             { 12.0f };
    f32                         hoursPerDay_     = 24.0f;
    f32                         secondsPerDay_   = 480.0f;
    f32                         dawnHours_       = 6.0f;
    f32                         duskHours_       = 18.0f;
    f32                         todScale_        = 0.0f;
    bool                        suspended_       = false;
};

}
