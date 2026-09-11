#include "sc2_tick.h"

#include "renderer/sc2/sc2_element.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace whiteout::flakes::renderer::particle {

namespace {

using Sc2Frame = model::FrameState::ParticleFrameState::Sc2ParticleFrame;

/// What `Sc2GroundContact` needs: the host's query, and the scale between the
/// runtime's SC2 units and the host's.
struct Sc2GroundContext {
    const GroundQuery* query = nullptr;
    f32 hostScale = 1.0f;
};

/// `CollideParticle` against the host's ground query.
///
/// The host answers with a HEIGHT where retail sweeps a 0.05 sphere against
/// the physics scene, so a sub-step hits the surface where it crosses it going
/// down. The segment arrives in WORLD space — `Sc2SimulateParticles` takes a
/// local emitter's out through its matrix and brings the contact back itself,
/// through the adjugate retail uses — and the normal is the grid's. The step
/// adds the push-out and forces the time to 1, as it does for retail's contact.
bool Sc2GroundContact(void* ctx, const Vector3f& a, const Vector3f& b, Sc2Contact& out) {
    const Sc2GroundContext& ground = *static_cast<const Sc2GroundContext*>(ctx);
    const f32 k = ground.hostScale;
    f32 z = 0.0f;
    const f32 reach = std::fabs(a.z - b.z) + 1.0f;
    // The segment is in SC2 units and the ground in the host's: asked in the
    // host's, and the height brought back.
    if (!(*ground.query)(Vector3f{b.x * k, b.y * k, b.z * k}, reach * k, reach * k, z))
        return false;
    z = z / k;
    if (a.z < z || b.z >= z)
        return false;
    const f32 t = (a.z - z) / (a.z - b.z);
    out.hit = true;
    out.position = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, z};
    out.normal = {0.0f, 0.0f, 1.0f};
    out.toi = t;
    return true;
}

/// The nine overlay groups, in the order the runtime groups them: yaw, pitch,
/// speed, size, alpha, colour (dead), rotation, horizontal, vertical. Group 0
/// drives YAW and group 1 PITCH — the swap RE §11.5 records, kept because it is
/// the file's field names that are wrong and not the runtime's behaviour.
Sc2Overlay Overlay(const Sc2EmitterDesc& d, const Sc2Frame& s, usize group) {
    Sc2Overlay o;
    o.type = d.emit.overlayType[group];
    o.amplitude = s.overlayAmp[group];
    o.frequency = s.overlayFreq[group];
    return o;
}

Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector3f Scale(const Vector3f& v, f32 k) { return {v.x * k, v.y * k, v.z * k}; }

/// `M3_ComputeSkinnedRegionPositions` for one vertex, in model space: the rest
/// position through each influence's `invBind · pose`, weighted, stopping at
/// the first zero weight as retail's loop does and never renormalised. OP13
/// stubbed this, so the mesh's own skin is the port's and not a measurement.
Vector3f Sc2MeshVertex(void* ctx, u32 v) {
    const Sc2Runtime& rt = *static_cast<const Sc2Runtime*>(ctx);
    const EmitMesh& m = *rt.emitMesh;
    if (v >= m.rest.size())
        return {0.0f, 0.0f, 0.0f};
    const Vector3f& rest = m.rest[v];
    if (v >= m.bones.size() || v >= m.weights.size() || rt.emitPose.empty() ||
        rt.emitInvBind.empty())
        return rest;
    Vector3f acc{0.0f, 0.0f, 0.0f};
    for (usize k = 0; k < kEmitMeshBones; ++k) {
        const f32 w = m.weights[v][k];
        if (!(w > 0.0f))
            break;
        const i32 b = m.bones[v][k];
        if (b < 0 || static_cast<usize>(b) >= rt.emitPose.size() ||
            static_cast<usize>(b) >= rt.emitInvBind.size())
            continue;
        const Vector3f p = whiteout::transform_point(
            rest, rt.emitInvBind[static_cast<usize>(b)] * rt.emitPose[static_cast<usize>(b)]);
        acc = {acc.x + p.x * w, acc.y + p.y * w, acc.z + p.z * w};
    }
    return acc;
}

Sc2SpawnPosInputs ShapeInputs(const Sc2EmitterDesc& d, const Sc2Frame& s) {
    Sc2SpawnPosInputs in;
    in.shape = static_cast<Sc2SpawnShape>(d.emit.shape);
    in.cutout = d.Has(ParticleFlag::EmitShapeCutout);
    in.shapeOuter = s.shapeOuter;
    in.shapeInner = s.shapeInner;
    in.outerRadius = s.outerRadius;
    in.innerRadius = s.innerRadius;
    in.spline = s.splinePoints;
    in.splineLowerBound = s.splineLower;
    in.splineUpperBound = s.splineUpper;
    return in;
}

