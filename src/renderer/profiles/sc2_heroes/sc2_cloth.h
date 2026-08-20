#pragma once

// ============================================================================
// `PHCL` cloth — the soft-body half of StarCraft II's Domino glue, beside
// `sc2_physics.h`'s rigid-body half.
//
// Cloth shares the engine's name and almost nothing else: `snowball::Cloth` is
// a position-based particle solver with no bodies, no islands and no scene, and
// the host feeds it a whole frame at a time rather than driving individual
// proxies. What it *does* share is the shape of the glue — one chunk family
// mapped onto Snowball's records, one per-frame bridge between the animated
// skeleton and the simulation, and one write-back.
//
// An M3 cloth is three chunks and two mesh regions:
//
//   - `PHCL` names the simulated region (in `clothMeshCount`, which is a REGN
//     *index*, not a count), carries the tuning block, and holds one
//     per-simulated-vertex record: a movable flag and a four-slot bone skin.
//   - `PHCC` is a capsule collider on a bone — the body the cloth drapes over.
//   - `PHAC` binds the *rendered* region to the simulated one: four cloth
//     vertices and four weights per visible vertex.
//
// So the region flagged `ClothSimulated` is a coarse invisible proxy, the
// region flagged `ClothInfluenced` is what draws, and the second is skinned to
// the first. That last step is why this file ends up in the skinning palette
// rather than in a vertex-deform pass: each particle carries a full output
// frame, so a particle is a bone as far as the shader is concerned, and the
// visible region's four-bone skin becomes a four-*particle* skin over a palette
// of nodes appended after the real skeleton (see @ref Sc2ClothBuild).
//
// That is not an economy taken here: it is what StarCraft II does, one layer
// down. `M3Cloth_SkinInfluencedMesh` builds a matrix per particle and skins the
// visible vertices by `inverseBind x liveFrame` on the CPU, into a dynamic
// vertex buffer. The product is the same; a renderer that already has a
// skinning palette can hand the second half to the GPU.
//
// The recovery is `DOMINO_GLUE.md` §6.8 (this half) and `CLOTH_HOST_NOTES.md`
// (the solver's host contract) in the Domino repo, from `CModelPhysics_Build`,
// `M3Physics_StepCloth` and `M3Cloth_SkinInfluencedMesh` in the SC2 IDB.
// Compiled only under WDX_ENABLE_PHYSICS.
// ============================================================================

#include "whiteout/flakes/pose_stage.h"

#include "snowball/cloth.h"

#include <memory>
#include <vector>

namespace whiteout {
namespace m3 {
struct Model;
}
} // namespace whiteout

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

/// @brief One `PHCL`, built into the records `snowball::Cloth::Create` consumes.
struct Sc2ClothPiece {
    /// @brief Everything but the per-frame inputs. Kept for the lifetime of the
    ///        stage because `Cloth::SetScale` rebuilds from it.
    ::snowball::ClothDef def;

    /// @brief Which `PHCL` this came from, so a caller can get back to the
    ///        `PHAC` records without re-deriving which chunk names which region.
    std::size_t chunkIndex = 0;
    /// @brief REGN index of the simulated (invisible proxy) region.
    std::size_t simRegion = 0;
    /// @brief REGN indices of the visible regions this cloth drives, from the
    ///        `PHAC` records that name it.
    std::vector<std::size_t> influencedRegions;

    /// @brief Simulated-region vertex -> particle, `-1` for a vertex the build
    ///        dropped. `BuildCloth` reorders particles pinned-first, so this is
    ///        the only way back from a `PHAC` slot to a palette slot.
    std::vector<::whiteout::i16> oldToNew;

    /// @brief Each particle's rest position, in particle order. The inverse
    ///        bind of the node the particle drives is `translate(-restPosition)`
    ///        — see @ref Sc2ClothBuild.
    std::vector<Vector3f> restPositions;

    /// @brief Bones whose anchor SQTs the frame has to refresh (`PHCL.skinBones`).
    std::vector<::whiteout::u16> skinBones;

    /// @brief First palette node this cloth's particles occupy, relative to the
    ///        end of the real skeleton.
    std::size_t firstParticle = 0;
    std::size_t particleCount = 0;
};

/// @brief Every cloth on a model, plus the palette layout they imply.
///
/// The particles of every piece are laid end to end after the model's real
/// bones, so node `bones.size() + firstParticle + i` is particle `i` of that
/// piece. Two things then fall out for free:
///
///   - the skinning palette needs no new concept — `SkeletonData::nodeCount`
///     grows, `inverseBindMatrices` gets `translate(-restPosition)` per
///     particle, and the influenced region's `subsetNodeIndices` names the
///     particle nodes instead of bones;
///   - the simulation is an ordinary pose stage, because "write a node's
///     model-space matrix" is exactly what a pose stage does.
///
/// A particle's inverse bind is a pure translation because the *rotation* half
/// is already inside the output frame: Snowball's `BuildOutputFrames` composes
/// the authoring-built reference rows through the live particle basis, and with
/// the identity bind rows fed in here that product is exactly
/// `restBasis^-1 * liveBasis`, with the live particle position as the origin.
struct Sc2ClothBuild {
    std::vector<Sc2ClothPiece> pieces;
    /// @brief Particles across all pieces; the number of nodes appended.
    std::size_t particleCount = 0;
};

/// @brief Build every `PHCL` on @p model, or an empty result if it has none we
///        can drive.
///
/// Three populations are skipped, and only the last is a limitation:
///
///   - a `PHCL` with **no per-vertex data at all**, which is not a cloth but a
///     *collider source* — capsules for another model's cape to collide with,
///     attached by the actor layer. Most of the corpus's records.
///   - a chunk that disagrees with its geometry (a `clothMeshCount` that is not
///     a region, a per-vertex array whose length is not the region's vertex
///     count). None measured.
///   - **more than 256 particles**, which neither an M3 vertex's `u8` bone
///     index nor a 256-matrix palette can address. Twenty of the corpus's 216
///     mesh-carrying cloths.
///
/// Skipped means the model renders with its authored skin, which carries the
/// cape rigidly rather than not at all.
Sc2ClothBuild Sc2BuildCloth(const ::whiteout::m3::Model& model);

/// @brief The per-frame bridge: animated skeleton in, particle frames out.
///
/// Runs after the rigid-body stage for the same reason that one runs last —
/// it consumes the corrected pose. Claims nothing: the nodes it writes are
/// appended past the skeleton and no sampler ever touches them, so there is
/// nothing for a claim to suppress.
///
/// @param firstNode  palette index of particle 0 of piece 0, i.e. the model's
///                   bone count.
std::unique_ptr<animation::IPoseStage>
CreateSc2ClothStage(const ::whiteout::m3::Model& model,
                    std::shared_ptr<const Sc2ClothBuild> build, i32 firstNode);

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
