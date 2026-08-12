#include "renderer/debug/draw_trace_hooks.h"

namespace whiteout::flakes::renderer::debug {

u32 TraceRootOrdinal(const TraceActorMap& actors, u32 handle) {
    auto it = actors.find(handle);
    if (it == actors.end() || !it->second)
        return 0;
    const model::Actor* a = it->second.get();
    for (i32 guard = 0; a->IsChild() && guard <= model::kMaxChildModelDepth + 1; ++guard) {
        auto p = actors.find(a->parent);
        if (p == actors.end() || !p->second)
            break;
        a = p->second.get();
    }
    u32 rank = 0;
    for (const auto& [h, other] : actors) {
        if (other && !other->IsChild() && h < a->handle)
            ++rank;
    }
    return rank;
}

u64 HashLightPalette(const bls::FrameInputs& f, i32 lightCount) {
    const i32 n = (lightCount < 0) ? 0 : (lightCount > bls::kMaxLights ? bls::kMaxLights : lightCount);
    u64 h = TraceHashBytes(&n, sizeof(n));
    if (n > 0) {
        h = TraceHashBytes(f.lights, sizeof(bls::ShaderLight) * (usize)n, h);
        h = TraceHashBytes(f.lightAmbientColors, sizeof(Vector3f) * (usize)n, h);
    }
    return h;
}

u64 HashTexMtx(const bls::FrameInputs& f) {
    u64 h = TraceHashBytes(&f.texMtx0, sizeof(f.texMtx0));
    return TraceHashBytes(&f.texMtx1, sizeof(f.texMtx1), h);
}

void RecordGeosetDraw(TraceDraw& d, const render_detail::RenderableView& view,
                      const model::GPUGeoset& geo, const render_detail::UnpackedLayer& layer,
                      i32 layerIndex, const bls::FrameInputs& frame) {
    auto& rec = DrawTraceRecorder::Instance();
    const TraceSubmitContext& ctx = rec.Context();

    d.passSlot = static_cast<u8>(ctx.pass);
    d.producer = static_cast<u8>(TraceProducer::Geoset);
    d.sortOrder = ctx.sortOrder;
    d.sqDist = ctx.sqDist;
    d.priorityPlane = ctx.priorityPlane;
    d.underWater = ctx.underWater;
    // The item's depth-fill mode, unless the caller already named a more
    // specific one — the HD fading-opaque prepass twin is a Depth draw of an
    // item whose mode is Color, and the two must not tie.
    if (d.depthFill == 0)
        d.depthFill = ctx.depthFill;

    d.actor.rootActor = view.rootActor;
    d.actor.role = view.actorRole;
    d.actor.treeDepth = view.actorDepth;
    d.actor.emitterId = view.spawnEmitterId;
    d.actor.slotIndex = view.spawnSlotIndex;

    // Geoset index within the actor, which is what OpaqueOrder tie-breaks on
    // and what `submesh` will name after the P-series rename.
    d.submesh = view.geosets ? static_cast<i32>(&geo - view.geosets->data()) : -1;
    d.surface = geo.materialId;
    d.layer = layerIndex;
    d.lod = geo.lod;
    d.indexCount = geo.indexCount;
    d.vertexCount = geo.vertexCount;

    d.filterMode = layer.filterMode;
    d.matFlags = layer.flags;
    d.texAnimId = layer.textureAnimationId;

    d.lightPaletteHash = HashLightPalette(frame, d.lightCount);
    d.texMtxHash = HashTexMtx(frame);

    rec.Record(d);
}

void RecordProducerDraw(TraceDraw& d) {
    auto& rec = DrawTraceRecorder::Instance();
    const TraceSubmitContext& ctx = rec.Context();
    d.passSlot = static_cast<u8>(ctx.pass);
    d.producer = static_cast<u8>(ctx.producer);
    d.sortOrder = ctx.sortOrder;
    d.sqDist = ctx.sqDist;
    d.priorityPlane = ctx.priorityPlane;
    d.underWater = ctx.underWater;
    rec.Record(d);
}

} // namespace whiteout::flakes::renderer::debug
