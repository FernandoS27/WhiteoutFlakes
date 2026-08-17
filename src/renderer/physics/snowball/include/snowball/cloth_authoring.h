//===----------------------------------------------------------------------===//
// snowball/cloth_authoring.h -- the mesh -> cloth-records build.
//
// The tool-side half of the cloth system: turns a skinned triangle mesh into the baked
// record set Cloth::Create consumes. The pipeline's operation ORDER is contract, two pieces
// of it especially:
//
//   * the sort tie order -- every comparator is single-key, so record order is decided by
//     the exact sort algorithm (a fixed vintage of libc++ std::sort, carried whole in the
//     .cpp), and the runtime's Gauss-Seidel sweeps observe that order;
//   * the degenerate-triangle window (a degenerate source triangle advances the record slot
//     without advancing the live count, shifting every later per-triangle pass) -- a
//     deliberate quirk, not a bug to close; see the .cpp.
//
// What the build derives, none of it authored per-vertex: inverse masses (1/3-area sums --
// the 0.16665 factor, deliberately not 1/6), the distance and bend records with their
// stiffness-lane classification, the transform-group islands, the pinned-first particle
// order with hop counts / collision proxies / geodesic tether lengths, and the bend rest
// dihedrals clamped to +-30 degrees with the wing heights that serve as correction clamps.
//
// The per-particle output-frame reference matrices, the rest-normal data and the anchor
// slot/weight skinning data are emitted into the def alongside the records, so a def built
// here is complete: Create consumes it without a second authoring pass.
//===----------------------------------------------------------------------===//
#pragma once

#include <array>
#include <vector>

#include "snowball/cloth.h"

namespace snowball {

/// One source vertex: bind rows, rest position, and the authored flags and skin data.
struct ClothMeshVertex {
    Vec4 bindX{constants::kUnitX};  // authored bind rows; Z doubles as the rest normal
    Vec4 bindY{constants::kUnitY};
    Vec4 bindZ{constants::kUnitZ};
    Vec4 restPosition{};
    bool movable{true};             // CLEAR means pinned
    bool selfCollision{false};      // participates in self collision
    std::array<i16, 4> anchors{-1, -1, -1, -1};
    std::array<f32, 4> weights{};
};

/// The build's input: the mesh to derive records from. Collider records pass through the
/// build untouched (capsules compacted on a zero feature type).
struct ClothMeshSource {
    std::vector<ClothMeshVertex> vertices;
    std::vector<std::array<u16, 3>> triangles;
    std::vector<ClothCapsuleDef> capsules;
    std::vector<ClothPlaneDef> planes;
};

/// The build's outputs: a ClothDef record set (world/scale/params left for the caller) plus
/// the observable side products the record buffers do not carry.
struct ClothBuildResult {
    ClothDef def;                 // particles/edges/bends/triangles/capsules/planes filled
    std::vector<i16> oldToNew;    // source vertex -> reordered particle index
    f32 totalArea{0.0f};
    u32 liveTriangleCount{0};     // def.triangles.size() equals this, window defect included
    u32 anchorIndexCount{0};      // max referenced anchor index + 1
    u32 anchorStateCount{0};      // anchors referenced by PINNED particles only
    std::vector<i16> anchorSlotLut;  // model anchor index -> compact state index or -1
};

/// Run the build. Returns false (result untouched) for vertexCount < 3 or an empty
/// triangle list -- the only reject path; everything else produces a def.
bool BuildCloth(const ClothMeshSource& source, ClothBuildResult& result);

}  // namespace snowball
