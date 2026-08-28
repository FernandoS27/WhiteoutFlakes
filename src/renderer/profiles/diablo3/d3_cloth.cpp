#include "renderer/profiles/diablo3/d3_cloth.h"

#include "renderer/animation/anim_math.h"

#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace whiteout::flakes::renderer::profiles::diablo3 {
namespace {

using ::whiteout::flakes::renderer::animation::IPoseStage;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::renderer::model::FrameState;

/// The client's fixed cadence, and the fixed point of its own dt filter.
constexpr f32 kFixedDt = 1.0f / 60.0f;

/// The bare literal added to the scaled `.clt` gravity. Not derivable from it,
/// and nothing to do with the rigid world's `-32.2`.
constexpr f32 kGravityBias = -43.2f;

/// `flInvMass` is authored against a reference mass; this is the divisor the
/// client rescales by, `1 / (flMass * 0.031056)`.
constexpr f32 kMassScale = 0.031056f;

/// `2 * 0.001215 * drag` is the aero pressure constant. The 0.001215 is the
/// same literal the rigid path's wind force uses, so it is an air density and
/// not a per-system fudge.
constexpr f32 kAirDensity = 0.001215f;

/// Cloth is not built below this mass — `ClothInstance_Init`'s first test.
constexpr f32 kMinMass = 1.0e-6f;

/// Squared length below which an accumulated quaternion is treated as
/// collapsed and last frame's is kept instead.
constexpr f32 kTinyQuatSq = 1.1921e-7f;

/// A basis row this short is not a rotation.
constexpr f32 kTinyScale = 1.0e-8f;

/// The plane pass keeps every vertex this far outside its plane.
constexpr f32 kPlaneSkin = 0.01f;

/// Past this squared displacement in one step the skin blend is forced to 1 —
/// the client's "the actor teleported, do not drag the cape across the map".
constexpr f32 kSnapDisplacementSq = 1.0f;

// ---------------------------------------------------------------------------
// Quaternions, in D3's convention
// ---------------------------------------------------------------------------
//
// `q x (0,v) x conj(q)` rotates `v` by `q`, which is also what `v * M` does for
// a row-vector matrix whose 3x3 is the transpose of the column basis — so
// @ref DecomposeFrame below and these two agree by construction. The one place
// the other convention appears is `conj(q) x (0,v) x q`, which the client uses
// exactly once, to undo a driving bone's previous frame.

struct Quat {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Conj(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

f32 Dot4(const Quat& a, const Quat& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

Quat Normalized(const Quat& q) {
    const f32 n2 = Dot4(q, q);
    if (n2 <= 0.0f)
        return {};
    const f32 inv = 1.0f / std::sqrt(n2);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Quat AsQuat(const Vector4f& v) {
    return {v.x, v.y, v.z, v.w};
}

Vector4f AsVec4(const Quat& q) {
    return {q.x, q.y, q.z, q.w};
}

Vector3f Rotate(const Quat& q, const Vector3f& v) {
    const f32 tx = 2.0f * (q.y * v.z - q.z * v.y);
    const f32 ty = 2.0f * (q.z * v.x - q.x * v.z);
    const f32 tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

/// @brief Rotate by the inverse. `conj(q) x (0,v) x q`.
Vector3f RotateInverse(const Quat& q, const Vector3f& v) {
    return Rotate(Conj(q), v);
}

/// @brief A uniform-scale rigid frame pulled out of a bone or skin matrix.
///
/// The same extraction `d3_physics.cpp` uses and for the same reason: reading
/// the 3x3 without transposing hands back the **conjugate**, which is stable,
/// plausible, and wrong for every asymmetric pose.
struct Frame {
    Quat rotation;
    Vector3f translation{0.0f, 0.0f, 0.0f};
    f32 scale = 1.0f;
};

Frame DecomposeFrame(const Matrix44f& m) {
    Frame out;
    out.translation = {m.data[3][0], m.data[3][1], m.data[3][2]};

    f32 n[3];
    for (int i = 0; i < 3; ++i) {
        n[i] = std::sqrt(m.data[i][0] * m.data[i][0] + m.data[i][1] * m.data[i][1] +
                         m.data[i][2] * m.data[i][2]);
    }
    out.scale = (std::min)({n[0], n[1], n[2]});
    if (n[0] <= kTinyScale || n[1] <= kTinyScale || n[2] <= kTinyScale) {
        out.scale = 1.0f;
        return out;
    }

    f32 r[3][3];
    for (int i = 0; i < 3; ++i) {
        const f32 inv = 1.0f / n[i];
        for (int j = 0; j < 3; ++j)
            r[j][i] = m.data[i][j] * inv;
    }

    const f32 trace = r[0][0] + r[1][1] + r[2][2];
    Quat q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25f * s};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const f32 s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q = {0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s};
    } else if (r[1][1] > r[2][2]) {
        const f32 s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q = {(r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q = {(r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s, (r[1][0] - r[0][1]) / s};
    }
    out.rotation = Normalized(q);
    return out;
}

// ---------------------------------------------------------------------------
// Small vector helpers
// ---------------------------------------------------------------------------

Vector3f Add(const Vector3f& a, const Vector3f& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3f Scale(const Vector3f& a, f32 s) {
    return {a.x * s, a.y * s, a.z * s};
}
f32 Dot3(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
f32 LengthSq(const Vector3f& a) {
    return Dot3(a, a);
}
Vector3f Cross3(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// @brief The client's reciprocal square root: `0x5F3759DF` and one Newton
///        step, spelled out in the capsule pass rather than called.
///
/// Kept as the shipped approximation instead of `1/std::sqrt` because it is the
/// one piece of arithmetic in this file that *can* be reproduced exactly on any
/// ISA — the rest of the binary is an ARM64 build whose `FRSQRTE` has no x86
/// equivalent, so a bit-exact claim is unavailable in principle (plan §10). Its
/// relative error is about 1.75e-3, which is a fifth of a millimetre on a
/// character-sized capsule and visible in nothing.
f32 FastRsqrt(f32 x) {
    i32 bits;
    std::memcpy(&bits, &x, sizeof(bits));
    bits = 1597463007 - (bits >> 1);
    f32 y;
    std::memcpy(&y, &bits, sizeof(y));
    return (1.5f + (x * -0.5f) * y * y) * y;
}

f32 Clamp01(f32 v) {
    if (!(v >= 0.0f))
        return 0.0f;
    return (std::min)(v, 1.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// `.clt` -> params
// ---------------------------------------------------------------------------

D3ClothParams D3ClothParamsOf(const d3n::Cloth& clt) {
    D3ClothParams p;
    p.relaxIterations = clt.dwRelaxIterations;
    p.mass = clt.flMass;
    p.skinBlendRate = clt.flSkinBlendRate;
    p.stretchStiffness0 = clt.flStretchStiffness0;
    p.stretchStiffness1 = clt.flStretchStiffness1;
    p.bendStiffness = clt.flBendStiffness;
    p.dragCoefficient = clt.flDragCoefficient;
    // The client stores `flGravity * 60 * 60` — units/tick^2 at 60 ticks/s
    // converted to units/s^2 — and adds the `-43.2` bias per step, not here.
    p.gravity = clt.flGravity * 60.0f * 60.0f;
    p.rootStiffness = clt.flRootStiffness;
    p.linearDamping = clt.flLinearDamping;
    p.contactDamping = clt.flContactDamping;
    p.flags = clt.dwFlags;
    p.useCustomWind = clt.nUseCustomWind != 0;
    p.windVelocity = clt.vWindVelocity;
    p.collisionPlane[0] = clt.nCollisionPlane0;
    p.collisionPlane[1] = clt.nCollisionPlane1;
    p.collisionPlane[2] = clt.nCollisionPlane2;
    p.collisionPlane[3] = clt.nCollisionPlane3;
    return p;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

/// @brief The frame a **hardpoint** rides, which is not the bone's own frame.
///
/// `Skeleton_BuildSkinningPaletteAndBounds` fills three transforms per 96-byte
/// palette entry and hardpoints read the middle one: `+0` is the animated bone
/// pose, `+32` is that pose composed with the bone's **`tTransform1`** (the
/// inverse of bind pose *A*), and `+64` is the same with `tTransform4` (the
/// inverse of *B*) — the skinning frame the staples use. `sub_7100213590`, the
/// shipped hardpoint resolver, reads `+32`.
///
/// It matters because a hardpoint's transform is authored in **model space**,
/// not in its bone's: `CS_collision_Pelvis1` sits at (0.06, 0, 4.69) and its
/// bone sits at (0.08, 0, 4.69). Composing it with the bone pose directly adds
/// the two and puts every collider at roughly twice its height — which reads on
/// screen as a capsule floating above the model rather than as a wrong frame.
Frame AttachFrame(const d3n::Appearances& app, const Matrix44f& boneWorld, i32 b) {
    const Frame w = DecomposeFrame(boneWorld);
    if (b < 0 || static_cast<usize>(b) >= app.arBones.size())
        return w;
    const d3n::PRSTransform& inv = app.arBones[static_cast<usize>(b)].tTransform1;
    Frame out;
    out.rotation = Mul(w.rotation, Normalized(AsQuat(inv.qRotation)));
    out.translation = Add(w.translation, Rotate(w.rotation, Scale(inv.vTranslation, w.scale)));
    out.scale = inv.flScale * w.scale;
    return out;
}

/// The same frame as a matrix, for a consumer that composes rather than solves.
Matrix44f AttachLocalMatrix(const d3n::Appearances& app, i32 b) {
    if (b < 0 || static_cast<usize>(b) >= app.arBones.size())
        return Matrix44f::identity();
    const d3n::PRSTransform& inv = app.arBones[static_cast<usize>(b)].tTransform1;
    const Quat q = Normalized(AsQuat(inv.qRotation));
    return renderer::animation::ComposePivotSRT(inv.vTranslation, Quaternion{q.x, q.y, q.z, q.w},
                                                {inv.flScale, inv.flScale, inv.flScale},
                                                {0.0f, 0.0f, 0.0f});
}

std::optional<D3ClothDef> D3BuildCloth(const d3n::SubObject& sub, const d3n::Cloth* clt, f32 scale) {
    // No `.clt` is not "use the defaults": without `flMass` the client never
    // reaches the builder at all, so a sub-object whose look names no cloth
    // simulates nothing however much geometry it carries.
    if (!clt || clt->flMass < kMinMass)
        return std::nullopt;
    if (sub.arClothData.empty())
        return std::nullopt;
    const d3n::ClothStructure& src = sub.arClothData.front();
    if (src.arVertices.empty() || src.arFaces.empty())
        return std::nullopt;

    D3ClothDef def;
    def.params = D3ClothParamsOf(*clt);
    def.scale = scale;
    def.externalForceScale = src.flExternalForceScale;
    def.firstFreeVertex = static_cast<i32>(src.arStaples.size());

    const f32 scaleSq = scale * scale;
    const f32 invScaleSq = (scale > kTinyScale) ? (1.0f / scaleSq) : 1.0f;
    const f32 massScale = 1.0f / (def.params.mass * kMassScale);

    const i32 meshCount = static_cast<i32>(sub.arVertices.size());
    def.vertices.reserve(src.arVertices.size());
    for (const d3n::ClothVertex& v : src.arVertices) {
        D3ClothVertex out;
        out.invMass = v.flInvMass * invScaleSq * massScale;
        out.collisionProxy = v.nCollisionProxyVertex;
        out.pinDistance = v.nPinDistance;
        out.meshVertex = v.nMeshVertexIndex;
        out.drivingBone = v.nDrivingBone;
        // Seeded from the mesh, not from the baked position: the client reads
        // the sub-object's own vertex array through `nMeshVertexIndex` and
        // writes both the live position (scaled, in its world) and the rest
        // position (unscaled). In model space those coincide at scale 1, which
        // is why `vPrevPosition == vPosition` on all 84,457 corpus vertices.
        const Vector3f mesh = (out.meshVertex >= 0 && out.meshVertex < meshCount)
                                  ? sub.arVertices[static_cast<usize>(out.meshVertex)].vPosition
                                  : v.vPosition;
        out.prevPosition = mesh;
        out.position = Scale(mesh, scale);
        out.velocity = {0.0f, 0.0f, 0.0f};
        def.vertices.push_back(out);
    }

    def.faces.reserve(src.arFaces.size());
    for (const d3n::ClothFace& f : src.arFaces) {
        D3ClothFace out;
        out.v[0] = f.nVertex0;
        out.v[1] = f.nVertex1;
        out.v[2] = f.nVertex2;
        out.restArea = f.flRestArea * scaleSq;
        def.faces.push_back(out);
    }

    def.staples.reserve(src.arStaples.size());
    def.stapleNormals.reserve(src.arStaples.size());
    for (const d3n::ClothStaple& s : src.arStaples) {
        D3ClothStaple out;
        out.vertex = s.nVertexIndex;
        out.bone[0] = s.nBoneIndex0;
        out.bone[1] = s.nBoneIndex1;
        out.bone[2] = s.nBoneIndex2;
        out.weight[0] = s.flWeight0;
        out.weight[1] = s.flWeight1;
        out.weight[2] = s.flWeight2;
        // A zero weight ends the list, tail first: the client rewrites bone 2
        // to -1 when weight 2 is zero, then bone 1, then bone 0, and stops at
        // the first non-zero. Not a filter — an authored `{w, 0, w}` keeps all
        // three, because the walk never reaches slot 0.
        if (out.weight[2] == 0.0f) {
            out.bone[2] = -1;
            if (out.weight[1] == 0.0f) {
                out.bone[1] = -1;
                if (out.weight[0] == 0.0f)
                    out.bone[0] = -1;
            }
        }
        def.staples.push_back(out);

        Vector3f n{0.0f, 0.0f, 1.0f};
        if (out.vertex >= 0 && static_cast<usize>(out.vertex) < def.vertices.size()) {
            const i32 mv = def.vertices[static_cast<usize>(out.vertex)].meshVertex;
            if (mv >= 0 && mv < meshCount)
                n = d3n::vertexNormal(sub.arVertices[static_cast<usize>(mv)]);
        }
        def.stapleNormals.push_back(n);
    }

    const auto copyConstraints = [&](const std::vector<d3n::ClothConstraint>& in,
                                     std::vector<D3ClothConstraint>& out) {
        out.reserve(in.size());
        for (const d3n::ClothConstraint& c : in) {
            D3ClothConstraint d;
            d.v0 = c.nVertex0;
            d.v1 = c.nVertex1;
            d.restLengthSq = c.flRestLengthSq * scaleSq;
            d.stiffnessBlend = c.flStiffnessBlend;
            out.push_back(d);
        }
    };
    copyConstraints(src.arStretchConstraints, def.stretch);
    copyConstraints(src.arBendConstraints, def.bend);

    // Endpoint weights, and the one place `flRootStiffness` acts. The endpoints
    // are **reordered** so slot 0 is the one nearer the pinned root, and that
    // one's inverse mass is divided down — which is how a cape gets heavier
    // toward its attachment without any per-vertex authoring.
    const f32 rootRecip = 1.0f / (std::max)(def.params.rootStiffness, 1.0f);
    const auto weighConstraints = [&](std::vector<D3ClothConstraint>& set) {
        for (D3ClothConstraint& c : set) {
            if (c.v0 < 0 || c.v1 < 0 || static_cast<usize>(c.v0) >= def.vertices.size() ||
                static_cast<usize>(c.v1) >= def.vertices.size())
                continue;
            if (def.vertices[static_cast<usize>(c.v0)].pinDistance >
                def.vertices[static_cast<usize>(c.v1)].pinDistance)
                std::swap(c.v0, c.v1);
            const D3ClothVertex& a = def.vertices[static_cast<usize>(c.v0)];
            const D3ClothVertex& b = def.vertices[static_cast<usize>(c.v1)];
            // Equal depths, or an endpoint at the root itself, take no
            // stiffening — the test is `near >= far || near <= 0`.
            const f32 k =
                (a.pinDistance >= b.pinDistance || a.pinDistance <= 0) ? 1.0f : rootRecip;
            const f32 wa = a.invMass * k;
            const f32 sum = wa + b.invMass;
            const f32 inv = (sum != 0.0f) ? (1.0f / sum) : 0.0f;
            c.weight0 = wa * inv;
            c.weight1 = b.invMass * inv;
        }
    };
    weighConstraints(def.stretch);
    weighConstraints(def.bend);

    // Driving bones. Membership is per staple (through its vertex), the vertex
    // *range* is per free vertex, and the two are counted separately — which is
    // why a bone can have contributors and no range, or a range and no
    // contributors, and both are handled rather than assumed away.
    const i32 boneCount = (std::max)(src.dwDrivingBoneCount, 0);
    def.drivingBones.assign(static_cast<usize>(boneCount), D3ClothDrivingBone{});
    for (const D3ClothStaple& s : def.staples) {
        if (s.vertex < 0 || static_cast<usize>(s.vertex) >= def.vertices.size())
            continue;
        const i32 b = def.vertices[static_cast<usize>(s.vertex)].drivingBone;
        if (b >= 0 && b < boneCount)
            ++def.drivingBones[static_cast<usize>(b)].contributors;
    }
    for (usize v = static_cast<usize>(def.firstFreeVertex); v < def.vertices.size(); ++v) {
        const i32 b = def.vertices[v].drivingBone;
        if (b < 0 || b >= boneCount)
            continue;
        D3ClothDrivingBone& db = def.drivingBones[static_cast<usize>(b)];
        const i32 idx = static_cast<i32>(v);
        // `minVertex` starts at 0 and 0 doubles as "unset" — the client's own
        // `if (min != 0) min = std::min(min, i)` — which is safe only because
        // the range covers free vertices and vertex 0 is pinned whenever there
        // is a staple at all.
        db.minVertex = (db.minVertex != 0) ? (std::min)(db.minVertex, idx) : idx;
        db.maxVertex = (std::max)(db.maxVertex, idx);
        ++db.vertexCount;
    }
    for (D3ClothDrivingBone& db : def.drivingBones) {
        if (db.vertexCount == 0)
            db.maxVertex = db.minVertex - 1;
        db.contiguous = (db.maxVertex + 1 - db.minVertex) == db.vertexCount;
        // Both frames carry the actor scale and are otherwise the identity the
        // struct already defaults to — `ClothInstance_Init` writes the actor
        // root into the *prev* slot and copies it forward, which is what makes
        // the first step's delta (`cur * conj(prev)`) come out as the whole
        // skinning transform rather than something degenerate.
        db.scale = scale;
        db.prevScale = scale;
    }

    // Mesh vertex -> cloth vertex. The runtime keeps this as a u16 array beside
    // the cloth block and the deform walks it; here it is the same table read
    // out of `FatVertex::dwClothVertexIndex`, whose only job in the file is to
    // hold it (`kSubObjectHasClothIndex`, exact over 191,543 sub-objects).
    def.meshToCloth.assign(static_cast<usize>(meshCount), -1);
    const bool hasIndex = (sub.dwVertexFormat & d3n::kSubObjectHasClothIndex) != 0;
    if (hasIndex) {
        for (i32 i = 0; i < meshCount; ++i) {
            const u32 ci = sub.arVertices[static_cast<usize>(i)].dwClothVertexIndex;
            if (ci < def.vertices.size())
                def.meshToCloth[static_cast<usize>(i)] = static_cast<i32>(ci);
        }
    } else {
        // No index field: fall back to the reverse of `nMeshVertexIndex`, which
        // is the same mapping read from the other end. It loses nothing except
        // the seam vertices that share a cloth vertex, and it keeps a
        // pre-`kSubObjectHasClothIndex` file drawing rather than static.
        for (usize c = 0; c < def.vertices.size(); ++c) {
            const i32 mv = def.vertices[c].meshVertex;
            if (mv >= 0 && mv < meshCount)
                def.meshToCloth[static_cast<usize>(mv)] = static_cast<i32>(c);
        }
    }
    return def;
}

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------

D3ClothSolver::D3ClothSolver(D3ClothDef def) : def_(std::move(def)) {
    ComputeBounds();
    RecomputeNormals();
}

void D3ClothSolver::SetCapsules(std::vector<D3ClothCapsule> capsules) {
    if (capsules.size() > static_cast<usize>(kD3ClothMaxCapsules))
        capsules.resize(static_cast<usize>(kD3ClothMaxCapsules));
    capsules_ = std::move(capsules);
}

void D3ClothSolver::SetPlanes(std::vector<D3ClothPlane> planes) {
    if (planes.size() > static_cast<usize>(kD3ClothMaxPlanes))
        planes.resize(static_cast<usize>(kD3ClothMaxPlanes));
    planes_ = std::move(planes);
}

void D3ClothSolver::UpdateDrivingBones(std::span<const Matrix44f> skin) {
    // Last step's frame is this step's `prev`, and the accumulator starts at
    // zero — a *sum* of weighted quaternions, not a slerp.
    for (D3ClothDrivingBone& db : def_.drivingBones) {
        db.prevRotation = db.rotation;
        db.prevTranslation = db.translation;
        db.prevScale = db.scale;
        db.rotation = Vector4f{0.0f, 0.0f, 0.0f, 0.0f};
        db.translation = {0.0f, 0.0f, 0.0f};
        db.scale = 0.0f;
        db.maxDisplacementSq = 0.0f;
    }

    const i32 boneCount = static_cast<i32>(def_.drivingBones.size());
    const i32 nodeCount = static_cast<i32>(skin.size());
    for (usize s = 0; s < def_.staples.size(); ++s) {
        const D3ClothStaple& st = def_.staples[s];
        if (st.vertex < 0 || static_cast<usize>(st.vertex) >= def_.vertices.size())
            continue;
        D3ClothVertex& v = def_.vertices[static_cast<usize>(st.vertex)];

        const Vector3f oldPos = v.position;
        const Vector3f restPos = v.prevPosition;
        const Vector3f restNormal = def_.stapleNormals[s];
        v.position = {0.0f, 0.0f, 0.0f};
        v.normal = {0.0f, 0.0f, 0.0f};

        D3ClothDrivingBone* db =
            (v.drivingBone >= 0 && v.drivingBone < boneCount)
                ? &def_.drivingBones[static_cast<usize>(v.drivingBone)]
                : nullptr;

        for (int k = 0; k < 3; ++k) {
            const i32 b = st.bone[k];
            if (b == -1)
                break;
            if (b < 0 || b >= nodeCount)
                continue;
            const Frame f = DecomposeFrame(skin[static_cast<usize>(b)]);
            const f32 w = st.weight[k];
            const f32 s3 = f.scale * def_.scale;

            v.position =
                Add(v.position, Scale(Add(f.translation, Rotate(f.rotation, Scale(restPos, s3))), w));
            // The normal takes the rotation only — no scale, no translation.
            v.normal = Add(v.normal, Scale(Rotate(f.rotation, restNormal), w));

            if (!db)
                continue;
            Quat q = f.rotation;
            // Align to the running sum before adding, or two bones a half turn
            // apart cancel into nothing and the frame collapses.
            if (Dot4(q, AsQuat(db->rotation)) < 0.0f)
                q = {-q.x, -q.y, -q.z, -q.w};
            db->rotation = Vector4f{db->rotation.x + w * q.x, db->rotation.y + w * q.y,
                                    db->rotation.z + w * q.z, db->rotation.w + w * q.w};
            db->translation = Add(db->translation, Scale(f.translation, w));
            db->scale += w * s3;
        }

        if (db) {
            const f32 moved = LengthSq(Sub(v.position, oldPos));
            db->maxDisplacementSq = (std::max)(db->maxDisplacementSq, moved);
        }
    }

    for (D3ClothDrivingBone& db : def_.drivingBones) {
        if (db.contributors != 0) {
            const f32 inv = 1.0f / static_cast<f32>(db.contributors);
            db.rotation = Vector4f{db.rotation.x * inv, db.rotation.y * inv, db.rotation.z * inv,
                                   db.rotation.w * inv};
            db.translation = Scale(db.translation, inv);
            db.scale *= inv;
            if (Dot4(AsQuat(db.rotation), AsQuat(db.rotation)) < kTinyQuatSq)
                db.rotation = db.prevRotation;
            db.rotation = AsVec4(Normalized(AsQuat(db.rotation)));
        } else {
            // Undriven bones take the actor root, which in model space is the
            // identity carrying the actor scale.
            db.rotation = Vector4f{0.0f, 0.0f, 0.0f, 1.0f};
            db.translation = {0.0f, 0.0f, 0.0f};
            db.scale = def_.scale;
        }
    }

    snapLatched_ = snapRequested_;
    snapRequested_ = false;
}

void D3ClothSolver::BlendToSkinnedPose(f32 /*dt*/) {
    snapped_ = false;
    const f32 rate = def_.params.skinBlendRate;
    const i32 boneCount = static_cast<i32>(def_.drivingBones.size());

    for (i32 bi = 0; bi < boneCount; ++bi) {
        D3ClothDrivingBone& db = def_.drivingBones[static_cast<usize>(bi)];
        if (db.contributors == 0)
            continue;

        f32 blend = rate;
        if (snapLatched_ || db.maxDisplacementSq > kSnapDisplacementSq) {
            blend = 1.0f;
            // Sticky for the rest of the pass, exactly as shipped: once one
            // bone snaps, every later bone also zeroes its vertices'
            // velocities even though its own blend may be well below 1.
            snapped_ = true;
        }

        // The rigid delta: undo last step's frame, apply this step's.
        //   target(p) = q1 * ((s1/s0) * (inv(q0) * (p - t0))) + t1
        const Quat cur = AsQuat(db.rotation);
        const Quat prev = AsQuat(db.prevRotation);
        const Quat delta = Mul(cur, Conj(prev));
        const f32 relScale = (db.prevScale != 0.0f) ? (db.scale / db.prevScale) : 1.0f;
        const Vector3f offset = Sub(
            db.translation, Rotate(cur, Scale(RotateInverse(prev, db.prevTranslation), relScale)));

        if (db.maxVertex < db.minVertex)
            continue;
        const usize lo = static_cast<usize>((std::max)(db.minVertex, 0));
        const usize hi = (std::min)(static_cast<usize>(db.maxVertex), def_.vertices.size() - 1);
        for (usize i = lo; i <= hi; ++i) {
            D3ClothVertex& v = def_.vertices[i];
            if (!db.contiguous && v.drivingBone != bi)
                continue;
            const Vector3f target = Add(Rotate(delta, Scale(v.position, relScale)), offset);
            v.position = Add(v.position, Scale(Sub(target, v.position), blend));
            if (snapped_)
                v.velocity = {0.0f, 0.0f, 0.0f};
        }

        db.prevRotation = db.rotation;
        db.prevTranslation = db.translation;
        db.prevScale = db.scale;
    }
    // The latch is deliberately NOT cleared here. `ClothSim_BlendToSkinnedPose`
    // only *reads* solver+60; the one place it is written is the tail of
    // `ClothInstance_UpdateDrivingBones`. That is what makes the presettle a
    // relaxation rather than a drop: `ClothInstance_Presettle` raises the
    // request, calls `UpdateDrivingBones` **once**, and then steps
    // `relaxIterations` times — so every one of those steps snaps, and every
    // one of them zeroes the velocities it just integrated. Consuming the
    // latch after a single step turned all 25 into a free fall instead, which
    // is the whole reason a settled cape hung away from its body.
}

void D3ClothSolver::Aero(f32 dt) {
    const f32 drag = def_.params.dragCoefficient;
    if (drag <= 0.0f)
        return;
    const Vector3f wind = def_.params.useCustomWind ? def_.params.windVelocity
                                                    : Vector3f{0.0f, 0.0f, 0.0f};
    const usize n = def_.vertices.size();
    for (D3ClothFace& f : def_.faces) {
        if (f.v[0] < 0 || f.v[1] < 0 || f.v[2] < 0)
            continue;
        if (static_cast<usize>(f.v[0]) >= n || static_cast<usize>(f.v[1]) >= n ||
            static_cast<usize>(f.v[2]) >= n)
            continue;
        D3ClothVertex& a = def_.vertices[static_cast<usize>(f.v[0])];
        D3ClothVertex& b = def_.vertices[static_cast<usize>(f.v[1])];
        D3ClothVertex& c = def_.vertices[static_cast<usize>(f.v[2])];

        // 0.333, not 1/3 — the client's literal, and it appears twice.
        const Vector3f mean = Scale(Add(Add(a.velocity, b.velocity), c.velocity), 0.333f);
        const Vector3f rel = Sub(wind, mean);
        const f32 speed = std::sqrt(LengthSq(rel));
        if (!(speed > 1.0e-6f) || !(speed < 3.4028e38f))
            continue;
        const Vector3f dir = Scale(rel, 1.0f / speed);
        const f32 pressure = (speed * speed) * (drag * kAirDensity + drag * kAirDensity) * dt;
        const f32 facing = (f.restArea * 0.333f) * std::fabs(Dot3(dir, f.normal));
        const Vector3f force = Scale(dir, pressure * facing);
        a.velocity = Add(a.velocity, Scale(force, a.invMass));
        b.velocity = Add(b.velocity, Scale(force, b.invMass));
        c.velocity = Add(c.velocity, Scale(force, c.invMass));
    }
}

void D3ClothSolver::Integrate(f32 dt) {
    // Both damping constants are `exp(-x/60)` **per step**, regardless of dt —
    // so a solver run at another rate damps differently. Shipped.
    const f32 damp = std::exp(def_.params.linearDamping * -kFixedDt);
    const f32 accelZ = (def_.params.gravity + kGravityBias) * dt;
    for (usize i = static_cast<usize>(def_.firstFreeVertex); i < def_.vertices.size(); ++i) {
        D3ClothVertex& v = def_.vertices[i];
        v.prevPosition = v.position;
        v.velocity = {damp * v.velocity.x, damp * v.velocity.y, damp * v.velocity.z + accelZ};
        v.position = Add(v.position, Scale(v.velocity, dt));
        v.contactFlag = 0;
    }
}

void D3ClothSolver::SolveDistance(std::span<D3ClothConstraint> set, f32 kA, f32 kB, f32 dt,
                                  bool compressionOnly) {
    const usize n = def_.vertices.size();
    for (D3ClothConstraint& c : set) {
        if (c.v0 < 0 || c.v1 < 0 || static_cast<usize>(c.v0) >= n || static_cast<usize>(c.v1) >= n)
            continue;
        D3ClothVertex& a = def_.vertices[static_cast<usize>(c.v0)];
        D3ClothVertex& b = def_.vertices[static_cast<usize>(c.v1)];
        const Vector3f d = Sub(b.position, a.position);
        // **The rational approximation, not `1 - L/d`.** They agree to first
        // order and part company under stretch, which is where a cape lives.
        const f32 term = (c.restLengthSq * -2.0f) / (c.restLengthSq + LengthSq(d)) + 1.0f;
        f32 k;
        if (compressionOnly) {
            if (!(term < 0.0f))
                continue;
            k = kA;
        } else {
            k = kB * c.stiffnessBlend + kA * (1.0f - c.stiffnessBlend);
        }
        const f32 correction = ((k * term) * dt) * 60.0f;
        a.position = Add(a.position, Scale(d, correction * c.weight0));
        b.position = Sub(b.position, Scale(d, correction * c.weight1));
    }
}

void D3ClothSolver::CollideCapsules() {
    if (capsules_.empty())
        return;
    struct Seg {
        Vector3f p0;
        Vector3f axis;
        f32 radius;
        f32 length;
    };
    Seg segs[kD3ClothMaxCapsules];
    const usize m = capsules_.size();
    for (usize i = 0; i < m; ++i) {
        const D3ClothCapsule& c = capsules_[i];
        Seg& s = segs[i];
        s.p0 = c.p0;
        s.radius = c.radius;
        const Vector3f d = Sub(c.p1, c.p0);
        const f32 len = std::sqrt(LengthSq(d));
        if (len > 1.0e-6f) {
            s.axis = Scale(d, 1.0f / len);
            s.length = len;
        } else {
            s.axis = d;
            s.length = 0.0f;
        }
    }

    const usize n = def_.vertices.size();
    for (usize i = static_cast<usize>(def_.firstFreeVertex); i < n; ++i) {
        D3ClothVertex& v = def_.vertices[i];
        // **The probe is another vertex.** The contact normal comes from
        // `nCollisionProxyVertex`'s position and the push is applied here — so a
        // thin fold is resolved as a group rather than each layer separately.
        const usize pi = (v.collisionProxy >= 0 && static_cast<usize>(v.collisionProxy) < n)
                             ? static_cast<usize>(v.collisionProxy)
                             : i;
        const Vector3f probe = def_.vertices[pi].position;
        Vector3f pos = v.position;
        for (usize s = 0; s < m; ++s) {
            const Seg& c = segs[s];
            f32 t = Dot3(Sub(probe, c.p0), c.axis);
            if (t > c.length)
                t = c.length;
            if (t < 0.0f)
                t = 0.0f;
            const Vector3f closest = Add(c.p0, Scale(c.axis, t));
            const Vector3f diff = Sub(probe, closest);
            const f32 inv = FastRsqrt(LengthSq(diff));
            const Vector3f nrm = Scale(diff, inv);
            const f32 depth = Dot3(Sub(pos, closest), nrm) - c.radius;
            if (depth < 0.0f)
                pos = Sub(pos, Scale(nrm, depth));
        }
        v.position = pos;
    }
}

void D3ClothSolver::CollidePlanes() {
    if (planes_.empty())
        return;
    for (const D3ClothPlane& p : planes_) {
        for (usize i = static_cast<usize>(def_.firstFreeVertex); i < def_.vertices.size(); ++i) {
            D3ClothVertex& v = def_.vertices[i];
            const f32 signed_ = p.d + Dot3(v.position, p.normal);
            if (signed_ >= kPlaneSkin)
                continue;
            v.position = Add(v.position, Scale(p.normal, kPlaneSkin - signed_));
            v.contactNormal = p.normal;
            v.contactFlag = 1;
        }
    }
}

void D3ClothSolver::DeriveVelocities(f32 dt) {
    const f32 invDt = 1.0f / dt;
    const f32 tangentDamp = std::exp(def_.params.contactDamping * -kFixedDt);
    for (usize i = static_cast<usize>(def_.firstFreeVertex); i < def_.vertices.size(); ++i) {
        D3ClothVertex& v = def_.vertices[i];
        Vector3f vel = Scale(Sub(v.position, v.prevPosition), invDt);
        if (v.contactFlag != 0) {
            // The whole friction model: keep the normal component, damp the
            // tangential one. Capsules never set the flag, so only a plane
            // contact is ever damped.
            const f32 vn = Dot3(vel, v.contactNormal);
            const Vector3f normalPart = Scale(v.contactNormal, vn);
            vel = Add(normalPart, Scale(Sub(vel, normalPart), tangentDamp));
        }
        v.velocity = vel;
    }
}

void D3ClothSolver::RecomputeNormals() {
    const i32 firstFree = def_.firstFreeVertex;
    const usize n = def_.vertices.size();
    for (usize i = static_cast<usize>(firstFree); i < n; ++i)
        def_.vertices[i].normal = {0.0f, 0.0f, 0.0f};

    for (D3ClothFace& f : def_.faces) {
        if (f.v[0] < 0 || f.v[1] < 0 || f.v[2] < 0)
            continue;
        if (static_cast<usize>(f.v[0]) >= n || static_cast<usize>(f.v[1]) >= n ||
            static_cast<usize>(f.v[2]) >= n)
            continue;
        const Vector3f& p0 = def_.vertices[static_cast<usize>(f.v[0])].position;
        const Vector3f& p1 = def_.vertices[static_cast<usize>(f.v[1])].position;
        const Vector3f& p2 = def_.vertices[static_cast<usize>(f.v[2])].position;
        Vector3f nrm = Cross3(Sub(p1, p0), Sub(p2, p0));
        const f32 len = std::sqrt(LengthSq(nrm));
        if (len > 1.0e-6f)
            nrm = Scale(nrm, 1.0f / len);
        f.normal = nrm;
        // **Pinned vertices are skipped.** Their normal is the skinned mesh
        // normal the staple pass rotated, and a face sum would overwrite it
        // with the cloth's own shading.
        for (int k = 0; k < 3; ++k) {
            if (f.v[k] >= firstFree)
                def_.vertices[static_cast<usize>(f.v[k])].normal =
                    Add(def_.vertices[static_cast<usize>(f.v[k])].normal, nrm);
        }
    }

    // The normalise pass covers every vertex, pinned included — which is what
    // makes the staple pass's un-normalised weighted sum come out unit.
    for (D3ClothVertex& v : def_.vertices) {
        const f32 len = std::sqrt(LengthSq(v.normal));
        if (len > 1.0e-6f)
            v.normal = Scale(v.normal, 1.0f / len);
    }
}

void D3ClothSolver::ComputeBounds() {
    if (def_.vertices.empty()) {
        centre_ = {0.0f, 0.0f, 0.0f};
        radius_ = 0.0f;
        return;
    }
    Vector3f lo = def_.vertices.front().position;
    Vector3f hi = lo;
    for (const D3ClothVertex& v : def_.vertices) {
        lo = {(std::min)(lo.x, v.position.x), (std::min)(lo.y, v.position.y),
              (std::min)(lo.z, v.position.z)};
        hi = {(std::max)(hi.x, v.position.x), (std::max)(hi.y, v.position.y),
              (std::max)(hi.z, v.position.z)};
    }
    centre_ = Scale(Add(lo, hi), 0.5f);
    radius_ = std::sqrt(LengthSq(Sub(hi, centre_)));
}

void D3ClothSolver::Step(f32 dt) {
    if (!(dt > 0.0f))
        return;
    BlendToSkinnedPose(dt);
    // Aero is skipped on a snap step — the velocities it would read are about
    // to be thrown away anyway.
    if (!snapped_)
        Aero(dt);
    Integrate(dt);

    const f32 kA = Clamp01(def_.params.stretchStiffness0);
    const f32 kB = Clamp01(def_.params.stretchStiffness1);
    const f32 kBend = Clamp01(def_.params.bendStiffness);
    SolveDistance(def_.stretch, kA, kB, dt, false);
    if (kBend > 0.0f)
        SolveDistance(def_.bend, kBend, kBend, dt, true);
    SolveDistance(def_.stretch, kA, kB, dt, false);

    CollideCapsules();
    CollidePlanes();
    DeriveVelocities(dt);
    ComputeBounds();
    RecomputeNormals();
}

void D3ClothSolver::Advance(f32 frameDt) {
    // `dt <- 0.9*dt + 0.1*clamp(frameDt, 0.005, 1/60)`, seeded at 1/60. At 60
    // Hz the clamp pins the input and the filter never leaves its fixed point.
    const f32 clamped = (std::min)((std::max)(frameDt, 0.005f), kFixedDt);
    smoothedDt_ = smoothedDt_ * 0.9f + clamped * 0.1f;
    Step(smoothedDt_);
}

void D3ClothSolver::Presettle() {
    const i32 iterations = (std::max)(def_.params.relaxIterations, 0);
    for (i32 i = 0; i < iterations; ++i)
        Step(kFixedDt);
}

void D3ClothSolver::PublishDeform(std::span<const Vector3f> restPositions,
                                  std::span<const Vector3f> restNormals,
                                  D3ClothDeform& out) const {
    const usize n = def_.meshToCloth.size();
    out.positions.resize(n);
    out.normals.resize(n);
    for (usize i = 0; i < n; ++i) {
        const i32 c = def_.meshToCloth[i];
        if (c >= 0 && static_cast<usize>(c) < def_.vertices.size()) {
            out.positions[i] = def_.vertices[static_cast<usize>(c)].position;
            out.normals[i] = def_.vertices[static_cast<usize>(c)].normal;
        } else {
            out.positions[i] = (i < restPositions.size()) ? restPositions[i] : Vector3f{0, 0, 0};
            out.normals[i] = (i < restNormals.size()) ? restNormals[i] : Vector3f{0, 0, 1};
        }
    }
}

// ---------------------------------------------------------------------------
// Colliders from the appearance
// ---------------------------------------------------------------------------

std::vector<D3ClothCapsule> D3GatherClothCapsules(const d3n::Appearances& app,
                                                  std::span<const Matrix44f> boneWorld,
                                                  const Vector3f& centre, f32 radius) {
    std::vector<D3ClothCapsule> out;
    const i32 nodes = static_cast<i32>(boneWorld.size());
    for (const d3n::CollisionCapsule& c : app.arCollisionCapsules) {
        if (static_cast<i32>(out.size()) >= kD3ClothMaxCapsules)
            break;
        const i32 b = c.tHardpoint.nBoneIndex;
        if (b < 0 || b >= nodes)
            continue;
        const Frame bone = AttachFrame(app, boneWorld[static_cast<usize>(b)], b);
        const d3n::PRTransform& hp = c.tHardpoint.tTransform;
        const Quat q = Mul(bone.rotation, Normalized(AsQuat(hp.qRotation)));
        const Vector3f origin =
            Add(bone.translation, Rotate(bone.rotation, Scale(hp.vTranslation, bone.scale)));

        // The cull is the client's, sphere against sphere: the capsule's own
        // bound is centred on the hardpoint with radius `r + length/2`.
        const f32 bound = c.flRadius + c.flLength * 0.5f;
        const f32 reach = radius + bound;
        if (LengthSq(Sub(origin, centre)) > reach * reach)
            continue;

        // **Local +Z, centred, half-length `0.5 * flLength`** — and scaled by
        // the actor scale only, never by the bone's, which is the client's own
        // asymmetry rather than an omission here.
        const f32 half = c.flLength * 0.5f;
        const Vector3f axis = Rotate(q, Vector3f{0.0f, 0.0f, 1.0f});
        D3ClothCapsule cap;
        cap.p0 = Sub(origin, Scale(axis, half));
        cap.p1 = Add(origin, Scale(axis, half));
        cap.radius = c.flRadius;
        out.push_back(cap);
    }
    return out;
}

std::vector<D3ClothPlane> D3BuildClothPlanes(const d3n::Appearances& app,
                                             const D3ClothParams& params,
                                             std::span<const Matrix44f> boneWorld) {
    std::vector<D3ClothPlane> out;
    const i32 nodes = static_cast<i32>(boneWorld.size());
    for (i32 slot = 0; slot < kD3ClothMaxPlanes; ++slot) {
        const i32 v = params.collisionPlane[slot];
        // 0 is `None` and the table has nine rows, so 9 is a valid index into
        // it holding a null name — the client's `v && v <= 9` lets it through
        // and the name lookup then fails. Same outcome, stated rather than
        // stumbled into.
        if (v <= 0 || v > 8)
            continue;
        char want[24];
        std::snprintf(want, sizeof(want), "HP_cloth_plane_%d", static_cast<int>(v));

        const d3n::Hardpoint* hp = nullptr;
        for (const d3n::Hardpoint& h : app.arHardpoints) {
            if (h.szName == want) {
                hp = &h;
                break;
            }
        }
        if (!hp)
            continue;

        Quat q = AsQuat(hp->tTransform.qRotation);
        Vector3f p = hp->tTransform.vTranslation;
        if (hp->nBoneIndex >= 0 && hp->nBoneIndex < nodes) {
            const Frame bone =
                AttachFrame(app, boneWorld[static_cast<usize>(hp->nBoneIndex)], hp->nBoneIndex);
            p = Add(bone.translation, Rotate(bone.rotation, Scale(p, bone.scale)));
            q = Mul(bone.rotation, Normalized(q));
        } else {
            q = Normalized(q);
        }

        // **Local +X**, from the constant at `0x7100E5DAB8` — a different axis
        // from the capsule's, out of a different constant.
        D3ClothPlane plane;
        plane.normal = Rotate(q, Vector3f{1.0f, 0.0f, 0.0f});
        plane.d = -Dot3(plane.normal, p);
        out.push_back(plane);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Finding the cloth on an appearance
// ---------------------------------------------------------------------------

std::vector<D3ClothPiece> D3FindClothPieces(const d3n::Appearances& app, u32 lookIndex) {
    std::vector<D3ClothPiece> out;
    const auto& subs = app.tGeoSet0.arSubObjects;
    for (usize i = 0; i < subs.size(); ++i) {
        const d3n::SubObject& sub = subs[i];
        if (sub.arClothData.empty() || sub.arClothData.front().arVertices.empty())
            continue;
        D3ClothPiece piece;
        piece.subObject = static_cast<i32>(i);
        // The `.clt` is a property of the *look*, not of the mesh: the same
        // sub-object under two looks can wear two different cloths. Joined on
        // `szName`, which is what carries the material name — see
        // `reference_d3_material_join`.
        for (const auto& mat : app.arMaterials) {
            if (mat.szName != sub.szName)
                continue;
            const usize v = (lookIndex < mat.arVariants.size()) ? lookIndex : 0;
            if (!mat.arVariants.empty())
                piece.clothSno = mat.arVariants[v].snoCloth.id;
            break;
        }
        out.push_back(piece);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The stage
// ---------------------------------------------------------------------------

namespace {

class D3ClothStage final : public IPoseStage {
public:
    struct Piece {
        D3ClothSolver solver;
        i32 subObject = -1;
        i32 geoset = -1;
        std::vector<Vector3f> restPositions;
        std::vector<Vector3f> restNormals;
    };

    D3ClothStage(const d3n::Appearances& app, std::vector<Piece> pieces,
                 std::shared_ptr<D3ClothOutput> output)
        : app_(app), pieces_(std::move(pieces)), output_(std::move(output)) {}

    void Run(FrameState& fs, const PoseStageContext& ctx) override;

    bool NeedsHostInputs() const override {
        // A cape is fully described by the model. Nothing here reads the ground
        // query or an aim target.
        return false;
    }

private:
    void BuildSkinPalette(const FrameState& fs);

    const d3n::Appearances& app_;
    std::vector<Piece> pieces_;
    std::shared_ptr<D3ClothOutput> output_;
    std::vector<Matrix44f> skin_;
    bool settled_ = false;
};

void D3ClothStage::BuildSkinPalette(const FrameState& fs) {
    const auto& bones = app_.arBones;
    const usize n = (std::min)(bones.size(), fs.boneWorldMatrices.size());
    skin_.resize(n);
    for (usize i = 0; i < n; ++i) {
        // `tTransform4` is the inverse of bind pose B, the one
        // `Skeleton_BuildSkinningPaletteAndBounds` reads — not `tTransform1`,
        // which is right for 92% of bones and catastrophic for the rest.
        const d3n::PRSTransform& t = bones[i].tTransform4;
        const Quat q = Normalized(AsQuat(t.qRotation));
        const Matrix44f inv = renderer::animation::ComposePivotSRT(
            t.vTranslation, Quaternion{q.x, q.y, q.z, q.w}, {t.flScale, t.flScale, t.flScale},
            {0.0f, 0.0f, 0.0f});
        skin_[i] = inv * fs.boneWorldMatrices[i];
    }
}

void D3ClothStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    if (pieces_.empty() || fs.boneWorldMatrices.empty())
        return;
    BuildSkinPalette(fs);

    const f32 frameDt = static_cast<f32>(ctx.frameDtMs) * 0.001f;
    for (Piece& p : pieces_) {
        // Colliders first, then the staples, then the step — the client's own
        // order (`ApplyClothAssetParams` gathers, `UpdateDrivingBones` skins,
        // the job steps). The gather reads last step's bounds either way.
        const auto gather = [&] {
            if ((p.solver.Def().params.flags & kD3ClothFlagCollideWorld) == 0 &&
                (p.solver.Def().params.flags & kD3ClothFlagCollideSelf) != 0) {
                p.solver.SetCapsules(D3GatherClothCapsules(app_, fs.boneWorldMatrices,
                                                           p.solver.BoundsCentre(),
                                                           p.solver.BoundsRadius()));
            }
            p.solver.SetPlanes(
                D3BuildClothPlanes(app_, p.solver.Def().params, fs.boneWorldMatrices));
        };
        gather();
        p.solver.UpdateDrivingBones(skin_);
        if (!settled_) {
            // The client presettles at spawn, so a cape enters hanging rather
            // than in its authored rest shape. Deferred to the first frame with
            // a pose because the staples need one — and the gather is repeated
            // per iteration, as `ClothInstance_Presettle` does, because the
            // cloth's bounds grow as it falls and a capsule that starts outside
            // them would otherwise never be seen.
            for (i32 k = 0; k < (std::max)(p.solver.Def().params.relaxIterations, 0); ++k) {
                p.solver.Step(kFixedDt);
                gather();
            }
        } else {
            // Unconditional, including a frame that reports no time at all.
            // `sub_7100024D00` floors its input at 0.005 s and always steps, so
            // the client's cloth is never left behind by a stalled clock — and
            // `BlendToSkinnedPose`, which is what carries the cloth along with
            // the body, only runs inside a step. Skipping the step froze the
            // free vertices in place while the pose kept moving.
            p.solver.Advance(frameDt);
        }
    }
    settled_ = true;

    // The debug overlay's feed. Unconditional, and separate from `output_`:
    // this is the solver's own particles, which is the picture a cloth has —
    // `output_` is the deformed *mesh* the renderer draws.
    fs.clothParticles.resize(pieces_.size());
    for (usize i = 0; i < pieces_.size(); ++i) {
        const auto verts = pieces_[i].solver.Vertices();
        auto& out = fs.clothParticles[i];
        out.resize(verts.size());
        for (usize v = 0; v < verts.size(); ++v)
            out[v] = verts[v].position;
    }

    if (!output_)
        return;
    output_->pieces.resize(pieces_.size());
    for (usize i = 0; i < pieces_.size(); ++i) {
        output_->pieces[i].geoset = pieces_[i].geoset;
        pieces_[i].solver.PublishDeform(pieces_[i].restPositions, pieces_[i].restNormals,
                                        output_->pieces[i]);
    }
    ++output_->revision;

    // And the deform itself. `output_` is a `shared_ptr` the adapter owns and
    // outlives the frame, so the renderer reads these spans in place rather
    // than taking a second copy of a few thousand vertices.
    fs.geosetDeforms.clear();
    for (const D3ClothDeform& piece : output_->pieces) {
        if (piece.geoset < 0 || piece.positions.empty())
            continue;
        FrameState::GeosetDeform out;
        out.geoset = piece.geoset;
        out.positions = piece.positions;
        out.normals = piece.normals;
        fs.geosetDeforms.push_back(out);
    }
}

} // namespace

std::unique_ptr<animation::IPoseStage>
CreateD3ClothStage(const d3n::Appearances& app, std::span<const D3ClothPiece> pieces,
                   std::span<const std::shared_ptr<const d3n::Cloth>> cloths,
                   std::shared_ptr<D3ClothOutput> output, f32 scale) {
    const auto& subs = app.tGeoSet0.arSubObjects;
    std::vector<D3ClothStage::Piece> built;
    for (usize i = 0; i < pieces.size(); ++i) {
        const D3ClothPiece& piece = pieces[i];
        if (piece.subObject < 0 || static_cast<usize>(piece.subObject) >= subs.size())
            continue;
        const d3n::Cloth* clt = (i < cloths.size() && cloths[i]) ? cloths[i].get() : nullptr;
        const d3n::SubObject& sub = subs[static_cast<usize>(piece.subObject)];
        auto def = D3BuildCloth(sub, clt, scale);
        if (!def)
            continue;

        D3ClothStage::Piece p{D3ClothSolver(std::move(*def)), piece.subObject, piece.geoset, {}, {}};
        p.restPositions.reserve(sub.arVertices.size());
        p.restNormals.reserve(sub.arVertices.size());
        for (const d3n::FatVertex& v : sub.arVertices) {
            p.restPositions.push_back(v.vPosition);
            p.restNormals.push_back(d3n::vertexNormal(v));
        }
        built.push_back(std::move(p));
    }
    if (built.empty())
        return nullptr;
    return std::make_unique<D3ClothStage>(app, std::move(built), std::move(output));
}

std::vector<model::ClothOverlayData>
D3BuildClothOverlays(const d3n::Appearances& app, std::span<const D3ClothPiece> pieces,
                     std::span<const std::shared_ptr<const d3n::Cloth>> cloths, f32 scale) {
    // The colliders are per appearance, not per cloth, and the overlay draws
    // them once per cloth — which is what the client collides with too, since
    // every cloth on a model culls against the same capsule array.
    //
    // Two conversions. The overlay's capsule runs along local **+X** and D3's
    // along local **+Z**, so the hardpoint frame is pre-multiplied by the cyclic
    // permutation (x,y,z) -> (y,z,x); and the overlay composes `cd.local *
    // boneWorld`, so the hardpoint's own model-space frame has to be brought
    // back into bone space first — @ref AttachLocalMatrix, the same
    // `tTransform1` the solver's @ref AttachFrame applies.
    std::vector<model::ClothColliderData> colliders;
    Matrix44f zToX = Matrix44f::identity();
    zToX.data[0][0] = 0.0f;
    zToX.data[0][2] = 1.0f;
    zToX.data[1][0] = 1.0f;
    zToX.data[1][1] = 0.0f;
    zToX.data[2][1] = 1.0f;
    zToX.data[2][2] = 0.0f;
    for (const d3n::CollisionCapsule& c : app.arCollisionCapsules) {
        const d3n::PRTransform& hp = c.tHardpoint.tTransform;
        const Quat q = Normalized(AsQuat(hp.qRotation));
        model::ClothColliderData cd;
        cd.node = c.tHardpoint.nBoneIndex;
        cd.local = zToX *
                   renderer::animation::ComposePivotSRT(hp.vTranslation,
                                                        Quaternion{q.x, q.y, q.z, q.w},
                                                        {scale, scale, scale},
                                                        {0.0f, 0.0f, 0.0f}) *
                   AttachLocalMatrix(app, cd.node);
        cd.radius0 = c.flRadius * scale;
        cd.radius1 = cd.radius0;
        cd.length = c.flLength * scale;
        colliders.push_back(cd);
    }

    const auto& subs = app.tGeoSet0.arSubObjects;
    std::vector<model::ClothOverlayData> out;
    for (usize i = 0; i < pieces.size(); ++i) {
        const D3ClothPiece& piece = pieces[i];
        if (piece.subObject < 0 || static_cast<usize>(piece.subObject) >= subs.size())
            continue;
        const d3n::Cloth* clt = (i < cloths.size() && cloths[i]) ? cloths[i].get() : nullptr;
        auto def = D3BuildCloth(subs[static_cast<usize>(piece.subObject)], clt, scale);
        if (!def)
            continue;

        model::ClothOverlayData o;
        // The **stretch** set only. Bend constraints span a vertex either side
        // of a shared edge, so drawing both turns the wireframe into a solid
        // and hides the thing it exists to show.
        o.links.reserve(def->stretch.size() * 2);
        for (const D3ClothConstraint& c : def->stretch) {
            if (c.v0 < 0 || c.v1 < 0 || c.v0 > 0xFFFF || c.v1 > 0xFFFF)
                continue;
            o.links.push_back(static_cast<u16>(c.v0));
            o.links.push_back(static_cast<u16>(c.v1));
        }
        // Pinned-first is measured, not assumed, over the whole corpus — see
        // the header — so the staple count *is* the pinned prefix.
        o.pinnedCount = static_cast<u32>((std::max)(def->firstFreeVertex, 0));
        o.colliders = colliders;
        // No `active` channel: D3 steps a cloth whenever its `.clt` gives it a
        // mass, and that is decided at build, not per frame.
        o.activeIndex = -1;
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
