#include "assets/sampler_asset_manager.h"
#include "assets/texture_asset_manager.h"
#include "assets/viewcube_atlas.h"
#include "compiled_shaders.h"
#include "constants.h"
#include "render_detail.h"
#include "render_service.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_pipeline_impl.h"
#include "renderer/render_service_impl.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace whiteout::flakes::renderer::debug {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::assets;

bool DebugRenderer::CreateResources() {
    if (!CreateGridResources())
        return false;
    // The viewcube used to be skipped on Vulkan because its face SRV
    // texture needed a layout transition the backend couldn't do at bind
    // time. That's no longer true — the Vulkan backend queues every
    // CreateTexture into pendingSrvTransitions and drains it to
    // eShaderReadOnlyOptimal before the first render pass each frame, the
    // same path every main-scene texture already takes.
    if (!CreateViewCubeResources())
        return false;
    return true;
}

void DebugRenderer::DestroyResources() {
    auto* dev = rs_.Pipeline().Gfx();
    if (!dev)
        return;
    dev->Destroy(gridVB_);
    gridVB_ = gfx::BufferHandle::Invalid;
    dev->Destroy(vcCubeVB_);
    vcCubeVB_ = gfx::BufferHandle::Invalid;
    dev->Destroy(vcCubeIB_);
    vcCubeIB_ = gfx::BufferHandle::Invalid;
    dev->Destroy(vcOutlineVB_);
    vcOutlineVB_ = gfx::BufferHandle::Invalid;
    dev->Destroy(vcHomeVB_);
    vcHomeVB_ = gfx::BufferHandle::Invalid;

    rs_.Textures().ReleaseOwned(kViewCubeFaceTexName);
    vcFaceTex_ = gfx::TextureHandle::Invalid;
    dev->Destroy(viewCubePSOHdr_);
    viewCubePSOHdr_ = gfx::PipelineHandle::Invalid;
    dev->Destroy(viewCubePSOSd_);
    viewCubePSOSd_ = gfx::PipelineHandle::Invalid;
    dev->Destroy(viewCubeVS_);
    viewCubeVS_ = gfx::ShaderHandle::Invalid;
    dev->Destroy(viewCubePS_);
    viewCubePS_ = gfx::ShaderHandle::Invalid;
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

bool DebugRenderer::CreateViewCubeResources() {

    {
        i32 tw, th;
        auto pixels = GenerateViewCubeAtlas(tw, th);
        vcFaceTex_ = rs_.Pipeline().Gfx()->CreateTexture(
            {
                .width = tw,
                .height = th,
                .format = gfx::Format::R8G8B8A8_UNORM,
                .usage = gfx::TextureUsage::ShaderResource,
            },
            pixels.data());
        rs_.Textures().RegisterOwned(kViewCubeFaceTexName, vcFaceTex_);
    }

    f32 s = 0.5f;

    auto uv = [](i32 face, i32 corner) -> std::pair<f32, f32> {
        f32 u0 = face / 6.0f, u1 = (face + 1) / 6.0f;
        switch (corner) {
        case 0:
            return {u0, 1.0f};
        case 1:
            return {u1, 1.0f};
        case 2:
            return {u1, 0.0f};
        case 3:
            return {u0, 0.0f};
        }
        return {0, 0};
    };

    std::vector<Vertex> verts;
    auto addFace = [&](i32 face, Vector3f p0, Vector3f p1, Vector3f p2, Vector3f p3, Vector3f n) {
        for (i32 c = 0; c < 4; c++) {
            auto [u, v] = uv(face, c);
            Vector3f p = (c == 0) ? p0 : (c == 1) ? p1 : (c == 2) ? p2 : p3;
            verts.push_back({p, n, {1, 1, 1, 1}, {u, v}});
        }
    };

    struct FaceSpec {
        Vector3f p0, p1, p2, p3, n;
    };
    FaceSpec faces[6];
    if constexpr (kDefaultCoordSpace == CoordSpace::Blizzard) {
        faces[0] = {{s, s, s}, {s, -s, s}, {s, -s, -s}, {s, s, -s}, {1, 0, 0}};
        faces[1] = {{-s, -s, s}, {-s, s, s}, {-s, s, -s}, {-s, -s, -s}, {-1, 0, 0}};
        faces[2] = {{s, s, s}, {-s, s, s}, {-s, s, -s}, {s, s, -s}, {0, 1, 0}};
        faces[3] = {{-s, -s, s}, {s, -s, s}, {s, -s, -s}, {-s, -s, -s}, {0, -1, 0}};
        faces[4] = {{-s, s, s}, {s, s, s}, {s, -s, s}, {-s, -s, s}, {0, 0, 1}};
        faces[5] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s}, {0, 0, -1}};
    } else {
        faces[0] = {{s, -s, -s}, {-s, -s, -s}, {-s, -s, s}, {s, -s, s}, {0, -1, 0}};
        faces[1] = {{-s, s, -s}, {s, s, -s}, {s, s, s}, {-s, s, s}, {0, 1, 0}};
        faces[2] = {{s, s, -s}, {s, -s, -s}, {s, -s, s}, {s, s, s}, {1, 0, 0}};
        faces[3] = {{-s, -s, -s}, {-s, s, -s}, {-s, s, s}, {-s, -s, s}, {-1, 0, 0}};
        faces[4] = {{-s, s, s}, {s, s, s}, {s, -s, s}, {-s, -s, s}, {0, 0, 1}};
        faces[5] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s}, {0, 0, -1}};
    }
    for (i32 i = 0; i < 6; ++i) {
        const auto& f = faces[i];
        addFace(i, f.p0, f.p1, f.p2, f.p3, f.n);
    }

    vcCubeVB_ = rs_.Pipeline().Gfx()->CreateBuffer(
        {
            .size = sizeof(Vertex) * verts.size(),
            .usage = gfx::BufferUsage::Vertex,
        },
        verts.data());

    std::vector<u32> idx;
    for (i32 f = 0; f < 6; f++) {
        u32 base = f * 4;
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    vcCubeIB_ = rs_.Pipeline().Gfx()->CreateBuffer(
        {
            .size = sizeof(u32) * idx.size(),
            .usage = gfx::BufferUsage::Index,
        },
        idx.data());

    std::vector<LineVertex> edges;
    Vector4f ec = {0.2f, 0.2f, 0.2f, 1.0f};
    f32 e = s * 1.001f;
    edges.push_back({{-e, -e, -e}, ec});
    edges.push_back({{e, -e, -e}, ec});
    edges.push_back({{e, -e, -e}, ec});
    edges.push_back({{e, e, -e}, ec});
    edges.push_back({{e, e, -e}, ec});
    edges.push_back({{-e, e, -e}, ec});
    edges.push_back({{-e, e, -e}, ec});
    edges.push_back({{-e, -e, -e}, ec});
    edges.push_back({{-e, -e, e}, ec});
    edges.push_back({{e, -e, e}, ec});
    edges.push_back({{e, -e, e}, ec});
    edges.push_back({{e, e, e}, ec});
    edges.push_back({{e, e, e}, ec});
    edges.push_back({{-e, e, e}, ec});
    edges.push_back({{-e, e, e}, ec});
    edges.push_back({{-e, -e, e}, ec});
    edges.push_back({{-e, -e, -e}, ec});
    edges.push_back({{-e, -e, e}, ec});
    edges.push_back({{e, -e, -e}, ec});
    edges.push_back({{e, -e, e}, ec});
    edges.push_back({{e, e, -e}, ec});
    edges.push_back({{e, e, e}, ec});
    edges.push_back({{-e, e, -e}, ec});
    edges.push_back({{-e, e, e}, ec});

    vcOutlineVB_ = rs_.Pipeline().Gfx()->CreateBuffer(
        {
            .size = sizeof(LineVertex) * edges.size(),
            .usage = gfx::BufferUsage::Vertex,
        },
        edges.data());

    using namespace whiteout::flakes::Shaders;
    const bool vk = rs_.Pipeline().Gfx()->GetApi() == gfx::GfxApi::Vulkan;
    const u8* vcVsBytes = vk ? kViewCubeVSSpv : kViewCubeVS;
    usize vcVsSize = vk ? sizeof(kViewCubeVSSpv) : sizeof(kViewCubeVS);
    const u8* vcPsBytes = vk ? kViewCubePSSpv : kViewCubePS;
    usize vcPsSize = vk ? sizeof(kViewCubePSSpv) : sizeof(kViewCubePS);
    viewCubeVS_ = rs_.Pipeline().Gfx()->CreateShader(gfx::ShaderStage::Vertex, vcVsBytes, vcVsSize);
    viewCubePS_ = rs_.Pipeline().Gfx()->CreateShader(gfx::ShaderStage::Pixel, vcPsBytes, vcPsSize);
    if (viewCubeVS_ == gfx::ShaderHandle::Invalid || viewCubePS_ == gfx::ShaderHandle::Invalid) {
        return false;
    }

    gfx::InputElement vcInput[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, 0},
        {"NORMAL", 0, gfx::Format::R32G32B32_FLOAT, 12},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, 24},
        {"TEXCOORD", 0, gfx::Format::R32G32_FLOAT, 40},
    };
    gfx::GraphicsPipelineDesc vcDesc;
    vcDesc.vs = viewCubeVS_;
    vcDesc.ps = viewCubePS_;
    vcDesc.inputLayout = vcInput;
    vcDesc.topology = gfx::PrimitiveTopology::TriangleList;
    vcDesc.blend.enable = false;

    vcDesc.rasterizer.cull = gfx::CullMode::None;
    vcDesc.rasterizer.frontCCW = true;

    vcDesc.dsvFormat = rs_.Pipeline().DepthStencilFormat();
    vcDesc.rtvFormat = RenderPipeline::kHdrSceneFormat;
    viewCubePSOHdr_ = rs_.Pipeline().Gfx()->CreateGraphicsPipeline(vcDesc);

    vcDesc.rtvFormat = RenderPipeline::kSdSceneFormat;
    viewCubePSOSd_ = rs_.Pipeline().Gfx()->CreateGraphicsPipeline(vcDesc);

    return vcCubeVB_ != gfx::BufferHandle::Invalid && vcCubeIB_ != gfx::BufferHandle::Invalid &&
           vcOutlineVB_ != gfx::BufferHandle::Invalid &&
           viewCubePSOHdr_ != gfx::PipelineHandle::Invalid &&
           viewCubePSOSd_ != gfx::PipelineHandle::Invalid;
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