Sc2SpawnVelInputs VelocityInputs(const Sc2EmitterDesc& d, const Sc2Frame& s,
                                 const Sc2EmitClock& clock) {
    Sc2SpawnVelInputs in;
    in.velocityType = d.emit.velocityType;
    in.spawnYaw = s.yawDeg;
    in.spawnPitch = s.pitchDeg;
    in.spawnHorizontal = s.horizontal;
    in.spawnVertical = s.vertical;
    in.speed = s.speed;
    in.speedRandom = s.speedRandom;
    in.speedIsEndpoint = d.Has(ParticleAdditionalFlag::EmitSpeedRandomize);
    in.flattenXY = (static_cast<u32>(d.rotationFlags) & 8u) != 0;
    in.variationTime = clock.variationTime;
    in.variationPhase = s.overlayPhase;
    in.yawOverlay = Overlay(d, s, 0);
    in.pitchOverlay = Overlay(d, s, 1);
    in.speedOverlay = Overlay(d, s, 2);
    in.horizontalOverlay = Overlay(d, s, 7);
    in.verticalOverlay = Overlay(d, s, 8);
    return in;
}

Sc2ClockInputs ClockInputs(const Sc2Runtime& rt, const Sc2TickFrame& f) {
    Sc2ClockInputs ci;
    ci.dtMs = f.dtMs;
    ci.timeScale = f.timeScale;
    ci.subStepRate = rt.subStepRate;
    ci.timeOffset = rt.timeOffset;
    ci.nowMs = f.nowMs;
    ci.frameIndex = f.frameIndex;
    ci.worldPos = f.worldPos;
    ci.modelPaused = f.modelPaused;
    // Retail's `g_force60Hz` debug byte is 0 in a shipped client.
    ci.force60Hz = false;
    return ci;
}

