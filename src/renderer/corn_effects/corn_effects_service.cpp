#include "renderer/corn_effects/corn_effects_service.h"

#include "renderer/assets/asset_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/bls_draw_helpers.h"
#include "renderer/bls/bls_pso_builder.h"
#include "renderer/bls/scoped_cb.h"
#include "renderer/corn_effects/corn_effects_vertex.h"

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/interface/service/service_types.hpp>
#include <cornflakes/interface/sim/effect_runtime.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace whiteout::flakes::renderer::corn_effects {

namespace {
// Must match the constants in corn_effects_gfx_backend.cpp — these are
// the BLS shader permutation indices for the basic UV-with-vertex-color
// corn-fx variant. If we use the wrong perm we end up with a shader
// that doesn't sample the diffuse texture and every particle renders
// white regardless of texture state.
constexpr u32 kVsPermBasicUVWithVC = 10;
constexpr u32 kPsPermBasicUVWithVC = (0 * 3 + 1) * 128 + 0x20;
} // namespace

CornEffectsService::CornEffectsService() = default;

CornEffectsService::~CornEffectsService() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

void CornEffectsService::AddCornEmitter(ActorId model, i32 emitterId,
                                        std::unique_ptr<CornEffectsEmitter> emitter) {
    std::lock_guard<std::mutex> lock(mutex_);
    emitter->gameToCornEffectsScale_ = gameToCornEffectsScale_;
    emitter->SetBackendInit(backendInit_);
    emitter->SetFrameArena(&frameArena_);
    emitter->SetResourceProviders(&meshProvider_, &textureProvider_, &meshProvider_);
    emitters_[{model, emitterId}] = std::move(emitter);
}

void CornEffectsService::SetContentProvider(io::IContentProvider* content) {
    std::lock_guard<std::mutex> lock(mutex_);
    meshProvider_.SetContentProvider(content);
    textureProvider_.SetContentProvider(content);
}

void CornEffectsService::SetBackendInit(const std::optional<CornEffectsGfxBackend::Init>& init) {
    std::lock_guard<std::mutex> lock(mutex_);
    backendInit_ = init;
    for (auto& [k, e] : emitters_) {
        e->SetBackendInit(backendInit_);
    }
}

void CornEffectsService::SetFrameInputs(const CornEffectsFrameInputs& fi) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameInputs_ = fi;
    if (gameToCornEffectsScale_ > 0.0f) {
        frameInputs_.cornEffectsScale = 1.0f / gameToCornEffectsScale_;
    }
    for (auto& [k, e] : emitters_) {
        e->SetFrameInputs(frameInputs_);
    }
}

void CornEffectsService::RemoveModel(ActorId model) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (it->first.model == model)
            it = emitters_.erase(it);
        else
            ++it;
    }
}

void CornEffectsService::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

CornEffectsEmitter* CornEffectsService::GetEmitter(ActorId model, i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    return (it != emitters_.end()) ? it->second.get() : nullptr;
}

