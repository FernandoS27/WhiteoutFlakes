#include "assets/sampler_asset_manager.h"
#include "assets/texture_asset_manager.h"
#include "constants.h"
#include "render_detail.h"
#include "render_service.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_pipeline_impl.h"
#include "renderer/render_service_impl.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace whiteout::flakes::renderer::debug {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::assets;

bool DebugRenderer::CreateResources() {
    return CreateGridResources();
}

void DebugRenderer::DestroyResources() {
    auto* dev = rs_.Pipeline().Gfx();
    if (!dev)
        return;
    dev->Destroy(gridVB_);
    gridVB_ = gfx::BufferHandle::Invalid;
    gridVertCount_ = 0;
}

bool DebugRenderer::CreateGridResources() {
    std::vector<LineVertex> lines;
    const f32 extent = 500.0f;
    const f32 step = 50.0f;
    Vector4f gridColor = {0.45f, 0.45f, 0.46f, 1.0f};
    Vector4f axisColorX = {0.75f, 0.2f, 0.2f, 1.0f};
    Vector4f axisColorY = {0.2f, 0.75f, 0.2f, 1.0f};

    for (f32 v = -extent; v <= extent; v += step) {
        Vector4f c = (v == 0.0f) ? axisColorY : gridColor;
        lines.push_back({{v, -extent, 0.0f}, c});
        lines.push_back({{v, extent, 0.0f}, c});
        c = (v == 0.0f) ? axisColorX : gridColor;
        lines.push_back({{-extent, v, 0.0f}, c});
        lines.push_back({{extent, v, 0.0f}, c});
    }
    gridVertCount_ = (i32)lines.size();

    gridVB_ = rs_.Pipeline().Gfx()->CreateBuffer(
        {
            .size = sizeof(LineVertex) * lines.size(),
            .usage = gfx::BufferUsage::Vertex,
        },
        lines.data());
    return gridVB_ != gfx::BufferHandle::Invalid;
}

