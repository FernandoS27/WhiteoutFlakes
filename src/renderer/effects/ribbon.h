#pragma once

// Internal ribbon simulation system. The public RibbonEmitterConfig
// canonically lives in include/whiteout/flakes/model_types.h; this header
// adds the emitter / segment / system internals built on top of it.

#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::effects {

struct RibbonSegment {
    Vector3f top = {0, 0, 0};
    Vector3f bot = {0, 0, 0};
    f32 age = 0;
};

struct RibbonEmitterState {
    Matrix44f transform = Matrix44f::identity();
    f32 above = 20.0f;
    f32 below = 20.0f;
    f32 alpha = 1.0f;
    Vector3f color = {1, 1, 1};
    f32 visibility = 1.0f;
    i32 slot = 0;
};

struct RibbonEmitter {
    RibbonEmitterConfig config;
    RibbonEmitterState state;
    std::vector<RibbonSegment> segments;
    f32 accumEmission = 0;
    f32 startTime = 0;
    bool posSet = false;

    Vector3f prevPos = {0, 0, 0};
    Vector3f currPos = {0, 0, 0};
    Vector3f prevDir = {0, 0, 1};
    Vector3f currDir = {0, 0, 1};
    Vector3f prevVertical = {0, 1, 0};
    Vector3f currVertical = {0, 1, 0};
};

class RibbonSystem {
public:
    void Clear();
    void AddEmitter(i32 id, const RibbonEmitterConfig& cfg);
    void UpdateEmitterState(i32 id, const RibbonEmitterState& st);

    bool HasEmitters() const;

    void Simulate(f32 dt);

    struct StripResult {
        std::vector<Vertex> vertices;
        std::vector<i32> emitterIds;
    };

    StripResult BuildStrips() const;

    const RibbonEmitterConfig* GetConfig(i32 id) const;
    i32 GetTotalSegmentCount() const;
    i32 GetEmitterVertCount(i32 emitterId) const;

private:
    std::unordered_map<i32, RibbonEmitter> emitters_;
};

} // namespace whiteout::flakes::renderer::effects