bool CornEffectsService::ComputeWorldParticleBounds(ActorId model, i32 emitterId, Vector3f& outMin,
                                                    Vector3f& outMax) const {
    namespace cf = ::whiteout::cornflakes;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    if (it == emitters_.end())
        return false;
    const auto* rt = it->second->LiveRuntime();
    if (!rt)
        return false;

    // Render-packet Position/Size are corn-space; the GPU multiplies by
    // cornEffectsScale (= 1/gameToCornEffectsScale) to reach world units. Pad
    // the AABB by each billboard's radius so its quad isn't clipped.
    Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    bool have = false;
    for (const auto& pkt : rt->lastPackets()) {
        if (pkt.particleCount == 0)
            continue;
        auto posBytes = pkt.slots[static_cast<std::size_t>(cf::RenderSlot::Position)];
        const std::size_t n = std::min<std::size_t>(pkt.particleCount,
                                                    posBytes.size() / sizeof(cf::Float3));
        if (n == 0)
            continue;
        const auto* pos = reinterpret_cast<const cf::Float3*>(posBytes.data());
        auto sizeBytes = pkt.slots[static_cast<std::size_t>(cf::RenderSlot::Size)];
        const f32* sz = sizeBytes.size() >= n * sizeof(f32)
                            ? reinterpret_cast<const f32*>(sizeBytes.data())
                            : nullptr;
        for (std::size_t p = 0; p < n; ++p) {
            const f32 r = (sz && std::isfinite(sz[p])) ? std::max(0.0f, sz[p]) : 0.0f;
            lo.x = std::min(lo.x, pos[p].x - r);
            lo.y = std::min(lo.y, pos[p].y - r);
            lo.z = std::min(lo.z, pos[p].z - r);
            hi.x = std::max(hi.x, pos[p].x + r);
            hi.y = std::max(hi.y, pos[p].y + r);
            hi.z = std::max(hi.z, pos[p].z + r);
            have = true;
        }
    }
    if (!have)
        return false;

    const f32 cornToGame =
        gameToCornEffectsScale_ > 0.0f ? (1.0f / gameToCornEffectsScale_) : 1.0f;
    outMin = {lo.x * cornToGame, lo.y * cornToGame, lo.z * cornToGame};
    outMax = {hi.x * cornToGame, hi.y * cornToGame, hi.z * cornToGame};
    return true;
}

i32 CornEffectsService::EmitterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<i32>(emitters_.size());
}

i32 CornEffectsService::TotalParticleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (const auto& [k, e] : emitters_) {
        total += e->TotalAlive();
    }
    return total;
}

bool CornEffectsService::HasEmittersForModel(ActorId model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        if (k.model == model)
            return true;
    }
    return false;
}

void CornEffectsService::SetOwningAgentVisibilityForModel(ActorId model, bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [k, e] : emitters_) {
        if (k.model == model)
            e->SetOwningAgentVisibility(visible);
    }
}

void CornEffectsService::SimulateInternal(f32 dt) {
    frameArena_.reset();
    // Phase 0: clear every emitter's pending batch. Inactive emitters
    // (visibility lost, not spawning) short-circuit out of Update before
    // submit() runs, so their pending_ would otherwise retain stale verts
    // / indices / draws from the last active frame — the service would
    // pack stale data into the shared VB and the offsets would be wrong.
    for (auto& [k, e] : emitters_) {
        if (e->live_.backend)
            e->live_.backend->Pending().Clear();
    }
    // Phase 1: tick every emitter — each backend's submit() fills
    // pending_ (CPU-only).
    for (auto& [k, e] : emitters_) {
        e->gameToCornEffectsScale_ = gameToCornEffectsScale_;
        e->Update(dt, false);
    }
}

void CornEffectsService::ResetRuntimes() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [k, e] : emitters_)
        e->ResetRuntime();
}

void CornEffectsService::Simulate(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    SimulateInternal(dt);
}

void CornEffectsService::SimulateAndRender(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    SimulateInternal(dt);
    if (ConsolidatePending())
        for (const auto& s : preparedSlices_)
            DrawPreparedSlice(s);
}

void CornEffectsService::PrepareInterleavedDraws(std::vector<CornDrawUnit>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ConsolidatePending())
        return;
    out.reserve(out.size() + preparedSlices_.size());
    for (u32 i = 0; i < preparedSlices_.size(); ++i)
        out.push_back({preparedSlices_[i].key.model, preparedSlices_[i].key.id, 0, i});
}

void CornEffectsService::DrawCornSlice(u32 slice) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slice < preparedSlices_.size())
        DrawPreparedSlice(preparedSlices_[slice]);
}