/// One `Tick` from its once-per-frame test on: the clock, emission, the
/// sub-step events and the CPU vertex rebuild. The pre-roll check retail runs
/// ahead of all that is `Sc2TickEmitter`'s, which calls this once per pre-roll
/// block and once for the frame.
Sc2TickResult TickOnce(Sc2Runtime& rt, const Sc2EmitterDesc& d, const Sc2TickFrame& f) {
    Sc2TickResult out;
    const Sc2Frame& s = rt.frame;

    // The sweep's two endpoints have to be read BEFORE the clock advances:
    // `Sc2TickClock` overwrites `emitterTime` with the frame's end and may
    // refresh `prevPos`, and the batch is laid from where the frame STARTED to
    // where it ended. Recovering the start by subtracting `catchUp` afterwards
    // would not even be the same float.
    const f32 startTime = rt.clock.emitterTime;
    const Vector3f startPos = rt.clock.prevPos;

    // ------------------------------------------------------------- PREP
    out.plan = Sc2TickClock(rt.clock, ClockInputs(rt, f));
    if (!out.plan.ticked)
        return out; // already ticked this frame; the clock is untouched
    rt.curPos = f.worldPos;

    // ------------------------------------------------------------- EMIT
    const usize slots = (std::max)(usize{1}, d.emit.slotBones.size());
    if (rt.slots.size() != slots)
        rt.slots.assign(slots, Sc2Runtime::Slot{});

    const f32 window = Sc2EmitWindow(out.plan.fullStep, out.plan.subDt, out.plan.dt);
    std::vector<f32> counts(slots, 0.0f);
    for (usize i = 0; i < slots; ++i) {
        Sc2EmitCountInputs ec;
        ec.rate = (i == 0 ? s.emissionRate
                          : (i - 1 < s.slots.size() ? s.slots[i - 1].emissionRate : 0.0f)) *
                  f.emissionScaler;
        ec.dt = window;
        ec.timeScale = f.playerTimeScale;
        // What the squirt keys the playhead crossed this host frame owe. Not
        // spent here: retail re-reads the same crossed keys on every count it
        // makes until the playhead moves again (OP7b's early-out), so each of
        // a pre-roll's blocks sees them. `Sc2TickEmitter` clears it once the
        // host frame is done.
        ec.burst = static_cast<f32>(rt.slots[i].burst);
        ec.lodCut = d.emit.lodCut;
        ec.lodReduce = d.emit.lodReduce;
        ec.quality = f.quality;
        ec.elemScaleX = f.elemScaleX;
        ec.suppressed = (rt.emitFlagsWord & 0x20u) != 0;
        ec.nodeVisible = s.active;
        counts[i] = Sc2ComputeEmitCount(ec);
    }

    std::vector<f32> carry(slots);
    std::vector<u32> targets(slots, 0u);
    for (usize i = 0; i < slots; ++i)
        carry[i] = rt.slots[i].carry;

    Sc2ScheduleInputs si;
    si.fullStep = out.plan.fullStep;
    si.nSubSteps = out.plan.nSteps;
    si.subDt = out.plan.subDt;
    si.frameDt = out.plan.dt;
    si.catchUp = out.plan.catchUp;
    si.remainder = out.plan.remainder;
    si.accumTime = rt.accumTime;
    si.frozen = rt.emissionFrozen;
    si.anythingAlive = rt.store.AliveCount() != 0;
    si.haveRequests = !rt.inbox.empty();
    si.counts = counts;

    std::vector<Sc2EmitEvent> events;
    const Sc2Schedule sched = Sc2SpawnSchedule(si, carry, targets, events);
    for (usize i = 0; i < slots; ++i) {
        rt.slots[i].carry = carry[i];
        rt.slots[i].target = targets[i];
        rt.slots[i].emitted = 0;
    }
    rt.accumTime = sched.accumTime;
    out.events = events.size();

    // The batch is laid along the frame: `spawnTimeStep` from the schedule,
    // `spawnPosStep` from the same division. The full-step path leaves the
    // position step at zero — it has no sweep to distribute.
    Vector3f posStep{0, 0, 0};
    if (!out.plan.fullStep && sched.total != 0) {
        // The same reciprocal the schedule used for the time lane: retail
        // builds `1 / total` once and multiplies all four lanes by it.
        const f32 inv = Sc2RcpNewton(static_cast<f32>(sched.total));
        posStep = Scale(Sub(f.worldPos, startPos), inv);
    }

    // `Sc2InitSpawned` advances these per element. The gate's fixture left
    // `curPos` at zero, so where the sweep BEGINS is a composition choice and
    // not a measurement: it begins where the frame did and, after `total`
    // elements, lands on where the emitter ended up.
    rt.initState.emitterTime = startTime;
    rt.initState.curPos = startPos;

    // The body the element's ONE stored vertex is built through. Constant for
    // the whole frame: every field of it is a load-time desc value or a host
    // one, so it is built once rather than per element.
    Sc2VertexBodyInputs vb;
    vb.drag = d.motion.drag;
    vb.gravity = d.motion.gravity3.z;
    vb.worldGravityScale = f.worldGravityScale;
    vb.instanceType = d.look.instanceType;
    vb.tailLength = d.look.tailLength;
    vb.instanceAngle = d.look.instanceAngle;
    // One emitter's constants are bound at row 0 — see `Sc2GpuVertex::batchIndex`.
    vb.batchIndex = 0;

    // ------------------------------------------------------------- MOVE
    // The CPU step's emitter-wide inputs, the same for every sub-step of the
    // frame. A viewer has no scene wind object, so the sample stays zero and
    // the multiplier multiplies nothing.
    Sc2SimulateInputs sim;
    sim.gravity = d.motion.gravity3;
    sim.gravityScale = f.worldGravityScale;
    sim.windMultiplier = d.motion.windMultiplier;
    sim.parFlags = static_cast<u32>(d.flags);
    sim.instanceType = d.look.instanceType;
    sim.drag = d.motion.drag;
    sim.bounce = d.motion.bounce;
    sim.friction = d.motion.friction;
    sim.collisionDieBounce = d.motion.collisionDieBounce;
    sim.killRadius = d.motion.killRadius;
    // The kill radius is measured in the ELEMENTS' space: a local-space
    // emitter's particles sit relative to its own origin.
    sim.origin = d.emit.worldSpace ? f.worldPos : Vector3f{0.0f, 0.0f, 0.0f};
    sim.rotationSmoothing = d.look.rotationSmoothing;
    sim.rotationMidTime = d.look.midTime[3];
    sim.rotationMidHold = d.look.midHold[3];

    sim.worldMatrix = Sc2Mat16(f.worldMatrix);
    sim.worldSpace = d.emit.worldSpace;
    sim.trailRate = s.trailEmissionRate;
    // The children as the desc resolved them at load, so the step never looks
    // an emitter up. A collision child counts only when it is world-space.
    sim.collisionChild =
        d.children.collisionSpawnIndex >= 0 && d.children.collisionChildIsWorldSpace;
    sim.trailChild = d.children.trailLinkIndex >= 0;
    sim.collisionSpawnChance = d.children.collisionSpawnChance;
    sim.collisionSpawnMin = d.children.collisionSpawnMin;
    sim.collisionSpawnMax = d.children.collisionSpawnMax;
    sim.collisionSpawnEnergy = d.children.collisionSpawnEnergy;
    sim.splat = d.children.splatProjectorIndex != -1;
    sim.splatChance = d.children.splatChance;

    Sc2Collider collider;
    Sc2GroundContext ground{&rt.groundQuery, f.hostScale};
    if (rt.groundQuery) {
        collider.ctx = &ground;
        collider.terrain = &Sc2GroundContact;
    }
    // Everything the frame's sub-steps ask of the children, sent once MOVE is
    // done.
    Sc2ChildRequests asked;
    // The scene switch is "is there anything to collide with". Objects stay
    // unqueried: a viewer has no other units for a particle to hit.
    sim.collisionEnabled = collider.terrain != nullptr;

    // The Mesh shape's surface, the same for every spawn of the frame. An
    // emitter pointed at no mesh gets none, and spawns at its origin drawing
    // nothing — as retail's does with no model asset.
    const Sc2MeshRegionBase meshRegion{};
    Sc2MeshSurfaceInputs meshIn;
    const bool haveMesh =
        d.emit.shape == static_cast<u8>(Sc2SpawnShape::Mesh) && rt.emitMesh != nullptr;
    if (haveMesh) {
        meshIn.triangles = rt.meshTriangles;
        meshIn.faces = rt.emitMesh->tris;
        meshIn.regions = std::span<const Sc2MeshRegionBase>(&meshRegion, 1);
        meshIn.colorR = rt.emitMesh->colorR;
        meshIn.ctx = &rt;
        meshIn.position = &Sc2MeshVertex;
    }

    // ModelParticles: each element waits in the pending list for its model,
    // and one that dies first is unregistered at the kill.
    const bool modelParticles = d.Has(ParticleFlag::ModelParticles);
    Sc2PendingModels& pending = rt.pending != nullptr ? *rt.pending : rt.ownPending;
    std::vector<i32> killed;

    // ------------------------------------------------------- the events
    for (const Sc2EmitEvent& ev : events) {
        switch (ev.kind) {
        case Sc2EmitEventKind::Count:
        case Sc2EmitEventKind::PreEmit:
            break;

        case Sc2EmitEventKind::Update:
            // `Update`'s own pick (A8). The analytic emitter integrates
            // nothing — the vertex shader moves its particles from the birth
            // state — so its whole MOVE is the retirement; every other one
            // takes a CPU sub-step.
            if (Sc2UseRetirePath(rt.clock.stateFlags, false)) {
                out.retired += Sc2RetireExpired(rt.store.list, rt.store.elements,
                                                rt.clock.emitterTime, rt.store.recycle);
            } else {
                sim.dt = out.plan.fullStep ? out.plan.dt : out.plan.subDt;
                sim.emitterTime = rt.clock.emitterTime;
                killed.clear();
                out.retired += Sc2SimulateParticles(rt.store.list, rt.store.elements, sim,
                                                    collider, rt.rng, asked,
                                                    modelParticles ? &killed : nullptr)
                                   .killed;
                if (modelParticles) {
                    // `SimulateParticles` re-poses every element holding a
                    // model inside its walk, after the move and before the
                    // kill test; one element's pose reads nothing another's
                    // step writes, so posing after the walk is the same poses.
                    for (i32 node = rt.store.list.head; node >= 0;
                         node = rt.store.list.next[static_cast<usize>(node)]) {
                        if (static_cast<usize>(node) < rt.hasModel.size() &&
                            rt.hasModel[static_cast<usize>(node)] != 0) {
                            rt.modelPose[static_cast<usize>(node)] =
                                Sc2PoseModelParticle(rt, d, f.worldMatrix, node);
                        }
                    }
                    for (const i32 node : killed) {
                        const usize n = static_cast<usize>(node);
                        if (n < rt.hasModel.size() && rt.hasModel[n] != 0) {
                            rt.modelDeaths.push_back(node);
                            rt.hasModel[n] = 0;
                        } else {
                            Sc2SwapRemovePending(pending, &rt, node);
                        }
                    }
                }
            }
            break;

        case Sc2EmitEventKind::Spawn: {
            const usize slot = (std::min)(static_cast<usize>(ev.slot), slots - 1);
            Sc2InitInputs in;
            in.shape = ShapeInputs(d, s);
            in.shape.mesh = haveMesh ? &meshIn : nullptr;
            in.velocity = VelocityInputs(d, s, rt.clock);
            in.color.keys = {s.colorBGRA[0], s.colorBGRA[1], s.colorBGRA[2]};
            in.color.randomKeys = {s.colorRandomBGRA[0], s.colorRandomBGRA[1],
                                   s.colorRandomBGRA[2]};
            in.color.randomEnable = d.emit.colorRandom;
            in.color.colorMidTime = d.look.midTime[1];
            in.color.alphaMidTime = d.look.midTime[2];
            in.color.alphaOverlay = Overlay(d, s, 4);
            in.color.variationTime = rt.clock.variationTime;
            in.color.variationPhase = s.overlayPhase;

            in.size.keys = {s.size3.x, s.size3.y, s.size3.z};
            in.size.randomKeys = {s.sizeRandom3.x, s.sizeRandom3.y, s.sizeRandom3.z};
            in.size.randomEnable = d.emit.sizeRandom;
            in.size.sizeOverlay = Overlay(d, s, 3);
            in.size.instanceType = d.look.instanceType;
            in.size.instanceDistance = d.look.instanceDistance;
            in.size.variationTime = rt.clock.variationTime;
            in.size.variationPhase = s.overlayPhase;

            in.rotation.keys = {s.rotation3.x, s.rotation3.y, s.rotation3.z};
            in.rotation.randomKeys = {s.rotationRandom3.x, s.rotationRandom3.y,
                                      s.rotationRandom3.z};
            in.rotation.randomEnable = d.emit.rotationRandom;
            in.rotation.relative = (static_cast<u32>(d.rotationFlags) & 2u) != 0;
            in.rotation.rotationMidTime = d.look.midTime[3];
            in.rotation.rotationOverlay = Overlay(d, s, 6);
            in.rotation.variationTime = rt.clock.variationTime;
            in.rotation.variationPhase = s.overlayPhase;

            in.parFlags = static_cast<u32>(d.flags);
            in.additionalFlags = static_cast<u32>(d.additionalFlags);
            in.rotationFlags = static_cast<u32>(d.rotationFlags);
            in.instanceType = d.look.instanceType;
            in.emitFlagsWord = rt.emitFlagsWord;
            in.stateFlags = rt.clock.stateFlags;
            in.noiseCoherence = d.motion.noiseCoherence;
            in.mass = d.motion.mass;
            in.massRandom = d.motion.massRandom;
            in.lifetime = s.lifetime;
            in.lifetimeRandom = s.lifetimeRandom;
            in.trailChance = d.children.trailChance;
            in.flipbookColumns = d.look.flipbookColumns;
            in.flipbookRows = d.look.flipbookRows;
            in.hasChildEmitter1 = d.children.trailLinkIndex >= 0;
            in.slot = static_cast<u32>(slot);
            in.worldMatrix = f.worldMatrix;
            // A `PARC` copy emits from its own bone. Without `hasBone` the
            // initialiser treats slot > 0 as slot 0 — every copy spawning on
            // the emitter itself at the emitter's size — which is what this
            // line did until X5.
            const bool copy = slot > 0 && slot - 1 < s.slots.size();
            // The copy's bone arrives in host units, as the frame's did.
            in.boneMatrix = copy ? Sc2FromHostSpace(s.slots[slot - 1].boneWorld, f.hostScale)
                                 : f.boneMatrix;
            in.hasBone = copy;
            in.spawnPosStep = posStep;
            in.spawnTimeStep = sched.spawnTimeStep;
            in.smoothedPos = s.parentVelocityScale != 0.0f ? rt.curPos : Vector3f{0, 0, 0};
            in.inheritVelocityScale = s.parentVelocityScale;
            in.nowMs = static_cast<u32>(f.nowMs);
            in.shape.elemScale = {f.elemScaleX, f.elemScaleX, f.elemScaleX};
            in.velocity.position = {0, 0, 0};

            // `SpawnParticles` in the order OP8b pins: the inbox first, then
            // this pass's own count, every element tested against the ceiling
            // in turn, and initialised in the flushes retail makes. Every spawn
            // call of the frame is handed the waiting requests; the first one
            // that initialises anything consumes them, and one that cannot
            // leaves them for the next.
            const u32 plain = static_cast<i32>(ev.count) > 0 ? ev.count : 0u;
            Sc2SpawnBatchInputs bi;
            bi.requests = static_cast<u32>(rt.inbox.size());
            bi.plain = plain;
            bi.elementCount = rt.store.AliveCount();
            bi.maxParticles = rt.store.Capacity();
            const Sc2SpawnBatchPlan batchPlan = Sc2PlanSpawnBatch(bi);
            u32 requestsMade = 0;
            u32 plainMade = 0;
            std::vector<Sc2SpawnedElement> batch;
            for (const Sc2SpawnFlush& fl : batchPlan.flushes) {
                batch.assign(fl.requests + fl.plain, Sc2SpawnedElement{});
                Sc2InitInputs flushIn = in;
                if (fl.requests != 0) {
                    flushIn.requests = std::span<const SpawnRequest>(
                        rt.inbox.data() + fl.requestBegin, fl.requests);
                }
                Sc2InitSpawned(rt.rng, flushIn, rt.initState, batch);
                for (const Sc2SpawnedElement& e : batch) {
                    // The plan stopped at the pool's own ceiling, so this
                    // cannot come back empty.
                    const i32 node = rt.store.Acquire();
                    if (node < 0)
                        break;
                    rt.store.elements[static_cast<usize>(node)] = e;
                    // `InitSpawnedParticles` appends the element to the batch
                    // and nulls its instance; the model comes at the walk.
                    if (modelParticles && static_cast<usize>(node) < rt.hasModel.size()) {
                        rt.hasModel[static_cast<usize>(node)] = 0;
                        pending.push_back({&rt, node});
                    }
                    // Retail writes the four vertices once, here, and on the
                    // analytic path never touches them again — which is why
                    // `Sc2VertexBody` has no second chance to correct anything.
                    // Corner 0 stands for all four: they differ only in the
                    // corner pair, and `Sc2BuildQuads` supplies that.
                    rt.store.vertices[static_cast<usize>(node)] = Sc2VertexBody(vb, e)[0];
                }
                requestsMade += fl.requests;
                plainMade += fl.plain;
            }
            out.spawned += batchPlan.created;
            out.refused += plain - plainMade;
            if (batchPlan.requestsConsumed) {
                out.refused += bi.requests - requestsMade;
                rt.inbox.clear();
            }
            // The whole count is booked whether it was made or not: the
            // shortfall is never retried (RE §16.9).
            rt.slots[slot].emitted += plain;
            break;
        }
        }
    }

    // What MOVE asked of the children leaves through the outbox, addressed by
    // the child's index. The service delivers it into the child's inbox, and
    // the 128 cap applies THERE, where every parent's requests meet.
    out.childRequests = asked.collision.size() + asked.trail.size();
    for (const SpawnRequest& r : asked.collision)
        rt.outbox.push_back({d.children.collisionSpawnIndex, r});
    for (const SpawnRequest& r : asked.trail)
        rt.outbox.push_back({d.children.trailLinkIndex, r});

    // ------------------------------------------------ BUILD, CPU half
    // Retail rebuilds every live element's vertex in the render build on this
    // path (`BuildParticleQuadVertices_List`, OP11). It runs here instead, once
    // the frame's MOVE is done: it reads exactly the element state MOVE left,
    // and it keeps the geometry build `const`. Without it the vertex the draw
    // expands is the one written at spawn, and a simulated particle is drawn
    // where it was born for its whole life.
    if (!Sc2UseRetirePath(rt.clock.stateFlags, false)) {
        Sc2CpuVertexInputs cv;
        cv.instanceType = d.look.instanceType;
        cv.tailLength = d.look.tailLength;
        cv.instanceAngle = d.look.instanceAngle;
        cv.drag = d.motion.drag;
        cv.gravity = d.motion.gravity3.z;
        cv.gravityScale = f.worldGravityScale;
        cv.sizeMidTime = d.look.midTime[0];
        cv.parFlags = static_cast<u32>(d.flags);
        cv.noise = (rt.emitFlagsWord & 8u) != 0;
        cv.noiseAmplitude = d.motion.noiseAmplitude;
        cv.noiseFrequency = d.motion.noiseFrequency;
        cv.noiseCoherence = d.motion.noiseCoherence;
        cv.noiseEdge = d.motion.noiseEdge;
        cv.emitterTime = rt.clock.emitterTime;
        cv.gpuMotion = false;
        // One emitter's constants are bound at row 0, as at spawn.
        cv.batchIndex = 0;
        for (i32 node = rt.store.list.head; node >= 0;
             node = rt.store.list.next[static_cast<usize>(node)]) {
            Sc2CpuVertexBody(cv, rt.store.elements[static_cast<usize>(node)],
                             rt.store.vertices[static_cast<usize>(node)]);
        }
    }

    // A frame that ran no `Update` retires NOTHING, and that is retail's
    // behaviour rather than an oversight: the retirement lives inside the
    // sub-step loop, and `nSteps` is legitimately zero on a frame shorter than
    // one sub-step. Those particles stay linked for one more frame and the
    // shader clamps their age — the shipped `Particle.fx` says so out loud,
    // "to fix the fact that dead particles can still render!".
    //
    // A defensive retirement here would also be indistinguishable from the one
    // inside the loop, which is exactly how a wrong argument to the real one
    // survived a mutation sweep before this comment replaced it.
    return out;
}

