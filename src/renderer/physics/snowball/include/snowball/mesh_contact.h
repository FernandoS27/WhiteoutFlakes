//===----------------------------------------------------------------------===//
// snowball/mesh_contact.h -- turning a pile of queried triangles into contacts.
//
// A body resting on terrain does not touch one triangle, it touches five or six, and every one
// of them produces its own manifold with its own normal. Handing all of those to the solver is
// what makes a body on a mesh behave differently from the same body on a box: the patches fight
// each other, and the body buzzes or catches. This is the layer that stops that.
//
// It is three ideas, in order:
//
//   * **A triangle is not a plane.** `MakePartialPolytope` builds the four planes the SAT runs
//     against, and three of them come from the *neighbours*. Where a neighbour bends up out of
//     the face, the edge plane is replaced by the face normal itself, so the triangle can never
//     push a body along an edge that is really interior to a flat surface. That is the internal
//     edge fix, and it is why `Triangle` carries adjacency at all. (The classic fix has a
//     second half -- `CheckAxis` refusing already-generated faceB/edge normals -- which is
//     deliberately absent from the collider; the capsule path keeps its own wing gate.)
//   * **Manifolds are clustered by normal, not merged.** `ReduceManifolds` runs k-means over the
//     contact normals with a 5-degree seeding threshold, so a body in a valley keeps one patch
//     per wall and a body on a flat run of triangles collapses to one. At most three clusters
//     survive, which is a hard cap and not a hint.
//   * **Each cluster is reduced to four points**, by the same deepest-then-widest rule the
//     convex path uses.
//
// Warm starting survives all of it, which is the part that is easy to lose: the accumulated
// impulse is matched across steps by *contact id*, and the mesh path stamps the triangle index
// into the high half of every id so that two neighbouring triangles cannot collide in the table.
//===----------------------------------------------------------------------===//
#pragma once

#include <span>
#include <vector>

#include "snowball/common_types.h"
#include "snowball/contact.h"
#include "snowball/heightfield.h"
#include "snowball/partial_polytope.h"
#include "snowball/toi.h"
#include "snowball/tree_mesh.h"
#include "snowball/triangle.h"

namespace snowball {

/// Two normals within this of each other are the same patch. `cos(5 degrees)`, spelled as the
/// exact float rather than re-derived at build time, so the threshold is bit-stable.
inline constexpr f32 kClusterCosine = 0.99619472f;

/// The cluster cap, and the size of every buffer that holds one per patch.
inline constexpr i32 kMaxMeshManifolds = 3;

/// Below this the reducer stops adding points: (2cm)^2 as a squared distance, and the same
/// number again as an area.
inline constexpr f32 kReduceTolerance = 0.00039999999f;

/// The 1% margin a candidate must beat the incumbent by at every stage of point reduction. The
/// same shape of bias as the SAT's face and edge preferences, and load-bearing for the same
/// reason: it keeps a manifold from flickering between two near-equal choices.
inline constexpr f32 kReduceBias = 0.99000001f;

/// @brief Choose at most four of `points` to represent the patch `normal` faces along.
///
/// Deepest point first, then the one farthest from it, then the one farthest off that line, then
/// the one lying most outside the triangle those three make. Each stage can stop early, so this
/// returns one, two, three or four points and never zero for a non-empty input.
///
/// `manifold.normal` must be set on entry -- the last two stages measure area against it, and a
/// zero normal collapses every candidate to the same score.
void ReducePoints(Manifold& manifold, std::span<const ManifoldPoint> points);

/// @brief Cluster `fresh` by normal, reduce each cluster, and warm start against `previous`.
///
/// `meshRotation` takes a normal from the mesh's frame to the world, and exists only so the new
/// normals can be compared against old ones that are already there. Contacts are matched to a
/// previous patch by normal to within `kClusterCosine`, an old patch may be claimed once, and a
/// new patch that matches nothing starts cold.
std::vector<Manifold> ReduceManifolds(std::span<const Manifold> fresh,
                                      std::span<const Manifold> previous,
                                      const Vec4& meshRotation);

/// What the contact layer needs of a mesh: triangles by index, and "which triangles" for a
/// box. Height fields and tree meshes both answer it, and neither knows about the other.
///
/// Two triangle accessors on purpose: the narrowphase needs adjacency (the internal-edge fix
/// runs on it), the time-of-impact core does not, and a height field pays real work to invent
/// neighbours at its borders that the TOI loop would then ignore.
class TriangleSource {
public:
    virtual ~TriangleSource() = default;
    virtual Triangle FullTriangle(i32 index) const = 0;
    virtual Triangle PlainTriangle(i32 index) const = 0;

