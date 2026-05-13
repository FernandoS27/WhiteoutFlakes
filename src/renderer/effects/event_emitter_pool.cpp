#include "renderer/effects/event_emitter_pool.h"

#include "particle/splat_service.h"
#include "renderer/effects/spn_spawner.h"
#include "renderer/model/model_instance.h"
#include "whiteout/flakes/sound_emitter.h"

#include "whiteout/flakes/event_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace whiteout::flakes::renderer::effects {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::io;

void EventEmitterPool::Reset(std::vector<EventObjectConfig> configs,
                             std::vector<u32> globalSequences) {
    entries_.clear();
    entries_.reserve(configs.size());
    for (auto& c : configs) {
        Entry e;
        e.cfg = std::move(c);
        entries_.push_back(std::move(e));
    }
    globalSequences_ = std::move(globalSequences);
    prevSeqIdx_ = -1;
}

namespace {

i32 KeysInHalfOpen(const std::vector<u32>& times, i32 lo, i32 hi, i32 windowLo, i32 windowHi) {
    if (times.empty() || lo >= hi)
        return 0;
    const i32 loB = std::max(lo, windowLo - 1);
    const i32 hiB = std::min(hi, windowHi);
    if (loB >= hiB)
        return 0;
    i32 n = 0;
    for (u32 raw : times) {
        const i32 t = (i32)raw;
        if (t > loB && t <= hiB)
            ++n;
    }
    return n;
}

Matrix44f BuildNodeWorld(const Matrix44f& bone, const Vector3f& pivot,
                         const Matrix44f& actorWorld) {
    Matrix44f pivotT = Matrix44f::translation({pivot.x, pivot.y, pivot.z});
    return pivotT * bone * actorWorld;
}

void ExtractSpawnFrame(const Matrix44f& m, f32 scale, Vector3f& outOrigin, Vector3f& outRight,
                       Vector3f& outForward) {
    auto normRow = [&](i32 r, Vector3f& out) {
        const f32 x = m.data[r][0], y = m.data[r][1], z = m.data[r][2];
        const f32 len = std::sqrt(x * x + y * y + z * z);
        const f32 inv = (len > 1e-6f) ? (scale / len) : 0.0f;
        out = Vector3f{x * inv, y * inv, z * inv};
    };
    outOrigin = Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
    normRow(0, outRight);
    normRow(1, outForward);
}

Vector3f ExtractWorldPos(const Matrix44f& m) {
    return Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
}

void ProjectToGroundPlane(Vector3f& origin, Vector3f& right, Vector3f& forward,
                          const Matrix44f& actorWorld) {
    const f32 groundZ = actorWorld.data[3][2];
    origin.z = groundZ;

    const f32 rLen2D = std::sqrt(right.x * right.x + right.y * right.y);
    const f32 rLen = std::sqrt(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rLen2D > 1e-6f) {
        const f32 k = rLen / rLen2D;
        right.x *= k;
        right.y *= k;
    }
    right.z = 0.f;

    const f32 fLen =
        std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
    const f32 rOnly = std::sqrt(right.x * right.x + right.y * right.y);
    if (rOnly > 1e-6f) {
        const f32 invR = fLen / rOnly;
        forward.x = -right.y * invR;
        forward.y = right.x * invR;
    } else {
        forward.x = 0.f;
        forward.y = fLen;
    }
    forward.z = 0.f;
}

} // namespace