Rect DebugRenderer::GetViewCubeRect() const {
    i32 s = kViewCubeSize;
    i32 margin = 10;
    i32 cubeTop = margin + 28;
    return {rs_.Pipeline().Width() - s - margin, margin, rs_.Pipeline().Width() - margin,
            cubeTop + s};
}

void DebugRenderer::RenderViewCube() {
    if (vcCubeVB_ == gfx::BufferHandle::Invalid || vcCubeIB_ == gfx::BufferHandle::Invalid)
        return;

    auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();

    i32 s = kViewCubeSize;
    i32 margin = 10;
    gfx::Viewport vp = {
        (f32)(rs_.Pipeline().Width() - s - margin), (f32)(margin + 28), (f32)s, (f32)s, 0.0f, 1.0f};
    cmd->SetViewport(vp);

    auto* pt = rs_.Pipeline().PrimaryTarget();
    if (!pt)
        return;
    cmd->ClearDepth(pt->depth, 1.0f, 0);

    Matrix44f vcView;
    {
        f32 dist = 3.5f;
        f32 cosP = cosf(rs_.Scene().Camera().GetPitch()),
            sinP = sinf(rs_.Scene().Camera().GetPitch());
        f32 cosY = cosf(rs_.Scene().Camera().GetYaw()), sinY = sinf(rs_.Scene().Camera().GetYaw());
        Vector3f eye = {dist * cosP * cosY, dist * cosP * sinY, dist * sinP};
        Vector3f tgt = {0, 0, 0};
        Vector3f up = {0, 0, 1};
        vcView = Matrix44f::look_at_rh(eye, tgt, up);
    }
    Matrix44f vcProj =
        Matrix44f::perspective_fov_rh(std::numbers::pi_v<f32> / 4.0f, 1.0f, 0.1f, 100.0f);

    {
        render_detail::CbPerFrameDesc d;
        d.view = vcView;
        d.projection = vcProj;
        d.lightDir = render_detail::NormalizedLightDir4(kViewCubeLightDir);
        d.lightColor = kViewCubeLightColor;
        d.ambientColor = kViewCubeAmbientColor;
        render_detail::WriteCbPerFrame(rs_.Pipeline().Gfx(), rs_.Pipeline().CbPerFrame(), d);
    }

    const auto vcPso =
        (rs_.Pipeline().FrameRenderMode() == RenderMode::HD) ? viewCubePSOHdr_ : viewCubePSOSd_;
    cmd->BindPipeline(vcPso);
    cmd->BindVertexBuffer(0, vcCubeVB_, sizeof(Vertex));
    cmd->BindIndexBuffer(vcCubeIB_, gfx::Format::R32_UINT);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, rs_.Pipeline().CbPerFrame());
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, rs_.Pipeline().CbPerFrame());
    cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().LinearWrap());
    if (vcFaceTex_ != gfx::TextureHandle::Invalid)
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, vcFaceTex_);
    else
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, rs_.Textures().GetDefaults().White);
    cmd->DrawIndexed(36, 0, 0);

    cmd->BindPipeline(rs_.Pipeline().CurrentLinePSO());
    cmd->BindVertexBuffer(0, vcOutlineVB_, sizeof(LineVertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, rs_.Pipeline().CbPerFrame());
    cmd->Draw(24, 0);

    if (vcHovered_) {
        gfx::Viewport homeVp = {
            vp.x + (f32)s * kViewCubeHomeOffset, vp.y - 24.0f, (f32)s * 0.3f, 20.0f, 0.0f, 1.0f};
        cmd->SetViewport(homeVp);

        {
            render_detail::CbPerFrameDesc d;
            d.projection = Matrix44f::orthographic_rh(2.0f, 2.0f, -1.0f, 1.0f);
            d.ambientColor = {1, 1, 1, 1};
            d.extraParams = {1, 0, 0, 0};
            render_detail::WriteCbPerFrame(rs_.Pipeline().Gfx(), rs_.Pipeline().CbPerFrame(), d);
        }

        if (vcHomeVB_ == gfx::BufferHandle::Invalid) {
            Vector4f hc = {0.7f, 0.7f, 0.7f, 1.0f};
            LineVertex house[] = {
                {{-0.4f, -0.6f, 0}, hc}, {{0.4f, -0.6f, 0}, hc}, {{-0.4f, -0.6f, 0}, hc},
                {{-0.4f, 0.0f, 0}, hc},  {{0.4f, -0.6f, 0}, hc}, {{0.4f, 0.0f, 0}, hc},
                {{-0.5f, 0.0f, 0}, hc},  {{0.0f, 0.6f, 0}, hc},  {{0.5f, 0.0f, 0}, hc},
                {{0.0f, 0.6f, 0}, hc},   {{-0.5f, 0.0f, 0}, hc}, {{0.5f, 0.0f, 0}, hc},
            };
            vcHomeVB_ = rs_.Pipeline().Gfx()->CreateBuffer(
                {
                    .size = sizeof(house),
                    .usage = gfx::BufferUsage::Vertex,
                },
                house);
        }
        if (vcHomeVB_ != gfx::BufferHandle::Invalid) {
            cmd->BindVertexBuffer(0, vcHomeVB_, sizeof(LineVertex));
            cmd->Draw(12, 0);
        }
    }

    cmd->SetViewport({0, 0, (f32)rs_.Pipeline().Width(), (f32)rs_.Pipeline().Height(), 0.0f, 1.0f});

    Matrix44f view, proj;
    {
        view = rs_.Scene().Camera().GetViewMatrix();
    }
    f32 aspect = (rs_.Pipeline().Height() > 0)
                     ? (f32)rs_.Pipeline().Width() / (f32)rs_.Pipeline().Height()
                     : 1.0f;
    proj = rs_.Scene().Camera().ProjectionRH(aspect);
    {
        render_detail::CbPerFrameDesc d;
        d.view = view;
        d.projection = proj;
        d.lightDir = render_detail::NormalizedLightDir4(kDefaultLightDir);
        d.lightColor = kGeosetLightColor;
        d.ambientColor = {kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z,
                          0.0f};
        render_detail::WriteCbPerFrame(rs_.Pipeline().Gfx(), rs_.Pipeline().CbPerFrame(), d);
    }
}

