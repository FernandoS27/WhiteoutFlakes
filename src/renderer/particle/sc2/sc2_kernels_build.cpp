#include "renderer/particle/sc2/sc2_kernels_build.h"

#include "renderer/particle/sc2/sc2_emitter_desc.h"
#include "renderer/particle/sc2/sc2_kernel_math.h"
#include "renderer/particle/sc2/sc2_kernels_move.h"

#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::sc2 {

namespace {

using detail::kFreezeSpeedSq;
// `(a·b)` left to right is the grouping the CPU vertex builder's tail clamp and
// velocity test use (OP11), and it is `vs::Dot3`'s. The simulate step groups
// its own dot products differently and spells them out where they are.
using renderer::sc2::vs::Dot3;
using renderer::sc2::kRotationQuant;
using renderer::sc2::kSizeQuant;

} // namespace

namespace {

/// The floor and the reciprocal's numerator are two separate globals in the
/// binary; naming them keeps the comparison's operand honest.
using renderer::sc2::kDragFloor;
using renderer::sc2::kInvDragUnderFloor;

/// What the clamped-tail latch adds to `spawnOrigin.x`: not an epsilon, a
/// distance no tail reaches, so the particle stays in the long branch.
constexpr f32 kTailLatchOffset = 10000.0f;

void Store3(f32 (&dst)[3], const Vector3f& v) {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}

} // namespace

DragLanes ComputeDragLanes(f32 drag) {
    DragLanes out;
    out.drag = (std::max)(kDragFloor, drag);
    // The comparison is against the RAW drag, before the floor.
    out.invDrag = drag < kDragFloor ? kInvDragUnderFloor : 1.0f / drag;
    return out;
}

namespace {

/// `vInterpolator2.xyz` for one instance type.
///
/// Types 1/10 put the tail length in x and zero y,z; 2 the velocity; 3/4 the
/// authored angle triple; 7/8 the element's orientation; 9 its spawn origin;
/// everything else — billboards included — zero.
Vector3f InstanceVector(const VertexBodyInputs& in, const SpawnedElement& e) {
    switch (static_cast<InstanceType>(in.instanceType)) {
    case InstanceType::Tail:
    case InstanceType::Trail:
        return {in.tailLength, 0.0f, 0.0f};
    case InstanceType::FaceTravelDir:
        return e.velocity;
    case InstanceType::FaceWorldDir:
    case InstanceType::SingleAxis:
        return in.instanceAngle;
    case InstanceType::EmitterOriented:
    case InstanceType::PhysicsOriented:
        return e.orientVec;
    case InstanceType::Pinned:
        return e.spawnOrigin;
    default:
        return {0.0f, 0.0f, 0.0f};
    }
}

} // namespace

GpuVertex VertexBody(const VertexBodyInputs& in, const SpawnedElement& e) {
    GpuVertex v{};
    Store3(v.position, e.position);
    // The element's lane, which nothing writes and no shader path reads.
    v.positionW = 0;
    for (usize i = 0; i < 4; ++i)
        v.size[i] = e.size[i];
    for (usize i = 0; i < 3; ++i)
        v.color[i] = e.colorNodes[i];
    for (usize i = 0; i < 3; ++i)
        v.rotation[i] = e.rotation[i];
    v.flipbookRand = e.flipbookRand;
    v.birthTime = e.birthTime;
    v.deathTime = e.deathTime;

    const DragLanes drag = ComputeDragLanes(in.drag);
    v.drag = drag.drag;
    v.invDrag = drag.invDrag;
    v.batchIndex = in.batchIndex;

    // One 16-byte copy in the binary, which is why `invMass` travels with the
    // velocity instead of staying the 0 the pending-spawn path leaves.
    Store3(v.velocity, e.velocity);
    v.invMass = e.invMass;

    Store3(v.instanceVec, InstanceVector(in, e));
    v.gravityZ = in.worldGravityScale * in.gravity;

    Store3(v.noise, e.noiseVec);
    v.flipbookRandStart = e.flipbookRandStart;
    return v;
}

