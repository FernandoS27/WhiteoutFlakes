#include "renderer/pe1_system.h"
#include "sim_util.h"
#include <cmath>
#include <algorithm>

namespace WhiteoutDex {

void PE1System::Clear() {
    emitters_.clear();
}

void PE1System::AddEmitter(i32 id, const PE1EmitterConfig& cfg) {
    emitters_[id].config = cfg;
    emitters_[id].particles.clear();
    emitters_[id].accumEmission = 0;
}

void PE1System::UpdateEmitterState(i32 id, const PE1EmitterState& st) {
    auto it = emitters_.find(id);
    if (it != emitters_.end())
        it->second.state = st;
}

bool PE1System::HasEmitters() const { return !emitters_.empty(); }

i32 PE1System::GetTotalParticleCount() const {
    i32 total = 0;
    for (auto& [id, em] : emitters_) total += (i32)em.particles.size();
    return total;
}

const PE1EmitterConfig* PE1System::GetConfig(i32 emitterId) const {
    auto it = emitters_.find(emitterId);
    return (it != emitters_.end()) ? &it->second.config : nullptr;
}

PE1SimResult PE1System::Simulate(f32 dt, u32& nextHandle) {
    PE1SimResult result;
    dt = ClampDeltaTime(dt);

    for (auto& [id, em] : emitters_) {

        for (auto it = em.particles.begin(); it != em.particles.end(); ) {
            if (it->lifeSpan <= 0) {
                result.died.push_back(it->childModelHandle);
                it = em.particles.erase(it);
            } else {
                ++it;
            }
        }

        if (IsEmitterVisible(em.state.visibility) && em.state.emissionRate > 0) {
            em.accumEmission += em.state.emissionRate * dt;
            while (em.accumEmission >= 1.0f) {
                SpawnParticle(em, dt, nextHandle, result);
                em.accumEmission -= 1.0f;
            }
        }

        f32 az = -(em.state.gravity);
        for (auto& p : em.particles) {
            p.position.x += p.velocity.x * dt;
            p.position.y += p.velocity.y * dt;
            p.position.z += p.velocity.z * dt + 0.5f * az * dt * dt;
            p.velocity.z += az * dt;
            p.lifeSpan -= dt;

            Matrix44f scale = Matrix44f::scaling({em.config.scale, em.config.scale, em.config.scale});
            Matrix44f trans = Matrix44f::translation({p.position.x, p.position.y, p.position.z});
            result.transforms.push_back({p.childModelHandle, scale * trans});
        }
    }

    return result;
}

void PE1System::SpawnParticle(PE1Emitter& em, f32 dt,
                               u32& nextHandle, PE1SimResult& result) {
    PE1Particle p;
    auto& cfg = em.config;
    auto& st = em.state;

    Vector3f wPos = whiteout::transform_point(Vector3f{0, 0, 0}, st.transform);
    p.position = wPos;

    f32 theta = (2.0f * st.latitude  * RandF(0.0f, 1.0f)) - st.latitude;
    f32 phi   = (2.0f * st.longitude * RandF(0.0f, 1.0f)) - st.longitude;

    f32 speed = st.speed;
    Vector3f vel = {0, 0, speed};

    f32 sinT = sinf(theta), cosT = cosf(theta);
    vel.x = vel.z * sinT;
    vel.z = vel.z * cosT;

    f32 sinP = sinf(phi), cosP = cosf(phi);
    vel.y = vel.x * sinP;
    vel.x = vel.x * cosP;

    p.velocity = whiteout::transform_normal(vel, st.transform);

    p.lifeSpan = cfg.lifespan;
    p.initLife = cfg.lifespan;
    p.emitterId = 0;
    p.childModelHandle = nextHandle++;

    f32 subAge = dt * RandF(0.0f, 1.0f);
    if (subAge > 0 && subAge < p.lifeSpan) {
        f32 az = -(st.gravity);
        p.position.x += p.velocity.x * subAge;
        p.position.y += p.velocity.y * subAge;
        p.position.z += p.velocity.z * subAge + 0.5f * az * subAge * subAge;
        p.velocity.z += az * subAge;
        p.lifeSpan -= subAge;
    }

    for (auto& [eid, e] : emitters_) {
        if (&e == &em) { p.emitterId = eid; break; }
    }

    Matrix44f scale = Matrix44f::scaling({cfg.scale, cfg.scale, cfg.scale});
    Matrix44f trans = Matrix44f::translation({p.position.x, p.position.y, p.position.z});
    PE1BirthEvent birth;
    birth.handle = p.childModelHandle;
    birth.emitterId = p.emitterId;
    birth.worldTransform = scale * trans;
    result.born.push_back(birth);

    em.particles.push_back(p);
}

f32 PE1System::RandF(f32 lo, f32 hi) {
    std::uniform_real_distribution<f32> dist(lo, hi);
    return dist(rng_);
}

}
