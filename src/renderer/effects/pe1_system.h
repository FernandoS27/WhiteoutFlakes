#pragma once

#include <functional>
#include <random>
#include <unordered_map>
#include <vector>
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::effects {

// Tree-depth and instance caps for the PE1 ("particles that ARE models")
// subsystem. FrameTicker reads these to throttle PE1 spawns; raise with care
// since each PE1 instance is a full Actor with GPU resources.
constexpr i32 kMaxPE1Depth = 3;
constexpr i32 kMaxPE1Instances = 256;

struct PE1EmitterState {
    Matrix44f transform = Matrix44f::identity();
    f32 emissionRate = 0;
    f32 speed = 0;
    f32 latitude = 0;
    f32 longitude = 0;
    f32 gravity = 0;
    f32 visibility = 1.0f;
};

struct PE1Particle {
    Vector3f position = {0, 0, 0};
    Vector3f velocity = {0, 0, 0};
    f32 lifeSpan = 0;
    f32 initLife = 0;
    u32 childModelHandle = 0;
    i32 emitterId = 0;
};

struct PE1Emitter {
    model::PE1EmitterConfig config;
    PE1EmitterState state;
    std::vector<PE1Particle> particles;
    f32 accumEmission = 0;
};

struct PE1BirthEvent {
    u32 handle;
    i32 emitterId;
    Matrix44f worldTransform;
};

struct PE1SimResult {
    std::vector<PE1BirthEvent> born;
    std::vector<u32> died;
    std::vector<std::pair<u32, Matrix44f>> transforms;
};

class PE1System {
public:
    void Clear();
    void AddEmitter(i32 id, const model::PE1EmitterConfig& cfg);
    void UpdateEmitterState(i32 id, const PE1EmitterState& st);
    bool HasEmitters() const;
    i32 GetTotalParticleCount() const;
    const model::PE1EmitterConfig* GetConfig(i32 emitterId) const;

    // `allocHandle` is invoked once per particle birth to mint a fresh
    // ActorId (the caller routes this through SceneManager::AllocActorId so
    // the renderer doesn't expose a mutable counter).
    using HandleAllocator = std::function<u32()>;
    PE1SimResult Simulate(f32 dt, const HandleAllocator& allocHandle);

private:
    void SpawnParticle(PE1Emitter& em, f32 dt, const HandleAllocator& allocHandle,
                       PE1SimResult& result);
    f32 RandF(f32 lo, f32 hi);

    std::unordered_map<i32, PE1Emitter> emitters_;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace whiteout::flakes::renderer::effects
