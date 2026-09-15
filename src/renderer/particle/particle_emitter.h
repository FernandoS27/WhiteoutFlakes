#pragma once

// ============================================================================
// ParticleEmitter — what the service and the actor layer hold.
//
// The operations they call on every emitter without asking which dialect it
// is, and the four blocks every dialect keeps: the pool, the placement, the
// surface and the per-frame enable. `Emitter2` (WC3, WoW, SC2) and
// `d3::Emitter` implement it side by side; neither inherits the other's state.
// A caller that needs one dialect's own surface asks the vtable
// (`AsEmitter2` / `AsD3`), which cannot disagree with what the object is.
// ============================================================================

#include "emitter_placement.h"
#include "particle_material.h"
#include "particle_output.h"
#include "particle_pool.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

class Emitter2;
struct BuildGeometryInput;

namespace d3 {
class Emitter;
}

/// What the service reads off an emitter to route and draw its geometry.
struct EmitterDrawHeader {
    /// The id space the emitter is registered in.
    ParticleOutput output = ParticleOutput::Billboard;
    i32 priorityPlane = 0;
    /// Never null for a live emitter; points into its desc.
    const ParticleMaterialDesc* material = nullptr;
    /// M2 `Refraction`: the draw goes to the refraction pass, never the scene.
    bool refraction = false;
    /// M2 `MultiTexture`: the vertices go to the three-layer stream.
    bool multiTexture = false;
};

class ParticleEmitter {
public:
    virtual ~ParticleEmitter() = default;

    // ---- the frame ----

    /// @p emissionScaler multiplies the emission rate. Threaded in from the
    /// owning service rather than read from a global, so two scenes can scale
    /// independently.
    virtual void Update(f32 elapsed, f32 emissionScaler) = 0;

    /// This emitter's geometry for one frame, appended to @p out; returns the
    /// vertex count.
    virtual i32 BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const = 0;

    /// @brief Throw away everything this emitter has spawned and start it over,
    ///        keeping the emitter itself registered.
    ///
    /// What a rewind needs. Emitters are registered when the model spawns, so
    /// dropping them (ParticleService::Clear) would leave the model with no
    /// particles at all until it was reloaded.
    virtual void ResetParticles() = 0;

    /// Outputs that drive something outside the vertex stream (child actors)
    /// report what happened during the last Update here.
    virtual void CollectOutputEvents(std::vector<ChildModelEvent>& out) {}

    virtual EmitterDrawHeader DrawHeader() const = 0;

    /// Live particles, wherever this emitter keeps them.
    virtual i32 TotalAlive() const = 0;

    // ---- the dialect, answered by the vtable ----

    virtual Emitter2* AsEmitter2() {
        return nullptr;
    }
    virtual const Emitter2* AsEmitter2() const {
        return nullptr;
    }
    virtual d3::Emitter* AsD3() {
        return nullptr;
    }
    virtual const d3::Emitter* AsD3() const {
        return nullptr;
    }

    // ---- placement ----

    void SetModelToWorld(const Matrix44f& m) {
        placement_.modelToWorld = m;
    }
    const Matrix44f& ModelToWorld() const {
        return placement_.modelToWorld;
    }

    /// Emitter world position for this frame. WoW spawns along the segment
    /// between the previous frame's position and this one, and derives the
    /// velocity a particle inherits from the same delta.
    void SetWorldPosition(const Vector3f& p) {
        placement_.MoveTo(p);
    }
    /// Read by the sim tests to bound where a spawn may land.
    const Vector3f& WorldPosition() const {
        return placement_.worldPos;
    }

    /// See @ref EmitterPlacement::unitScale.
    f32 UnitScale() const {
        return placement_.unitScale;
    }
    /// @brief Set it directly, for a dialect that takes no per-frame track.
    ///
    /// Diablo III emitters carry no per-frame track at all (everything
    /// animated lives inside the `.prt`), so the host hands this over on its
    /// own. Ignored values <= 0 keep the current one.
    void SetUnitScale(f32 s) {
        if (s > 0.0f)
            placement_.unitScale = s;
    }

    // ---- the per-frame enable ----

    void SetVisible(bool v) {
        visible_ = v;
    }
    bool Visible() const {
        return visible_;
    }

    // ---- surface ----

    /// The ground this emitter's particles collide against. Installed once by
    /// the service and again only when the host replaces it.
    void SetGroundQuery(GroundQuery q) {
        surface_.groundQuery = std::move(q);
    }

    /// The mesh a surface-shaped emitter is born on. Set once by the loader.
    void SetEmitMesh(std::shared_ptr<const EmitMesh> mesh) {
        surface_.mesh = std::move(mesh);
    }
    bool HasEmitMesh() const {
        return surface_.mesh && !surface_.mesh->Empty();
    }

    /// The pose that skins @ref SetEmitMesh into world space, per frame. Views
    /// into the actor's own arrays: the caller keeps them alive across Update.
    void SetEmitMeshPose(std::span<const Matrix44f> pose, std::span<const Matrix44f> invBind,
                         const Matrix44f& toWorld) {
        surface_.pose = pose;
        surface_.invBind = invBind;
        surface_.toWorld = toWorld;
    }

    // ---- pool ----

    const ParticlePool& Pool() const {
        return pool_;
    }
    ParticlePool& Pool() {
        return pool_;
    }

protected:
    ParticleEmitter() = default;

    ParticlePool pool_;
    EmitterPlacement placement_;
    EmitSurface surface_;
    bool visible_ = false;
};

} // namespace whiteout::flakes::renderer::particle