void CpuVertexBody(const CpuVertexInputs& in, SpawnedElement& e, GpuVertex& cache) {
    // A ONE-byte store: `HIBYTE(*(u32*)(sys+0x34C))` into element `0x80`, and the
    // four-byte lane then copied — so the upper three bytes are whatever the
    // element held. OP11's `hole` rows poison them to show it.
    cache.batchIndex = (cache.batchIndex & 0xFFFFFF00u) | (in.batchIndex & 0xFFu);

    if (in.gpuMotion) {
        const DragLanes drag = ComputeDragLanes(in.drag);
        cache.drag = drag.drag;
        cache.invDrag = drag.invDrag;
        Store3(cache.velocity, e.velocity);
        cache.invMass = e.invMass;
        cache.gravityZ = in.gravity * in.gravityScale;
    }

    const f32 elapsed = in.emitterTime - e.birthTime;
    const f32 age = whiteout::flakes::renderer::sc2::vs::Saturate(elapsed / (e.deathTime - e.birthTime));

    if (in.noise) {
        // The ramp HOLDS above the edge: past it the amplitude is simply full.
        const f32 ramp = age < in.noiseEdge ? age / in.noiseEdge : 1.0f;
        const f32 amp = in.noiseAmplitude * ramp;
        f32 s[3];
        whiteout::flakes::renderer::sc2::GlobalNoiseTable().Sample(
            elapsed * in.noiseFrequency, age * in.noiseCoherence + e.noisePhase, s);
        // Into the ELEMENT, then copied: on this path the element's cache is
        // the shader's input.
        e.noiseVec = {amp * s[0], amp * s[1], amp * s[2]};
    }

    // Types 5 and 6 read the terrain's vector field under the particle, and
    // their gravity lane becomes a has-field flag rather than a gravity.
    const auto terrainLanes = [&] {
        if (in.field) {
            f32 lanes[2];
            in.field(in.fieldCtx, e.position.x, e.position.y, lanes);
            cache.instanceVec[0] = lanes[0];
            cache.instanceVec[1] = lanes[1];
            cache.instanceVec[2] = e.position.y;
            cache.gravityZ = 1.0f;
        } else {
            cache.instanceVec[0] = 0.0f;
            cache.instanceVec[1] = 0.0f;
            cache.instanceVec[2] = 1.0f;
            cache.gravityZ = 0.0f;
        }
        cache.invMass = 1.0f;
    };

    switch (static_cast<InstanceType>(in.instanceType)) {
    case InstanceType::Tail:
    case InstanceType::Trail: {
        Store3(cache.velocity, e.velocity);
        // `.x` alone — the list builder leaves `.y/.z` as the cache had them.
        cache.instanceVec[0] = in.tailLength;
        if (Has(in.parFlags, ParticleFlag::ClampTailLength) && !in.gpuMotion) {
            const Vector3f d{e.position.x - e.spawnOrigin.x, e.position.y - e.spawnOrigin.y,
                             e.position.z - e.spawnOrigin.z};
            const f32 dist2 = Dot3(d, d);
            const f32 speed = std::sqrt(Dot3(e.velocity, e.velocity));
            f32 reach = in.tailLength * speed;
            if (!Has(in.parFlags, ParticleFlag::FixTailLengthOnCreation))
                reach = (std::max)(reach, in.tailLength);
            if (dist2 >= reach * reach) {
                // Not an epsilon: nothing writes `spawnOrigin.x` back, so this
                // pins the particle in the long branch for the rest of its
                // life — and destroys the spawn origin for anything else.
                e.spawnOrigin.x = e.position.x + kTailLatchOffset;
            } else {
                // A TIME, on the short branch only: the distance over the
                // speed, in units of the particle's size now — the same
                // two-segment knee the spawn size uses.
                const f32 k0 = static_cast<f32>(e.size[0]) / kSizeQuant;
                const f32 k1 = static_cast<f32>(e.size[1]) / kSizeQuant;
                const f32 k2 = static_cast<f32>(e.size[2]) / kSizeQuant;
                const f32 mid = in.sizeMidTime;
                const f32 sizeNow = age < mid ? k0 + (k1 - k0) * (age / mid)
                                              : k1 + (k2 - k1) * ((age - mid) / (1.0f - mid));
                cache.instanceVec[0] = std::sqrt(dist2) / (speed * sizeNow);
            }
        }
        break;
    }
    case InstanceType::FaceTravelDir:
        Store3(cache.instanceVec, e.velocity);
        break;
    case InstanceType::FaceWorldDir:
    case InstanceType::SingleAxis:
        Store3(cache.instanceVec, in.instanceAngle);
        break;
    case InstanceType::TerrainOriented:
        terrainLanes();
        Store3(cache.velocity, in.instanceAngle);
        break;
    case InstanceType::TerrainDirOriented: {
        terrainLanes();
        Store3(cache.velocity,
               Dot3(e.velocity, e.velocity) < kFreezeSpeedSq ? e.orientVec : e.velocity);
        // The tail length, not `instanceAngle.x`, and one dword store that zeroes
        // the other two keys and stops short of `flipbookRand`.
        e.rotation = {static_cast<u16>(static_cast<i32>(in.tailLength * kRotationQuant)), 0, 0};
        break;
    }
    case InstanceType::EmitterOriented:
    case InstanceType::PhysicsOriented:
        Store3(cache.instanceVec, e.orientVec);
        break;
    case InstanceType::Pinned:
        Store3(cache.instanceVec, e.spawnOrigin);
        break;
    default:
        // Type 0 writes nothing: the cache keeps whatever was last there.
        break;
    }

    // The one 112-byte run from the element.
    Store3(cache.position, e.position);
    for (usize i = 0; i < 4; ++i)
        cache.size[i] = e.size[i];
    for (usize i = 0; i < 3; ++i) {
        cache.color[i] = e.colorNodes[i];
        cache.rotation[i] = e.rotation[i];
    }
    cache.flipbookRand = e.flipbookRand;
    cache.birthTime = e.birthTime;
    cache.deathTime = e.deathTime;
    Store3(cache.noise, e.noiseVec);
    cache.flipbookRandStart = e.flipbookRandStart;
}

