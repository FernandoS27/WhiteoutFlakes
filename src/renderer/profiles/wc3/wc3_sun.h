#pragma once

// Relocated verbatim from render_pipeline.cpp's anonymous namespace (P1). It
// was already shared by the CSM shadow pass and both geoset shading baselines;
// moving the pass classes out of that file is what forced it to have a header.

#include "renderer/dnc/dnc_service.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::profiles::wc3 {

// Front-biased day-night sun travel direction (world space, the way the light
// travels toward the scene). The raw DNC light node points wherever the asset
// authored it, which back-lights the viewer's default view; this anchors the
// sun in front of and above the model so the frontal part stays lit, sweeping
// elevation with TOD. The unit faces +X in renderer space (Max -Y — the same
// facing the default camera and view-cube use), world up is +Z, so the sun is
// toward {+X front, +Z up} with a slight +Y for three-quarter form. Shared by
// the CSM shadow pass and the geoset shading baseline so the two agree.
inline Vector3f ComputeSunDirWS(const dnc::DncService* dnc) {
    f32 height = 1.2f; // sun elevation above the horizon; swept by TOD below
    if (dnc) {
        constexpr f32 kPi = 3.14159265f;
        const f32 hpd = dnc->GetHoursPerDay();
        const f32 tod = dnc->GetTimeOfDay();
        const f32 theta = ((tod - hpd * 0.25f) / (hpd * 0.5f)) * kPi;
        // Low near dawn/dusk, high at noon — but always above the horizon so
        // the front never falls into shadow.
        height = 0.4f + 0.9f * std::max(0.1f, std::sin(theta));
    }
    // Direction *to* the sun, then negate for the travel direction.
    const Vector3f toSun = {1.0f, 0.35f, height};
    Vector3f travel = {-toSun.x, -toSun.y, -toSun.z};
    const f32 len = std::sqrt(travel.x * travel.x + travel.y * travel.y + travel.z * travel.z);
    if (len > 1.0e-4f) {
        const f32 inv = 1.0f / len;
        travel = {travel.x * inv, travel.y * inv, travel.z * inv};
    }
    return travel;
}

} // namespace whiteout::flakes::renderer::profiles::wc3
