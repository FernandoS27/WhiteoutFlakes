#pragma once

// ============================================================================
// WhiteoutFlakes — public display / camera / sequence value types.
//
// Things that surface flags + small descriptors used by the renderer's
// pipeline / settings / camera / animation surface.
// ============================================================================

#include "enums.h"
#include "shadow_params.h"
#include "types.h"

#include <functional>
#include <string>

namespace whiteout::flakes {

using RenderTargetId = u32;
// ShadowParams comes from shadow_params.h (already in this namespace).

struct DisplayFlags {
    bool showGrid = true;
    bool showParticles = true;
    bool showRibbons = true;
    bool showCollisions = false;
    bool showLights = false;
    bool showEvents = true;
    RenderMode renderMode = RenderMode::SD;
};

// ShadowParams mirrors the renderer's internal cascade-shadow tuning struct
// 1:1; defined in src/renderer/shadow/shadow_params.h and re-exported here.

// Aggregates the five frame stats the pipeline exposes. Names mirror the
// internal RenderPipeline::GetFrameStats out parameters.
struct FrameStats {
    i32 geosets = 0;
    i32 textures = 0;
    i32 nodes = 0;
    i32 particles = 0;
    i32 segments = 0;
};

// Camera preset — scripted camera the host may pick from the model. Mirrors
// the internal CameraPreset 1:1 and is re-exported into the model namespace
// via a using alias so existing code keeps compiling.
struct CameraPreset {
    // UTF-8 (was std::wstring; switched so the same string flows from the MDX
    // adapter through the host UI without per-platform wide-char roundtrips).
    std::string name;
    bool isLive = false;

    Vector3f position{0.f, 0.f, 0.f};
    Vector3f target{0.f, 0.f, 0.f};
    f32 fovDiagonal = 0.95f;
    f32 zNear = 1.0f;
    f32 zFar = 10000.0f;
    f32 staticRoll = 0.0f;

    f32 pitch = 0.0f;
    f32 yaw = 0.0f;
    f32 distance = 100.0f;

    std::function<void(Vector3f& pos, Vector3f& target, f32& roll, i32 timeMs, i32 seqStart,
                       i32 seqEnd)>
        animator;
};

struct SequenceInfo {
    std::string name;
    i32 startMs = 0;
    i32 endMs = 0;
    f32 moveSpeed = 0.0f;
    bool nonLooping = false;
};

} // namespace whiteout::flakes