bool CornEffectsService::EnsureSharedBuffers(u32 totalVerts, u32 totalIndices) {
    if (!backendInit_ || !backendInit_->device) return false;
    auto* device = backendInit_->device;

    if (sharedVsCb_ == gfx::BufferHandle::Invalid) {
        gfx::BufferDesc bd;
        bd.size = sizeof(bls::HdVsCb);
        bd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
        sharedVsCb_ = device->CreateBuffer(bd);
    }
    if (sharedPsCb_ == gfx::BufferHandle::Invalid) {
        gfx::BufferDesc bd;
        bd.size = sizeof(bls::HdPsCb);
        bd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
        sharedPsCb_ = device->CreateBuffer(bd);
    }
    // Cap the doubling growth — a one-frame spike (transient burst of
    // particles) shouldn't permanently pin VRAM. The cap sets the
    // *doubling ceiling*: we always allocate enough to fit
    // `totalVerts`, but if doubling overshoots we clamp to this max
    // and grow no further. 256k verts × 64 B × kFramesInFlight ≈
    // 50 MiB worst case.
    constexpr u32 kMaxSharedVbCap = 256 * 1024;
    constexpr u32 kMaxSharedIbCap = 384 * 1024;
    if (totalVerts > sharedVbCap_ || sharedVb_ == gfx::BufferHandle::Invalid) {
        device->Destroy(sharedVb_);
        const u32 doubled = std::max<u32>(sharedVbCap_ * 2u, 1024u);
        const u32 clamped = std::min(doubled, kMaxSharedVbCap);
        sharedVbCap_ = std::max(totalVerts, clamped);
        gfx::BufferDesc bd;
        bd.size = sizeof(CornEffectsVertex) * sharedVbCap_;
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        sharedVb_ = device->CreateBuffer(bd);
    }
    if (totalIndices > sharedIbCap_ || sharedIb_ == gfx::BufferHandle::Invalid) {
        device->Destroy(sharedIb_);
        const u32 doubled = std::max<u32>(sharedIbCap_ * 2u, 1536u);
        const u32 clamped = std::min(doubled, kMaxSharedIbCap);
        sharedIbCap_ = std::max(totalIndices, clamped);
        gfx::BufferDesc bd;
        bd.size = sizeof(u32) * sharedIbCap_;
        bd.usage = gfx::BufferUsage::Index | gfx::BufferUsage::CpuWritable;
        sharedIb_ = device->CreateBuffer(bd);
    }
    return sharedVb_ != gfx::BufferHandle::Invalid && sharedIb_ != gfx::BufferHandle::Invalid &&
           sharedVsCb_ != gfx::BufferHandle::Invalid && sharedPsCb_ != gfx::BufferHandle::Invalid;
}

