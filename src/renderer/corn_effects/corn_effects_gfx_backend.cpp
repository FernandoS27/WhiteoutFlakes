#include "renderer/corn_effects/corn_effects_gfx_backend.h"

#include "../gfx/gfx.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/bls/bls_draw_helpers.h"
#include "renderer/bls/bls_mat_params.h"
#include "renderer/bls/bls_permuter.h"
#include "renderer/bls/bls_program.h"
#include "renderer/bls/bls_pso_builder.h"
#include "renderer/bls/scoped_cb.h"
#include "renderer/corn_effects/corn_effects_vertex.h"

#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/semantic_slot_reader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

namespace whiteout::flakes::renderer::corn_effects {

using ::whiteout::flakes::renderer::bls::BlsProgram;
using ::whiteout::flakes::renderer::bls::BlsPsoBuilder;

namespace {

constexpr u32 kVsPermBasicUVWithVC = 10;

constexpr u32 kPsPermBasicUVWithVC = (0 * 3 + 1) * 128 + 0x20;

} // namespace

bls::GxMatAlpha CornEffectsGfxBackend::BlendModeToGxAlpha(u8 blendMode) {
    switch (blendMode) {
    case 0:
        return bls::GxMatAlpha::Add;
    case 1:
        return bls::GxMatAlpha::Add;
    case 2:
        return bls::GxMatAlpha::Blend;
    case 3:
        return bls::GxMatAlpha::Add;
    case 4:
        return bls::GxMatAlpha::Opaque;
    case 5:
        return bls::GxMatAlpha::AlphaKey;
    default:
        return bls::GxMatAlpha::Blend;
    }
}

CornEffectsGfxBackend::CornEffectsGfxBackend(const Init& init)
    : device_(init.device), program_(init.program), psoBuilder_(init.psoBuilder),
      textures_(init.textures), samplers_(init.samplers), assets_(init.assets),
      slotAcquire_(init.slotAcquire) {}

CornEffectsGfxBackend::~CornEffectsGfxBackend() {
    // Release any AssetManager slots prepare() acquired for diffuse
    // textures. Without this each emitter teardown leaks one slot ref
    // per layer, which would keep texture handles alive past the
    // model's lifetime.
    if (assets_) {
        for (auto& ls : layerStates_) {
            if (ls.diffuseSlot != 0)
                assets_->Release(ls.diffuseSlot);
        }
    }
    // GPU buffers used to live here; now the owning CornEffectsService
    // shares one VB/IB/CB set across every emitter — nothing to free.
}

bool CornEffectsGfxBackend::prepare(std::span<const ::whiteout::cornflakes::LayerProgram> layers,
                                    ::whiteout::cornflakes::IssueBag& /*issues*/) {
    // Release any slots a previous prepare() owned — emitters get
    // re-prepared when their .pkb is re-acquired, and we don't want to
    // leak slot refs across re-prepares.
    if (assets_) {
        for (auto& old : layerStates_) {
            if (old.diffuseSlot != 0)
                assets_->Release(old.diffuseSlot);
        }
    }
    layerStates_.clear();
    layerStates_.resize(layers.size());

    for (std::size_t i = 0; i < layers.size(); ++i) {
        auto& st = layerStates_[i];
        const auto& lp = layers[i];
        if (lp.renderers.empty()) {
            st.renderable = false;
            continue;
        }
        const auto& rr = lp.renderers[0];
        st.renderable = (rr.cls == ::whiteout::cornflakes::RendererClass::Billboard);
        st.isDistortion = rr.isDistortion;

        if (st.isDistortion) {
            st.renderable = false;
        }

        // Acquire a slot for this layer's diffuse. The slot starts
        // bound to the shared white placeholder; once the host fetches
        // the texture bytes and Apply runs, TextureOf returns the real
        // handle automatically. No per-frame retry needed.
        if (slotAcquire_ && !rr.diffuseTexturePath.empty()) {
            st.diffuseSlot = slotAcquire_(rr.diffuseTexturePath);
        }
        st.atlasX = rr.atlasSubDivX;
        st.atlasY = rr.atlasSubDivY;
        st.flipU = rr.hasFlipUVs || rr.textureFlipU;
        st.flipV = rr.hasFlipUVs || rr.textureFlipV;
        st.rotate = rr.textureRotateTexture;
        st.size2D = rr.hasEnableSize2D;
    }
    return true;
}

std::uint32_t CornEffectsGfxBackend::LayerDiffuseSlot(u32 layerIdx) const {
    if (layerIdx >= layerStates_.size())
        return 0;
    return layerStates_[layerIdx].diffuseSlot;
}

void CornEffectsGfxBackend::submit(std::span<const ::whiteout::cornflakes::RenderPacket> packets,
                                   const ::whiteout::cornflakes::ViewParams& /*view*/,
                                   ::whiteout::cornflakes::IssueBag& /*issues*/) {
    // CPU-only: produces pending_.verts/indices/draws. The owning
    // CornEffectsService aggregates every emitter's batch into one
    // shared VB/IB/CB pair and issues consolidated draws in
    // FlushBatchedDraws — single MapBuffer cycle, single CB write,
    // and one DrawIndexed per draw (with baseVertex/firstIndex into
    // the shared buffers).
    pending_.Clear();
    if (!device_ || !program_ || !psoBuilder_) {
        return;
    }

    const Matrix44f& v = frame_.view;
    const Vector3f camRight = {v.data[0][0], v.data[1][0], v.data[2][0]};
    const Vector3f camUp = {v.data[0][1], v.data[1][1], v.data[2][1]};
    const Vector3f camForward = {v.data[0][2], v.data[1][2], v.data[2][2]};

    ::whiteout::cornflakes::SemanticSlotReader reader;

    struct PacketCache {
        std::span<const ::whiteout::cornflakes::Float3> positions;
        std::span<const f32> sizes;
        std::span<const ::whiteout::cornflakes::Float2> sizes2;
        std::span<const f32> rotations;
        std::span<const ::whiteout::cornflakes::Float3> axes0;
        std::span<const ::whiteout::cornflakes::Float3> axes1;
        const ::whiteout::cornflakes::Float4* colors = nullptr;
        size_t colorCount = 0;
        const f32* texIds = nullptr;
        size_t texIdCount = 0;
        u32 particleCount = 0;
        u32 layerValue = 0;
        u8 blendMode = 0;
        u8 billboardingMode = 0;
    };
    std::vector<PacketCache> cache;
    cache.reserve(packets.size());
    std::vector<u32> packetOf;
    std::vector<u32> partOf;
    std::vector<f32> depthOf;

    for (const auto& pkt : packets) {
        if (pkt.cls != ::whiteout::cornflakes::RendererClass::Billboard)
            continue;
        if (pkt.layer.value >= layerStates_.size())
            continue;
        const auto& ls = layerStates_[pkt.layer.value];
        if (!ls.renderable)
            continue;
        if (pkt.particleCount == 0)
            continue;

        PacketCache pc{};
        pc.positions = reader.readPosition(pkt);
        pc.sizes = reader.readSize(pkt);
        pc.rotations = reader.readRotation(pkt);
        pc.axes0 = reader.readAxis(pkt);
        pc.axes1 = reader.readNormalAxis(pkt);
        if (ls.size2D) {
            auto sz2Bytes =
                pkt.slots[static_cast<size_t>(::whiteout::cornflakes::RenderSlot::Size)];
            pc.sizes2 = std::span<const ::whiteout::cornflakes::Float2>(
                reinterpret_cast<const ::whiteout::cornflakes::Float2*>(sz2Bytes.data()),
                sz2Bytes.size() / sizeof(::whiteout::cornflakes::Float2));
        }
        auto colorBytes = pkt.slots[static_cast<size_t>(::whiteout::cornflakes::RenderSlot::Color)];
        pc.colors = reinterpret_cast<const ::whiteout::cornflakes::Float4*>(colorBytes.data());
        pc.colorCount = colorBytes.size() / sizeof(::whiteout::cornflakes::Float4);
        auto texIdBytes =
            pkt.slots[static_cast<size_t>(::whiteout::cornflakes::RenderSlot::TextureID)];
        pc.texIds = reinterpret_cast<const f32*>(texIdBytes.data());
        pc.texIdCount = texIdBytes.size() / sizeof(f32);
        pc.particleCount = pkt.particleCount;
        pc.layerValue = pkt.layer.value;
        pc.blendMode = pkt.blendMode;
        pc.billboardingMode = pkt.billboardingMode;

        const u32 packetIdx = static_cast<u32>(cache.size());
        cache.push_back(pc);

        for (u32 p = 0; p < pc.particleCount; ++p) {
            const f32 pSize = !pc.sizes.empty() ? pc.sizes[p] : 1.0f;
            const f32 pAlpha = (p < pc.colorCount) ? pc.colors[p].w : 1.0f;
            if (pSize == 0.0f && pAlpha == 0.0f)
                continue;

            packetOf.push_back(packetIdx);
            partOf.push_back(p);
            f32 depth = 0.0f;
            if (p < pc.positions.size()) {
                const f32 wx = pc.positions[p].x;
                const f32 wy = pc.positions[p].y;
                const f32 wz = pc.positions[p].z;
                depth = wx * v.data[0][2] + wy * v.data[1][2] + wz * v.data[2][2] + v.data[3][2];
            }
            depthOf.push_back(depth);
        }
    }
    if (packetOf.empty())
        return;

    std::vector<u32> order(packetOf.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return depthOf[a] < depthOf[b]; });

