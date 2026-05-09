#include "renderer/dnc/dnc_asset.h"

#include "common_types.h"
#include "io/mdx_animation.h"

#include <whiteout/vector_types.h>

#include <cmath>
#include <cstdio>

namespace whiteout::flakes::renderer::dnc {

using namespace ::whiteout::flakes::io;

namespace {
using whiteout::Vector3f;
using whiteout::mdx::Light;
using whiteout::mdx::Model;
}

DncSample Sample(const DncAsset& asset,
                 f32             todHours,
                 f32             hoursPerDay,
                 f32             ambModifier) {
    DncSample s;
    if (!asset.HasLight() || hoursPerDay <= 0.0f) {
        return s;
    }

    f32 wrapped = std::fmod(todHours, hoursPerDay);
    if (wrapped < 0.0f) wrapped += hoursPerDay;

    const i32 animLen   = asset.AnimLengthMs();
    const i32 animMsRel = (animLen > 0)
                              ? static_cast<i32>((wrapped * static_cast<f32>(animLen)) / hoursPerDay)
                              : 0;
    const i32 animMs    = asset.seqStartMs + animMsRel;

    const auto& L = asset.model.lights[0];

    std::vector<Matrix44f> boneWorld, allNodes;
    asset.hierarchy.Evaluate(animMs, asset.seqStartMs, asset.seqEndMs,
                             asset.model.globalSequences, boneWorld, allNodes,
                               nullptr,   animMs);
    Matrix44f lightWorld = (asset.lightNodeIdx >= 0 && asset.lightNodeIdx < (i32)allNodes.size())
                               ? allNodes[asset.lightNodeIdx]
                               : Matrix44f::identity();

    Vector3f color = L.color;
    if (L.colorTracks.isUsed) {
        Vector3f animated = EvaluateTrackVec3(L.colorTracks, animMs,
                                              asset.seqStartMs, asset.seqEndMs, L.color);
        color = {animated.z, animated.y, animated.x};
    }
    const f32 intensity =
        std::max(0.0f, EvaluateTrackF32(L.intensityTracks, animMs,
                                        asset.seqStartMs, asset.seqEndMs, L.intensity));
    s.diffuse = { color.x * intensity, color.y * intensity, color.z * intensity };

    Vector3f ambColor = L.ambientColor;
    if (L.ambientColorTracks.isUsed) {
        Vector3f animated = EvaluateTrackVec3(L.ambientColorTracks, animMs,
                                              asset.seqStartMs, asset.seqEndMs, L.ambientColor);
        ambColor = {animated.z, animated.y, animated.x};
    }
    const f32 ambI =
        std::max(0.0f, EvaluateTrackF32(L.ambientIntensityTracks, animMs,
                                        asset.seqStartMs, asset.seqEndMs, L.ambientIntensity));
    s.ambient = { ambColor.x * ambModifier + ambI,
                  ambColor.y * ambModifier + ambI,
                  ambColor.z * ambModifier + ambI };

    s.worldDir = whiteout::transform_normal(Vector3f{0.0f, 0.0f, -1.0f}, lightWorld);
    const f32 n2 = s.worldDir.x * s.worldDir.x +
                   s.worldDir.y * s.worldDir.y +
                   s.worldDir.z * s.worldDir.z;
    if (n2 > 1.0e-12f) {
        const f32 invLen = 1.0f / std::sqrt(n2);
        s.worldDir = { s.worldDir.x * invLen, s.worldDir.y * invLen, s.worldDir.z * invLen };
    } else {

        s.worldDir = { 0.0f, 0.0f, -1.0f };
    }

    s.valid = true;
    return s;
}

}
