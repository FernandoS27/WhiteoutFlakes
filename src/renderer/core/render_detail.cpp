#include "bls/bls_frame.h"
#include "constants.h"
#include "debug/draw_trace_hooks.h"
#include "core/render_detail.h"
#include "shading/shading_model.h"
#include "shading/shading_registry.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/render_service.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::render_detail {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::bls;

namespace {
using ActorMap = std::unordered_map<u32, std::unique_ptr<model::Actor>>;

// Bind a RenderableView to an actor's render state. Shared by both the legacy
// per-geoset collector and BuildDrawLists so the wiring lives in one place.
void FillRenderableView(RenderableView& view, model::Actor& mi, const ActorMap& actors) {
    view.geosets = &mi.render.gpuGeosets;
    view.surfaceTable = mi.render.surfaceTable.get();
    view.textures = mi.render.textures.get();
    view.skinning = &mi.render.skinning;
    view.texAnimPalette = &mi.render.texAnimPalette;
    view.surfaceAnim = &mi.render.surfaceAnim;
    // Scaled, not raw: `worldTransform` is the host's, in game units. Returns
    // the same object for WC3, whose scale is 1.
    view.worldTransform = mi.ScaledWorldTransform();
    view.parentVisibility = mi.parentVisibility;
    view.hasLods = mi.render.hasLods;
    view.teamColor = mi.teamColor;
    view.actorRole = static_cast<u8>(mi.role);
    view.actorDepth = static_cast<u8>(mi.treeDepth);
    view.spawnEmitterId = mi.spawnEmitterId;
    view.spawnSlotIndex = mi.spawnSlotIndex;
    // Ranking the root against every top-level actor is linear per actor, so
    // it only runs when something is going to read it.
    view.rootActor = debug::DrawTraceEnabled() ? debug::TraceRootOrdinal(actors, mi.handle) : 0;
}

bool GeosetDrawable(const model::GPUGeoset& geo) {
    return geo.unskinnedVb != gfx::BufferHandle::Invalid &&
           geo.ib != gfx::BufferHandle::Invalid && geo.indexCount != 0;
}
} // namespace