bool CornEffectsService::ConsolidatePending() {
    preparedSlices_.clear();
    if (!backendInit_ || !backendInit_->device || !backendInit_->program ||
        !backendInit_->psoBuilder || !frameInputs_.cmd)
        return false;

    // Sum totals across every emitter's pending_ batch.
    u32 totalVerts = 0, totalIndices = 0, totalDraws = 0;
    for (auto& [k, e] : emitters_) {
        const auto* be = e->live_.backend.get();
        if (!be) continue;
        const auto& p = be->Pending();
        if (p.Empty()) continue;
        totalVerts   += static_cast<u32>(p.verts.size());
        totalIndices += static_cast<u32>(p.indices.size());
        totalDraws   += static_cast<u32>(p.draws.size());
    }
    if (totalDraws == 0) return false;
    if (!EnsureSharedBuffers(totalVerts, totalIndices)) return false;

    auto* device = backendInit_->device;

    // Pack every emitter's verts/indices contiguously into the shared buffers;
    // each emitter's slice records its (baseVertex, baseIndex) + key so its
    // draws can run later in sorted order via DrawCornSlice.
    if (void* vp = device->MapBuffer(sharedVb_)) {
        auto* dst = static_cast<CornEffectsVertex*>(vp);
        u32 voff = 0;
        for (auto& [k, e] : emitters_) {
            auto* be = e->live_.backend.get();
            if (!be) continue;
            const auto& p = be->Pending();
            if (p.Empty()) continue;
            std::memcpy(dst + voff, p.verts.data(), sizeof(CornEffectsVertex) * p.verts.size());
            voff += static_cast<u32>(p.verts.size());
        }
        device->UnmapBuffer(sharedVb_);
    }
    if (void* ip = device->MapBuffer(sharedIb_)) {
        auto* dst = static_cast<u32*>(ip);
        u32 voff = 0, ioff = 0;
        for (auto& [k, e] : emitters_) {
            auto* be = e->live_.backend.get();
            if (!be) continue;
            const auto& p = be->Pending();
            if (p.Empty()) continue;
            std::memcpy(dst + ioff, p.indices.data(), sizeof(u32) * p.indices.size());
            preparedSlices_.push_back({be, voff, ioff, k});
            voff += static_cast<u32>(p.verts.size());
            ioff += static_cast<u32>(p.indices.size());
        }
        device->UnmapBuffer(sharedIb_);
    }

    // Write the shared CBs once — frame inputs are identical for every emitter.
    const Matrix44f worldView     = frameInputs_.view;
    const Matrix44f worldViewProj = frameInputs_.view * frameInputs_.projection;
    if (auto vs = bls::ScopedCb<bls::HdVsCb>(device, sharedVsCb_)) {
        bls::HdVsCb& cb = *vs;
        cb.world         = Matrix44f::identity();
        cb.worldView     = worldView;
        cb.worldViewProj = worldViewProj;
        cb.misc          = {frameInputs_.effectTime, frameInputs_.cornEffectsScale, 0.0f, 0.0f};
        cb.diffuseColor  = {1, 1, 1, 1};
        cb.texMtx0       = {};
        cb.texMtx1       = {};
    }
    if (auto ps = bls::ScopedCb<bls::HdPsCb>(device, sharedPsCb_)) {
        bls::HdPsCb& cb = *ps;
        std::memset(&cb, 0, sizeof(cb));
        cb.alphaRef      = 0.0f;
        cb.fogParams     = {0, 0, 0, 0};
        cb.fogColor      = {0, 0, 0, 1};
        cb.worldView     = worldView;
        cb.view          = frameInputs_.view;
        cb.projection    = frameInputs_.projection;
        // NOTE: the popcorn PS block aliases two of these. At offset 240 the
        // shader reads `depthUVRemap` (engine value (1,-1,0,1)) where we write
        // viewportRect, and at 256 it reads `depthFadeScale` (engine default
        // 1e-6, per-draw 1/softness) where we write pixelParams1.x = 1.0f.
        // Inert today because no perm we select reads them — but a soft-particle
        // perm would fade everything to nothing and send the depth lookup
        // off-screen. Fix these before enabling SoftParticles.
        cb.viewportRect  = frameInputs_.viewportRect;
        cb.pixelParams1  = {1.0f, 0.0f, 0.0f, 0.0f};
        cb.effectTime    = frameInputs_.effectTime;
        cb.lightCount    = 0.0f;
    }
    return true;
}

