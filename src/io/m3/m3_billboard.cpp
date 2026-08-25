#include "io/m3/m3_billboard.h"

#include <cmath>

namespace whiteout::flakes::io {
namespace {

// The three constants `CBBSolver::ApplyBillboard` tests against, verbatim.
constexpr f32 kAxisEpsilon = 2.384185791015625e-07f; // dword_103C472B4
constexpr f32 kDegenerateLenSq = 1.0e-4f;            // dword_103BB6B78
constexpr f32 kOrthonormalTol = 1.0e-3f;             // dword_103BC8C10

Vector3f RowOf(const Matrix44f& m, i32 r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

void SetRow(Matrix44f& m, i32 r, const Vector3f& v) {
    m.data[r][0] = v.x;
    m.data[r][1] = v.y;
    m.data[r][2] = v.z;
}

// The engine normalises through a refined `rsqrtps` and leaves the vector
// alone when there is no length; the refinement is not reproduced (we are not
// bit-matching a CPU pose), the "leave it alone" is.
Vector3f SafeNormalize(const Vector3f& v) {
    const f32 lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (lenSq <= 0.0f)
        return v;
    return v * (1.0f / std::sqrt(lenSq));
}

// The perpendicular the axis-locked types build: the aim direction turned a
// quarter turn inside the plane it was projected onto, renormalised in 2-D
// because the fallback vector is not in that plane.
Vector3f QuarterTurn(f32 a, f32 b) {
    const f32 lenSq = a * a + b * b;
    if (lenSq <= 0.0f)
        return {a, b, 0.0f};
    const f32 inv = 1.0f / std::sqrt(lenSq);
    return {a * inv, b * inv, 0.0f};
}

// Hamilton product, the operand order the engine uses: `a (x) b` applies b
// first. Row-vector matrices compose the other way round, which is why this is
// not just `M(a) * M(b)`.
Quaternion QuatMul(const Quaternion& a, const Quaternion& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quaternion QuatConjugate(const Quaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

// A matrix's rotation, the way `M3Node_GetWorldRotationQuat` (SC2
// `0x102874730`) takes one: normalise the first two rows, rebuild the third
// from their cross product, convert. Rebuilding the third is what makes the
// result a proper rotation even when the matrix it came from was mirrored.
Quaternion RotationOf(const Matrix44f& m) {
    const Vector3f x = SafeNormalize(RowOf(m, 0));
    const Vector3f y = SafeNormalize(RowOf(m, 1));
    const Vector3f z = SafeNormalize(whiteout::cross(x, y));
    Matrix44f basis = Matrix44f::identity();
    SetRow(basis, 0, x);
    SetRow(basis, 1, y);
    SetRow(basis, 2, z);
    return M3QuatFromRows(basis);
}

bool RowIsUnit(const Matrix44f& m, i32 r) {
    const Vector3f v = RowOf(m, r);
    return std::fabs(v.x * v.x + v.y * v.y + v.z * v.z - 1.0f) < kOrthonormalTol;
}

} // namespace

M3CameraFrame M3BuildCameraFrame(const Matrix44f& world, const Matrix44f& view,
                                 const Vector3f& cameraPos) {
    const Matrix44f inv = Matrix44f::inverse(world);

    // `look_at_rh`/`look_at_lh` write the camera basis down the columns, so
    // these are its axes in world space. StarCraft II's camera matrix is
    // (X, forward, up, position) in its own +X-left / -Y-forward / +Z-up
    // basis, and `right == cross(forward, up)` in both — which is why the
    // graphics "right" lands in the slot SC2 calls X with no sign fix.
    const Vector3f right{view.data[0][0], view.data[1][0], view.data[2][0]};
    const Vector3f up{view.data[0][1], view.data[1][1], view.data[2][1]};
    const Vector3f back{view.data[0][2], view.data[1][2], view.data[2][2]};

    M3CameraFrame cam;
    cam.position = whiteout::transform_point(cameraPos, inv);
    cam.axisX = SafeNormalize(whiteout::transform_normal(right, inv));
    cam.forward = SafeNormalize(whiteout::transform_normal(back * -1.0f, inv));
    cam.up = SafeNormalize(whiteout::transform_normal(up, inv));
    return cam;
}

Matrix44f M3RotationRows(const Quaternion& q) {
    const f32 x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const f32 xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    const f32 yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    const f32 wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;

    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = 1.0f - yy - zz;
    m.data[0][1] = xy + wz;
    m.data[0][2] = xz - wy;
    m.data[1][0] = xy - wz;
    m.data[1][1] = 1.0f - xx - zz;
    m.data[1][2] = yz + wx;
    m.data[2][0] = xz + wy;
    m.data[2][1] = yz - wx;
    m.data[2][2] = 1.0f - xx - yy;
    return m;
}

Quaternion M3QuatFromRows(const Matrix44f& m) {
    const f32 m0 = m.data[0][0], m1 = m.data[0][1], m2 = m.data[0][2];
    const f32 m3 = m.data[1][0], m4 = m.data[1][1], m5 = m.data[1][2];
    const f32 m6 = m.data[2][0], m7 = m.data[2][1], m8 = m.data[2][2];

    Quaternion q{0.0f, 0.0f, 0.0f, 1.0f};
    const f32 trace = m0 + m4 + m8 + 1.0f;
    if (trace > 1.0f) {
        q = {m5 - m7, m6 - m2, m1 - m3, trace};
    } else if (m0 > m4 && m0 > m8) {
        q = {(1.0f + m0) - m4 - m8, m3 + m1, m6 + m2, m5 - m7};
    } else if (m4 > m8) {
        q = {m3 + m1, (m4 + (1.0f - m0)) - m8, m7 + m5, m6 - m2};
    } else {
        q = {m6 + m2, m7 + m5, ((1.0f - m0) - m4) + m8, m1 - m3};
    }

    const f32 lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq <= 0.0f)
        return {0.0f, 0.0f, 0.0f, 1.0f};
    const f32 inv = 1.0f / std::sqrt(lenSq);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

bool M3BillboardBasis(::whiteout::u8 billboardType, bool cameraLookAt, const M3CameraFrame& cam,
                      const Matrix44f& boneWorld, Matrix44f& outBasis) {
    // The aim direction points AWAY from the eye — `bone - camera` when the
    // record asks to look at the camera, otherwise the camera's own view
    // direction, which points into the scene. Both branches agree on the sign,
    // which is what fixes the sign of everything below.
    Vector3f d;
    if (cameraLookAt)
        d = SafeNormalize(RowOf(boneWorld, 3) - cam.position);
    else
        d = cam.forward;

    if (std::fabs(d.x) < kAxisEpsilon && std::fabs(d.y) < kAxisEpsilon &&
        std::fabs(d.z) < kAxisEpsilon)
        return false;

    Matrix44f b = Matrix44f::identity();
    switch (billboardType) {
    case 0: {
        // Locked to world X: the direction is flattened into the YZ plane and
        // local +Y aims along it. The fallback replaces the whole vector, X
        // included, which is why the orthonormality gate below can still fail.
        Vector3f v{0.0f, d.y, d.z};
        if (d.y * d.y + d.z * d.z < kDegenerateLenSq)
            v = cam.axisX;
        v = SafeNormalize(v);
        const Vector3f p = QuarterTurn(-v.z, v.y);
        SetRow(b, 0, {1.0f, 0.0f, 0.0f});
        SetRow(b, 1, v);
        SetRow(b, 2, {0.0f, p.x, p.y});
        break;
    }
    case 1: {
        // Locked to world Y. Y is the axis that cannot move, so this is the
        // one type that aims local +X instead of local +Y.
        Vector3f v{d.x, 0.0f, d.z};
        if (d.z * d.z + d.x * d.x < kDegenerateLenSq)
            v = cam.up;
        v = SafeNormalize(v);
        const Vector3f p = QuarterTurn(-v.z, v.x);
        SetRow(b, 0, v);
        SetRow(b, 1, {0.0f, 1.0f, 0.0f});
        SetRow(b, 2, {p.x, 0.0f, p.y});
        break;
    }
    case 2: {
        // Locked to world Z — the upright poster, and the only axis-locked type
        // shipped in quantity.
        Vector3f v{d.x, d.y, 0.0f};
        if (d.y * d.y + d.x * d.x < kDegenerateLenSq)
            v = cam.up;
        v = SafeNormalize(v);
        const Vector3f p = QuarterTurn(v.y, -v.x);
        SetRow(b, 0, {p.x, p.y, 0.0f});
        SetRow(b, 1, v);
        SetRow(b, 2, {0.0f, 0.0f, 1.0f});
        break;
    }
    case 3: {
        // Locked to the bone's OWN current world X. `A x (A x d)` is minus the
        // part of the direction perpendicular to A, so local +Z ends up facing
        // the camera rather than away from it — the opposite sign to types
        // 0/1/2, and deliberate: these two types name a different facing axis.
        const Vector3f a = SafeNormalize(RowOf(boneWorld, 0));
        const Vector3f c = whiteout::cross(a, d);
        if (c.dot(c) < kDegenerateLenSq)
            return false;
        const Vector3f cn = SafeNormalize(c);
        SetRow(b, 0, a);
        SetRow(b, 1, cn);
        SetRow(b, 2, whiteout::cross(a, cn));
        break;
    }
    case 4:
        // Parsed, instantiated, never applied. 54 records ask for it.
        return false;
    case 5: {
        // The same triple as type 3, rotated one slot: the locked axis is the
        // bone's own world Y and it becomes local Z, leaving local +Y facing
        // the camera.
        const Vector3f a = SafeNormalize(RowOf(boneWorld, 1));
        const Vector3f c = whiteout::cross(a, d);
        if (c.dot(c) < kDegenerateLenSq)
            return false;
        const Vector3f cn = SafeNormalize(c);
        SetRow(b, 0, cn);
        SetRow(b, 1, whiteout::cross(a, cn));
        SetRow(b, 2, a);
        break;
    }
    case 6: {
        // Free, with the camera's up as the roll hint. A direction parallel to
        // that hint leaves the basis IDENTITY rather than bailing out — the
        // record's correction quaternion still gets applied on top, so this is
        // not the same as doing nothing.
        const Vector3f e = whiteout::cross(d, cam.up);
        if (e.dot(e) < kDegenerateLenSq)
            break;
        const Vector3f en = SafeNormalize(e);
        SetRow(b, 0, en);
        SetRow(b, 1, d);
        SetRow(b, 2, SafeNormalize(whiteout::cross(en, d)));
        break;
    }
    default:
        // Nothing in the corpus, and the engine's switch falls through to its
        // untouched identity scratch rather than skipping the bone.
        break;
    }

    // The engine's own sanity gate, on all three rows. It is what rejects the
    // axis-locked fallbacks, whose replacement vector is not in the plane the
    // other two rows were built for.
    if (!RowIsUnit(b, 0) || !RowIsUnit(b, 1) || !RowIsUnit(b, 2))
        return false;

    outBasis = b;
    return true;
}

bool M3ApplyBillboard(const ::whiteout::m3::BillboardBehavior& bb, const M3CameraFrame& cam,
                      const Quaternion* spin, const Vector3f& localScale,
                      const Matrix44f* parentWorld, Matrix44f& boneWorld) {
    Matrix44f basis;
    if (!M3BillboardBasis(bb.billboardType, bb.cameraLookAt != 0, cam, boneWorld, basis))
        return false;

    // Everything past this point happens to a QUATERNION in the engine, and it
    // has to happen to one here as well. Not for tidiness: the axis-locked
    // fallbacks can hand back a basis whose rows are each unit but not mutually
    // square — the engine's own gate only measures lengths — and it is this
    // conversion that turns it back into a rotation. Composing the row matrices
    // instead leaves the skew in, and a skewed bone shears its skin.
    Quaternion q = M3QuatFromRows(basis);

    // `q_billboard (x) q_spin (x) q_correction`, in that order.
    if (spin != nullptr)
        q = QuatMul(q, *spin);

    // Which correction, and whether there is one at all, is decided by the type
    // and by the same root/child split: the axis-locked types always take the
    // first quaternion, the free type takes the second and only on a bone with
    // a real parent, and types 3/4/5 never take either.
    if (bb.billboardType <= 2)
        q = QuatMul(q, bb.up);
    else if (bb.billboardType == 6 && spin == nullptr)
        q = QuatMul(q, bb.forward);

    // Back down into the bone's own frame, exactly where the engine puts it,
    // and back up through the local scale and the parent. The position is not
    // recomputed: a local rotation cannot move it, so whatever the chain
    // already put in row 3 is what the engine would have left there.
    if (parentWorld != nullptr)
        q = QuatMul(QuatConjugate(RotationOf(*parentWorld)), q);

    const Matrix44f local = M3RotationRows(q);
    const f32 scale[3] = {localScale.x, localScale.y, localScale.z};
    Matrix44f composed = Matrix44f::identity();
    for (i32 r = 0; r < 3; ++r)
        SetRow(composed, r, RowOf(local, r) * scale[r]);
    if (parentWorld != nullptr) {
        for (i32 r = 0; r < 3; ++r) {
            Vector3f row{0.0f, 0.0f, 0.0f};
            for (i32 k = 0; k < 3; ++k)
                row = row + RowOf(*parentWorld, k) * composed.data[r][k];
            SetRow(composed, r, row);
        }
    }
    for (i32 r = 0; r < 3; ++r)
        SetRow(boneWorld, r, RowOf(composed, r));
    return true;
}

} // namespace whiteout::flakes::io