CollectedDrawLists BuildDrawLists(
    const std::unordered_map<u32, std::unique_ptr<model::Actor>>& models, i32 selectedLod,
    const Vector3f& cameraPos, const shading::IShadingModel& shadingModel,
    bool unlitOddGeosets, const shading::ShadingRegistry* registry) {
    const core::ShadingModelId activeModel = shadingModel.Id();
    CollectedDrawLists out;
    out.views.reserve(models.size());
    out.lists.opaque.reserve(models.size() * 4);
    out.lists.transparent.reserve(models.size() * 2);
    out.sceneLights.reserve(models.size());

    // Iterate by sorted handle, not in map order. ActorManager::Map is an
    // unordered_map, `out.views` is filled in whatever order it yields, and
    // OpaqueOrder compares `a.view < b.view` — raw pointers into that vector.
    // So submit order was a function of container iteration order, which is
    // STL-implementation dependent (a baseline recorded on Windows would not
    // reproduce on Linux CI) and insertion/erasure-history dependent (PE1
    // children spawn and die continuously). Sorting here rather than changing
    // ActorManager::Map to std::map keeps the cost on the one site that needs
    // the guarantee.
    std::vector<u32> handles;
    handles.reserve(models.size());
    for (const auto& [h, miPtr] : models)
        handles.push_back(h);
    std::sort(handles.begin(), handles.end());

    for (u32 h : handles) {
        auto it = models.find(h);
        if (it == models.end() || !it->second)
            continue;
        Actor* mi = it->second.get();
        if (mi->parentVisibility <= 0.02f)
            continue;
        // Skip skinned actors whose bone palette hasn't been uploaded yet —
        // drawing here would bind a zero bone palette (matches the old pass).
        if (mi->render.skinning.HasSkeleton() && !mi->render.skinning.IsReady())
            continue;
        // `views` is reserved to models.size() so these pointers stay valid.
        RenderableView& view = out.views.emplace_back();
        FillRenderableView(view, *mi, models);

        // LightState positions/directions already carry the actor's world
        // transform (MdxModelAdapter::Evaluate applies it), so pooling across
        // actors needs no further transform.
        out.sceneLights.insert(out.sceneLights.end(), mi->render.activeLights.begin(),
                               mi->render.activeLights.end());

        // An actor can name its own model — an M2 has no materials, so asking
        // the WC3 model to classify it would mean reading a surface table that
        // was never built. Falls back to the profile's when unset, which is
        // every WC3 actor.
        const core::ShadingModelId actorModel = (mi->shadingModel != core::ShadingModelId::None)
                                                    ? mi->shadingModel
                                                    : activeModel;
        const shading::IShadingModel* classifier = &shadingModel;
        if (actorModel != activeModel && registry) {
            if (auto* m = registry->Get(actorModel))
                classifier = m;
        }

        // Which model each item names. The debug toggle overrides per geoset;
        // see RenderSettings::DebugUnlitOddGeosets for why it has to be per
        // geoset rather than global.
        auto modelFor = [&](i32 geoIdx) {
            return (unlitOddGeosets && (geoIdx & 1)) ? core::ShadingModelId::Unlit : actorModel;
        };

        const i32 modelLod = mi->render.hasLods ? selectedLod : 0;
        const i32 geosetCount = static_cast<i32>(mi->render.gpuGeosets.size());
        for (i32 i = 0; i < geosetCount; ++i) {
            const auto& geo = mi->render.gpuGeosets[i];
            if (!GeosetPassesLod(geo.lod, modelLod) || !GeosetDrawable(geo))
                continue;

            // One item per (geoset, surface). `key` arrives pre-filled for a
            // multi-surface geoset and default for a whole-geoset one, so the
            // single-draw path below is bit-for-bit what it always was.
            auto emit = [&](const core::SurfaceClass& sc, core::SurfaceKey key) {
                key.model = modelFor(i);
                if (sc.blend != core::BlendClass::Transparent) {
                    // Opaque: layers in order; the depth buffer sorts it
                    // against the rest of the opaque set.
                    DrawItem o;
                    o.view = &view;
                    o.geoIdx = i;
                    o.key = key;
                    out.lists.opaque.push_back(o);
                } else {
                    // Transparent: sorted back-to-front by world-space centroid.
                    DrawItem t;
                    t.view = &view;
                    t.geoIdx = i;
                    t.key = key;
                    // HD opaque-fading geosets carry the Color depth-fill twin
                    // (WC3 RenderGeoset's DEPTHFILL_COLOR); true blends stay None.
                    t.depthFill = sc.needsDepthFill ? bls::DepthFill::Color : bls::DepthFill::None;
                    const Vector3f wc = GeosetCentroidWS(view, geo);
                    const Vector3f d = {wc.x - cameraPos.x, wc.y - cameraPos.y,
                                        wc.z - cameraPos.z};
                    t.sqDist = d.x * d.x + d.y * d.y + d.z * d.z;
                    // A surface's own plane wins when it has one; the geoset's
                    // is the fallback and the only value WC3 ever sets.
                    t.priorityPlane = key.priorityPlane ? key.priorityPlane : geo.priorityPlane;
                    out.lists.transparent.push_back(t);
                }
            };

            if (geo.surfaceCount == 0) {
                // The sole authority. Asked per geoset per frame, through the
                // interface, so a non-WC3 model answers with its own rule.
                const core::SurfaceClass sc = classifier->Classify(view, geo);
                if (sc.visible)
                    emit(sc, core::SurfaceKey{});
                continue;
            }

            // A submesh with N batches: each is its own material, its own
            // blend class, and its own draw.
            const auto& surfaces = mi->render.surfaces;
            const u32 end =
                (std::min)(geo.surfaceBegin + geo.surfaceCount, static_cast<u32>(surfaces.size()));
            for (u32 s = geo.surfaceBegin; s < end; ++s) {
                // The model's own table index, not the position in `surfaces` —
                // the same value `Draw` reads back off `item.key.surface`.
                const core::SurfaceClass sc =
                    classifier->ClassifySurface(view, geo, surfaces[s].surface);
                if (sc.visible)
                    emit(sc, surfaces[s]);
            }
        }
    }

    // stable_sort, not sort: items tying on every comparator key would
    // otherwise resolve in an unspecified order seeded by the input order.
    // With the handle sort above, the input order is now well-defined, so
    // stability is what carries that guarantee through to submit order.
    std::stable_sort(out.lists.opaque.begin(), out.lists.opaque.end(), OpaqueOrder);
    std::stable_sort(out.lists.transparent.begin(), out.lists.transparent.end(), TransparentOrder);
    return out;
}

