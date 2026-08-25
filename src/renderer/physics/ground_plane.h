#pragma once
// ============================================================================
// The viewer's ground: the grid.
//
// One plane, every consumer. The physics stages collide against a static slab
// whose top face is this plane, terrain IK plants feet on it through
// FlatGroundQuery, and DebugRenderer draws the grid on it. Defining the height
// once is what keeps a ragdoll's landing, a planted foot and the drawn grid
// from disagreeing by a constant nobody can see.
//
// Model space, which for this plane is also scene space: the grid draws at
// z = 0, every host leaves the actor at the origin, and a profile's uniform
// WorldScale maps z = 0 onto itself. A game host with real terrain overrides
// the IK half through RenderSettings::SetGroundQuery; the physics half has no
// override because the shipped colliders it stands in for were never
// transcribed (see the stage sources).
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::physics {

/// Height of the ground plane — the grid.
inline constexpr f32 kGroundZ = 0.0f;

/// Half-extent of the physics stand-in slab, in model units (yards for WoW,
/// SC2 units for `.m3`). Nothing simulated leaves a model's own neighbourhood,
/// so 100 of either is a world.
inline constexpr f32 kGroundSlabHalfExtent = 100.0f;

/// Slab thickness below the plane. A box rather than a halfspace because it is
/// the shape the narrowphase already proves.
inline constexpr f32 kGroundSlabThickness = 1.0f;

/// Terrain-IK ground query against the plane. `up` / `down` bound how far a
/// foot will reach for a surface, exactly as an IKJT chunk's raycast range
/// intends; outside that window there is no ground, and the stage takes its
/// documented no-hit fallback.
inline bool FlatGroundQuery(const Vector3f& pos, f32 up, f32 down, f32& outZ) {
    if (kGroundZ > pos.z + up || kGroundZ < pos.z - down)
        return false;
    outZ = kGroundZ;
    return true;
}

} // namespace whiteout::flakes::renderer::physics
