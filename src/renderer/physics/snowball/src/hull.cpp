#include "snowball/hull.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace snowball {
namespace {

using Edge = std::pair<i32, i32>;

struct Triangle {
    i32 v[3]{};
    Vec4 normal{};
    f32 offset{0.0f};
};

f32 Length3(const Vec4& v) { return std::sqrt(LengthSquared3(v)); }

/// A triangle wound so its normal points away from `interior`.
Triangle MakeTriangle(const std::vector<Vec4>& pts, i32 a, i32 b, i32 c, const Vec4& interior) {
    Triangle t;
    t.v[0] = a;
    t.v[1] = b;
    t.v[2] = c;
    Vec4 n = Cross3(pts[b] - pts[a], pts[c] - pts[a]);
    const f32 len = Length3(n);
    n = len > 0.0f ? n * (1.0f / len) : Vec4{0.0f, 0.0f, 0.0f};
    f32 d = Dot3(n, pts[a]);
    if (Dot3(n, interior) - d > 0.0f) {
        std::swap(t.v[1], t.v[2]);
        n = -n;
        d = -d;
    }
    t.normal = n;
    t.offset = d;
    return t;
}

/// The four seed points: an extreme vertex, the point farthest from it, the point farthest
/// from that line, and the point farthest from that plane. Returns false when the cloud is
/// collinear or coplanar and so encloses nothing.
bool SeedTetrahedron(const std::vector<Vec4>& pts, f32 tolerance, i32 seed[4]) {
    const auto n = static_cast<i32>(pts.size());
    i32 i0 = 0;
    for (i32 i = 1; i < n; ++i) {
        if (pts[i].x < pts[i0].x) {
            i0 = i;
        }
    }

    i32 i1 = -1;
    f32 best = tolerance;
    for (i32 i = 0; i < n; ++i) {
        const f32 d = Length3(pts[i] - pts[i0]);
        if (d > best) {
            best = d;
            i1 = i;
        }
    }
    if (i1 < 0) {
        return false;
    }

    i32 i2 = -1;
    best = tolerance;
    const Vec4 axis = pts[i1] - pts[i0];
    for (i32 i = 0; i < n; ++i) {
        const f32 d = Length3(Cross3(axis, pts[i] - pts[i0]));
        if (d > best) {
            best = d;
            i2 = i;
        }
    }
    if (i2 < 0) {
        return false;
    }

    Vec4 normal = Cross3(pts[i1] - pts[i0], pts[i2] - pts[i0]);
    const f32 len = Length3(normal);
    if (len <= tolerance) {
        return false;
    }
    normal = normal * (1.0f / len);

    i32 i3 = -1;
    best = tolerance;
    for (i32 i = 0; i < n; ++i) {
        const f32 d = std::fabs(Dot3(normal, pts[i] - pts[i0]));
        if (d > best) {
            best = d;
            i3 = i;
        }
    }
    if (i3 < 0) {
        return false;
    }

    seed[0] = i0;
    seed[1] = i1;
    seed[2] = i2;
    seed[3] = i3;
    return true;
}

std::vector<Triangle> BuildTriangleHull(const std::vector<Vec4>& pts, f32 tolerance) {
    i32 seed[4];
    if (!SeedTetrahedron(pts, tolerance, seed)) {
        return {};
    }

    // Stays inside for the whole build: the hull only ever grows outward from here, so it is a
    // reliable reference for which way a face should point.
    const Vec4 interior =
        (pts[seed[0]] + pts[seed[1]] + pts[seed[2]] + pts[seed[3]]) * 0.25f;

    std::vector<Triangle> faces;
    for (i32 skip = 0; skip < 4; ++skip) {
        i32 v[3], k = 0;
        for (i32 i = 0; i < 4; ++i) {
            if (i != skip) {
                v[k++] = seed[i];
            }
        }
        faces.push_back(MakeTriangle(pts, v[0], v[1], v[2], interior));
    }

    std::vector<u8> consumed(pts.size(), 0);
    for (const i32 s : seed) {
        consumed[static_cast<usize>(s)] = 1;
    }

    for (usize pi = 0; pi < pts.size(); ++pi) {
        if (consumed[pi]) {
            continue;
        }
        const Vec4& p = pts[pi];

        std::vector<u8> visible(faces.size(), 0);
        bool any = false;
        for (usize f = 0; f < faces.size(); ++f) {
            if (Dot3(faces[f].normal, p) - faces[f].offset > tolerance) {
                visible[f] = 1;
                any = true;
            }
        }
        if (!any) {
            continue;  // inside the hull already
        }

        // The horizon is every directed edge of the visible set whose reverse is not also
        // visible -- i.e. the silhouette where the visible cap meets the rest of the hull.
        std::set<Edge> visibleEdges;
        for (usize f = 0; f < faces.size(); ++f) {
            if (!visible[f]) {
                continue;
            }
            for (i32 k = 0; k < 3; ++k) {
                visibleEdges.emplace(faces[f].v[k], faces[f].v[(k + 1) % 3]);
            }
        }
        std::vector<Edge> horizon;
        for (const Edge& e : visibleEdges) {
            if (visibleEdges.find({e.second, e.first}) == visibleEdges.end()) {
                horizon.push_back(e);
            }
        }

        std::vector<Triangle> kept;
        kept.reserve(faces.size());
        for (usize f = 0; f < faces.size(); ++f) {
            if (!visible[f]) {
                kept.push_back(faces[f]);
            }
        }
        faces.swap(kept);
        for (const Edge& e : horizon) {
            faces.push_back(MakeTriangle(pts, e.first, e.second, static_cast<i32>(pi), interior));
        }
        consumed[pi] = 1;
    }
    return faces;
}

/// Assign each triangle to a plane group, so coplanar neighbours become one polygon.
std::vector<i32> GroupByPlane(const std::vector<Triangle>& faces, f32 tolerance) {
    std::vector<i32> group(faces.size(), -1);
    std::vector<const Triangle*> planes;
    for (usize f = 0; f < faces.size(); ++f) {
        for (usize g = 0; g < planes.size(); ++g) {
            if (Dot3(faces[f].normal, planes[g]->normal) > 1.0f - tolerance &&
                std::fabs(faces[f].offset - planes[g]->offset) <= tolerance) {
                group[f] = static_cast<i32>(g);
                break;
            }
        }
        if (group[f] < 0) {
            group[f] = static_cast<i32>(planes.size());
            planes.push_back(&faces[f]);
        }
    }
    return group;
}

/// Chain a group's boundary edges into a single closed loop of vertices.
std::vector<i32> BoundaryLoop(const std::vector<Triangle>& faces, const std::vector<i32>& group,
                              i32 which) {
    std::set<Edge> inside;
    for (usize f = 0; f < faces.size(); ++f) {
        if (group[f] != which) {
            continue;
        }
        for (i32 k = 0; k < 3; ++k) {
            inside.emplace(faces[f].v[k], faces[f].v[(k + 1) % 3]);
        }
    }

    std::map<i32, i32> nextOf;
    for (const Edge& e : inside) {
        if (inside.find({e.second, e.first}) == inside.end()) {
            nextOf[e.first] = e.second;
        }
    }
    if (nextOf.empty()) {
        return {};
    }

    std::vector<i32> loop;
    const i32 start = nextOf.begin()->first;
    i32 at = start;
    do {
        loop.push_back(at);
        const auto it = nextOf.find(at);
        if (it == nextOf.end()) {
            return {};  // not a closed boundary; the group was not a simple polygon
        }
        at = it->second;
    } while (at != start && loop.size() <= nextOf.size());
    return at == start ? loop : std::vector<i32>{};
}

}  // namespace