namespace {

/// `vRotation.w`'s byte pair: split at 256, each byte over 255 — not 256, so a
/// full byte shifts a whole tile.
constexpr f32 kUvByteSplit = 256.0f;
constexpr f32 kUvByteMax = 255.0f;

/// HLSL integer `/`: truncation toward zero, not C++'s — which agrees, but
/// only since C++11, and the shader's intent is worth spelling out.
i32 IDiv(i32 a, i32 b) {
    const i32 q = std::abs(a) / std::abs(b);
    return (a < 0) != (b < 0) ? -q : q;
}

i32 IMod(i32 a, i32 b) {
    return a - IDiv(a, b) * b;
}

} // namespace

Vector2f ParticleUv(const QuadInput& v, const i16 (&corner)[2], f32 age,
                       const QuadBatch& b, const QuadFlags& fl) {
    f32 u = static_cast<f32>(corner[0]) * 0.5f + 0.5f;
    // The V axis is flipped, which is why the corner order reads bottom-up.
    f32 vv = static_cast<f32>(corner[1]) * -0.5f + 0.5f;

    if (fl.flipbookUv) {
        const f32 mid = b.flipbookMidKeyTime;
        f32 cellF = 0.0f;
        // `<=`, as the shader spells it; equivalent to `<`, since both arms
        // return `flipbookFrames[1]` at `age == mid`. See SC2_PARTICLE_RE.md §17.4.
        if (age <= mid) {
            const f32 range = b.flipbookFrames[1] - b.flipbookFrames[0];
            cellF = b.flipbookFrames[0] + std::floor(range * (age / mid) + 0.5f);
        } else {
            const f32 range = b.flipbookFrames[2] - b.flipbookFrames[1];
            cellF = b.flipbookFrames[1] +
                    std::floor(range * ((age - mid) / (1.0f - mid)) + 0.5f);
        }
        i32 cell = static_cast<i32>(std::trunc(cellF));
        if (fl.randomFlipbookStart) {
            // The element's `flipbookRandStart` at +108, floored — a whole
            // number of cells, so two particles never land mid-frame.
            cell += static_cast<i32>(std::trunc(std::floor(v.flipbookRandStart)));
        }
        const f32 colsF = b.flipbookColumns == 0.0f ? 1.0f : b.flipbookColumns;
        const i32 cols = static_cast<i32>(std::trunc(colsF));
        const i32 cellX = IMod(cell, cols);
        const i32 cellY = IDiv(cell, cols);
        u = u * b.cellSize[0] + static_cast<f32>(cellX) * b.cellSize[0];
        vv = vv * b.cellSize[1] + static_cast<f32>(cellY) * b.cellSize[1];
    } else if (fl.uvRandomOffset) {
        // `vRotation.w` = the element's `flipbookRand` u16, split hi/lo and
        // divided by 255 — not 256, so a full byte shifts a whole tile.
        const f32 r = v.rotation[3];
        const f32 x = std::floor(r / kUvByteSplit);
        const f32 y = r - x * kUvByteSplit;
        u = u + x / kUvByteMax;
        vv = vv + y / kUvByteMax;
    }
    return {u, vv};
}

