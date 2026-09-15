#include "renderer/particle/base/model_particle_emitter.h"

#include "renderer/particle/base/particle_constants.h"
#include "renderer/particle/output/particle_geometry.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

// Below this the client leaves the orientation alone (`MoveParticle`
// @0x1016a15c0 gates on the angular speed, not on the axis).
constexpr f32 kMinAngularSpeed = 1e-4f;

// The emitter matrix with its translation dropped and its uniform scale divided
// out of each row — the emitter's orientation and nothing else. Deliberately a
// matrix, not the client's quaternion: M2_PARTICLE_DESIGN.md §11.7.
Matrix44f RotationOnly(const Matrix44f& m) {
    Matrix44f r = Matrix44f::identity();
    for (i32 row = 0; row < 3; ++row) {
        Vector3f v{m.data[row][0], m.data[row][1], m.data[row][2]};
        const f32 l2 = v.x * v.x + v.y * v.y + v.z * v.z;
        if (l2 > kBasisRowEpsilon) {
            const f32 inv = 1.0f / std::sqrt(l2);
            v = {v.x * inv, v.y * inv, v.z * inv};
        }
        r.data[row][0] = v.x;
        r.data[row][1] = v.y;
        r.data[row][2] = v.z;
    }
    return r;
}

} // namespace

void ModelParticleEmitter::OnPoolResized(usize capacity) {
    ChildModelEmitter::OnPoolResized(capacity);
    spin_.resize(capacity);
}

void ModelParticleEmitter::OnParticleBorn(u32 poolIndex) {
    if (poolIndex >= spin_.size())
        spin_.resize(Pool().Capacity());

    // Runs after Emitter2::CreateParticle and before the base class mints the
    // child handle — the same position the client's own
    // `CreateParticle(CModelParticle&)` occupies, so these draws land on the
    // emitter's stream in the client's order.
    Spin& s = spin_[poolIndex];

    // A world-space particle is stamped into the world at birth, orientation
    // included; a model-space one keeps riding the emitter, so its basis is read
    // live at placement instead and this stays identity.
    s.basis = Desc().modelSpace ? Matrix44f::identity() : RotationOnly(ModelToWorld());

    // Three draws, x then y then z. The client reads the RANGE where the min
    // belongs on Y and Z: `min.x + u*range.x`, `range.y*(1 + u)`, `range.z*(1 + u)`.
    // Reproduced, not corrected. M2_PARTICLE_DESIGN.md §11.7.
    const f32 ux = CRandom::real_(SpawnStream());
    const f32 uy = CRandom::real_(SpawnStream());
    const f32 uz = CRandom::real_(SpawnStream());
    s.omega = {Desc().tumbleBase.x + ux * Desc().tumbleVary.x, Desc().tumbleVary.y * (1.0f + uy),
               Desc().tumbleVary.z * (1.0f + uz)};

    // NegateSpinRandom costs three more draws and flips each component
    // independently on the parity of its own draw.
    if (Desc().negateSpinRandom) {
        const f32 sx = (CRandom::next_u32(SpawnStream()) & 1u) ? 1.0f : -1.0f;
        const f32 sy = (CRandom::next_u32(SpawnStream()) & 1u) ? 1.0f : -1.0f;
        const f32 sz = (CRandom::next_u32(SpawnStream()) & 1u) ? 1.0f : -1.0f;
        s.omega = {s.omega.x * sx, s.omega.y * sy, s.omega.z * sz};
    }

    ChildModelEmitter::OnParticleBorn(poolIndex);
}

f32 ModelParticleEmitter::VisibilityFor(u32 poolIndex) const {
    if (Desc().twinklePercent >= 1.0f)
        return 1.0f;
    const Particle2& p = Pool()[poolIndex];
    const f32 entry = TwinkleTable()[TwinkleIndex(p.RenderSeed(), p.age, Desc().twinkleSpeed)];
    return (Desc().twinklePercent < entry) ? 0.0f : 1.0f;
}

Matrix44f ModelParticleEmitter::TransformFor(u32 poolIndex) const {
    const Particle2& p = Pool()[poolIndex];
    const EmitterDesc& d = Desc();

    // Size: the scale track at this particle's own normalised age, pulsed by
    // twinkle. No size-variation and no cell draw here — RenderParticle reads
    // neither, so a model particle's render stream is shorter than a
    // billboard's, not merely different.
    const f32 life = EffectiveLifeSpan(p);
    const f32 t = (life > 0.0f) ? (p.age / life) : 0.0f;
    Vector2f size = d.curves.size.Evaluate(t, 0);
    if (d.twinklePercent < 1.0f || d.twinkleVary != 0.0f) {
        const f32 entry = TwinkleTable()[TwinkleIndex(p.RenderSeed(), p.age, d.twinkleSpeed)];
        const f32 tw = d.twinkleBase + d.twinkleVary * entry;
        size.x *= tw;
        size.y *= tw;
    }
    // Z takes the mean of the two authored axes — the record has no third scale,
    // and the client fills the gap this way rather than with 1.
    const Vector3f scale{size.x, size.y, (size.x + size.y) * 0.5f};

    // Tumble. A constant body-frame angular velocity integrates in closed form,
    // so there is no per-frame quaternion to carry: the client's incremental
    // `q *= dq` in MoveParticle @0x1016a15c0 and this are the same rotation, and
    // this one cannot drift.
    Matrix44f m = Matrix44f::scaling(scale);
    const Spin& s = spin_[poolIndex];
    const f32 speed =
        std::sqrt(s.omega.x * s.omega.x + s.omega.y * s.omega.y + s.omega.z * s.omega.z);
    if (speed > kMinAngularSpeed) {
        const f32 inv = 1.0f / speed;
        const Quaternion q = Quaternion::from_axis_angle(
            {s.omega.x * inv, s.omega.y * inv, s.omega.z * inv}, speed * p.age);
        m = m * Matrix44f::rotation(q).transpose();
    }

    // Then the emitter's orientation, last: frozen at birth for a world-space
    // emitter, read live for a model-space one. Rotation only, unlike the
    // client's whole matrix — the child actor applies its own `worldScale`, and
    // `pos` already carries the translation. M2_PARTICLE_DESIGN.md §11.7.
    Vector3f pos = p.position;
    if (d.modelSpace) {
        pos = whiteout::transform_point(pos, ModelToWorld());
        m = m * RotationOnly(ModelToWorld());
    } else {
        m = m * s.basis;
    }
    if (d.coordSpace != CoordinateSystem::Default())
        pos = CoordinateSystem::ToDefault(d.coordSpace, pos);

    return m * Matrix44f::translation(pos);
}

} // namespace whiteout::flakes::renderer::particle