i32 DebugRenderer::HitTestViewCube(i32 mx, i32 my) const {
    Rect r = GetViewCubeRect();

    i32 cubeTop = r.top + 28;
    if (vcHovered_ && mx >= r.left && mx <= r.right && my >= r.top && my <= cubeTop)
        return 6;

    if (mx < r.left || mx > r.right || my < cubeTop || my > r.bottom)
        return -1;

    i32 s = kViewCubeSize;
    f32 vcX = (f32)(rs_.Pipeline().Width() - s - 10);
    f32 vcY = 10.0f + 28.0f;

    Matrix44f vcView;
    {
        f32 dist = 3.5f;
        f32 cosP = cosf(rs_.Scene().Camera().GetPitch()),
            sinP = sinf(rs_.Scene().Camera().GetPitch());
        f32 cosY = cosf(rs_.Scene().Camera().GetYaw()), sinY = sinf(rs_.Scene().Camera().GetYaw());
        Vector3f eye = {dist * cosP * cosY, dist * cosP * sinY, dist * sinP};
        Vector3f up = {0, 0, 1};
        vcView = Matrix44f::look_at_rh(eye, {0, 0, 0}, up);
    }
    Matrix44f vcProj =
        Matrix44f::perspective_fov_rh(std::numbers::pi_v<f32> / 4.0f, 1.0f, 0.1f, 100.0f);
    Matrix44f vp_mat = vcView * vcProj;

    Vector3f centers[] = {{0, .5f, 0}, {0, -.5f, 0}, {-.5f, 0, 0},
                          {.5f, 0, 0}, {0, 0, .5f},  {0, 0, -.5f}};
    Vector3f normals[] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1}};

    f32 cosP = cosf(rs_.Scene().Camera().GetPitch()), sinP = sinf(rs_.Scene().Camera().GetPitch());
    f32 cosY = cosf(rs_.Scene().Camera().GetYaw()), sinY = sinf(rs_.Scene().Camera().GetYaw());
    Vector3f camDir = {-cosP * cosY, -cosP * sinY, -sinP};

    i32 bestFace = -1;
    f32 bestDist = 1e9f;

    for (i32 i = 0; i < 6; i++) {
        f32 dot = normals[i].x * camDir.x + normals[i].y * camDir.y + normals[i].z * camDir.z;
        if (dot > -0.05f)
            continue;

        Vector3f c = centers[i];
        f32 cx = c.x * vp_mat.data[0][0] + c.y * vp_mat.data[1][0] + c.z * vp_mat.data[2][0] +
                 vp_mat.data[3][0];
        f32 cy = c.x * vp_mat.data[0][1] + c.y * vp_mat.data[1][1] + c.z * vp_mat.data[2][1] +
                 vp_mat.data[3][1];
        f32 cw = c.x * vp_mat.data[0][3] + c.y * vp_mat.data[1][3] + c.z * vp_mat.data[2][3] +
                 vp_mat.data[3][3];
        if (fabsf(cw) < 1e-6f)
            continue;
        f32 ndcX = cx / cw;
        f32 ndcY = cy / cw;
        f32 spx = vcX + (f32)s * (1.0f + ndcX) * 0.5f;
        f32 spy = vcY + (f32)s * (1.0f - ndcY) * 0.5f;

        f32 dx = spx - mx;
        f32 dy = spy - my;
        f32 d = dx * dx + dy * dy;
        if (d < bestDist && d < (s * s * 0.06f)) {
            bestDist = d;
            bestFace = i;
        }
    }
    return bestFace;
}

} // namespace whiteout::flakes::renderer::debug