void DebugRenderer::RenderGrid() {
    if (gridVB_ == gfx::BufferHandle::Invalid)
        return;
    auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();
    cmd->BindPipeline(rs_.Pipeline().CurrentLinePSO());
    cmd->BindVertexBuffer(0, gridVB_, sizeof(LineVertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, rs_.Pipeline().CbPerFrame());
    cmd->Draw(gridVertCount_, 0);
}

void DebugRenderer::RenderCollisions() {
    std::vector<CollisionShape> shapes;
    Matrix44f viewMat;
    {
        for (auto& [h, mi] : rs_.Scene().Actors().All()) {
            if (mi->parentVisibility <= 0.02f)
                continue;
            shapes.insert(shapes.end(), mi->render.collisionShapes.begin(),
                          mi->render.collisionShapes.end());
        }
        if (shapes.empty())
            return;
        viewMat = rs_.Scene().Camera().GetViewMatrix();
    }

    auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();
    cmd->BindPipeline(rs_.Pipeline().CurrentLinePSO());

    struct LV {
        Vector3f pos;
        Vector4f col;
    };
    Vector4f col = {0.0f, 1.0f, 0.3f, 1.0f};

    std::vector<LV> lines;

    for (auto& cs : shapes) {

        const Vector3f& piv = cs.pivot;
        auto pushLine = [&](const Vector3f& a, const Vector3f& b) {
            Vector3f ap = {a.x + piv.x, a.y + piv.y, a.z + piv.z};
            Vector3f bp = {b.x + piv.x, b.y + piv.y, b.z + piv.z};
            Vector3f pa = whiteout::transform_point(ap, cs.transform);
            Vector3f pb = whiteout::transform_point(bp, cs.transform);
            lines.push_back({pa, col});
            lines.push_back({pb, col});
        };
        auto emitCircle = [&](const Vector3f& c, f32 r, i32 axis) {
            const i32 segs = 24;
            for (i32 i = 0; i < segs; i++) {
                f32 a0 = (f32)i / segs * 6.28318530f;
                f32 a1 = (f32)(i + 1) / segs * 6.28318530f;
                f32 c0 = r * cosf(a0), s0 = r * sinf(a0);
                f32 c1 = r * cosf(a1), s1 = r * sinf(a1);
                Vector3f p0, p1;
                if (axis == 2) {
                    p0 = {c.x + c0, c.y + s0, c.z};
                    p1 = {c.x + c1, c.y + s1, c.z};
                } else if (axis == 1) {
                    p0 = {c.x + c0, c.y, c.z + s0};
                    p1 = {c.x + c1, c.y, c.z + s1};
                } else {
                    p0 = {c.x, c.y + c0, c.z + s0};
                    p1 = {c.x, c.y + c1, c.z + s1};
                }
                pushLine(p0, p1);
            }
        };

        if (cs.type == 0) {
            Vector3f mn = cs.vmin, mx = cs.vmax;
            Vector3f corners[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z},
                                   {mn.x, mx.y, mn.z}, {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
                                   {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
            i32 edges[24] = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6,
                             6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};
            for (i32 i = 0; i < 24; i += 2)
                pushLine(corners[edges[i]], corners[edges[i + 1]]);
        } else if (cs.type == 2) {
            emitCircle(cs.vmin, cs.radius, 0);
            emitCircle(cs.vmin, cs.radius, 1);
            emitCircle(cs.vmin, cs.radius, 2);
        } else if (cs.type == 1) {
            Vector3f axisVec = {cs.vmax.x - cs.vmin.x, cs.vmax.y - cs.vmin.y,
                                cs.vmax.z - cs.vmin.z};
            f32 axisLen =
                std::sqrt(axisVec.x * axisVec.x + axisVec.y * axisVec.y + axisVec.z * axisVec.z);
            Vector3f axis = (axisLen > 1e-5f) ? Vector3f{axisVec.x / axisLen, axisVec.y / axisLen,
                                                         axisVec.z / axisLen}
                                              : Vector3f{0, 0, 1};
            Vector3f tmp = (std::abs(axis.z) < 0.9f) ? Vector3f{0, 0, 1} : Vector3f{1, 0, 0};
            Vector3f u = {axis.y * tmp.z - axis.z * tmp.y, axis.z * tmp.x - axis.x * tmp.z,
                          axis.x * tmp.y - axis.y * tmp.x};
            f32 uLen = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
            if (uLen > 1e-5f) {
                u.x /= uLen;
                u.y /= uLen;
                u.z /= uLen;
            }
            Vector3f v = {axis.y * u.z - axis.z * u.y, axis.z * u.x - axis.x * u.z,
                          axis.x * u.y - axis.y * u.x};
            const i32 segs = 24;
            auto ringPt = [&](const Vector3f& c, f32 a) {
                f32 cs_ = cs.radius * cosf(a), sn_ = cs.radius * sinf(a);
                return Vector3f{c.x + u.x * cs_ + v.x * sn_, c.y + u.y * cs_ + v.y * sn_,
                                c.z + u.z * cs_ + v.z * sn_};
            };
            for (i32 i = 0; i < segs; i++) {
                f32 a0 = (f32)i / segs * 6.28318530f;
                f32 a1 = (f32)(i + 1) / segs * 6.28318530f;
                pushLine(ringPt(cs.vmin, a0), ringPt(cs.vmin, a1));
                pushLine(ringPt(cs.vmax, a0), ringPt(cs.vmax, a1));
            }
            for (i32 i = 0; i < 4; i++) {
                f32 a = (f32)i / 4 * 6.28318530f;
                pushLine(ringPt(cs.vmin, a), ringPt(cs.vmax, a));
            }
        } else if (cs.type == 3) {
            f32 hw = cs.vmin.x, hh = cs.vmin.y;
            Vector3f p0 = {-hw, -hh, 0}, p1 = {hw, -hh, 0};
            Vector3f p2 = {hw, hh, 0}, p3 = {-hw, hh, 0};
            pushLine(p0, p1);
            pushLine(p1, p2);
            pushLine(p2, p3);
            pushLine(p3, p0);
        }
    }

    if (lines.empty())
        return;

    {
        f32 aspect = (rs_.Pipeline().Height() > 0)
                         ? (f32)rs_.Pipeline().Width() / (f32)rs_.Pipeline().Height()
                         : 1.0f;
        render_detail::CbPerFrameDesc d;
        d.view = viewMat;
        d.projection = rs_.Scene().Camera().ProjectionRH(aspect);
        d.lightColor = kCollisionLightColor;
        d.ambientColor = kCollisionAmbientColor;
        render_detail::WriteCbPerFrame(rs_.Pipeline().Gfx(), rs_.Pipeline().CbPerFrame(), d);
    }
    DrawWireLines(rs_.Pipeline().Gfx(), cmd, rs_.Pipeline().CbPerFrame(), lines);
}

void DebugRenderer::RenderLightMarkers() {
    struct MarkerLight {
        Vector3f worldPos;
        Vector3f worldDir;
        Vector3f diffuse;
        bool isDirectional;
        bool enabled;
    };
    std::vector<MarkerLight> lights;
    Matrix44f viewMat;
    {
        for (auto& [h, mi] : rs_.Scene().Actors().All()) {
            if (mi->parentVisibility <= 0.02f)
                continue;
            for (const auto& L : mi->render.activeLights) {
                const bool dir = (L.kind == FrameState::LightKind::Directional);
                lights.push_back(
                    {dir ? whiteout::transform_point(Vector3f{0, 0, 0}, mi->worldTransform)
                         : L.worldPos,
                     L.worldDir, L.diffuse, dir, L.enabled});
            }
        }
        if (lights.empty())
            return;
        viewMat = rs_.Scene().Camera().GetViewMatrix();
    }

    auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();
    cmd->BindPipeline(rs_.Pipeline().CurrentLinePSO());

    struct LV {
        Vector3f pos;
        Vector4f col;
    };
    std::vector<LV> verts;
    verts.reserve(lights.size() * 3 * 24 * 2 + lights.size() * 2);

    const f32 kMarkerRadius = 20.0f;
    const i32 kSegs = 24;
    for (const auto& m : lights) {
        Vector4f col = m.enabled
                           ? Vector4f{std::max(m.diffuse.x, 0.2f), std::max(m.diffuse.y, 0.2f),
                                      std::max(m.diffuse.z, 0.2f), 1.0f}
                           : Vector4f{0.25f, 0.25f, 0.25f, 1.0f};

        const Vector3f c = m.worldPos;
        for (i32 plane = 0; plane < 3; ++plane) {
            for (i32 i = 0; i < kSegs; ++i) {
                f32 a0 = (f32)i / kSegs * 6.28318530f;
                f32 a1 = (f32)(i + 1) / kSegs * 6.28318530f;
                f32 c0 = kMarkerRadius * std::cos(a0), s0 = kMarkerRadius * std::sin(a0);
                f32 c1 = kMarkerRadius * std::cos(a1), s1 = kMarkerRadius * std::sin(a1);
                Vector3f p0, p1;
                if (plane == 0) {
                    p0 = {c.x + c0, c.y + s0, c.z};
                    p1 = {c.x + c1, c.y + s1, c.z};
                } else if (plane == 1) {
                    p0 = {c.x + c0, c.y, c.z + s0};
                    p1 = {c.x + c1, c.y, c.z + s1};
                } else {
                    p0 = {c.x, c.y + c0, c.z + s0};
                    p1 = {c.x, c.y + c1, c.z + s1};
                }
                verts.push_back({p0, col});
                verts.push_back({p1, col});
            }
        }

        if (m.isDirectional) {
            Vector3f d = m.worldDir;
            f32 n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (n > 1e-6f) {
                d = {d.x / n, d.y / n, d.z / n};
            } else {
                d = {0, 0, 1};
            }
            const f32 L = kMarkerRadius * 4.0f;
            Vector3f tip = {c.x - d.x * L, c.y - d.y * L, c.z - d.z * L};
            verts.push_back({c, col});
            verts.push_back({tip, col});
        }
    }

    if (verts.empty())
        return;

    {
        f32 aspect = (rs_.Pipeline().Height() > 0)
                         ? (f32)rs_.Pipeline().Width() / (f32)rs_.Pipeline().Height()
                         : 1.0f;
        render_detail::CbPerFrameDesc d;
        d.view = viewMat;
        d.projection = rs_.Scene().Camera().ProjectionRH(aspect);
        render_detail::WriteCbPerFrame(rs_.Pipeline().Gfx(), rs_.Pipeline().CbPerFrame(), d);
    }
    DrawWireLines(rs_.Pipeline().Gfx(), cmd, rs_.Pipeline().CbPerFrame(), verts);
}

} // namespace whiteout::flakes::renderer::debug
