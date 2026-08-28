#include "renderer/profiles/diablo3/d3_collision.h"

#include "io/d3/d3_types.h"

#include <cstddef>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace {

using model::CollisionShapeData;
using model::CollisionShapeType;

} // namespace

bool D3BoneIsDynamic(const d3n::BoneStructure& bone, i32 lod) {
    for (const auto& s : bone.arCollisionShapes)
        if (s.nLodIndex == lod && s.flScaleX > 0.0f)
            return true;
    return false;
}

bool D3HasDynamicBody(const d3n::Appearances& app, i32 lod) {
    for (const auto& b : app.arBones)
        if (D3BoneIsDynamic(b, lod))
            return true;
    return false;
}

bool D3BoneIsAnchor(const d3n::BoneStructure& bone) {
    for (const auto& s : bone.arCollisionShapes)
        if ((s.dwFlags & 1) != 0)
            return true;
    return false;
}

bool D3HasRagdollAnchor(const d3n::Appearances& app) {
    for (const auto& b : app.arBones)
        if (D3BoneIsAnchor(b))
            return true;
    return false;
}

D3CollisionShapes D3BuildCollisionShapes(const d3n::Appearances& app,
                                         std::span<const ::whiteout::u8> appBytes, i32 lodIndex) {
    D3CollisionShapes out;
    // The stage's own body count, replayed so the overlay and the solver agree
    // on which bones made the cap. It counts the bodies that *survive*: the
    // builder creates one per bone with a non-empty shape list, the bridge
    // destroys any that ended up with no fixture, and the counter increments
    // only when a body comes back. So a bone whose shapes are all authored for
    // another LOD costs nothing against the 64.
    std::size_t bodies = 0;
    for (std::size_t b = 0; b < app.arBones.size(); ++b) {
        const auto& bone = app.arBones[b];
        bool atLod = false;
        for (const auto& s : bone.arCollisionShapes)
            if (s.nLodIndex == lodIndex) {
                atLod = true;
                break;
            }
        const bool bodied = atLod && bodies < kD3MaxBodies;
        if (bodied)
            ++bodies;

        // Static unless a LOD-matching shape has a positive `flScaleX`, which
        // is the whole of `Physics_CreateActorBoneBodies`'s body-type decision.
        // Per BONE, not per shape: one body carries the bone's whole shape list.
        model::CollisionBodyKind kind = model::CollisionBodyKind::None;
        if (bodied) {
            kind = D3BoneIsDynamic(bone, lodIndex) ? model::CollisionBodyKind::Dynamic
                                                   : model::CollisionBodyKind::Static;
        }

        for (const auto& s : bone.arCollisionShapes) {
            if (s.nLodIndex != lodIndex)
                continue;

            CollisionShapeData d;
            d.bodyKind = static_cast<i32>(kind);
            // **Left -1 on purpose, unlike StarCraft II.** There, a body's type
            // is a channel and the overlay has to re-read it per frame through
            // `FrameState::physicsBodyDynamic`; D3 decides once, at build, from
            // one float per shape, so the authored answer above IS the resolved
            // one and a per-frame lookup would have nothing to say.
            d.bodyIndex = -1;

            switch (s.eShapeType) {
            case 0:
                d.type = static_cast<i32>(CollisionShapeType::Sphere);
                d.vertices[0] = s.vPointA;
                d.vertices[1] = s.vPointA;
                d.radius = s.flRadius;
                break;
            case 1:
                d.type = static_cast<i32>(CollisionShapeType::Capsule);
                d.vertices[0] = s.vPointA;
                d.vertices[1] = s.vPointB;
                d.radius = s.flRadius;
                break;
            case 2: {
                if (appBytes.empty())
                    continue;
                auto poly = io::D3ReadPolytope(s.arPolytopeData, appBytes);
                if (!poly)
                    continue;
                d.type = static_cast<i32>(CollisionShapeType::Hull);
                d.radius = 0.0f;
                d.vertices[0] = poly->centroid;
                d.vertices[1] = poly->centroid;
                d.hullPoints = std::move(poly->points);
                d.hullEdges = std::move(poly->edges);
                break;
            }
            default:
                // Unreached on shipped content — the corpus holds 0, 1 and 2
                // and nothing else — so this is a bad file, not a fourth kind.
                continue;
            }

            out.shapes.push_back(std::move(d));
            out.bones.push_back(static_cast<i32>(b));
        }
    }
    return out;
}

void D3PlaceCollisionShapes(std::span<const i32> bones, std::span<const Matrix44f> boneWorld,
                            std::vector<Matrix44f>& out) {
    out.assign(bones.size(), Matrix44f::identity());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const i32 b = bones[i];
        if (b >= 0 && static_cast<std::size_t>(b) < boneWorld.size())
            out[i] = boneWorld[static_cast<std::size_t>(b)];
    }
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