    /// Triangle indices overlapping `bounds`, in the mesh's own frame, **strictly ascending**
    /// -- both implementations below produce them sorted and the buffer merge relies on it.
    virtual void Query(const Aabb& bounds, std::vector<i32>& out) const = 0;
};

class HeightFieldSource final : public TriangleSource {
public:
    explicit HeightFieldSource(const HeightField& field) : field_(&field) {}
    Triangle FullTriangle(i32 index) const override {
        return HeightFieldTriangleFull(*field_, index);
    }
    Triangle PlainTriangle(i32 index) const override {
        return HeightFieldTriangle(*field_, index);
    }
    void Query(const Aabb& bounds, std::vector<i32>& out) const override {
        HeightFieldQuery(*field_, bounds, out);
    }

private:
    const HeightField* field_;
};

class TreeMeshSource final : public TriangleSource {
public:
    explicit TreeMeshSource(const TreeMesh& mesh) : mesh_(&mesh) {}
    Triangle FullTriangle(i32 index) const override { return mesh_->GetTriangleFull(index); }
    Triangle PlainTriangle(i32 index) const override { return mesh_->GetTriangle(index); }
    void Query(const Aabb& bounds, std::vector<i32>& out) const override {
        mesh_->Query(bounds, out);
    }

private:
    const TreeMesh* mesh_;
};

/// @brief Collide a shape against every triangle the entries name, and reduce the result.
///
/// `xfMesh` places the mesh in the world and is applied **last**: everything upstream of it runs
/// in the mesh's own frame, which is why a mesh never has to transform its triangles. The shape
/// arrives already expressed in that frame -- a whole transform for the polytope, a transformed
/// copy for the sphere and capsule.
///
/// The three are separate entry points on purpose: they disagree about how the 0.01 margin
/// enters (the capsule reads the triangle's stamped radius, the sphere and polytope bake their
/// own), and unifying them would change resting separations. Only the polytope entry is
/// stateful: it runs each triangle's `SatCache` protocol (mutable entries), and `invalidateSat`
/// -- set by a TOI advance -- cools every cache first, because a freshly teleported body's
/// pre-advance separating axes mean nothing. Sphere and capsule take the entries only for the
/// triangle indices; their GJK warm start changes which simplex the exact answer is found from,
/// never the answer.
std::vector<Manifold> CollideMeshSphere(const TriangleSource& mesh,
                                        std::span<const MeshTriangleEntry> entries,
                                        const Sphere& sphere, const Transform& xfMesh,
                                        std::span<const Manifold> previous);
std::vector<Manifold> CollideMeshCapsule(const TriangleSource& mesh,
                                         std::span<const MeshTriangleEntry> entries,
                                         const Capsule& capsule, const Transform& xfMesh,
                                         std::span<const Manifold> previous);
std::vector<Manifold> CollideMeshPolytope(const TriangleSource& mesh,
                                          std::span<MeshTriangleEntry> entries,
                                          const Polytope& polytope, const Transform& xfShape,
                                          const Transform& xfMesh,
                                          std::span<const Manifold> previous, bool invalidateSat);

// -- the triangle buffer: which triangles this contact is near, with per-triangle warm state --
//
// `MeshTriangleEntry` and `MeshContactState` live in contact.h -- the Contact owns one -- and
// the two functions below are their whole behaviour.

/// @brief Refresh `state` for a convex fixture whose swept bounds are `localBounds` (already
/// in the mesh's frame).
///
/// If the box still fits inside the cached fattened one, the previous triangle set and all of
/// its warm state are kept untouched -- a body settling on terrain re-queries nothing. On a
/// refresh, entries are merged by ascending triangle index: a survivor keeps its cache and
/// axis, a newcomer starts cold.
void UpdateTriangleBuffer(MeshContactState& state, const TriangleSource& mesh,
                          const Aabb& localBounds);

/// @brief The mesh half of continuous collision: the convex core once per candidate triangle,
/// wrapped in per-triangle early-outs and a deliberately asymmetric reduction.
///
/// Per triangle, in order: the cached-axis skip (separated at both sweep endpoints and
/// closing by less than 0.005 over the step -- against zero, not the core's target, so a
/// triangle inside the target band can legitimately be skipped); the back-face cull against
/// the convex shape's centroid at sweep start, in mesh space; then `TimeOfImpact` with the
/// entry's warm cache, every triangle proxy carrying radius 0.01.
///
/// The reduction is min-t over touching results -- but an **overlap on any triangle wins
/// instantly**, returning mid-scan and discarding an already-found earlier touch. That early
/// return is intended behaviour, not an optimisation to tidy (see the loop). No triangle
/// touching returns `{Separated, FLT_MAX}`.
ToiOutput MeshTimeOfImpact(MeshContactState& state, const TriangleSource& mesh,
                           const Sweep& sweepMesh, const Vec4& localCentreMesh,
                           const SupportProxy& convex, const Vec4& convexCentroid,
                           const Sweep& sweepConvex, const Vec4& localCentreConvex);

}  // namespace snowball