void CornEffectsService::DrawPreparedSlice(const PreparedSlice& s) {
    auto* cmd = frameInputs_.cmd;
    if (!cmd)
        return;

    // Re-bind the shared resources each call so this is safe interleaved with
    // other transparent producers; redundant-bind suppression de-dupes runs of
    // consecutive corn slices.
    cmd->BindVertexBuffer(0, sharedVb_, sizeof(CornEffectsVertex));
    cmd->BindIndexBuffer(sharedIb_, gfx::Format::R32_UINT);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, sharedVsCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2, sharedPsCb_);
    if (backendInit_->samplers) {
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0,
                         backendInit_->samplers->WrapVariant(assets::kSamplerWrapBitsMask));
    }

    auto* assets = backendInit_->assets;
    auto* tex_mgr = backendInit_->textures;
    const auto& p = s.backend->Pending();
    for (const auto& d : p.draws) {
        bls::MatParams mp;
        mp.shaderId    = bls::GxShaderID::CornFx;
        mp.alpha       = CornEffectsGfxBackend::BlendModeToGxAlpha(d.blendMode);
        mp.disables    = bls::kDisableLighting | bls::kDisableDepthWrite | bls::kDisableCull;
        mp.diffuseColor = {1, 1, 1, 1};

        // Classified per (layer, renderer) in the backend's prepare(). Falls
        // back to the BasicUV+VC pair for a draw that predates classification.
        bls::PermuteIndices perm{d.vsPerm ? d.vsPerm : kVsPermBasicUVWithVC,
                                 d.psPerm ? d.psPerm : kPsPermBasicUVWithVC};

        auto req =
            bls::MakePsoRequest(backendInit_->program, bls::VertexLayoutKind::CornFx, mp, perm);
        req.rtvFormat = frameInputs_.rtvFormat;
        req.extraRtvCount = frameInputs_.extraRtvCount;
        for (u32 i = 0; i < frameInputs_.extraRtvCount &&
                        i < gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments;
             ++i) {
            req.extraRtvFormats[i] = frameInputs_.extraRtvFormats[i];
        }
        req.dsvFormat = frameInputs_.dsvFormat;
        auto pso = backendInit_->psoBuilder->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid)
            continue;
        cmd->BindPipeline(pso);

        const auto diffuseSlot = s.backend->LayerDiffuseSlot(d.layerIdx, d.rendererIdx);
        gfx::TextureHandle tex = (diffuseSlot != 0 && assets) ? assets->TextureOf(diffuseSlot)
                                                              : gfx::TextureHandle::Invalid;
        if (tex == gfx::TextureHandle::Invalid && tex_mgr)
            tex = tex_mgr->GetDefaults().White;
        if (tex != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, tex);

        // AlphaRemap LUT at t1/s1. Only the perms with the LUT bit sample it,
        // and prepare() only sets that bit when this slot is non-zero.
        const auto lutSlot = s.backend->LayerAlphaLutSlot(d.layerIdx, d.rendererIdx);
        if (lutSlot != 0 && assets) {
            const gfx::TextureHandle lut = assets->TextureOf(lutSlot);
            if (lut != gfx::TextureHandle::Invalid) {
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, lut);
                if (backendInit_->samplers) {
                    // Clamp: the LUT is a curve, not a tiling texture — wrapping
                    // would fold the ends of the remap onto each other.
                    cmd->BindSampler(gfx::ShaderStage::Pixel, 1,
                                     backendInit_->samplers->WrapVariant(
                                         assets::WrapMode::ClampClamp));
                }
            }
        }

        cmd->DrawIndexed(d.indexCount, s.baseIndex + d.indexFirst, static_cast<i32>(s.baseVertex));
    }
}

void CornEffectsService::ReleaseGpuResources() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!backendInit_ || !backendInit_->device) return;
    auto* device = backendInit_->device;
    if (sharedVb_ != gfx::BufferHandle::Invalid) {
        device->Destroy(sharedVb_);
        sharedVb_ = gfx::BufferHandle::Invalid;
        sharedVbCap_ = 0;
    }
    if (sharedIb_ != gfx::BufferHandle::Invalid) {
        device->Destroy(sharedIb_);
        sharedIb_ = gfx::BufferHandle::Invalid;
        sharedIbCap_ = 0;
    }
    if (sharedVsCb_ != gfx::BufferHandle::Invalid) {
        device->Destroy(sharedVsCb_);
        sharedVsCb_ = gfx::BufferHandle::Invalid;
    }
    if (sharedPsCb_ != gfx::BufferHandle::Invalid) {
        device->Destroy(sharedPsCb_);
        sharedPsCb_ = gfx::BufferHandle::Invalid;
    }
}

std::vector<std::string> CornEffectsService::ExtractDiffuseTexturePaths(
    const ::whiteout::cornflakes::EffectAssetModel& model) {
    std::vector<std::string> out;
    ::whiteout::cornflakes::ExpandingArena bindArena(std::size_t{1U} << 16);
    ::whiteout::cornflakes::IssueBag issues;
    ::whiteout::cornflakes::EffectBinder binder;
    auto plan = binder.bind(model, ::whiteout::cornflakes::EffectId{0}, bindArena, issues);
    if (!plan.has_value() || issues.hasFatal())
        return out;
    std::unordered_set<std::string> seen;
    for (const auto& lp : plan->layers) {
        for (const auto& rr : lp.renderers) {
            if (rr.diffuseTexturePath.empty()) continue;
            std::string s(rr.diffuseTexturePath);
            if (seen.insert(s).second) out.push_back(std::move(s));
        }
    }
    return out;
}

} // namespace whiteout::flakes::renderer::corn_effects
