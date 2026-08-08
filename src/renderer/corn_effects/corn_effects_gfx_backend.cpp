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
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/semantic_slot_reader.hpp>
// Impl-side headers: buildRibbonGeometry is the shared expansion the
// engine-verified GL reference also drives its ribbon path from, and
// classifyPopcornPerm is the same asset-flags -> perm classifier it uses.
#include <cornflakes/render/ribbon_geometry.hpp>
#include <cornflakes/render/shader_perm.hpp>

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

// Per-particle decorrelation value for the AlphaRemap LUT's V axis. Billboards
// have no Cursor stream to read (the sim only fills it for ribbons), so the
// engine-verified reference synthesises one by hashing the particle index —
// same hash, so the same particle picks the same row of the LUT.
f32 ParticleRandom(u32 particleIndex, u32 layerValue) noexcept {
    u32 h = (particleIndex ^ (layerValue * 2654435761u)) + 0x9E3779B9u;
    h = (h ^ (h >> 16)) * 0x85EBCA6Bu;
    h = (h ^ (h >> 13)) * 0xC2B2AE35u;
    h = h ^ (h >> 16);
    return static_cast<f32>(h & 0x00FFFFFFu) / static_cast<f32>(0x01000000u);
}

} // namespace

// PopcornFX Transparent.Type -> BLS blend state. Mirrors the engine's own
// per-mode glBlendFunc (verified in the cornflakes_gl reference): NoAlphaAdd
// and BlendAdd are distinct from plain Add, and every mode leaves destination
// alpha untouched.
bls::GxMatAlpha CornEffectsGfxBackend::BlendModeToGxAlpha(u8 blendMode) {
    switch (blendMode) {
    case 0: // Add
        return bls::GxMatAlpha::Add;
    case 1: // NoAlphaAdd — colour carries its own intensity, alpha is inert.
        return bls::GxMatAlpha::AddNoAlpha;
    case 2: // Blend
        return bls::GxMatAlpha::BlendKeepDst;
    case 3: // BlendAdd — premultiplied "over": adds AND attenuates the dest.
        return bls::GxMatAlpha::PremulBlend;
    case 4:
        return bls::GxMatAlpha::Opaque;
    case 5:
        return bls::GxMatAlpha::AlphaKey;
    default:
        return bls::GxMatAlpha::BlendKeepDst;
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
        for (auto& states : layerStates_) {
            for (auto& ls : states) {
                if (ls.diffuseSlot != 0)
                    assets_->Release(ls.diffuseSlot);
                if (ls.alphaLutSlot != 0)
                    assets_->Release(ls.alphaLutSlot);
            }
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
        for (auto& states : layerStates_) {
            for (auto& old : states) {
                if (old.diffuseSlot != 0)
                    assets_->Release(old.diffuseSlot);
                if (old.alphaLutSlot != 0)
                    assets_->Release(old.alphaLutSlot);
            }
        }
    }
    layerStates_.clear();
    layerStates_.resize(layers.size());

    for (std::size_t i = 0; i < layers.size(); ++i) {
        const auto& lp = layers[i];
        auto& states = layerStates_[i];
        states.assign(lp.renderers.size(), RendererState{});

        for (std::size_t r = 0; r < lp.renderers.size(); ++r) {
            auto& st = states[r];
            const auto& rr = lp.renderers[r];
            st.isRibbon = (rr.cls == ::whiteout::cornflakes::RendererClass::Ribbon);
            st.renderable = (rr.cls == ::whiteout::cornflakes::RendererClass::Billboard) ||
                            st.isRibbon;
            st.isDistortion = rr.isDistortion;

            if (st.isDistortion) {
                st.renderable = false;
            }

            // Acquire a slot for this renderer's diffuse. The slot starts
            // bound to the shared white placeholder; once the host fetches
            // the texture bytes and Apply runs, TextureOf returns the real
            // handle automatically. No per-frame retry needed.
            if (slotAcquire_ && !rr.diffuseTexturePath.empty()) {
                st.diffuseSlot = slotAcquire_(rr.diffuseTexturePath);
            }
            if (slotAcquire_ && rr.hasAlphaLut && !rr.alphaRemapMapPath.empty()) {
                st.alphaLutSlot = slotAcquire_(rr.alphaRemapMapPath);
            }
            st.atlasX = rr.atlasSubDivX;
            st.atlasY = rr.atlasSubDivY;
            st.flipU = rr.hasFlipUVs || rr.textureFlipU;
            st.flipV = rr.hasFlipUVs || rr.textureFlipV;
            st.rotate = rr.textureRotateTexture;
            st.customTextureU = rr.hasCustomTextureU;
            st.correctDeformation = rr.hasCorrectDeformation;
            // Atlas cross-fade needs a real grid AND the second frame's UV,
            // which only the billboard path emits (the ribbon helper folds the
            // atlas cell into its own scale/bias and stays on BasicUV).
            st.isAtlas = rr.isAtlas && !st.isRibbon && st.atlasX > 0 && st.atlasY > 0;

            // Classify the shader perm from the asset instead of pinning one
            // constant. Only the dimensions we can actually feed are enabled:
            //  * isBillboard (modeIdx 1) synthesises the quad UV from
            //    SV_VertexID; we expand quads on the CPU and bake uv0, so it
            //    stays off for billboards AND ribbons.
            //  * hasNT / isLit / hasSoftParticles all need per-vertex normals
            //    and tangents we don't produce. SoftParticles additionally
            //    reads worldPos at varying location 7, which only the NT-bearing
            //    modeIdx-1 perms emit.
            ::whiteout::cornflakes::LayerRendererFlags flags{};
            flags.hasUV = true;
            flags.hasVC = true;
            flags.isBillboard = false;
            flags.isAtlas = st.isAtlas;
            flags.hasNT = false;
            flags.isLit = false;
            flags.hasSoftParticles = false;
            flags.writeGBuffer = false;
            // The LUT's V axis is the `random` attribute, and the PS only
            // compiles the remap under HAS_RANDOM_UV — so the two flags move
            // together, and only once we actually have a LUT to bind.
            flags.hasAlphaLut = (st.alphaLutSlot != 0);
            flags.hasRandom = flags.hasAlphaLut;
            const auto key = ::whiteout::cornflakes::classifyPopcornPerm(
                flags, ::whiteout::cornflakes::FogMode::None,
                ::whiteout::cornflakes::RenderPass::Color);
            st.vsPerm = key.vsPerm;
            st.psPerm = key.psPerm;
        }
    }
    return true;
}

std::uint32_t CornEffectsGfxBackend::LayerDiffuseSlot(u32 layerIdx, u32 rendererIdx) const {
    if (layerIdx >= layerStates_.size())
        return 0;
    const auto& states = layerStates_[layerIdx];
    if (rendererIdx >= states.size())
        return 0;
    return states[rendererIdx].diffuseSlot;
}

std::uint32_t CornEffectsGfxBackend::LayerAlphaLutSlot(u32 layerIdx, u32 rendererIdx) const {
    if (layerIdx >= layerStates_.size())
        return 0;
    const auto& states = layerStates_[layerIdx];
    if (rendererIdx >= states.size())
        return 0;
    return states[rendererIdx].alphaLutSlot;
}

void CornEffectsGfxBackend::submit(std::span<const ::whiteout::cornflakes::RenderPacket> packets,
                                   const ::whiteout::cornflakes::ViewParams& view,
                                   ::whiteout::cornflakes::IssueBag& /*issues*/) {
    // CPU-only: produces pending_.verts/indices/draws. The owning
    // CornEffectsService aggregates every emitter's batch into one
    // shared VB/IB/CB pair (ConsolidatePending) and issues consolidated
    // draws — single MapBuffer cycle, single CB write, and one
    // DrawIndexed per draw (with baseVertex/firstIndex into the shared
    // buffers).
    pending_.Clear();
    if (!device_ || !program_ || !psoBuilder_) {
        return;
    }

    const Matrix44f& v = frame_.view;
    const Vector3f camRight = {v.data[0][0], v.data[1][0], v.data[2][0]};
    const Vector3f camUp = {v.data[0][1], v.data[1][1], v.data[2][1]};
    const Vector3f camForward = {v.data[0][2], v.data[1][2], v.data[2][2]};
    // Camera world position: the view maps eye -> origin, so eye·col_k = -V[3][k]
    // and the basis columns are orthonormal. Used by the AxisAlignedSpheroid path.
    const Vector3f eyePos = {
        -(v.data[3][0] * camRight.x + v.data[3][1] * camUp.x + v.data[3][2] * camForward.x),
        -(v.data[3][0] * camRight.y + v.data[3][1] * camUp.y + v.data[3][2] * camForward.y),
        -(v.data[3][0] * camRight.z + v.data[3][1] * camUp.z + v.data[3][2] * camForward.z),
    };

    ::whiteout::cornflakes::SemanticSlotReader reader;

    struct PacketCache {
        std::span<const ::whiteout::cornflakes::Float3> positions;
        std::span<const f32> sizes;
        std::span<const f32> rotations;
        // Read as f32, NOT via SemanticSlotReader::readEnabled — that
        // reinterprets the slot as u8 while packSlot writes f32 lanes, so it
        // reads the low byte of 1.0f (0x00) and reports everything disabled.
        const f32* enabled = nullptr;
        size_t enabledCount = 0;
        std::span<const ::whiteout::cornflakes::Float3> axes0;
        std::span<const ::whiteout::cornflakes::Float3> axes1;
        const ::whiteout::cornflakes::Float4* colors = nullptr;
        size_t colorCount = 0;
        const f32* texIds = nullptr;
        size_t texIdCount = 0;
        u32 particleCount = 0;
        u32 layerValue = 0;
        u32 rendererIndex = 0;
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
            continue; // ribbons are handled by SubmitRibbons below
        if (pkt.layer.value >= layerStates_.size())
            continue;
        const auto& layerRenderers = layerStates_[pkt.layer.value];
        if (pkt.rendererIndex >= layerRenderers.size())
            continue;
        const auto& ls = layerRenderers[pkt.rendererIndex];
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
        auto enabledBytes =
            pkt.slots[static_cast<size_t>(::whiteout::cornflakes::RenderSlot::Enabled)];
        pc.enabled = reinterpret_cast<const f32*>(enabledBytes.data());
        pc.enabledCount = enabledBytes.size() / sizeof(f32);
        auto colorBytes = pkt.slots[static_cast<size_t>(::whiteout::cornflakes::RenderSlot::Color)];
        pc.colors = reinterpret_cast<const ::whiteout::cornflakes::Float4*>(colorBytes.data());
        pc.colorCount = colorBytes.size() / sizeof(::whiteout::cornflakes::Float4);
        auto texIdBytes =
            pkt.slots[static_cast<size_t>(::whiteout::cornflakes::RenderSlot::TextureID)];
        pc.texIds = reinterpret_cast<const f32*>(texIdBytes.data());
        pc.texIdCount = texIdBytes.size() / sizeof(f32);
        pc.particleCount = pkt.particleCount;
        pc.layerValue = pkt.layer.value;
        pc.rendererIndex = pkt.rendererIndex;
        pc.blendMode = pkt.blendMode;
        pc.billboardingMode = pkt.billboardingMode;

        const u32 packetIdx = static_cast<u32>(cache.size());
        cache.push_back(pc);

        for (u32 p = 0; p < pc.particleCount; ++p) {
            // particleCount is the POOL CAPACITY, not the live count. A
            // particle whose renderer Enabled pin reads 0 keeps real
            // position/size/colour, so this gate — not the dead-particle
            // proxy below — is what culls it.
            if (p < pc.enabledCount && pc.enabled[p] == 0.0f)
                continue;
            const f32 pSize = (p < pc.sizes.size()) ? pc.sizes[p] : 1.0f;
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
    if (packetOf.empty()) {
        SubmitRibbons(packets, view);
        return;
    }

    std::vector<u32> order(packetOf.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    // Back-to-front. Our view is left-handed (look_at_lh_sgcompat puts
    // +zaxis = forward in column 2), so `depth` grows with distance and the
    // farthest particle must come first. The GL reference sorts ascending
    // because its right-handed view makes the same expression negative in
    // front — porting that comparator verbatim inverted the order.
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return depthOf[a] > depthOf[b]; });

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
        const auto& rs = layerStates_[pc.layerValue][pc.rendererIndex];
        draws.push_back({runFirst, endIndex - runFirst, pc.layerValue, pc.rendererIndex,
                         pc.blendMode, rs.vsPerm, rs.psPerm});
    };

    for (size_t k = 0; k < order.size(); ++k) {
        const u32 entry = order[k];
        const u32 packetIdx = packetOf[entry];
        const u32 p = partOf[entry];
        const PacketCache& pc = cache[packetIdx];
        const auto& ls = layerStates_[pc.layerValue][pc.rendererIndex];

        if (packetIdx != runPacketI) {
            flushRun(static_cast<u32>(indices.size()));
            runFirst = static_cast<u32>(indices.size());
            runPacketI = packetIdx;
        }

        const Vector3f pos = (p < pc.positions.size())
                                 ? Vector3f{pc.positions[p].x, pc.positions[p].y, pc.positions[p].z}
                                 : Vector3f{0, 0, 0};

        // Size is a single component for every non-Mesh renderer class
        // (see componentsForSlot in pool_extractor.cpp), so there is no
        // Float2 to read here — an earlier EnableSize2D path reinterpreted
        // this slot as Float2[] and lost the back half of every packet.
        f32 sx = 0.0f, sy = 0.0f;
        {
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

        const bool atlas = ls.atlasX > 0 && ls.atlasY > 0;
        const f32 cellU = atlas ? 1.0f / static_cast<f32>(ls.atlasX) : 1.0f;
        const f32 cellV = atlas ? 1.0f / static_cast<f32>(ls.atlasY) : 1.0f;
        const u32 maxFrame = atlas ? u32(ls.atlasX) * ls.atlasY : 1u;
        f32 cellU0 = 0.0f, cellV0 = 0.0f;
        f32 cellU0b = 0.0f, cellV0b = 0.0f;
        f32 atlasCursor = 0.0f;
        if (atlas && maxFrame > 0) {
            const f32 rawId = (p < pc.texIdCount) ? pc.texIds[p] : 0.0f;
            // CLAMP, don't wrap. TextureID curves normally ramp 0 -> frameCount
            // over lifetime and overshoot slightly; a modulo snaps the dying
            // particle back to frame 0, which pops at the end of every
            // explosion. Negative cursors mirror rather than collapsing to 0.
            f32 cursor = std::isfinite(rawId) ? std::fabs(rawId) : 0.0f;
            const f32 maxCursor = static_cast<f32>(maxFrame) - 1.0f;
            if (cursor > maxCursor)
                cursor = maxCursor;
            atlasCursor = cursor;
            const u32 frameA = static_cast<u32>(cursor);
            const u32 frameB = (frameA + 1u < maxFrame) ? (frameA + 1u) : (maxFrame - 1u);
            cellU0 = (frameA % ls.atlasX) * cellU;
            cellV0 = (frameA / ls.atlasX) * cellV;
            cellU0b = (frameB % ls.atlasX) * cellU;
            cellV0b = (frameB / ls.atlasX) * cellV;
        }

        const u8 bbMode = pc.billboardingMode;

        // AxisAlignedCapsule is not a quad: the engine
        // (TAxialBillboarderCapsuleImpl<0>::Align) emits a 6-corner hexagonal
        // sleeve whose two caps extend `rad` past each end of Axis0, giving a
        // pointed streak with a constant-width core. Rendering it as a plain
        // axial quad loses the caps and shortens the streak to |Axis0|.
        if (bbMode == 4 && p < pc.axes0.size()) {
            const Vector3f ax{pc.axes0[p].x, pc.axes0[p].y, pc.axes0[p].z};
            Vector3f c2p{pos.x - eyePos.x, pos.y - eyePos.y, pos.z - eyePos.z};
            const f32 cl2 = c2p.x * c2p.x + c2p.y * c2p.y + c2p.z * c2p.z;
            if (cl2 > 1e-10f) {
                const f32 inv = 1.0f / std::sqrt(cl2);
                c2p = {c2p.x * inv, c2p.y * inv, c2p.z * inv};
            } else {
                c2p = {-camForward.x, -camForward.y, -camForward.z};
            }
            Vector3f side = ::whiteout::cross(ax, c2p);
            const f32 sl2 = side.x * side.x + side.y * side.y + side.z * side.z;
            if (sl2 > 1e-10f) {
                const f32 inv = 1.0f / std::sqrt(sl2);
                side = {side.x * inv, side.y * inv, side.z * inv};
            } else {
                side = camRight; // Axis0 ∥ view direction
            }
            side = {side.x * sx, side.y * sx, side.z * sx};
            const Vector3f up = ::whiteout::cross(c2p, side);
            const Vector3f e0{pos.x + ax.x * 0.5f, pos.y + ax.y * 0.5f, pos.z + ax.z * 0.5f};
            const Vector3f e1{pos.x - ax.x * 0.5f, pos.y - ax.y * 0.5f, pos.z - ax.z * 0.5f};
            const Vector3f cv[6] = {
                {e0.x + side.x, e0.y + side.y, e0.z + side.z},
                {e1.x + side.x, e1.y + side.y, e1.z + side.z},
                {e1.x - side.x, e1.y - side.y, e1.z - side.z},
                {e0.x - side.x, e0.y - side.y, e0.z - side.z},
                {e0.x + up.x, e0.y + up.y, e0.z + up.z},
                {e1.x - up.x, e1.y - up.y, e1.z - up.z},
            };
            // Diagonal-only UV set — the caps sample the sprite's corners.
            static constexpr f32 kCapsuleUV[6][2] = {{1, 1}, {1, 1}, {0, 0},
                                                     {0, 0}, {1, 0}, {0, 1}};
            static constexpr u32 kCapsuleIdx[12] = {0, 1, 2, 2, 3, 0, 3, 4, 0, 1, 5, 2};

            const u32 cb = static_cast<u32>(verts.size());
            const Vector4f cpivot = {pos.x, pos.y, pos.z, 1.0f};
            const f32 caprnd = ls.alphaLutSlot != 0 ? ParticleRandom(p, pc.layerValue) : 0.0f;
            for (i32 c = 0; c < 6; ++c) {
                f32 cu = kCapsuleUV[c][0];
                f32 cvv = kCapsuleUV[c][1];
                if (ls.rotate) {
                    const f32 t = cu;
                    cu = cvv;
                    cvv = 1.0f - t;
                }
                if (ls.flipU)
                    cu = 1.0f - cu;
                if (ls.flipV)
                    cvv = 1.0f - cvv;
                CornEffectsVertex vtx{};
                vtx.position = cv[c];
                vtx.color = color;
                vtx.random = caprnd;
                vtx.uv0 = {cellU0 + cu * cellU, cellV0 + cvv * cellV};
                vtx.pivot = cpivot;
                vtx.modeSlot4 = {cellU0b + cu * cellU, cellV0b + cvv * cellV, 0.0f, 0.0f};
                vtx.modeSlot5 = {atlasCursor, 0.0f, 0.0f, 0.0f};
                verts.push_back(vtx);
            }
            for (u32 t = 0; t < 12; ++t)
                indices.push_back(cb + kCapsuleIdx[t]);
            continue;
        }

        Vector3f r0, u0;
        const bool wantAxis0 = (bbMode == 2 || bbMode == 3 || bbMode == 4);
        const bool wantBothAxes =
            (bbMode == 5) || (bbMode == 0 && !pc.axes0.empty() && !pc.axes1.empty());

        if (wantBothAxes && p < pc.axes0.size() && p < pc.axes1.size()) {
            if (bbMode == 5) {
                // PlaneAligned (engine CPlanarBillboarderQuad::Align): the quad's
                // WIDTH axis is normalize(Axis0 × Axis1) and its HEIGHT axis is
                // cross(Axis1, width). A blind Y<->Z swap would put the width on
                // Axis0 itself (e.g. a vertical lightning bolt instead of
                // horizontal). r0/u0 are used directly with NO Y<->Z swap,
                // matching the engine-verified cornflakes_gl reference.
                const f32 a0x = pc.axes0[p].x, a0y = pc.axes0[p].y, a0z = pc.axes0[p].z;
                const f32 a1x = pc.axes1[p].x, a1y = pc.axes1[p].y, a1z = pc.axes1[p].z;
                f32 wx = a0y * a1z - a0z * a1y;
                f32 wy = a0z * a1x - a0x * a1z;
                f32 wz = a0x * a1y - a0y * a1x;
                const f32 wl2 = wx * wx + wy * wy + wz * wz;
                if (wl2 > 1e-20f) {
                    const f32 inv = 1.0f / std::sqrt(wl2);
                    wx *= inv;
                    wy *= inv;
                    wz *= inv;
                } else {
                    // Axis0 ∥ Axis1 (or one is zero) — the cross is
                    // degenerate and the quad would collapse to nothing.
                    // Any unit vector perpendicular to Axis1 will do.
                    const f32 a1l2 = a1x * a1x + a1y * a1y + a1z * a1z;
                    if (a1l2 > 1e-20f) {
                        const bool useX = std::fabs(a1z) > 0.9f * std::sqrt(a1l2);
                        const f32 hx = useX ? 1.0f : 0.0f;
                        const f32 hz = useX ? 0.0f : 1.0f;
                        wx = a1y * hz - a1z * 0.0f;
                        wy = a1z * hx - a1x * hz;
                        wz = a1x * 0.0f - a1y * hx;
                        const f32 nl2 = wx * wx + wy * wy + wz * wz;
                        if (nl2 > 1e-20f) {
                            const f32 inv = 1.0f / std::sqrt(nl2);
                            wx *= inv;
                            wy *= inv;
                            wz *= inv;
                        }
                    }
                }
                // Height axis is cross(Axis1, axisU), NOT Axis0 rescaled to
                // |Axis1|. The two agree only when Axis0 ⟂ Axis1; for oblique
                // pairs the rescale sheared the quad into a parallelogram.
                r0 = {wx, wy, wz};
                u0 = {a1y * wz - a1z * wy, a1z * wx - a1x * wz, a1x * wy - a1y * wx};
            } else {
                // ScreenAligned with explicit axes: keep the Y<->Z mapping.
                r0 = {pc.axes0[p].x, pc.axes0[p].z, pc.axes0[p].y};
                u0 = {pc.axes1[p].x, pc.axes1[p].z, pc.axes1[p].y};
            }
        } else if (bbMode == 3 && p < pc.axes0.size()) {
            // AxisAlignedSpheroid: the quad faces the camera around Axis0. Width is
            // perpendicular to both Axis0 and the eye->particle direction; height is
            // Axis0·0.5 plus the in-view "up" so the spheroid leans toward the eye.
            // Engine-faithful (matches the GL reference billboarder).
            const Vector3f ax{pc.axes0[p].x, pc.axes0[p].y, pc.axes0[p].z};
            Vector3f c2p{pos.x - eyePos.x, pos.y - eyePos.y, pos.z - eyePos.z};
            const f32 cl2 = c2p.x * c2p.x + c2p.y * c2p.y + c2p.z * c2p.z;
            if (cl2 > 1e-10f) {
                const f32 inv = 1.0f / std::sqrt(cl2);
                c2p = {c2p.x * inv, c2p.y * inv, c2p.z * inv};
            } else {
                c2p = {-camForward.x, -camForward.y, -camForward.z};
            }
            Vector3f side = ::whiteout::cross(ax, c2p);
            const f32 sl2 = side.x * side.x + side.y * side.y + side.z * side.z;
            if (sl2 > 1e-12f) {
                const f32 inv = 1.0f / std::sqrt(sl2);
                side = {side.x * inv, side.y * inv, side.z * inv};
            } else {
                side = camRight;
            }
            side = {side.x * sx, side.y * sx, side.z * sx};
            const Vector3f up = ::whiteout::cross(c2p, side);
            const Vector3f phd{ax.x * 0.5f + up.x, ax.y * 0.5f + up.y, ax.z * 0.5f + up.z};
            // r0 is negated: TAxialBillboarderSpheroidalImpl<0>::Align writes
            // its corners in the mirror of the shared corner order, so an
            // un-negated side axis samples the sprite flipped horizontally.
            const f32 invSz = (sx != 0.0f) ? (1.0f / sx) : 0.0f;
            r0 = {side.x * -invSz, side.y * -invSz, side.z * -invSz};
            u0 = {phd.x * invSz, phd.y * invSz, phd.z * invSz};
        } else if (wantAxis0 && p < pc.axes0.size()) {
            const f32 ax = pc.axes0[p].x;
            const f32 ay = pc.axes0[p].y;
            const f32 az = pc.axes0[p].z;
            const f32 invSz = (sx != 0.0f) ? (1.0f / sx) : 0.0f;
            // Axis0 is the full quad height (extent |Axis0|), so the half-extent
            // carried in u0 is Axis0·0.5. Omitting the 0.5 doubled the height.
            u0 = {ax * 0.5f * invSz, ay * 0.5f * invSz, az * 0.5f * invSz};
            const f32 alen2 = ax * ax + ay * ay + az * az;
            if (alen2 > 1e-12f) {
                const f32 invLen = 1.0f / std::sqrt(alen2);
                const Vector3f a{ax * invLen, ay * invLen, az * invLen};
                // TAxialBillboarderQuadImpl<0>::Align takes the perpendicular
                // from the PER-PARTICLE view vector. A single packet-wide
                // camForward skews the quad toward the edges of frame — the
                // error is zero at screen centre, so centred previews miss it.
                const Vector3f c2p{pos.x - eyePos.x, pos.y - eyePos.y, pos.z - eyePos.z};
                Vector3f c = ::whiteout::cross(c2p, a);
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

        // The axial billboarders (quad/spheroid/capsule) derive their basis
        // from Axis0 and ignore Rotation entirely; only the screen-facing and
        // plane-aligned ones rotate. PlaneAligned rotates the opposite way —
        // TRotationHelper<1>::RotateAxis uses Nmsub/Madd where AlignAndRotate
        // inlines the mix.
        const bool bbModeRotates = (bbMode != 2 && bbMode != 3 && bbMode != 4);
        const f32 rot = (p < pc.rotations.size()) ? pc.rotations[p] : 0.0f;
        if (rot != 0.0f && bbModeRotates) {
            const f32 ca = std::cos(rot);
            const f32 sa = std::sin(rot);
            const f32 sgn = (bbMode == 5) ? -1.0f : 1.0f;
            const Vector3f rk = r0;
            const Vector3f uk = u0;
            r0 = {ca * rk.x + sgn * sa * uk.x, ca * rk.y + sgn * sa * uk.y,
                  ca * rk.z + sgn * sa * uk.z};
            u0 = {-sgn * sa * rk.x + ca * uk.x, -sgn * sa * rk.y + ca * uk.y,
                  -sgn * sa * rk.z + ca * uk.z};
        }

        const Vector3f r = {r0.x * sx, r0.y * sx, r0.z * sx};
        const Vector3f u = {u0.x * sy, u0.y * sy, u0.z * sy};
        const Vector3f c0v = {pos.x - r.x - u.x, pos.y - r.y - u.y, pos.z - r.z - u.z};
        const Vector3f c1v = {pos.x + r.x - u.x, pos.y + r.y - u.y, pos.z + r.z - u.z};
        const Vector3f c2v = {pos.x + r.x + u.x, pos.y + r.y + u.y, pos.z + r.z + u.z};
        const Vector3f c3v = {pos.x - r.x + u.x, pos.y - r.y + u.y, pos.z - r.z + u.z};

        // v = 0 is the +up corner: the engine's quad puts the image's top row
        // at the top of the billboard, and our BasicUV shader path passes uv0
        // through untouched (popcorn_vs.slang) so nothing downstream flips it.
        static constexpr f32 kCornerUV[4][2] = {
            {0.0f, 1.0f},
            {1.0f, 1.0f},
            {1.0f, 0.0f},
            {0.0f, 0.0f},
        };
        Vector2f uv[4];
        Vector2f uvB[4];
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
            // Same corner, next atlas cell. The atlas perm's PS lerps A→B by
            // frac(cursor); on the non-atlas perm these lanes are never read.
            uvB[corner].x = cellU0b + cu * cellU;
            uvB[corner].y = cellV0b + cv * cellV;
        }

        const u32 b = static_cast<u32>(verts.size());
        const Vector4f pivot = {pos.x, pos.y, pos.z, 1.0f};
        const Vector4f slot5 = {atlasCursor, 0.0f, 0.0f, 0.0f};
        const Vector3f cvp[4] = {c0v, c1v, c2v, c3v};
        const f32 rnd = ls.alphaLutSlot != 0 ? ParticleRandom(p, pc.layerValue) : 0.0f;
        for (i32 corner = 0; corner < 4; ++corner) {
            CornEffectsVertex vtx{};
            vtx.position = cvp[corner];
            vtx.color = color;
            vtx.uv0 = uv[corner];
            vtx.random = rnd;
            vtx.pivot = pivot;
            vtx.modeSlot4 = {uvB[corner].x, uvB[corner].y, 0.0f, 0.0f};
            vtx.modeSlot5 = slot5;
            verts.push_back(vtx);
        }
        indices.push_back((b + 0));
        indices.push_back((b + 1));
        indices.push_back((b + 2));
        indices.push_back((b + 0));
        indices.push_back((b + 2));
        indices.push_back((b + 3));
    }
    flushRun(static_cast<u32>(indices.size()));

    SubmitRibbons(packets, view);
    // GPU emission is deferred to CornEffectsService — it walks every
    // emitter's pending_ batch, packs them into one shared VB/IB pair,
    // writes the shared CBs once, then issues all DrawIndexeds with
    // (baseVertex, firstIndex) into the shared buffers.
}

void CornEffectsGfxBackend::SubmitRibbons(
    std::span<const ::whiteout::cornflakes::RenderPacket> packets,
    const ::whiteout::cornflakes::ViewParams& view) {
    bool anyRibbon = false;
    for (const auto& pkt : packets) {
        if (pkt.cls == ::whiteout::cornflakes::RendererClass::Ribbon && pkt.particleCount != 0) {
            anyRibbon = true;
            break;
        }
    }
    if (!anyRibbon)
        return;

    // buildRibbonGeometry returns spans into this arena; we copy straight out
    // into pending_, so it only has to live for the loop.
    ::whiteout::cornflakes::ExpandingArena arena;

    auto& verts = pending_.verts;
    auto& indices = pending_.indices;
    auto& draws = pending_.draws;

    for (const auto& pkt : packets) {
        if (pkt.cls != ::whiteout::cornflakes::RendererClass::Ribbon)
            continue;
        if (pkt.particleCount == 0)
            continue;
        if (pkt.layer.value >= layerStates_.size())
            continue;
        const auto& layerRenderers = layerStates_[pkt.layer.value];
        if (pkt.rendererIndex >= layerRenderers.size())
            continue;
        const auto& ls = layerRenderers[pkt.rendererIndex];
        if (!ls.renderable || !ls.isRibbon)
            continue;

        ::whiteout::cornflakes::RibbonUVConfig uvCfg;
        uvCfg.flipU = ls.flipU;
        uvCfg.flipV = ls.flipV;
        uvCfg.rotate = ls.rotate;
        uvCfg.customTextureU = ls.customTextureU;
        uvCfg.atlasSubDivX = ls.atlasX;
        uvCfg.atlasSubDivY = ls.atlasY;
        // correctDeformation / needsNormals stay off: both are carried
        // per-vertex (uvFactors, normal+tangent) and the BasicUV perm has no
        // attributes for them. Leaving them off keeps uvFactors all-ones,
        // which is exactly the case where the shader's UV formula collapses
        // to the affine bake below.

        const auto out = ::whiteout::cornflakes::buildRibbonGeometry(pkt, view, uvCfg, arena);
        if (out.vertices.empty())
            continue;

        const u32 baseVertex = static_cast<u32>(verts.size());
        const u32 firstIndex = static_cast<u32>(indices.size());

        verts.reserve(verts.size() + out.vertices.size());
        for (const auto& rv : out.vertices) {
            CornEffectsVertex cv{};
            cv.position = {rv.position.x, rv.position.y, rv.position.z};
            cv.color = {rv.color.x, rv.color.y, rv.color.z, rv.color.w};
            // The billboard perms synthesise the quad UV in-shader and apply
            // uvScaleBias there; BasicUV samples uv0 directly, so fold the
            // same affine transform in here. Exact while uvFactors are ones.
            cv.uv0 = {rv.u * rv.uvScaleBias.x + rv.uvScaleBias.z,
                      rv.v * rv.uvScaleBias.y + rv.uvScaleBias.w};
            // Ribbons DO have a real Cursor stream — buildRibbonGeometry
            // carries it per vertex, so the LUT reads the authored curve
            // rather than a synthesised hash.
            cv.random = rv.cursor;
            // popcornBlendPivot is lerp(pivot.xyz, position, pivot.www), so
            // w = 1 means "use the vertex position as authored".
            cv.pivot = {0.0f, 0.0f, 0.0f, 1.0f};
            verts.push_back(cv);
        }

        // 4 verts per quad, two triangles sharing the 1-2 edge. Strand-end
        // particles emit an all-zero degenerate quad so the slot alignment
        // buildRibbonGeometry relies on survives; those rasterise to a point.
        const size_t quadCount = out.vertices.size() / 4U;
        indices.reserve(indices.size() + quadCount * 6U);
        for (size_t q = 0; q < quadCount; ++q) {
            const u32 b = baseVertex + static_cast<u32>(q * 4U);
            indices.push_back((b + 0));
            indices.push_back((b + 1));
            indices.push_back((b + 2));
            indices.push_back((b + 1));
            indices.push_back((b + 3));
            indices.push_back((b + 2));
        }

        draws.push_back({firstIndex, static_cast<u32>(indices.size()) - firstIndex,
                         pkt.layer.value, pkt.rendererIndex, pkt.blendMode, ls.vsPerm,
                         ls.psPerm});
    }
}

void CornEffectsGfxBackend::shutdown(::whiteout::cornflakes::IssueBag& /*issues*/) {
    layerStates_.clear();
}

} // namespace whiteout::flakes::renderer::corn_effects