/// The frame with the host's world scale off its transforms and position. The
/// slot bones and the ground query are converted where they are read, off the
/// `hostScale` this keeps.
Sc2TickFrame InSc2Units(const Sc2TickFrame& host) {
    Sc2TickFrame f = host;
    const f32 k = host.hostScale;
    if (k == 1.0f || !(k > 0.0f))
        return f;
    f.worldMatrix = Sc2FromHostSpace(host.worldMatrix, k);
    f.boneMatrix = Sc2FromHostSpace(host.boneMatrix, k);
    f.worldPos = {host.worldPos.x / k, host.worldPos.y / k, host.worldPos.z / k};
    return f;
}

} // namespace

Matrix44f Sc2FromHostSpace(const Matrix44f& m, f32 hostScale) {
    Matrix44f out = m;
    if (hostScale == 1.0f || !(hostScale > 0.0f))
        return out;
    for (usize r = 0; r < 4; ++r)
        for (usize c = 0; c < 3; ++c)
            out.data[r][c] = m.data[r][c] / hostScale;
    return out;
}

void Sc2NoteActiveSequence(Sc2Runtime& rt, const Sc2EmitterDesc& d, i32 sequence) {
    if (!d.Has(ParticleFlag::SimulateInit) || sequence == rt.activeSequence)
        return;
    rt.activeSequence = sequence;
    rt.preRollPending = true;
}