gfx::BufferHandle PickSlot0Vb(const GPUGeoset& geo, i32 coordId) {
    // Slot 0 is always a complete interleaved vertex; coordId only chooses
    // which of the two copies (see core::StreamId).
    if (coordId == 1 && geo.Stream(core::StreamId::BaseUv1) != gfx::BufferHandle::Invalid)
        return geo.Stream(core::StreamId::BaseUv1);
    return geo.Stream(core::StreamId::Base);
}

bool BindSdMeshGeometry(gfx::IGFXCommandList* cmd, const GPUGeoset& geo,
                        gfx::BufferHandle paletteCb, i32 coordId) {

    cmd->BindVertexBuffer(0, PickSlot0Vb(geo, coordId), sizeof(Vertex));
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);

    const bool hasBones =
        (geo.boneVb != gfx::BufferHandle::Invalid) && (paletteCb != gfx::BufferHandle::Invalid);
    if (hasBones) {
        cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
    }
    return hasBones;
}

void BindLayerAlbedo(gfx::IGFXCommandList* cmd, TextureAssetManager::ModelScope* scope,
                     i32 textureId, gfx::TextureHandle defaultTex, SamplerAssetManager& samplers,
                     u32 slot) {

    u32 wrapFlags = kSamplerWrapBitsMask;
    bool hasTex = false;
    if (textureId >= 0 && scope) {
        const gfx::TextureHandle h = scope->Get(textureId);
        if (h != gfx::TextureHandle::Invalid) {
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, h);
            wrapFlags = scope->WrapFlags(textureId);
            hasTex = true;
        }
    }
    if (!hasTex)
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, defaultTex);
    cmd->BindSampler(gfx::ShaderStage::Pixel, slot, samplers.WrapVariant(wrapFlags));
}

void WriteCbPerFrame(gfx::IGFXDevice* gfx, gfx::BufferHandle cb, const CbPerFrameDesc& d) {
    if (!gfx || cb == gfx::BufferHandle::Invalid)
        return;
    auto* p = static_cast<CBPerFrame*>(gfx->MapBuffer(cb));
    if (!p)
        return;
    p->world = d.world.transpose();
    p->view = d.view.transpose();
    p->projection = d.projection.transpose();
    p->lightDir = d.lightDir;
    p->lightColor = d.lightColor;
    p->ambientColor = d.ambientColor;
    p->extraParams = d.extraParams;
    p->texAnimParams = d.texAnimParams;
    p->materialFlags = d.materialFlags;
    gfx->UnmapBuffer(cb);
}

Vector4f NormalizedLightDir4(const Vector4f& dir) {
    const f32 n = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (n <= 1e-6f)
        return {0.0f, 0.0f, 0.0f, 0.0f};
    return {dir.x / n, dir.y / n, dir.z / n, 0.0f};
}

void ApplyTexAnimPaletteToFrame(bls::FrameInputs& frame,
                                const std::vector<RenderModel::TexAnimPaletteEntry>* palette,
                                i32 textureAnimationId) {
    if (palette && textureAnimationId >= 0 &&
        textureAnimationId < static_cast<i32>(palette->size())) {
        const auto& e = (*palette)[textureAnimationId];
        frame.texMtx0.rows[0] = {e.row0[0], e.row0[1], e.row0[2], e.row0[3]};
        frame.texMtx0.rows[1] = {e.row1[0], e.row1[1], e.row1[2], e.row1[3]};
    } else {
        frame.texMtx0 = bls::IdentityTexMtx();
    }
    frame.texMtx1 = bls::IdentityTexMtx();
}

} // namespace whiteout::flakes::renderer::render_detail
