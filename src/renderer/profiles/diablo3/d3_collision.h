#pragma once

// ============================================================================
// `Appearances::arBones[].arCollisionShapes` as drawable collision primitives.
//
// D3 authors a rig's geometry per bone, in three kinds and nothing else
// (`CollisionShape::eShapeType`, measured over all 36,861 shapes in the corpus):
//
//     0  sphere     57      one centre and a radius
//     1  capsule    11,029  two endpoints and a radius
//     2  polytope   25,775  a 96-byte offline cook, `io::D3ReadPolytope`
//
// The cross-tab has no exceptions: kinds 0 and 1 always carry a radius and
// never a cook, kind 2 always carries a cook and never a radius. That is what
// makes the mapping a measurement rather than a reading of the decompile, and
// why `CollisionShape`'s first two fields were renamed — `+0` is a flags byte
// that only ever holds 0 or 1, and the *kind* is at `+4`. See
// D3_PHYSICS_PLAN.md §3.3.
//
// Unlike StarCraft II's `PHSH`, a D3 shape carries **no matrix of its own**:
// its points are authored directly in its bone's space, and the client's only
// transform is a multiply by `actorScale * boneScale` before it hands the
// fixture over. D3 bone scale is a single float (`PRSTransform::flScale`), so
// the bone's world matrix already applies exactly that uniform factor to both
// the points and — because the overlay builds its rings in shape space and
// transforms the finished vertices — the radius. So there is no per-shape local
// frame here and none is needed; @ref D3PlaceCollisionShapes is the bone matrix
// and nothing else.
//
// The one thing that does NOT survive the shortcut is the client's
// `max(bodyScale * r, 0.0833)` floor on a radius, which is applied after the
// scale and so cannot be folded into an unscaled one. It belongs to the fixture
// builder, where the scale is known, not to the overlay.
//
// This file is data only and is compiled whether or not physics is: a collider
// you can look at is worth having before there is a solver to run it, and it is
// the only cheap test of the shape decode.
// ============================================================================

#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <span>
#include <vector>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief Rigid bodies per actor, from `Physics_CreateActorBoneBodies`'s own
///        `if (count >= 64) break`.
///
/// Here rather than beside the stage because the overlay has to replay the same
/// cap to colour the same bones, and this header is compiled whether or not
/// there is a solver.
inline constexpr std::size_t kD3MaxBodies = 64;

/// @brief The lod `Physics_CreateActorBoneBodies` is called with.
///
/// `Physics_CreateActorBoneBodies(desc, actor, 1)` at its one call site. A body
/// whose shapes all sit at another lod gets no fixture and the bridge destroys
/// it, so this is not a preference: at lod 0 the same builder produces a
/// *breakable*, at lod 1 a character ragdoll. Measured over the corpus:
///
///     lod 0   34,082 shapes on 2,460 models   the full collider set; the 2,367
///                                             with a dynamic bone are
///                                             barricades, statues, carts
///     lod 1    2,779 shapes on   570 models   a coarse 7-10 shape per-limb
///                                             proxy, dynamic on all 570 --
///                                             skeletons, crypt children
inline constexpr i32 kD3BoneBodyLod = 1;

/// @brief Whether @p bone becomes a dynamic body at @p lod.
///
/// The whole of `Physics_CreateActorBoneBodies`'s body-type decision: the
/// descriptor's type flag starts at 1 and is cleared by the first LOD-matching
/// shape whose `flScaleX` is positive, and `PhysicsBridge_CreateBody` maps 1 to
/// Domino type 2 (Static) and 0 to type 0 (Dynamic). That builder never
/// produces a **kinematic** body; the other one does, after the fact — see
/// @ref D3BoneIsAnchor.
///
/// `flScaleX` is also the fixture density multiplier
/// (`density = g_density(nBodyClass) * flScaleX`), so "has mass" and "is
/// dynamic" are the same question asked once. That is why a shape authoring
/// zero there produces a body that cannot move rather than a massless one that
/// falls through the floor.
///
/// Here rather than beside the stage because the overlay colours by it and this
/// header is compiled whether or not there is a solver.
bool D3BoneIsDynamic(const d3n::BoneStructure& bone, i32 lod);

/// @brief Whether any bone would, i.e. whether @p app has a rig at all.
bool D3HasDynamicBody(const d3n::Appearances& app, i32 lod = kD3BoneBodyLod);

/// @brief Whether @p bone carries a ragdoll **anchor** shape.
///
/// `CollisionShape::dwFlags` bit 0 — 600 shapes on 272 models, and the field
/// whose meaning the plan had backwards until the second builder was
/// disassembled. `sub_71003E07B0` creates every body Dynamic and then calls
/// `dmBody_SetType(body, 1)` on exactly the bones this returns true for, so the
/// bit marks the **kinematic anchor** a hanging rig is nailed to, not the
/// members of a ragdoll.
bool D3BoneIsAnchor(const d3n::BoneStructure& bone);

/// @brief Whether any bone does, i.e. whether @p app has an anchored rig.
bool D3HasRagdollAnchor(const d3n::Appearances& app);

struct D3CollisionShapes {
    std::vector<model::CollisionShapeData> shapes;
    /// @brief The bone each shape rides. Parallel to @ref shapes.
    std::vector<i32> bones;
};

/// @brief Every collision shape on @p app's bones, at LOD @p lodIndex.
///
/// @param appBytes the whole `.app` file. A kind-2 cook is a header holding
///        four more payload references, and WhiteoutLib resolves only the first
///        level — so without the file the majority of shapes cannot be read at
///        all. Passing an empty span drops them rather than drawing a box in
///        their place: a hull drawn as its bounding box is both far larger than
///        the collider and the same picture for every limb.
/// @param lodIndex `CollisionShape::nLodIndex`; shapes authored for another LOD
///        are skipped, which is what stops a model drawing its colliders twice.
D3CollisionShapes D3BuildCollisionShapes(const d3n::Appearances& app,
                                         std::span<const ::whiteout::u8> appBytes,
                                         i32 lodIndex = 0);

/// @brief Place a shape list from a posed skeleton, into @p out.
///
/// One matrix per shape, and it is the bone's — see the header note on why D3
/// needs no per-shape frame. Shapes whose bone is out of range get identity
/// rather than being dropped, so @p out stays parallel to the shape list.
void D3PlaceCollisionShapes(std::span<const i32> bones, std::span<const Matrix44f> boneWorld,
                            std::vector<Matrix44f>& out);

} // namespace whiteout::flakes::renderer::profiles::diablo3