f32 Sc2PreRollPeakFor(const Sc2EmitterDesc& d, i32 sequence) {
    // A sequence past the columns needs more sequences than containers; retail
    // would read the next row there, and the init value stands in.
    const auto& peaks = d.emit.preRollPeaks;
    if (sequence < 0 || static_cast<usize>(sequence) >= peaks.size())
        return d.emit.preRollInit;
    return peaks[static_cast<usize>(sequence)];
}

Sc2TickResult Sc2TickEmitter(Sc2Runtime& rt, const Sc2EmitterDesc& d,
                             const Sc2TickFrame& host) {
    namespace bits = whiteout::flakes::renderer::sc2;
    // Everything below runs in SC2 units.
    const Sc2TickFrame f = InSc2Units(host);

    // The actor layer's ask becomes bit 31, which is the only thing `Tick`
    // reads.
    if (rt.preRollPending) {
        rt.clock.stateFlags |= bits::kStateSequenceChanged;
        rt.preRollPending = false;
    }

    Sc2TickResult out;
    // `Tick`'s first act, ahead of its once-per-frame test.
    const bool neverTicked = rt.clock.lastFrameIndex < 0;
    const Sc2RestartCheck restart = Sc2TickRestartCheck(rt.clock, ClockInputs(rt, f));
    // An emitter that has never ticked has, for the pre-roll, been waiting
    // forever: retail's session clock is already large when an emitter is
    // created, so its gap is the whole session. This wall clock starts with
    // the emitter, and would never show more than one frame.
    if (restart.armed && (restart.owed || neverTicked)) {
        const u32 gap = neverTicked ? 0xFFFFFFFFu : restart.gapMs;
        // `EmitBurst` returns before its first block while no sequence has
        // resolved.
        const Sc2PreRollPlan plan =
            rt.activeSequence < 0 ? Sc2PreRollPlan{}
                                  : Sc2PlanPreRoll(Sc2PreRollPeakFor(d, rt.activeSequence), gap);
        rt.clock.stateFlags |= bits::kStateRestartBusy;
        for (u32 k = 0; k < plan.blocks; ++k) {
            // `EmitBurst`: the frame index forced back so the block ticks,
            // and the offset pushed on first — so the emitter clock runs ahead
            // of the scene by the whole pre-roll from here on.
            --rt.clock.lastFrameIndex;
            rt.timeOffset = rt.timeOffset + kSc2PreRollOffsetStep;
            Sc2TickFrame block = f;
            block.dtMs = kSc2PreRollBlockMs;
            // Design §8: retail leaves `timeOffset` in the register this
            // argument is read from.
            block.timeScale = 1.0f;
            const Sc2TickResult r = TickOnce(rt, d, block);
            out.spawned += r.spawned;
            out.retired += r.retired;
            out.refused += r.refused;
            out.childRequests += r.childRequests;
            out.events += r.events;
            ++out.preRollBlocks;
        }
        rt.clock.stateFlags &= ~static_cast<u32>(bits::kStateRestartBusy);
    }

    // The frame itself. After a pre-roll its once-per-frame test returns at
    // once — the last block already stamped this frame — as retail's does.
    const Sc2TickResult frame = TickOnce(rt, d, f);
    out.plan = frame.plan;
    out.spawned += frame.spawned;
    out.retired += frame.retired;
    out.refused += frame.refused;
    out.childRequests += frame.childRequests;
    out.events += frame.events;

    // The squirt bursts were owed to every count this host frame made.
    for (Sc2Runtime::Slot& slot : rt.slots)
        slot.burst = 0;
    return out;
}