std::vector<i32> Hull::FaceVertices(i32 face) const {
    std::vector<i32> out;
    if (face < 0 || face >= static_cast<i32>(faces.size())) {
        return out;
    }
    const i32 first = faces[static_cast<usize>(face)].firstEdge;
    i32 e = first;
    do {
        out.push_back(edges[static_cast<usize>(e)].origin);
        e = edges[static_cast<usize>(e)].next;
    } while (e != first && out.size() <= edges.size());
    return out;
}

Hull BuildHull(std::span<const Vec4> points, f32 tolerance) {
    Hull hull;

    std::vector<Vec4> pts;
    pts.reserve(points.size());
    for (const Vec4& p : points) {
        const bool duplicate = std::any_of(pts.begin(), pts.end(), [&](const Vec4& q) {
            return LengthSquared3(p - q) <= tolerance * tolerance;
        });
        if (!duplicate) {
            pts.push_back(p);
        }
    }
    if (pts.size() < 4) {
        return hull;
    }

    const std::vector<Triangle> triangles = BuildTriangleHull(pts, tolerance);
    if (triangles.empty()) {
        return hull;
    }

    // Merge coplanar triangles, then keep only the vertices the merged faces actually use --
    // points strictly inside the hull never appear in a loop and must not be counted.
    const std::vector<i32> group = GroupByPlane(triangles, 1e-4f);
    const i32 groupCount = *std::max_element(group.begin(), group.end()) + 1;

    std::vector<std::vector<i32>> loops;
    std::vector<Triangle> loopPlane;
    for (i32 g = 0; g < groupCount; ++g) {
        std::vector<i32> loop = BoundaryLoop(triangles, group, g);
        if (loop.size() < 3) {
            return {};  // a group that is not a simple polygon means the hull is unusable
        }
        for (usize f = 0; f < triangles.size(); ++f) {
            if (group[f] == g) {
                loopPlane.push_back(triangles[f]);
                break;
            }
        }
        loops.push_back(std::move(loop));
    }

    std::map<i32, i32> remap;
    for (const std::vector<i32>& loop : loops) {
        for (const i32 v : loop) {
            if (remap.find(v) == remap.end()) {
                const i32 index = static_cast<i32>(hull.vertices.size());
                remap.emplace(v, index);
                hull.vertices.push_back(pts[static_cast<usize>(v)]);
            }
        }
    }

    for (usize g = 0; g < loops.size(); ++g) {
        HullFace face;
        face.firstEdge = static_cast<i32>(hull.edges.size());
        face.normal = loopPlane[g].normal;
        face.offset = loopPlane[g].offset;
        const i32 base = face.firstEdge;
        const auto count = static_cast<i32>(loops[g].size());
        for (i32 k = 0; k < count; ++k) {
            HalfEdge e;
            e.origin = remap[loops[g][static_cast<usize>(k)]];
            e.face = static_cast<i32>(hull.faces.size());
            e.next = base + (k + 1) % count;
            hull.edges.push_back(e);
        }
        hull.faces.push_back(face);
    }

    std::map<Edge, i32> byEndpoints;
    for (usize e = 0; e < hull.edges.size(); ++e) {
        const i32 from = hull.edges[e].origin;
        const i32 to = hull.edges[static_cast<usize>(hull.edges[e].next)].origin;
        byEndpoints.emplace(Edge{from, to}, static_cast<i32>(e));
    }
    for (usize e = 0; e < hull.edges.size(); ++e) {
        const i32 from = hull.edges[e].origin;
        const i32 to = hull.edges[static_cast<usize>(hull.edges[e].next)].origin;
        const auto it = byEndpoints.find({to, from});
        hull.edges[e].twin = it == byEndpoints.end() ? -1 : it->second;
    }
    return hull;
}

void ScaleHull(Hull& hull, f32 scale) {
    for (Vec4& v : hull.vertices) {
        v = Vec4{v.x * scale, v.y * scale, v.z * scale, 0.0f};
    }
    for (HullFace& f : hull.faces) {
        f.offset *= scale;
    }
}

}  // namespace snowball
