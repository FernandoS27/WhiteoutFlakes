#include "renderer/dnc/dnc_service.h"

#include "dnc_asset.h"
#include "dnc_cache.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace whiteout::flakes::renderer::dnc {

using namespace ::whiteout::flakes::io;

namespace {

f32 WrapTod(f32 tod, f32 hoursPerDay) {
    if (hoursPerDay <= 0.0f)
        return 0.0f;
    f32 w = std::fmod(tod, hoursPerDay);
    if (w < 0.0f)
        w += hoursPerDay;
    return w;
}
} // namespace

DncService::DncService(IContentProvider* contentProvider)
    : contentProvider_(contentProvider), cache_(std::make_unique<DncCache>(contentProvider)),
      unitPath_(kDefaultUnitMdl) {

    unitAsset_ = cache_->Acquire(unitPath_);
    if (!unitAsset_ || !unitAsset_->HasLight()) {
        std::fprintf(stderr,
                     "[dnc] WARN: default unit MDL failed to acquire a usable "
                     "light: %s\n",
                     unitPath_.c_str());
    }
}

DncService::~DncService() {
    if (unitAsset_) {
        cache_->Release(unitAsset_);
        unitAsset_ = nullptr;
    }
}

void DncService::SetUnitMdl(const std::string& path) {
    if (path == unitPath_)
        return;

    if (unitAsset_) {
        cache_->Release(unitAsset_);
        unitAsset_ = nullptr;
    }
    unitPath_ = path;
    if (!path.empty()) {
        unitAsset_ = cache_->Acquire(path);
    }
}

bool DncService::HasAsset() const {
    return unitAsset_ != nullptr && unitAsset_->HasLight();
}

void DncService::SetTimeOfDay(f32 hours) {
    tod_.store(WrapTod(hours, hoursPerDay_), std::memory_order_relaxed);
}

void DncService::Advance(f32 dtSec) {
    if (suspended_ || todScale_ <= 0.0f || dtSec <= 0.0f)
        return;
    if (secondsPerDay_ <= 0.0f)
        return;

    const f32 deltaHours = dtSec * todScale_ * hoursPerDay_ / secondsPerDay_;
    const f32 current = tod_.load(std::memory_order_relaxed);
    tod_.store(WrapTod(current + deltaHours, hoursPerDay_), std::memory_order_relaxed);
}

DncSample DncService::SampleNow() const {
    if (!unitAsset_)
        return DncSample{};
    return Sample(*unitAsset_, tod_.load(std::memory_order_relaxed), hoursPerDay_);
}

DncSample DncService::SampleAt(f32 todHours) const {
    if (!unitAsset_)
        return DncSample{};
    return Sample(*unitAsset_, todHours, hoursPerDay_);
}

DncService::EnvMapBlend DncService::ComputeEnvMapBlend() const {

    const f32 currentTod = tod_.load(std::memory_order_relaxed);
    const f32 window = hoursPerDay_ * 0.1f;
    EnvMapBlend out;

    if (currentTod < dawnHours_) {
        const f32 t = std::clamp((dawnHours_ - currentTod) / window, 0.0f, 1.0f);
        out.transitionT = 1.0f - t;
        out.isDaytime = false;
    } else if (currentTod < duskHours_) {
        const f32 t = std::clamp((duskHours_ - currentTod) / window, 0.0f, 1.0f);
        out.transitionT = 1.0f - t;
        out.isDaytime = true;
    } else {
        out.transitionT = 0.0f;
        out.isDaytime = false;
    }
    return out;
}

} // namespace whiteout::flakes::renderer::dnc