Sc2ModelPose Sc2PoseModelParticle(const Sc2Runtime& rt, const Sc2EmitterDesc& d,
                                  const Matrix44f& world, i32 node) {
    const usize n = static_cast<usize>(node);
    const Sc2SpawnedElement& e = rt.store.elements[n];
    const Sc2GpuVertex& vert = rt.store.vertices[n];
    const f32 unit = rt.actorWorldScale > 0.0f ? rt.actorWorldScale : 1.0f;

    Sc2ModelPoseInputs in;
    in.position = e.position;
    in.velocity = e.velocity;
    in.orientVec = e.orientVec;
    in.spawnOrigin = e.spawnOrigin;
    in.randomDirection = {vert.velocity[0], vert.velocity[1], vert.velocity[2]};
    in.birthTime = e.birthTime;
    in.deathTime = e.deathTime;
    for (usize k = 0; k < 3; ++k) {
        in.elementSize[k] = e.size[k];
        in.elementRotation[k] = e.rotation[k];
        in.elementColors[k] = e.colorNodes[k];
    }

    in.world = Sc2Mat16(world);
    in.emitterTime = rt.clock.emitterTime;
    in.stateFlags = rt.clock.stateFlags;
    const auto& s = rt.frame;
    in.sizeKeys = {s.size3.x, s.size3.y, s.size3.z};
    in.rotationKeys = {s.rotation3.x, s.rotation3.y, s.rotation3.z};
    in.colorKeys = {s.colorBGRA[0], s.colorBGRA[1], s.colorBGRA[2]};

    in.parFlags = static_cast<u32>(d.flags);
    in.additionalFlags = static_cast<u32>(d.additionalFlags);
    in.rotationFlags = static_cast<u32>(d.rotationFlags);
    in.instanceType = d.look.instanceType;
    in.sizeSmoothing = d.look.sizeSmoothing;
    in.colorSmoothing = d.look.colorSmoothing;
    in.rotationSmoothing = d.look.rotationSmoothing;
    for (usize k = 0; k < 4; ++k) {
        in.midTime[k] = d.look.midTime[k];
        in.midHold[k] = d.look.midHold[k];
    }
    in.instanceAngle = d.look.instanceAngle;
    in.tailLength = d.look.tailLength;
    in.legacyOrient = d.children.modelOrientLegacy;
    in.orientVariant = d.children.modelOrientVariant;
    in.camera = rt.camera;

    Sc2ModelPose pose = Sc2ModelParticlePose(in);
    pose.position = {pose.position.x * unit, pose.position.y * unit, pose.position.z * unit};
    return pose;
}