    const size_t totalLive = order.size();
    auto& verts   = pending_.verts;
    auto& indices = pending_.indices;
    auto& draws   = pending_.draws;
    verts.reserve(totalLive * 4);
    indices.reserve(totalLive * 6);
    draws.reserve(packets.size());

    u32 runFirst = 0;
    u32 runPacketI = UINT32_MAX;
    auto flushRun = [&](u32 endIndex) {
        if (runPacketI == UINT32_MAX)
            return;
        if (endIndex == runFirst)
            return;
        const auto& pc = cache[runPacketI];
        draws.push_back({runFirst, endIndex - runFirst, pc.layerValue, pc.blendMode});
    };

    for (size_t k = 0; k < order.size(); ++k) {
        const u32 entry = order[k];
        const u32 packetIdx = packetOf[entry];
        const u32 p = partOf[entry];
        const PacketCache& pc = cache[packetIdx];
        const auto& ls = layerStates_[pc.layerValue];

        if (packetIdx != runPacketI) {
            flushRun(static_cast<u32>(indices.size()));
            runFirst = static_cast<u32>(indices.size());
            runPacketI = packetIdx;
        }

        const Vector3f pos = (p < pc.positions.size())
                                 ? Vector3f{pc.positions[p].x, pc.positions[p].y, pc.positions[p].z}
                                 : Vector3f{0, 0, 0};

        f32 sx = 0.0f, sy = 0.0f;
        if (ls.size2D) {
            if (p < pc.sizes2.size()) {
                sx = pc.sizes2[p].x;
                sy = pc.sizes2[p].y;
            }
        } else {
            const f32 raw = (p < pc.sizes.size()) ? pc.sizes[p] : 0.0f;
            if (std::isfinite(raw) && raw > 0.0f) {
                sx = raw;
                sy = raw;
            }
        }

        Vector4f color = {1, 1, 1, 1};
        if (p < pc.colorCount) {
            color = {
                std::max(0.0f, pc.colors[p].x),
                std::max(0.0f, pc.colors[p].y),
                std::max(0.0f, pc.colors[p].z),
                std::max(0.0f, pc.colors[p].w),
            };
        }

        Vector3f r0, u0;
        const u8 bbMode = pc.billboardingMode;
        const bool wantAxis0 = (bbMode == 2 || bbMode == 3 || bbMode == 4);
        const bool wantBothAxes =
            (bbMode == 5) || (bbMode == 0 && !pc.axes0.empty() && !pc.axes1.empty());

        if (wantBothAxes && p < pc.axes0.size() && p < pc.axes1.size()) {
            r0 = {pc.axes0[p].x, pc.axes0[p].z, pc.axes0[p].y};
            u0 = {pc.axes1[p].x, pc.axes1[p].z, pc.axes1[p].y};
        } else if (wantAxis0 && p < pc.axes0.size()) {
            const f32 ax = pc.axes0[p].x;
            const f32 ay = pc.axes0[p].y;
            const f32 az = pc.axes0[p].z;
            const f32 invSz = (sx != 0.0f) ? (1.0f / sx) : 0.0f;
            u0 = {ax * invSz, ay * invSz, az * invSz};
            const f32 alen2 = ax * ax + ay * ay + az * az;
            if (alen2 > 1e-12f) {
                const f32 invLen = 1.0f / std::sqrt(alen2);
                const Vector3f a{ax * invLen, ay * invLen, az * invLen};
                Vector3f c = ::whiteout::cross(camForward, a);
                const f32 clen2 = c.x * c.x + c.y * c.y + c.z * c.z;
                if (clen2 > 1e-12f) {
                    const f32 inv = 1.0f / std::sqrt(clen2);
                    r0 = {c.x * inv, c.y * inv, c.z * inv};
                } else {
                    r0 = camRight;
                }
            } else {
                r0 = camRight;
            }
        } else {
            r0 = camRight;
            u0 = camUp;
        }

        const f32 rot = (p < pc.rotations.size()) ? pc.rotations[p] : 0.0f;
        if (rot != 0.0f) {
            const f32 ca = std::cos(rot);
            const f32 sa = std::sin(rot);
            const Vector3f rk = r0;
            const Vector3f uk = u0;
            r0 = {ca * rk.x + sa * uk.x, ca * rk.y + sa * uk.y, ca * rk.z + sa * uk.z};
            u0 = {-sa * rk.x + ca * uk.x, -sa * rk.y + ca * uk.y, -sa * rk.z + ca * uk.z};
        }

        const Vector3f r = {r0.x * sx, r0.y * sx, r0.z * sx};
        const Vector3f u = {u0.x * sy, u0.y * sy, u0.z * sy};
        const Vector3f c0v = {pos.x - r.x - u.x, pos.y - r.y - u.y, pos.z - r.z - u.z};
        const Vector3f c1v = {pos.x + r.x - u.x, pos.y + r.y - u.y, pos.z + r.z - u.z};
        const Vector3f c2v = {pos.x + r.x + u.x, pos.y + r.y + u.y, pos.z + r.z + u.z};
        const Vector3f c3v = {pos.x - r.x + u.x, pos.y - r.y + u.y, pos.z - r.z + u.z};

        const bool atlas = ls.atlasX > 0 && ls.atlasY > 0;
        const f32 cellU = atlas ? 1.0f / static_cast<f32>(ls.atlasX) : 1.0f;
        const f32 cellV = atlas ? 1.0f / static_cast<f32>(ls.atlasY) : 1.0f;
        const u32 maxFrame = atlas ? u32(ls.atlasX) * ls.atlasY : 1u;
        f32 cellU0 = 0.0f, cellV0 = 0.0f;
        if (atlas) {
            const f32 texId = (p < pc.texIdCount) ? pc.texIds[p] : 0.0f;
            u32 frame = static_cast<u32>(std::max(0.0f, texId));
            if (maxFrame > 0)
                frame %= maxFrame;
            cellU0 = (frame % ls.atlasX) * cellU;
            cellV0 = (frame / ls.atlasX) * cellV;
        }

        static constexpr f32 kCornerUV[4][2] = {
            {0.0f, 0.0f},
            {1.0f, 0.0f},
            {1.0f, 1.0f},
            {0.0f, 1.0f},
        };
        Vector2f uv[4];
        for (i32 corner = 0; corner < 4; ++corner) {
            f32 cu = kCornerUV[corner][0];
            f32 cv = kCornerUV[corner][1];
            if (ls.rotate) {
                const f32 t = cu;
                cu = cv;
                cv = 1.0f - t;
            }
            if (ls.flipU) {
                cu = 1.0f - cu;
            }
            if (ls.flipV) {
                cv = 1.0f - cv;
            }
            uv[corner].x = cellU0 + cu * cellU;
            uv[corner].y = cellV0 + cv * cellV;
        }

        const u32 b = static_cast<u32>(verts.size());
        const Vector4f pivot = {pos.x, pos.y, pos.z, 1.0f};
        verts.push_back({c0v, 0.0f, color, uv[0], {0, 0}, pivot});
        verts.push_back({c1v, 0.0f, color, uv[1], {0, 0}, pivot});
        verts.push_back({c2v, 0.0f, color, uv[2], {0, 0}, pivot});
        verts.push_back({c3v, 0.0f, color, uv[3], {0, 0}, pivot});
        indices.push_back(static_cast<u16>(b + 0));
        indices.push_back(static_cast<u16>(b + 1));
        indices.push_back(static_cast<u16>(b + 2));
        indices.push_back(static_cast<u16>(b + 0));
        indices.push_back(static_cast<u16>(b + 2));
        indices.push_back(static_cast<u16>(b + 3));
    }
    flushRun(static_cast<u32>(indices.size()));
    // GPU emission is deferred to CornEffectsService — it walks every
    // emitter's pending_ batch, packs them into one shared VB/IB pair,
    // writes the shared CBs once, then issues all DrawIndexeds with
    // (baseVertex, firstIndex) into the shared buffers.
}

void CornEffectsGfxBackend::shutdown(::whiteout::cornflakes::IssueBag& /*issues*/) {
    layerStates_.clear();
}

} // namespace whiteout::flakes::renderer::corn_effects
