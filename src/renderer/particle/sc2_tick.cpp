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
Sc2Overlay Overlay(const Sc2EmitterDesc& d, const Sc2Frame& s, i32 group) {
    Sc2Overlay o;
    o.type = d.emit.overlayType[group];
    o.amplitude = s.overlayAmp[group];
    o.frequency = s.overlayFreq[group];
    return o;
}

/// `M3_ComputeSkinnedRegionPositions` for one vertex, in model space: the rest
/// position through each influence's `invBind · pose`, weighted, stopping at
/// the first zero weight as retail's loop does and never renormalised. OP13
/// stubbed this, so the mesh's own skin is the port's and not a measurement.
Vector3f Sc2MeshVertex(void* ctx, u32 v) {
    const EmitSurface& surface = *static_cast<const EmitSurface*>(ctx);
    const EmitMesh& m = *surface.mesh;
    if (v >= m.rest.size())
        return {0.0f, 0.0f, 0.0f};
    const Vector3f& rest = m.rest[v];
    if (v >= m.bones.size() || v >= m.weights.size() || surface.pose.empty() ||
        surface.invBind.empty())
        return rest;
    Vector3f acc{0.0f, 0.0f, 0.0f};
    for (usize k = 0; k < kEmitMeshBones; ++k) {
        const f32 w = m.weights[v][k];
        if (!(w > 0.0f))
            break;
        const i32 b = m.bones[v][k];
        if (b < 0 || static_cast<usize>(b) >= surface.pose.size() ||
            static_cast<usize>(b) >= surface.invBind.size())
            continue;
        const Vector3f p = whiteout::transform_point(
            rest, surface.invBind[static_cast<usize>(b)] * surface.pose[static_cast<usize>(b)]);
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
    in.flattenXY = d.Has(Sc2RotationBit::FlattenVelocityXY);
    in.variation = {clock.variationTime, s.overlayPhase};
    in.yawOverlay = Overlay(d, s, sc2::OverlayGroup::Yaw);
    in.pitchOverlay = Overlay(d, s, sc2::OverlayGroup::Pitch);
    in.speedOverlay = Overlay(d, s, sc2::OverlayGroup::Speed);
    in.horizontalOverlay = Overlay(d, s, sc2::OverlayGroup::Horizontal);
    in.verticalOverlay = Overlay(d, s, sc2::OverlayGroup::Vertical);
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

/// What one `TickOnce` reads and what its stages hand each other. Built on
/// the stack per call; the buffers it fills are the runtime's scratch.
struct Sc2FrameCtx {
    Sc2FrameCtx(Sc2Runtime& runtime, const Sc2EmitterDesc& desc, const Sc2TickFrame& frame)
        : rt(runtime), d(desc), f(frame), s(runtime.frame) {}
    Sc2FrameCtx(const Sc2FrameCtx&) = delete; // the inputs below point into it

    Sc2Runtime& rt;
    const Sc2EmitterDesc& d;
    const Sc2TickFrame& f;
    const Sc2Frame& s;
    Sc2TickResult out;

    // PREP
    f32 startTime = 0.0f;
    Vector3f startPos{0, 0, 0};

    // EMIT
    usize slots = 1;
    Sc2Schedule sched;
    Vector3f posStep{0, 0, 0};

    // The inputs constant over the frame's events.
    Sc2VertexBodyInputs vb;
    Sc2SimulateInputs sim;
    Sc2GroundContext ground{nullptr, 1.0f};
    Sc2Collider collider;
    Sc2MeshRegionBase meshRegion{};
    Sc2MeshSurfaceInputs meshIn;
    bool haveMesh = false;
    bool modelParticles = false;
    Sc2PendingModels* pending = nullptr;
};

/// PREP: the clock. False when this frame already ticked, which leaves the
/// clock untouched.
bool PrepSc2(Sc2FrameCtx& c) {
    // The sweep's two endpoints have to be read BEFORE the clock advances:
    // `Sc2TickClock` overwrites `emitterTime` with the frame's end and may
    // refresh `prevPos`, and the batch is laid from where the frame STARTED to
    // where it ended. Recovering the start by subtracting `catchUp` afterwards
    // would not even be the same float.
    c.startTime = c.rt.clock.emitterTime;
    c.startPos = c.rt.clock.prevPos;

    c.out.plan = Sc2TickClock(c.rt.clock, ClockInputs(c.rt, c.f));
    if (!c.out.plan.ticked)
        return false;
    c.rt.curPos = c.f.worldPos;
    return true;
}

/// EMIT: each slot's count, the schedule of the frame's events, and the sweep
/// the batch is laid along.
void ScheduleSc2(Sc2FrameCtx& c) {
    Sc2Runtime& rt = c.rt;
    const Sc2EmitterDesc& d = c.d;
    const Sc2TickFrame& f = c.f;
    const Sc2Frame& s = c.s;
    Sc2TickScratch& scratch = rt.scratch;

    const usize slots = (std::max)(usize{1}, d.emit.slotBones.size());
    c.slots = slots;
    if (rt.slots.size() != slots)
        rt.slots.assign(slots, Sc2Runtime::Slot{});

    const f32 window = Sc2EmitWindow(c.out.plan.fullStep, c.out.plan.subDt, c.out.plan.dt);
    scratch.counts.assign(slots, 0.0f);
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
        ec.suppressed = (rt.emitFlagsWord & sc2::kEmitSuppressed) != 0;
        ec.nodeVisible = s.active;
        scratch.counts[i] = Sc2ComputeEmitCount(ec);
    }

    scratch.carry.assign(slots, 0.0f);
    scratch.targets.assign(slots, 0u);
    scratch.emitted.resize(slots);
    for (usize i = 0; i < slots; ++i)
        scratch.carry[i] = rt.slots[i].carry;

    Sc2ScheduleInputs si;
    si.fullStep = c.out.plan.fullStep;
    si.nSubSteps = c.out.plan.nSteps;
    si.subDt = c.out.plan.subDt;
    si.frameDt = c.out.plan.dt;
    si.catchUp = c.out.plan.catchUp;
    si.remainder = c.out.plan.remainder;
    si.accumTime = rt.accumTime;
    si.frozen = rt.emissionFrozen;
    si.anythingAlive = rt.store.AliveCount() != 0;
    si.haveRequests = !rt.inbox.empty();
    si.counts = scratch.counts;

    c.sched =
        Sc2SpawnSchedule(si, scratch.carry, scratch.targets, scratch.emitted, scratch.events);
    for (usize i = 0; i < slots; ++i) {
        rt.slots[i].carry = scratch.carry[i];
        rt.slots[i].target = scratch.targets[i];
        rt.slots[i].emitted = 0;
    }
    rt.accumTime = c.sched.accumTime;

    // The batch is laid along the frame: `spawnTimeStep` from the schedule,
    // `spawnPosStep` from the sweep, which also moves `prevPos` on for the next
    // frame. The clock touched `prevPos` only on a full step, so on the
    // sub-stepped path it is still `startPos`.
    Sc2SweepInputs swi;
    swi.fullStep = c.out.plan.fullStep;
    swi.nSubSteps = c.out.plan.nSteps;
    swi.total = c.sched.total;
    swi.prevPos = rt.clock.prevPos;
    swi.worldPos = f.worldPos;
    const Sc2Sweep sweep = Sc2SpawnSweep(swi);
    c.posStep = sweep.spawnPosStep;
    rt.clock.prevPos = sweep.prevPos;

    // `Sc2InitSpawned` advances these per element. The gate's fixture left
    // `curPos` at zero, so where the sweep BEGINS is a composition choice and
    // not a measurement: it begins where the frame did and, after `total`
    // elements, lands on where the emitter ended up.
    rt.initState.emitterTime = c.startTime;
    rt.initState.curPos = c.startPos;
}

/// The MOVE and SPAWN inputs that hold for every event of the frame.
void ArmEventsSc2(Sc2FrameCtx& c) {
    Sc2Runtime& rt = c.rt;
    const Sc2EmitterDesc& d = c.d;
    const Sc2TickFrame& f = c.f;
    const Sc2Frame& s = c.s;

    // The body the element's ONE stored vertex is built through. Constant for
    // the whole frame: every field of it is a load-time desc value or a host
    // one, so it is built once rather than per element.
    Sc2VertexBodyInputs& vb = c.vb;
    vb.drag = d.motion.drag;
    vb.gravity = d.motion.gravity3.z;
    vb.worldGravityScale = f.worldGravityScale;
    vb.instanceType = d.look.instanceType;
    vb.tailLength = d.look.tailLength;
    vb.instanceAngle = d.look.instanceAngle;
    // One emitter's constants are bound at row 0 — see `Sc2GpuVertex::batchIndex`.
    vb.batchIndex = 0;

    // The CPU step's emitter-wide inputs, the same for every sub-step of the
    // frame. A viewer has no scene wind object, so the sample stays zero and
    // the multiplier multiplies nothing.
    Sc2SimulateInputs& sim = c.sim;
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
    sim.rotationMidTime = d.look.midTime[sc2::MidChannel::Rotation];
    sim.rotationMidHold = d.look.midHold[sc2::MidChannel::Rotation];

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

    c.ground = Sc2GroundContext{f.surface ? &f.surface->groundQuery : nullptr,
                                Sc2HostScale(f.hostScale)};
    if (f.surface && f.surface->groundQuery) {
        c.collider.ctx = &c.ground;
        c.collider.terrain = &Sc2GroundContact;
    }
    // Everything the frame's sub-steps ask of the children, sent once MOVE is
    // done.
    rt.scratch.asked.collision.clear();
    rt.scratch.asked.trail.clear();
    // The scene switch is "is there anything to collide with". Objects stay
    // unqueried: a viewer has no other units for a particle to hit.
    sim.collisionEnabled = c.collider.terrain != nullptr;

    // The Mesh shape's surface, the same for every spawn of the frame. An
    // emitter pointed at no mesh gets none, and spawns at its origin drawing
    // nothing — as retail's does with no model asset.
    c.haveMesh =
        d.emit.shape == static_cast<u8>(Sc2SpawnShape::Mesh) && f.surface && f.surface->mesh;
    if (c.haveMesh) {
        c.meshIn.triangles = rt.meshTriangles;
        c.meshIn.faces = f.surface->mesh->tris;
        c.meshIn.regions = std::span<const Sc2MeshRegionBase>(&c.meshRegion, 1);
        c.meshIn.colorR = f.surface->mesh->colorR;
        c.meshIn.ctx = const_cast<EmitSurface*>(f.surface);
        c.meshIn.position = &Sc2MeshVertex;
    }

    // ModelParticles: each element waits in the pending list for its model,
    // and one that dies first is unregistered at the kill.
    c.modelParticles = d.Has(ParticleFlag::ModelParticles);
    c.pending = rt.pending != nullptr ? rt.pending : &rt.ownPending;
}

/// Every spawn event's `Sc2InitInputs` but the three that depend on the slot.
/// Reads the frame and the clock and draws nothing, so building it once ahead
/// of the events is the same inputs each event built for itself.
Sc2InitInputs SpawnInputsFor(Sc2FrameCtx& c) {
    const Sc2Runtime& rt = c.rt;
    const Sc2EmitterDesc& d = c.d;
    const Sc2TickFrame& f = c.f;
    const Sc2Frame& s = c.s;

    // One overlay clock for every sampler the spawn calls.
    const Sc2Variation variation{rt.clock.variationTime, s.overlayPhase};

    Sc2InitInputs in;
    in.shape = ShapeInputs(d, s);
    in.shape.mesh = c.haveMesh ? &c.meshIn : nullptr;
    in.velocity = VelocityInputs(d, s, rt.clock);
    in.color.keys = {s.colorBGRA[0], s.colorBGRA[1], s.colorBGRA[2]};
    in.color.randomKeys = {s.colorRandomBGRA[0], s.colorRandomBGRA[1], s.colorRandomBGRA[2]};
    in.color.randomEnable = d.emit.colorRandom;
    in.color.colorMidTime = d.look.midTime[sc2::MidChannel::Color];
    in.color.alphaMidTime = d.look.midTime[sc2::MidChannel::Alpha];
    in.color.alphaOverlay = Overlay(d, s, sc2::OverlayGroup::Alpha);
    in.color.variation = variation;

    in.size.keys = {s.size3.x, s.size3.y, s.size3.z};
    in.size.randomKeys = {s.sizeRandom3.x, s.sizeRandom3.y, s.sizeRandom3.z};
    in.size.randomEnable = d.emit.sizeRandom;
    in.size.sizeOverlay = Overlay(d, s, sc2::OverlayGroup::Size);
    in.size.instanceType = d.look.instanceType;
    in.size.instanceDistance = d.look.instanceDistance;
    in.size.variation = variation;

    in.rotation.keys = {s.rotation3.x, s.rotation3.y, s.rotation3.z};
    in.rotation.randomKeys = {s.rotationRandom3.x, s.rotationRandom3.y, s.rotationRandom3.z};
    in.rotation.randomEnable = d.emit.rotationRandom;
    in.rotation.relative = d.Has(Sc2RotationBit::Relative);
    in.rotation.rotationMidTime = d.look.midTime[sc2::MidChannel::Rotation];
    in.rotation.rotationOverlay = Overlay(d, s, sc2::OverlayGroup::Rotation);
    in.rotation.variation = variation;

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
    in.worldMatrix = f.worldMatrix;
    in.spawnPosStep = c.posStep;
    in.spawnTimeStep = c.sched.spawnTimeStep;
    in.smoothedPos = s.parentVelocityScale != 0.0f ? rt.curPos : Vector3f{0, 0, 0};
    in.inheritVelocityScale = s.parentVelocityScale;
    in.nowMs = static_cast<u32>(f.nowMs);
    in.shape.elemScale = {f.elemScaleX, f.elemScaleX, f.elemScaleX};
    return in;
}

/// MOVE: one `Update` event.
void MoveSc2(Sc2FrameCtx& c) {
    Sc2Runtime& rt = c.rt;
    // `Update`'s own pick (A8). The analytic emitter integrates nothing — the
    // vertex shader moves its particles from the birth state — so its whole
    // MOVE is the retirement; every other one takes a CPU sub-step.
    if (Sc2UseRetirePath(rt.clock.stateFlags, false)) {
        c.out.retired += Sc2RetireExpired(rt.store.list, rt.store.elements, rt.clock.emitterTime,
                                          rt.store.recycle);
        return;
    }
    std::vector<i32>& killed = rt.scratch.killed;
    c.sim.dt = c.out.plan.fullStep ? c.out.plan.dt : c.out.plan.subDt;
    c.sim.emitterTime = rt.clock.emitterTime;
    killed.clear();
    c.out.retired += Sc2SimulateParticles(rt.store.list, rt.store.elements, c.sim, c.collider,
                                          rt.rng, rt.scratch.asked,
                                          c.modelParticles ? &killed : nullptr)
                         .killed;
    if (!c.modelParticles)
        return;
    // `SimulateParticles` re-poses every element holding a model inside its
    // walk, after the move and before the kill test; one element's pose reads
    // nothing another's step writes, so posing after the walk is the same
    // poses.
    for (i32 node = rt.store.list.head; node >= 0;
         node = rt.store.list.next[static_cast<usize>(node)]) {
        if (static_cast<usize>(node) < rt.hasModel.size() &&
            rt.hasModel[static_cast<usize>(node)] != 0) {
            rt.modelPose[static_cast<usize>(node)] =
                Sc2PoseModelParticle(rt, c.d, c.f.worldMatrix, node);
        }
    }
    for (const i32 node : killed) {
        const usize n = static_cast<usize>(node);
        if (n < rt.hasModel.size() && rt.hasModel[n] != 0) {
            rt.modelDeaths.push_back(node);
            rt.hasModel[n] = 0;
        } else {
            Sc2SwapRemovePending(*c.pending, &rt, node);
        }
    }
}

/// SPAWN: one `Spawn` event, from @p base with the slot's own three fields.
void SpawnSc2(Sc2FrameCtx& c, const Sc2InitInputs& base, const Sc2EmitEvent& ev) {
    Sc2Runtime& rt = c.rt;
    const Sc2Frame& s = c.s;

    const usize slot = (std::min)(static_cast<usize>(ev.slot), c.slots - 1);
    Sc2InitInputs in = base;
    in.slot = static_cast<u32>(slot);
    // A `PARC` copy emits from its own bone. Without `hasBone` the initialiser
    // treats slot > 0 as slot 0 — every copy spawning on the emitter itself at
    // the emitter's size — which is what this line did until X5.
    const bool copy = slot > 0 && slot - 1 < s.slots.size();
    // The copy's bone arrives in host units, as the frame's did.
    in.boneMatrix =
        copy ? Sc2FromHostSpace(s.slots[slot - 1].boneWorld, c.f.hostScale) : c.f.boneMatrix;
    in.hasBone = copy;

    // `SpawnParticles` in the order OP8b pins: the inbox first, then this
    // pass's own count, every element tested against the ceiling in turn, and
    // initialised in the flushes retail makes. Every spawn call of the frame is
    // handed the waiting requests; the first one that initialises anything
    // consumes them, and one that cannot leaves them for the next.
    const u32 plain = static_cast<i32>(ev.count) > 0 ? ev.count : 0u;
    Sc2SpawnBatchInputs bi;
    bi.requests = static_cast<u32>(rt.inbox.size());
    bi.plain = plain;
    bi.elementCount = rt.store.AliveCount();
    bi.maxParticles = rt.store.Capacity();
    Sc2SpawnBatchPlan& batchPlan = rt.scratch.batchPlan;
    Sc2PlanSpawnBatch(bi, batchPlan);
    u32 requestsMade = 0;
    u32 plainMade = 0;
    std::vector<Sc2SpawnedElement>& batch = rt.scratch.batch;
    for (const Sc2SpawnFlush& fl : batchPlan.flushes) {
        batch.assign(fl.requests + fl.plain, Sc2SpawnedElement{});
        std::span<const SpawnRequest> requests;
        if (fl.requests != 0)
            requests = std::span<const SpawnRequest>(rt.inbox.data() + fl.requestBegin, fl.requests);
        Sc2InitSpawned(rt.rng, in, requests, rt.initState, batch);
        for (const Sc2SpawnedElement& e : batch) {
            // The plan stopped at the pool's own ceiling, so this cannot come
            // back empty.
            const i32 node = rt.store.Acquire();
            if (node < 0)
                break;
            rt.store.elements[static_cast<usize>(node)] = e;
            // `InitSpawnedParticles` appends the element to the batch and nulls
            // its instance; the model comes at the walk.
            if (c.modelParticles && static_cast<usize>(node) < rt.hasModel.size()) {
                rt.hasModel[static_cast<usize>(node)] = 0;
                c.pending->push_back({&rt, node});
            }
            // Retail writes the four vertices once, here, and on the analytic
            // path never touches them again — which is why `Sc2VertexBody` has
            // no second chance to correct anything. One stands for all four:
            // they differ only in the corner pair, and `Sc2BuildQuads`
            // supplies that.
            rt.store.vertices[static_cast<usize>(node)] = Sc2VertexBody(c.vb, e);
        }
        requestsMade += fl.requests;
        plainMade += fl.plain;
    }
    c.out.spawned += batchPlan.created;
    c.out.refused += plain - plainMade;
    if (batchPlan.requestsConsumed) {
        c.out.refused += bi.requests - requestsMade;
        rt.inbox.clear();
    }
    // The whole count is booked whether it was made or not: the shortfall is
    // never retried (RE §16.9).
    rt.slots[slot].emitted += plain;
}

/// What MOVE asked of the children leaves through the outbox, addressed by the
/// child's index. The service delivers it into the child's inbox, and the 128
/// cap applies THERE, where every parent's requests meet.
void RouteChildRequests(Sc2FrameCtx& c) {
    const Sc2ChildRequests& asked = c.rt.scratch.asked;
    for (const SpawnRequest& r : asked.collision)
        c.rt.outbox.push_back({c.d.children.collisionSpawnIndex, r});
    for (const SpawnRequest& r : asked.trail)
        c.rt.outbox.push_back({c.d.children.trailLinkIndex, r});
}

/// BUILD, CPU half. Retail rebuilds every live element's vertex in the render
/// build on this path (`BuildParticleQuadVertices_List`, OP11). It runs here
/// instead, once the frame's MOVE is done: it reads exactly the element state
/// MOVE left, and it keeps the geometry build `const`. Without it the vertex
/// the draw expands is the one written at spawn, and a simulated particle is
/// drawn where it was born for its whole life.
void RebuildCpuVertices(Sc2FrameCtx& c) {
    Sc2Runtime& rt = c.rt;
    const Sc2EmitterDesc& d = c.d;
    if (Sc2UseRetirePath(rt.clock.stateFlags, false))
        return;
    Sc2CpuVertexInputs cv;
    cv.instanceType = d.look.instanceType;
    cv.tailLength = d.look.tailLength;
    cv.instanceAngle = d.look.instanceAngle;
    cv.drag = d.motion.drag;
    cv.gravity = d.motion.gravity3.z;
    cv.gravityScale = c.f.worldGravityScale;
    cv.sizeMidTime = d.look.midTime[sc2::MidChannel::Size];
    cv.parFlags = static_cast<u32>(d.flags);
    cv.noise = (rt.emitFlagsWord & sc2::kEmitNoise) != 0;
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

/// One `Tick` from its once-per-frame test on: the clock, emission, the
/// sub-step events and the CPU vertex rebuild. The pre-roll check retail runs
/// ahead of all that is `Sc2TickEmitter`'s, which calls this once per pre-roll
/// block and once for the frame.
Sc2TickResult TickOnce(Sc2Runtime& rt, const Sc2EmitterDesc& d, const Sc2TickFrame& f) {
    Sc2FrameCtx c(rt, d, f);
    if (!PrepSc2(c))
        return c.out; // already ticked this frame; the clock is untouched
    ScheduleSc2(c);
    ArmEventsSc2(c);
    const Sc2InitInputs spawnBase = SpawnInputsFor(c);
    for (const Sc2EmitEvent& ev : rt.scratch.events) {
        if (ev.kind == Sc2EmitEventKind::Update)
            MoveSc2(c);
        else if (ev.kind == Sc2EmitEventKind::Spawn)
            SpawnSc2(c, spawnBase, ev);
        // `Count` and `PreEmit` mark the schedule's shape and do nothing here.
    }
    RouteChildRequests(c);
    RebuildCpuVertices(c);

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
    return c.out;
}

/// The frame with the host's world scale off its transforms and position. The
/// slot bones and the ground query are converted where they are read, off the
/// `hostScale` this keeps.
Sc2TickFrame InSc2Units(const Sc2TickFrame& host) {
    Sc2TickFrame f = host;
    const f32 k = Sc2HostScale(host.hostScale);
    if (k == 1.0f)
        return f;
    f.worldMatrix = Sc2FromHostSpace(host.worldMatrix, k);
    f.boneMatrix = Sc2FromHostSpace(host.boneMatrix, k);
    f.worldPos = {host.worldPos.x / k, host.worldPos.y / k, host.worldPos.z / k};
    return f;
}

} // namespace

Matrix44f Sc2FromHostSpace(const Matrix44f& m, f32 hostScale) {
    Matrix44f out = m;
    const f32 k = Sc2HostScale(hostScale);
    if (k == 1.0f)
        return out;
    for (usize r = 0; r < 4; ++r)
        for (usize c = 0; c < 3; ++c)
            out.data[r][c] = m.data[r][c] / k;
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
    // Everything below runs in SC2 units.
    const Sc2TickFrame f = InSc2Units(host);

    // The actor layer's ask becomes bit 31, which is the only thing `Tick`
    // reads.
    if (rt.preRollPending) {
        rt.clock.stateFlags |= sc2::kStateSequenceChanged;
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
        rt.clock.stateFlags |= sc2::kStateRestartBusy;
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
            ++out.preRollBlocks;
        }
        rt.clock.stateFlags &= ~static_cast<u32>(sc2::kStateRestartBusy);
    }

    // The frame itself. After a pre-roll its once-per-frame test returns at
    // once — the last block already stamped this frame — as retail's does.
    const Sc2TickResult frame = TickOnce(rt, d, f);
    out.plan = frame.plan;
    out.spawned += frame.spawned;
    out.retired += frame.retired;
    out.refused += frame.refused;

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
    const f32 unit = Sc2HostScale(rt.actorWorldScale);

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
    for (usize k = 0; k < sc2::MidChannel::kCount; ++k) {
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
    constexpr u8 kBezier = element::SmoothingMode::Bezier;
    // `sizeMidTime` for all three channels, as `UpdateAnimatedParams` passes it.
    const f32 t = d.look.midTime[element::MidChannel::Size];
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
