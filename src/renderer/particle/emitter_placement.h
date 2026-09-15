#pragma once

// ============================================================================
// Where an emitter is, and the surface its particles are born on and collide
// against — two blocks every dialect reads, each with one home.
//
// WoW spreads a frame's spawns along the placement's segment, SC2 sweeps its
// spawns along it, and Diablo III lerps its births across it. The surface is
// what the SC2 and Diablo III simulations sample and collide with; the service
// installs it without knowing which dialect it is holding.
// ============================================================================

#include "emit_mesh.h"
#include "ground_query.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <span>

namespace whiteout::flakes::renderer::particle {

/// The emitter's transform and the segment it travelled this frame.
struct EmitterPlacement {
    Matrix44f modelToWorld = Matrix44f::identity();

    /// This frame's emitter position and the previous one.
    Vector3f worldPos{0, 0, 0};
    Vector3f prevWorldPos{0, 0, 0};

    /// False until the first @ref MoveTo, and again after a rewind, so the
    /// next move seeds both ends and no spawn is spread across a path the
    /// emitter never travelled.
    bool seeded = false;

    /// Renderer units per model unit. The `.m2` size, twinkle and tail values
    /// are authored in yards while the emitter draws in renderer units, so the
    /// geometry builder converts with this. MDX models are already in renderer
    /// units and leave it at 1.
    f32 unitScale = 1.0f;

    void MoveTo(const Vector3f& p) {
        // The first frame has no previous position, so seeding both suppresses
        // one spurious spawn-spread across whatever distance the emitter was
        // placed at.
        if (!seeded) {
            prevWorldPos = p;
            seeded = true;
        } else {
            prevWorldPos = worldPos;
        }
        worldPos = p;
    }

    void Unseed() {
        seeded = false;
    }
};

/// The model surface an emitter samples and the ground it collides with.
struct EmitSurface {
    /// Installed by the service at registration and whenever the host changes
    /// it — never rebuilt per frame, because a `std::function` copy per
    /// particle per sub-step is exactly the cost this avoids.
    GroundQuery groundQuery;

    /// The mesh a surface-shaped emitter is born on. Shared with every actor
    /// spawned from the same model; the pose that skins it arrives per frame as
    /// views into the actor's own arrays, so nothing is copied per spawn.
    std::shared_ptr<const EmitMesh> mesh;
    std::span<const Matrix44f> pose;
    std::span<const Matrix44f> invBind;
    /// Model space -> renderer units, scale included.
    Matrix44f toWorld = Matrix44f::identity();
};

} // namespace whiteout::flakes::renderer::particle
