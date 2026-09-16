#include "core/mesh_overlay.h"

#include <algorithm>
#include <unordered_map>

namespace whiteout::flakes::renderer::core {

bool SetMeshElementFlags(MeshElementStates& states, MeshElementKind kind, u32 elementCount,
                         std::span<const u32> ids, u8 mask, u8 flags) {
    std::vector<u8>& v = (kind == MeshElementKind::Vertex) ? states.vertices : states.faces;
    bool changed = false;
    for (u32 id : ids) {
        if (id >= elementCount)
            continue;
        if (v.size() < elementCount) {
            // Clearing on an untouched geoset leaves it untouched.
            if ((flags & mask) == 0)
                continue;
            v.resize(elementCount, 0);
        }
        const u8 next = static_cast<u8>((v[id] & ~mask) | (flags & mask));
        changed |= next != v[id];
        v[id] = next;
    }
    return changed;
}

std::vector<MeshEdge> BuildMeshEdges(std::span<const u32> indices, u32 vertexCount) {
    std::vector<MeshEdge> edges;
    edges.reserve(indices.size() / 2);
    std::unordered_map<u64, u32> seen;
    seen.reserve(indices.size());

    const u32 faceCount = static_cast<u32>(indices.size() / 3);
    for (u32 f = 0; f < faceCount; ++f) {
        const u32 tri[3] = {indices[3 * f], indices[3 * f + 1], indices[3 * f + 2]};
        if (tri[0] >= vertexCount || tri[1] >= vertexCount || tri[2] >= vertexCount)
            continue;
        for (u32 k = 0; k < 3; ++k) {
            const u32 a = std::min(tri[k], tri[(k + 1) % 3]);
            const u32 b = std::max(tri[k], tri[(k + 1) % 3]);
            if (a == b)
                continue;
            const u64 key = (static_cast<u64>(a) << 32) | b;
            auto [it, inserted] = seen.try_emplace(key, static_cast<u32>(edges.size()));
            if (inserted) {
                MeshEdge e;
                e.a = a;
                e.b = b;
                e.faces[0] = f;
                edges.push_back(e);
                continue;
            }
            MeshEdge& e = edges[it->second];
            // A triangle whose two corners repeat can name the same edge twice.
            if (e.faces[0] != f && e.faces[1] == kMeshNoFace)
                e.faces[1] = f;
        }
    }
    return edges;
}

namespace {

u8 StateAt(const std::vector<u8>& v, u32 i) {
    return i < v.size() ? v[i] : 0;
}

} // namespace

u8 MeshEdgeState(const MeshEdge& edge, const MeshElementStates& states) {
    const u8 ends = StateAt(states.vertices, edge.a) & StateAt(states.vertices, edge.b);
    u8 marks = ends & (kMeshElementSelected | kMeshElementHovered);
    bool allFacesHidden = true;
    for (u32 face : edge.faces) {
        if (face == kMeshNoFace)
            continue;
        const u8 s = StateAt(states.faces, face);
        marks |= s & (kMeshElementSelected | kMeshElementHovered);
        allFacesHidden &= (s & kMeshElementHidden) != 0;
    }
    const u8 anyEnd = StateAt(states.vertices, edge.a) | StateAt(states.vertices, edge.b);
    const bool hidden =
        (anyEnd & kMeshElementHidden) != 0 || (edge.faces[0] != kMeshNoFace && allFacesHidden);
    return static_cast<u8>(marks | (hidden ? kMeshElementHidden : 0));
}

bool AnyMarkedFace(const MeshElementStates& states) {
    return std::any_of(states.faces.begin(), states.faces.end(), [](u8 s) {
        return (s & (kMeshElementSelected | kMeshElementHovered)) != 0 &&
               (s & kMeshElementHidden) == 0;
    });
}

MeshOverlayLayout MeshOverlayLayoutFor(u32 vertexCount, u32 edgeCount, u32 faceCount) {
    MeshOverlayLayout l;
    l.vertexCount = vertexCount;
    l.edgeCount = edgeCount;
    l.faceCount = faceCount;
    l.vertexBase = 0;
    l.edgeBase = vertexCount * 3;
    l.faceBase = l.edgeBase + edgeCount;
    l.elementCount = l.faceBase + faceCount;
    return l;
}

void PackMeshOverlay(std::span<f32> out, const MeshOverlayGeometry& g,
                     const MeshElementStates* states, const MeshOverlayLayout& layout) {
    if (out.size() < static_cast<usize>(layout.elementCount) * 4)
        return;
    static const MeshElementStates kNone;
    const MeshElementStates& s = states ? *states : kNone;
    const bool skinned = g.boneWeights.size() >= static_cast<usize>(layout.vertexCount) * 4 &&
                         g.boneIndices.size() >= static_cast<usize>(layout.vertexCount) * 4;

    for (u32 v = 0; v < layout.vertexCount; ++v) {
        f32* r = &out[(layout.vertexBase + v * 3) * 4];
        const Vector3f p = v < g.positions.size() ? g.positions[v] : Vector3f{0, 0, 0};
        r[0] = p.x;
        r[1] = p.y;
        r[2] = p.z;
        r[3] = static_cast<f32>(StateAt(s.vertices, v));
        for (u32 k = 0; k < 4; ++k) {
            r[4 + k] = skinned ? static_cast<f32>(g.boneWeights[v * 4 + k]) / 255.0f : 0.0f;
            r[8 + k] = skinned ? static_cast<f32>(g.boneIndices[v * 4 + k]) : 0.0f;
        }
    }
    for (u32 e = 0; e < layout.edgeCount && e < g.edges.size(); ++e) {
        f32* r = &out[(layout.edgeBase + e) * 4];
        r[0] = static_cast<f32>(g.edges[e].a);
        r[1] = static_cast<f32>(g.edges[e].b);
        r[2] = static_cast<f32>(MeshEdgeState(g.edges[e], s));
        r[3] = 0.0f;
    }
    for (u32 f = 0; f < layout.faceCount; ++f) {
        f32* r = &out[(layout.faceBase + f) * 4];
        const bool valid = static_cast<usize>(f) * 3 + 2 < g.indices.size() &&
                           g.indices[f * 3] < layout.vertexCount &&
                           g.indices[f * 3 + 1] < layout.vertexCount &&
                           g.indices[f * 3 + 2] < layout.vertexCount;
        for (u32 k = 0; k < 3; ++k)
            r[k] = valid ? static_cast<f32>(g.indices[f * 3 + k]) : 0.0f;
        // A triangle that names a vertex the geoset does not have draws nothing.
        r[3] = static_cast<f32>(valid ? StateAt(s.faces, f) : kMeshElementHidden);
    }
}

namespace {

void Rgb(f32 out[4], u32 packed, f32 alpha) {
    out[0] = static_cast<f32>(packed & 0xFFu) / 255.0f;
    out[1] = static_cast<f32>((packed >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<f32>((packed >> 16) & 0xFFu) / 255.0f;
    out[3] = alpha;
}

void Set(f32 out[4], f32 r, f32 g, f32 b, f32 a) {
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

} // namespace

MeshOverlayColors MeshOverlayColorsFor(DebugView view, u32 teamColorRgb, u32 backgroundRgb) {
    MeshOverlayColors c;
    Rgb(c.face, teamColorRgb, 1.0f);
    Set(c.vertex, 0.04f, 0.04f, 0.04f, 1.0f);
    if (view == DebugView::Wireframe) {
        f32 bg[4];
        Rgb(bg, backgroundRgb, 1.0f);
        const f32 luma = 0.2126f * bg[0] + 0.7152f * bg[1] + 0.0722f * bg[2];
        if (luma > 0.5f)
            Set(c.edge, 0.08f, 0.08f, 0.10f, 1.0f);
        else
            Set(c.edge, 0.85f, 0.87f, 0.90f, 1.0f);
    } else {
        Set(c.edge, 0.02f, 0.02f, 0.02f, 0.85f);
    }
    return c;
}

} // namespace whiteout::flakes::renderer::core
