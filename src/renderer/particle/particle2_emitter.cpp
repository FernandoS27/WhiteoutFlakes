#include "renderer/particle/particle2_emitter.h"

#include <algorithm>
#include <cmath>

namespace WhiteoutDex::particle {

namespace {

std::atomic<u32> g_seedCounter{1};

constexpr f32 kMaxDt = 0.5f;

f32 g_globalScaler = 1.0f;

}

void SetGlobalEmissionScaler(f32 s) { g_globalScaler = s; }
f32 GetGlobalEmissionScaler() { return g_globalScaler; }

Emitter2::Emitter2() {

    u32 counter = g_seedCounter.fetch_add(1, std::memory_order_relaxed);
    randSeed_.SetSeed(MakeSeedFromTime(counter));
}

void Emitter2::SetParticleStyle(bool hasHead, bool hasTail, f32 tailLength) {
    SetFlag(kFlagHasHead, hasHead);
    SetFlag(kFlagHasTail, hasTail);
    tailLength_ = tailLength;
}

void Emitter2::SetTextureDimensions(u32 rows, u32 cols) {

    textureRows_ = (rows > 0) ? rows : 1;
    textureCols_ = (cols > 0) ? cols : 1;

    ooTextureWidth_  = 1.0f / static_cast<f32>(textureCols_);
    ooTextureHeight_ = 1.0f / static_cast<f32>(textureRows_);

    textureLog_ = 0;
    u32 c = textureCols_;

    while (c > 1) { c >>= 1; ++textureLog_; }
}

void Emitter2::SetKey(i32 index, const ParticleKey& k) {
    if (index == 0 || index == 1) {
        keys_[index] = k;
    }
}

void Emitter2::Flush() {

    while (pool_.AliveCount() > 0) {
        usize last = pool_.AliveCount() - 1;
        u32 idx = pool_.AliveAt(last);
        pool_.RemoveAliveAt(last);
        pool_.PushDead(idx);
    }
}

f32 Emitter2::CalcVelocity() {
    f32 r = CRandom::reals_(randSeed_);
    return velocity_ * (1.0f + r * velocityVariation_);
}

void Emitter2::MoveParticle(Particle2& p, f32 elapsed) const {

    const f32 az = -acceleration_;
    p.position.x += p.velocity.x * elapsed;
    p.position.y += p.velocity.y * elapsed;
    p.position.z += p.velocity.z * elapsed + 0.5f * az * elapsed * elapsed;
    p.velocity.z += az * elapsed;
}

void Emitter2::InternalUpdate(f32 elapsed) {

    if (elapsed < 0.0f) elapsed = 0.0f;
    if (elapsed > kMaxDt) elapsed = kMaxDt;

    const bool squirtPending = (flags_ & kFlagNeedSquirt) != 0;
    const bool paused        = (flags_ & kFlagPaused) != 0;
    const bool dead          = (flags_ & kFlagSystemDead) != 0;
    const bool enabled       = Enabled();

    if (enabled || squirtPending) {
        Sync();
    }

    if (squirtPending && !paused && !dead) {
        i32 numToEmit = static_cast<i32>(emissionRate_ * g_globalScaler);
        while (numToEmit > 0 && !pool_.DeadEmpty()) {
            u32 idx = pool_.PopDead();
            pool_.PushAlive(idx);
            CreateParticle(pool_[idx], 0.0f);
            --numToEmit;
        }
        flags_ &= ~kFlagNeedSquirt;
    }

    if (enabled && !paused && !dead) {
        numNew_ += elapsed * emissionRate_ * g_globalScaler;
        u32 planned = static_cast<u32>(numNew_);
        u32 emitted = 0;
        while (planned > 0 && !pool_.DeadEmpty()) {
            u32 idx = pool_.PopDead();
            pool_.PushAlive(idx);
            CreateParticle(pool_[idx], elapsed);
            ++emitted;
            --planned;
        }
        numNew_ -= static_cast<f32>(emitted);
    }

    for (usize i = 0; i < pool_.AliveCount(); ) {
        u32 idx = pool_.AliveAt(i);
        Particle2& p = pool_[idx];

        p.age += elapsed;

        while (p.keyFrame < 2 && p.age > keys_[p.keyFrame].endTime) {
            ++p.keyFrame;
        }

        if (lifeSpan_ <= p.age || p.keyFrame >= 2) {
            pool_.PushDead(idx);
            pool_.RemoveAliveAt(i);

        } else {
            MoveParticle(p, elapsed);
            ++i;
        }
    }

    if (pool_.AliveCount() == 0 && CRandom::dice_(32, g_globalRnd) == 0) {
        pool_.Compact();
    }

    flags_ &= ~kFlagVisible;
}

void Emitter2::Update(f32 elapsed) {
    if ((flags_ & kFlagUpdated) == 0) {
        InternalUpdate(elapsed);
    }
    flags_ &= ~kFlagUpdated;
    flags_ &= ~kFlagPaused;
}

void Emitter2::Update(f32 elapsed, const Matrix44f& worldMatrix) {
    modelToWorld_ = worldMatrix;

    if (elapsed == 0.0f) {
        flags_ |= kFlagPaused;
        return;
    }

    flags_ |= kFlagUpdated;
    InternalUpdate(elapsed);

    if ((flags_ & kFlagUpdatedByAnim) == 0) {
        flags_ |= kFlagUpdatedByAnim;

    }
}

}