void EventEmitterPool::Tick(const Actor& actor, const std::vector<Matrix44f>& boneWorldMatrices,
                            i32 activeSeqIdx, i32 localTimeMs, i32 globalTimeMs, i32 seqStartMs,
                            i32 seqEndMs, particle::SplatService* splats, SpnSpawner* spn,
                            ISoundEmitter* sounds) {
    if (entries_.empty())
        return;

    if (activeSeqIdx != prevSeqIdx_) {
        for (auto& e : entries_)
            e.lastFrame = -1;
        prevSeqIdx_ = activeSeqIdx;
    }

    for (auto& e : entries_) {
        const auto& cfg = e.cfg;
        if (cfg.kind == EventObjectConfig::Kind::Unknown)
            continue;
        if (cfg.eventTrackTimes.empty())
            continue;
        if (e.resolutionFailed)
            continue;

        i32 frame = 0;
        i32 windowLo = 0;
        i32 windowHi = 0;
        if (cfg.globalSequenceId != 0xFFFFFFFFu && cfg.globalSequenceId < globalSequences_.size()) {
            const u32 dur = globalSequences_[cfg.globalSequenceId];
            if (dur == 0)
                continue;
            frame = (i32)((u32)globalTimeMs % dur);
            windowLo = 0;
            windowHi = (i32)dur - 1;
        } else {
            frame = localTimeMs;
            windowLo = seqStartMs;
            windowHi = seqEndMs;
        }

        i32 fireCount = 0;
        if (e.lastFrame >= 0) {
            if (frame >= e.lastFrame) {
                fireCount =
                    KeysInHalfOpen(cfg.eventTrackTimes, e.lastFrame, frame, windowLo, windowHi);
            } else {
                fireCount =
                    KeysInHalfOpen(cfg.eventTrackTimes, e.lastFrame, windowHi, windowLo, windowHi) +
                    KeysInHalfOpen(cfg.eventTrackTimes, windowLo - 1, frame, windowLo, windowHi);
            }
        }
        e.lastFrame = frame;

        if (fireCount <= 0)
            continue;

        Matrix44f nodeWorld;
        if (cfg.nodeIndex >= 0 && cfg.nodeIndex < (i32)boneWorldMatrices.size()) {
            nodeWorld =
                BuildNodeWorld(boneWorldMatrices[cfg.nodeIndex], cfg.pivot, actor.worldTransform);
        } else {
            nodeWorld = actor.worldTransform;
        }

        const i32 dispatchCount = (cfg.kind == EventObjectConfig::Kind::SND) ? 1 : fireCount;
        for (i32 k = 0; k < dispatchCount && !e.resolutionFailed; ++k) {
            switch (cfg.kind) {
            case EventObjectConfig::Kind::SPN: {
                if (!spn)
                    break;
                const io::SpnEntry* row = io::FindSpn(cfg.id);
                if (!row) {

                    std::fprintf(stderr, "[WDEX events] SPN id '%s' not in SpawnData.slk\n",
                                 cfg.id.c_str());
                    e.resolutionFailed = true;
                    break;
                }
                spn->Spawn(actor.handle, row->modelPath, nodeWorld, globalTimeMs);
                break;
            }
            case EventObjectConfig::Kind::SPL:
            case EventObjectConfig::Kind::FPT: {
                if (!splats)
                    break;
                const io::SplEntry* row = io::FindSpl(cfg.id);
                if (!row) {
                    std::fprintf(stderr, "[WDEX events] SPL/FPT id '%s' not in SplatData.slk\n",
                                 cfg.id.c_str());
                    e.resolutionFailed = true;
                    break;
                }
                Vector3f origin, right, forward;
                ExtractSpawnFrame(nodeWorld, row->scale, origin, right, forward);
                ProjectToGroundPlane(origin, right, forward, actor.worldTransform);
                splats->SpawnSpl(*row, origin, right, forward);
                break;
            }
            case EventObjectConfig::Kind::UBR: {
                if (!splats)
                    break;
                const io::UbrEntry* row = io::FindUbr(cfg.id);
                if (!row) {
                    std::fprintf(stderr, "[WDEX events] UBR id '%s' not in UberSplatData.slk\n",
                                 cfg.id.c_str());
                    e.resolutionFailed = true;
                    break;
                }
                Vector3f origin, right, forward;
                ExtractSpawnFrame(nodeWorld, row->scale, origin, right, forward);
                ProjectToGroundPlane(origin, right, forward, actor.worldTransform);
                splats->SpawnUbr(*row, origin, right, forward);
                break;
            }
            case EventObjectConfig::Kind::SND: {
                if (!sounds)
                    break;
                const io::SndEntry* row = io::FindSnd(cfg.id);
                if (!row) {
                    std::fprintf(stderr,
                                 "[WDEX events] SND id '%s' not in any UI/SoundInfo/*Sounds*.slk\n",
                                 cfg.id.c_str());
                    e.resolutionFailed = true;
                    break;
                }
                sounds->Play(*row, ExtractWorldPos(nodeWorld));
                break;
            }
            case EventObjectConfig::Kind::Unknown:
                break;
            }
        }
    }
}

} // namespace whiteout::flakes::renderer::effects