void Sc2SwapRemovePending(Sc2PendingModels& list, const Sc2Runtime* runtime, i32 node) {
    for (usize i = 0; i < list.size(); ++i) {
        if (list[i].runtime == runtime && list[i].node == node) {
            list[i] = list.back();
            list.pop_back();
            return;
        }
    }
}

void Sc2ConvertBezierKeys(model::FrameState::ParticleFrameState::Sc2ParticleFrame& s,
                          const Sc2EmitterDesc& d) {
    namespace element = whiteout::flakes::renderer::sc2;
    constexpr u8 kBezier = 2;
    // `sizeMidTime` for all three channels, as `UpdateAnimatedParams` passes it.
    const f32 t = d.look.midTime[0];
    const auto convert = [t](Vector3f& keys) {
        f32 k[3] = {keys.x, keys.y, keys.z};
        element::ConvertColorNode(k, t);
        keys.y = k[1];
    };
    if (d.look.sizeSmoothing == kBezier) {
        convert(s.size3);
        convert(s.sizeRandom3);
    }
    if (d.look.rotationSmoothing == kBezier) {
        convert(s.rotation3);
        convert(s.rotationRandom3);
    }
    if (d.look.colorSmoothing == kBezier) {
        s.colorBGRA[1] =
            element::ConvertColorNode3(s.colorBGRA[0], s.colorBGRA[1], s.colorBGRA[2], t);
        // The randoms are sampled, and converted, only with random colour on.
        if (d.emit.colorRandom) {
            s.colorRandomBGRA[1] = element::ConvertColorNode3(
                s.colorRandomBGRA[0], s.colorRandomBGRA[1], s.colorRandomBGRA[2], t);
        }
    }
}

} // namespace whiteout::flakes::renderer::particle
