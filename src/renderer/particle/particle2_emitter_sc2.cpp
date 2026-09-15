#include "renderer/particle/particle2_emitter.h"

// ============================================================================
// Emitter2's StarCraft II touch points.
//
// Everything that reads or writes `sc2_`, the runtime every other family leaves
// null: the SC2 halves of describe, rewind and ApplyState, the public touch
// points the service and the actor layer call on every emitter they hold
// without asking what it is, the tick, and the quad build. Keeping them here is
// what lets the WC3/WoW loop in particle2_emitter.cpp stay free of the runtime.
// SC2_PARTICLE_DESIGN.md §13.
// ============================================================================

#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/sc2_runtime.h"
#include "renderer/particle/sc2_tick.h"

#include <algorithm>

namespace whiteout::flakes::renderer::particle {

void Emitter2::DescribeSc2() {
    if (!sc2_)
        sc2_ = std::make_unique<Sc2Runtime>();
    // The pool is sized once, here, and the runtime words derived.
    sc2_->Arm(desc_->sc2);
    // The emission slots too, before anything can owe one a burst: the
    // actor layer's first crossing arrives ahead of the first tick, and a
    // burst queued on a slot that does not exist yet is dropped.
    sc2_->slots.assign((std::max)(usize{1}, desc_->sc2.emit.slotBones.size()),
                       Sc2Runtime::Slot{});
    // The model-particle side state, with the store it indexes.
    sc2_->host = this;
    sc2_->pending = sc2PendingList_ ? sc2PendingList_ : &sc2_->ownPending;
    const usize models =
        desc_->sc2.Has(ParticleFlag::ModelParticles) ? sc2_->store.Capacity() : 0u;
    sc2_->hasModel.assign(models, 0);
    sc2_->modelPath.assign(models, 0);
    sc2_->modelPose.assign(models, Sc2ModelPose{});
    sc2_->modelDeaths.clear();
    // The triangle table is the desc's regions over the mesh, so a new desc
    // rebuilds it on the next tick.
    sc2_->meshTrianglesOf = nullptr;
}

void Emitter2::RefreshSc2MeshTriangles() {
    const EmitMesh* mesh = surface_.mesh.get();
    if (mesh == sc2_->meshTrianglesOf)
        return;
    sc2_->meshTrianglesOf = mesh;
    // The emitter's slot of retail's per-emitter-region table: the triangles of
    // the regions this `PAR_` names. The mesh has one sub per region of the
    // division, so a region id indexes it with no remap.
    sc2_->meshTriangles.clear();
    if (!mesh)
        return;
    for (const i32 region : desc_->sc2.emit.shapeRegions) {
        if (region < 0 || static_cast<usize>(region) >= mesh->subs.size())
            continue;
        const EmitMesh::SubMesh& sub = mesh->subs[static_cast<usize>(region)];
        for (u32 t = 0; t < sub.triCount; ++t)
            sc2_->meshTriangles.push_back({(sub.firstTri + t) * 3u, 0u});
    }
}

void Emitter2::RewindSc2() {
    // A model particle's child actor is released through the death hook
    // too, and one still waiting for its model has nothing to release —
    // it simply leaves the pending list.
    for (i32 node = sc2_->store.list.head; node >= 0;
         node = sc2_->store.list.next[static_cast<usize>(node)]) {
        if (static_cast<usize>(node) < sc2_->hasModel.size() &&
            sc2_->hasModel[static_cast<usize>(node)] != 0)
            OnParticleDied(static_cast<u32>(node));
    }
    std::fill(sc2_->hasModel.begin(), sc2_->hasModel.end(), u8{0});
    sc2_->modelDeaths.clear();
    if (sc2_->pending) {
        std::erase_if(*sc2_->pending, [rt = sc2_.get()](const Sc2PendingModel& entry) {
            return entry.runtime == rt;
        });
    }
    sc2_->Rearm(desc_->sc2);
}

// ---- the dialect-neutral SC2 surface -------------------------------------
// Each of these is a no-op while `sc2_` is null. That is not defensive: the
// service and the actor layer call them on every emitter they hold without
// asking what it is, which is the whole point of putting them on the base.

void Emitter2::QueueSpawnRequest(const SpawnRequest& req) {
    if (!sc2_)
        return;
    // Retail drops past the cap rather than growing the queue, and a dropped
    // request is a particle that never spawns — reproduce the loss.
    Sc2QueueSpawnRequest(sc2_->inbox, req);
}

void Emitter2::DrainSpawnRequests(std::vector<RoutedSpawnRequest>& out) {
    if (!sc2_ || sc2_->outbox.empty())
        return;
    out.insert(out.end(), sc2_->outbox.begin(), sc2_->outbox.end());
    sc2_->outbox.clear();
}

void Emitter2::QueueBurst(u32 slot, u32 count) {
    if (!sc2_ || slot >= sc2_->slots.size())
        return;
    sc2_->slots[slot].burst += count;
}

void Emitter2::SetSc2ActiveSequence(i32 sequence) {
    if (sc2_)
        Sc2NoteActiveSequence(*sc2_, desc_->sc2, sequence);
}

void Emitter2::AttachSc2PendingList(Sc2PendingModels* list) {
    sc2PendingList_ = list;
    if (sc2_)
        sc2_->pending = list ? list : &sc2_->ownPending;
}

void Emitter2::ServiceSc2PendingModel(i32 node) {
    if (!sc2_ || node < 0 || static_cast<usize>(node) >= sc2_->hasModel.size())
        return;
    const usize n = static_cast<usize>(node);
    const Sc2SpawnedElement& e = sc2_->store.elements[n];
    const bool randomDirection = desc_->sc2.Has(Sc2RotationBit::RandomDirection);
    Sc2PendingDraw draw;
    if (!Sc2PendingSpawnDraw(sc2_->rng, e.deathTime, sc2_->clock.emitterTime,
                             static_cast<u32>(desc_->childModelPaths.size()), randomDirection,
                             draw))
        return;
    sc2_->modelPath[n] = draw.pathIndex;
    if (draw.hasDirection) {
        // Into the lane the quad path calls `gpuVelocity`, with its inverse
        // mass zeroed in the same block — where retail writes it, and where a
        // type-3 pose reads its spin axis back.
        Sc2GpuVertex& v = sc2_->store.vertices[n];
        v.velocity[0] = draw.direction.x;
        v.velocity[1] = draw.direction.y;
        v.velocity[2] = draw.direction.z;
        v.invMass = 0.0f;
    }
    sc2_->hasModel[n] = 1;
    sc2_->modelPose[n] = Sc2PoseModelParticle(
        *sc2_, desc_->sc2, Sc2FromHostSpace(placement_.modelToWorld, sc2_->actorWorldScale), node);
    OnParticleBorn(static_cast<u32>(node));
}

void Emitter2::ApplySc2Frame(
    const model::FrameState::ParticleFrameState::Sc2ParticleFrame& frame) {
    // The whole `PAR_` sample set, copied once. Every EMIT and SPAWN input the
    // tick builds is read out of it, so a stage never reaches back into the
    // frame state and the tick can be driven from a test without one.
    sc2_->frame = frame;
    // A Bezier channel's control point, out of this frame's fresh samples.
    Sc2ConvertBezierKeys(sc2_->frame, desc_->sc2);
}

void Emitter2::SetSc2Scene(const Matrix44f& worldToView, f32 actorWorldScale) {
    if (!sc2_)
        return;
    const Sc2QuadCamera cam = Sc2CameraFromView(worldToView);
    // Retail's three camera rows are right, view direction, up; the pose's
    // type-0 basis is exactly them, so a model's +Y looks away from the eye
    // and its SC2 front, −Y, faces it.
    sc2_->camera = {cam.billboardRight, cam.direction, cam.billboardUp};
    sc2_->actorWorldScale = Sc2HostScale(actorWorldScale);
}

void Emitter2::TickSc2(f32 elapsed, f32 emissionScaler) {
    if (elapsed < 0.0f)
        elapsed = 0.0f;

    Sc2TickFrame f;
    // Milliseconds as an integer, because the time scale multiplies it while it
    // still is one — which is where a negative scale stops being a small
    // backwards step and becomes a 4.29-billion-ms one (RE §5.1).
    f.dtMs = static_cast<i32>(elapsed * sc2::kMsPerSec + 0.5f);
    sc2_->wallMs += f.dtMs;
    f.nowMs = sc2_->wallMs;
    f.frameIndex = ++sc2_->frameIndex;
    f.timeScale = 1.0f;
    f.emissionScaler = emissionScaler;
    f.modelPaused = false;
    f.quality = sc2::kViewerQuality;
    // The game's emission scale (`Model_Set*EmissionScale`), which a viewer
    // never sets — not `placement_.unitScale`. The world scale is already in the world
    // matrix, so feeding it here multiplied every count and every shape extent
    // by 100 (design §8).
    f.elemScaleX = 1.0f;
    f.worldMatrix = placement_.modelToWorld;
    f.boneMatrix = placement_.modelToWorld;
    f.worldPos = placement_.worldPos;
    // Renderer units per SC2 unit, which the tick takes off every host-space
    // input so the runtime runs in SC2 units; BUILD puts it back.
    f.hostScale = sc2_->actorWorldScale;
    f.surface = &surface_;
    RefreshSc2MeshTriangles();

    Sc2TickEmitter(*sc2_, desc_->sc2, f);

    // The children whose particles died holding one, in the order they died.
    for (const i32 node : sc2_->modelDeaths)
        OnParticleDied(static_cast<u32>(node));
    sc2_->modelDeaths.clear();

    // Not registered with a service, so nobody else walks this emitter's
    // pending models: it does, once its own frame is done.
    if (sc2_->pending == &sc2_->ownPending) {
        for (usize i = 0; i < sc2_->ownPending.size(); ++i)
            ServiceSc2PendingModel(sc2_->ownPending[i].node);
        sc2_->ownPending.clear();
    }
}

i32 Emitter2::BuildSc2Geometry(const Emitter2& e, const BuildGeometryInput& in,
                               std::vector<Vertex>& out) {
    if (!in.worldToView || e.sc2_->store.AliveCount() == 0)
        return 0;
    const std::size_t start = out.size();

    // The runtime's space: the transform without the host's world scale, as the
    // tick handed it to the simulation. The corners go back into the host's.
    const f32 hostScale = Sc2HostScale(e.sc2_->actorWorldScale);
    Sc2BatchFrame frame;
    frame.world = Sc2Mat16(Sc2FromHostSpace(e.placement_.modelToWorld, hostScale));
    frame.emitterTime = e.sc2_->clock.emitterTime;
    // No model-instancing node: `b_useModelInstancing` is a render-context
    // capability we do not turn on, so the instance transform stays identity.
    Sc2WriteQuadBatch(e.sc2_->batch, Sc2BatchDescFrom(e.desc_->sc2), frame);

    // Both UV arms are per texture SLOT and live on the MATERIAL, not on
    // `PAR_`. The flipbook one is resolved at load into the desc; the random
    // offset stays off because the `.m3` field behind it is unidentified
    // (RE §16.29) — a guessed source would shift every UV by a random byte
    // pair, which is far worse than not offsetting at all.
    const Sc2QuadFlags flags =
        Sc2QuadFlagsFrom(e.desc_->sc2, e.desc_->sc2.look.flipbookUv, false);
    const Sc2SortKey sort = !e.desc_->sc2.Has(ParticleFlag::Sort)         ? Sc2SortKey::None
                            : e.desc_->sc2.Has(ParticleFlag::SortHeight) ? Sc2SortKey::Height
                                                                        : Sc2SortKey::Depth;
    // The eye into SC2 units with everything else; the directions carry no scale.
    Sc2QuadCamera camera = Sc2CameraFromView(*in.worldToView);
    if (hostScale != 1.0f)
        camera.eye = {camera.eye.x / hostScale, camera.eye.y / hostScale, camera.eye.z / hostScale};
    Sc2BuildQuads(e.sc2_->store, e.sc2_->batch, camera, flags,
                  e.desc_->sc2.Has(ParticleFlag::SortReverse), out, sort, hostScale);
    return static_cast<i32>(out.size() - start);
}

} // namespace whiteout::flakes::renderer::particle