QuadResult ExpandQuad(const QuadInput& v, const QuadBatch& b,
                            const QuadCamera& cam, const QuadFlags& fl) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;
    QuadResult out;
    const auto type = static_cast<InstanceType>(fl.instanceType);
    out.supported = fl.instanceType <= static_cast<u32>(InstanceType::Trail);

    f32 inSize[4];
    for (usize i = 0; i < 4; ++i)
        inSize[i] = v.size[i] * renderer::sc2::kInvSizeQuant;
    inSize[3] = vs::Saturate(inSize[3]);
    f32 inRot[3];
    for (usize i = 0; i < 3; ++i)
        inRot[i] = v.rotation[i] * renderer::sc2::kInvRotationQuant;

    out.age = vs::Saturate((b.systemTime - v.birthTime) / (v.deathTime - v.birthTime));

    // The SCALAR overload broadcast into .xyz — so the size takes mode 4's
    // substitution variant, never the float3 plateau.
    namespace mid = renderer::sc2::MidChannel;
    out.size = vs::InterpolateValue(out.age, inSize[0], inSize[1], inSize[2], b.midKey[mid::Size],
                                    b.invMidKey[mid::Size], b.hold[mid::Size], fl.sizeInterp);

    // The shader's own copy of `Input`. `CalculatePositionAndVelocity` writes
    // two of its registers back, and the tail types add the noise into one.
    Vector3f position = v.position;
    Vector3f interp1 = v.velocity;
    Vector3f interp2 = v.instanceVec;
    if (fl.proceduralPosition) {
        AnalyticInputs step;
        step.position = position;
        step.velocity0 = v.velocity;
        step.invMass = v.invMass;
        step.birthTime = v.birthTime;
        step.deathTime = v.deathTime;
        step.drag = v.drag;
        step.invDrag = v.invDrag;
        step.gravityZ = v.gravityZ;
        step.systemTime = b.systemTime;
        step.instanceType = fl.instanceType;
        step.tailLength = interp2.x;
        step.size = out.size;
        step.fixedTailLength = fl.fixedTailLength;
        step.clampedTailLength = fl.clampedTailLength;
        const AnalyticStep st = StepAnalytic(step);
        position = st.position;
        if (type == InstanceType::FaceTravelDir) {
            interp2 = st.velocity;
        } else if (type == InstanceType::Tail || type == InstanceType::Pinned ||
                   type == InstanceType::Trail) {
            interp1 = st.velocity;
            interp2.x = st.tailLength;
        }
    }
    if (fl.localSpace)
        position = vs::MulPointMat4(position, b.prWorld);
    // Noise is added in WORLD space, after the local transform, for every
    // instance type — so a rotated emitter matrix never turns it.
    position = vs::Add(position, v.noise);

    const f32 angle =
        vs::InterpolateValue(out.age, inRot[0], inRot[1], inRot[2], b.midKey[mid::Rotation],
                             b.invMidKey[mid::Rotation], b.hold[mid::Rotation], fl.rotationInterp);
    const auto rgbOf = [&](usize k) {
        return Vector3f{v.color[k][0], v.color[k][1], v.color[k][2]};
    };
    const Vector3f rgb = vs::InterpolateValue3(
        out.age, rgbOf(0), rgbOf(1), rgbOf(2), b.midKey[mid::Color], b.invMidKey[mid::Color],
        b.hold[mid::Color], fl.colorInterp);
    out.color[0] = rgb.x;
    out.color[1] = rgb.y;
    out.color[2] = rgb.z;
    // The alpha carries its OWN mid key, not the RGB one.
    out.color[3] = vs::InterpolateValue(
        out.age, v.color[0][3], v.color[1][3], v.color[2][3], b.midKey[mid::Alpha],
        b.invMidKey[mid::Alpha], b.hold[mid::Alpha], fl.colorInterp);

    const f32 scale = b.elementScale;

    // The two camera-facing idioms disagree exactly on the particle's plane:
    // 5 and 6 negate with a `> 0` ternary, the 2/3/7/8 second pass multiplies
    // by `sign()` and so ZEROES the frame there (OP12). Both run on the
    // corner's final position.
    const auto faceCamera = [&](const Vector3f& p, QuadCorner& c, bool useSign) {
        const f32 d = -vs::Dot3(p, c.normal);
        const f32 dist = ((cam.eye.x * c.normal.x + cam.eye.y * c.normal.y) +
                          cam.eye.z * c.normal.z) +
                         1.0f * d;
        f32 sgn = 1.0f;
        if (useSign)
            sgn = dist > 0.0f ? 1.0f : (dist < 0.0f ? -1.0f : 0.0f);
        else if (!(dist > 0.0f))
            sgn = -1.0f;
        c.normal = vs::Scale(c.normal, sgn);
        c.tangent = vs::Scale(c.tangent, sgn);
        c.binormal = vs::Scale(c.binormal, sgn);
    };

    // `UnpackNormals` for 2/3/7/8. Type 7 in local space takes the emitter's
    // own rows; in world space it unpacks the byte-packed basis spawn wrote.
    const auto unpackNormals = [&](Vector3f& right, Vector3f& up, Vector3f& forward) {
        if (type == InstanceType::EmitterOriented) {
            if (fl.localSpace) {
                right = vs::Normalize3({b.prWorld[0], b.prWorld[1], b.prWorld[2]});
                up = vs::Normalize3({b.prWorld[4], b.prWorld[5], b.prWorld[6]});
            } else {
                const f32 ry = std::trunc(interp2.x / renderer::sc2::kPackHalf);
                const f32 rx = interp2.x - ry * renderer::sc2::kPackHalf;
                const f32 rz = std::trunc(interp2.y / renderer::sc2::kPackHalf);
                const f32 ux = interp2.y - rz * renderer::sc2::kPackHalf;
                const f32 uy = std::trunc(interp2.z / renderer::sc2::kPackHalf);
                const f32 uz = interp2.z - uy * renderer::sc2::kPackHalf;
                const auto unit = [](f32 c) { return ((c / kUvByteMax) * 2.0f) - 1.0f; };
                right = vs::Normalize3({unit(rx), unit(ry), unit(rz)});
                up = vs::Normalize3({unit(ux), unit(uy), unit(uz)});
            }
            forward = vs::Normalize3(vs::Cross3(right, up));
            return;
        }
        forward = vs::Normalize3(vs::Add(interp2, Vector3f{0.0f, renderer::sc2::kFacingYGuard, 0.0f}));
        right = vs::Normalize3(vs::Cross3(Vector3f{0.0f, 0.0f, 1.0f}, forward));
        up = vs::Normalize3(vs::Cross3(forward, right));
    };

    // The corner-invariant half of every arm, built once. Each expression
    // reads `interp1`/`interp2` as the procedural step left them, the camera
    // and the angle, and nothing a corner writes, so building it ahead of the
    // corners is the same bits the shader's per-vertex evaluation makes.
    //   right    the quad's x axis and the corner's tangent
    //   up       its y axis, the binormal's source — each arm says what it is
    //   forward  the rotation axis, or the frame's direction
    struct CornerFrame {
        Vector3f right{}, up{}, forward{}, normal{};
        vs::Mat3 m{};
        f32 vsize = 0.0f;
        Vector3f delta{}, centre{};
    } f;
    switch (type) {
    case InstanceType::SingleAxis:
        f.right = vs::Normalize3(vs::Cross3(interp2, cam.direction));
        f.forward = vs::Normalize3(vs::Cross3(f.right, interp2));
        f.m = vs::MakeRotation(angle, f.forward);
        f.normal = vs::Normalize3(vs::Cross3(f.right, f.forward));
        break;
    case InstanceType::FaceTravelDir:
    case InstanceType::FaceWorldDir:
    case InstanceType::EmitterOriented:
    case InstanceType::PhysicsOriented:
        // `forward` is the direction: the rotation axis here, and the normal
        // the second pass faces the camera with.
        unpackNormals(f.right, f.up, f.forward);
        f.m = vs::MakeRotation(angle, f.forward);
        break;
    case InstanceType::TerrainOriented: {
        // `up` is the projected direction, rotated.
        Vector3f projected = vs::Sub(interp1, vs::Scale(interp2, vs::Dot3(interp1, interp2)));
        const vs::Mat3 m = vs::MakeRotation(angle, interp2);
        if (vs::Dot3(projected, projected) < renderer::sc2::kTerrainProjectMinSq)
            projected = {1.0f, 0.0f, 0.0f};
        projected = vs::Normalize3(projected);
        const Vector3f right = vs::Cross3(projected, interp2);
        f.up = vs::MulVecMat3(projected, m);
        f.right = vs::MulVecMat3(right, m);
        f.normal = vs::Normalize3(vs::Cross3(f.right, f.up));
        break;
    }
    case InstanceType::TerrainDirOriented: {
        // No rotation at all: `rot.x` is a length SCALE here, which is what
        // the CPU builder's re-key of `rotation[0]` is for. `up` is the
        // projected direction, scaled.
        const f32 mag = vs::Length3(interp1);
        const Vector3f direction = vs::Normalize3(interp1);
        const Vector3f projected = vs::Normalize3(
            vs::Sub(direction, vs::Scale(interp2, vs::Dot3(direction, interp2))));
        f.right = vs::Cross3(projected, interp2);
        f.up = vs::Scale(projected, (std::max)(inRot[0], mag * inRot[0]));
        f.normal = vs::Normalize3(vs::Cross3(f.right, f.up));
        break;
    }
    case InstanceType::Tail:
    case InstanceType::Trail: {
        // `up` is the direction, scaled by the tail.
        Vector3f velocity = vs::Add(interp1, v.noise);
        // Taken BEFORE the local-to-world rotation.
        const f32 mag = vs::Length3(velocity);
        if (fl.localSpace)
            velocity = vs::MulVecMat4As3(velocity, b.prWorld);
        const Vector3f direction = vs::Normalize3(velocity);
        f.right = vs::Normalize3(vs::Cross3(cam.direction, direction));
        const f32 tail =
            fl.fixedTailLength ? interp2.x : (std::max)(interp2.x, mag * interp2.x);
        f.up = vs::Scale(direction, tail);
        f.vsize = scale * out.size;
        f.normal = vs::Normalize3(vs::Cross3(f.right, f.up));
        break;
    }
    case InstanceType::Pinned: {
        // The noise moved the head end only, so a noisy Pinned particle
        // lengthens its streak rather than displacing it.
        const Vector3f origin =
            fl.localSpace ? vs::MulPointMat4(interp2, b.prWorld) : interp2;
        f.delta = vs::Sub(position, origin);
        f.forward = vs::SafeNormalize(f.delta, Vector3f{1.0f, 0.0f, 0.0f});
        f.right = vs::Normalize3(vs::Cross3(cam.direction, f.forward));
        f.centre = vs::Scale(vs::Add(position, origin), 0.5f);
        f.normal = vs::Cross3(f.right, f.forward);
        break;
    }
    default:
        // The billboard: the quad is laid on the camera's axes, and the frame
        // is those axes rotated.
        f.m = vs::MakeRotation(angle, cam.direction);
        f.right = vs::MulVecMat3(cam.billboardRight, f.m);
        f.up = vs::MulVecMat3(cam.billboardUp, f.m);
        f.normal = vs::Normalize3(vs::Cross3(f.right, f.up));
        break;
    }

    for (usize k = 0; k < 4; ++k) {
        const f32 ox = static_cast<f32>(kCorners[k][0]);
        const f32 oy = static_cast<f32>(kCorners[k][1]);
        QuadCorner& c = out.corner[k];
        Vector3f p = position;

        switch (type) {
        case InstanceType::SingleAxis: {
            Vector3f off = vs::Add(vs::Scale(f.right, ox), vs::Scale(interp2, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::MulVecMat3(vs::Scale(off, out.size), f.m));
            c.normal = f.normal;
            c.tangent = f.right;
            c.binormal = f.forward;
            break;
        }
        case InstanceType::FaceTravelDir:
        case InstanceType::FaceWorldDir:
        case InstanceType::EmitterOriented:
        case InstanceType::PhysicsOriented: {
            Vector3f off = vs::Add(vs::Scale(f.right, ox), vs::Scale(f.up, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::MulVecMat3(vs::Scale(off, out.size), f.m));
            // The frame is the SECOND pass's, after the instance transform.
            break;
        }
        case InstanceType::TerrainOriented:
        case InstanceType::TerrainDirOriented: {
            Vector3f off = vs::Add(vs::Scale(f.right, ox), vs::Scale(f.up, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::Scale(off, out.size));
            c.normal = f.normal;
            c.tangent = f.right;
            c.binormal = f.up;
            faceCamera(p, c, false);
            break;
        }
        case InstanceType::Tail:
        case InstanceType::Trail: {
            const Vector3f off = vs::Add(vs::Scale(f.right, ox), vs::Scale(f.up, oy));
            p = vs::Add(p, vs::Scale(off, f.vsize));
            if (type == InstanceType::Trail)
                p = vs::Sub(p, vs::Scale(f.up, f.vsize));
            c.normal = f.normal;
            c.tangent = f.right;
            // Scaled by the tail: this binormal is NOT unit (1, 6, 10 only).
            c.binormal = vs::Scale(f.up, -1.0f);
            break;
        }
        case InstanceType::Pinned: {
            const f32 endScale = vs::Lerp(1.0f, inSize[3], oy * 0.5f + 0.5f);
            // No elementScale on this branch — the one type whose quad does not
            // follow the emitter's world scale — and the only trapezoid.
            const Vector3f off = vs::Add(vs::Scale(vs::Scale(f.right, ox * out.size), endScale),
                                         vs::Scale(f.delta, 0.5f * oy));
            p = vs::Add(f.centre, off);
            c.normal = f.normal;
            c.tangent = f.right;
            c.binormal = vs::Scale(f.forward, -1.0f);
            break;
        }
        default: {
            // The billboard, and the shader's final `else` for anything past
            // the eleven. Type 0 transforms the position BEFORE building its
            // quad and skips the common instance transform after.
            if (fl.modelInstancing)
                p = vs::MulPointMat4(p, b.instanceTransform);
            Vector3f off = vs::Add(vs::Scale(cam.billboardRight, ox),
                                   vs::Scale(cam.billboardUp, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::MulVecMat3(vs::Scale(off, out.size), f.m));
            c.normal = f.normal;
            c.tangent = f.right;
            c.binormal = vs::Scale(f.up, -1.0f);
            break;
        }
        }

        const bool billboard = !out.supported || type == InstanceType::Billboard;
        if (fl.modelInstancing && !billboard)
            p = vs::MulPointMat4(p, b.instanceTransform);

        if (type == InstanceType::FaceTravelDir || type == InstanceType::FaceWorldDir ||
            type == InstanceType::EmitterOriented ||
            type == InstanceType::PhysicsOriented) {
            c.normal = f.forward;
            c.tangent = f.right;
            c.binormal = f.up;
            faceCamera(p, c, true);
        }

        c.position = p;
        c.uv = ParticleUv(v, kCorners[k], out.age, b, fl);
    }
    return out;
}

namespace {

/// `p_v..._ElementScale_...y` — the largest row length of the world matrix.
/// `std::sqrt` for retail's `rsqrtss` + one Newton step, so the OP15 replay
/// gives this one lane a relative tolerance. See SC2_PARTICLE_RE.md §17.4.
f32 ElementScale(const std::array<f32, 16>& m) {
    // `z*z + (y*y + x*x)`, in that association — the binary adds the z term to
    // the already-summed pair, and float addition does not reassociate.
    const auto sq = [](f32 x, f32 y, f32 z) { return z * z + (y * y + x * x); };
    const f32 r0 = sq(m[0], m[1], m[2]);
    const f32 r1 = sq(m[4], m[5], m[6]);
    const f32 r2 = sq(m[8], m[9], m[10]);
    // Rows 0..2 only: neither the translation row nor the w column takes part,
    // which is what the `translate` and `wrow` vectors pin.
    const f32 mx = (std::max)(r2, (std::max)(r0, r1));
    // The zero guard is retail's and INERT under `std::sqrt`; kept because an
    // estimate would need it back. See SC2_PARTICLE_RE.md §17.4.
    return mx == 0.0f ? 0.0f : std::sqrt(mx);
}

} // namespace

void WriteQuadBatch(QuadBatch& row, const BatchDesc& d,
                       const BatchFrame& f) {
    if (!d.worldSpace)
        row.prWorld = f.world;

    row.instanceTransform = f.hasInstanceNode ? f.instanceTransform : renderer::sc2::kIdentityMat16;

    row.midKey = {d.sizeMidTime, d.colorMidTime, d.alphaMidTime, d.rotationMidTime};
    // A plain division, no floor and no guard: an authored mid time of 0
    // uploads an infinity. Measured (OP15). See SC2_PARTICLE_RE.md §17.4.
    for (usize i = 0; i < 4; ++i)
        row.invMidKey[i] = 1.0f / row.midKey[i];
    row.hold = {d.sizeMidHoldTime, d.colorMidHoldTime, d.alphaMidHoldTime,
                d.rotationMidHoldTime};

    row.systemTime = f.emitterTime;
    row.elementScale = ElementScale(f.world);

    if (d.flipbookColumns != 0 && d.flipbookRows != 0) {
        row.flipbookMidKeyTime = d.flipbookMidTime;
        row.flipbookColumns = static_cast<f32>(d.flipbookColumns);
        row.flipbookFrames = {static_cast<f32>(d.flipbookStartInitIndex),
                              static_cast<f32>(d.flipbookStartStopIndex),
                              static_cast<f32>(d.flipbookEndInitIndex)};
        row.cellSize = {d.flipbookColumnFraction, d.flipbookRowFraction};
    } else {
        // The shader's own comment calls the 1 a fix for an integer overflow
        // on the C++ side; the frames and the cell size are simply not written.
        row.flipbookMidKeyTime = 1.0f;
        row.flipbookColumns = 1.0f;
    }
}

} // namespace whiteout::flakes::renderer::particle::sc2
