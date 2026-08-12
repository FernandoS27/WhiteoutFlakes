#pragma once

// Bridge between the submission paths and draw_trace's POD schema. Kept
// separate from draw_trace.{h,cpp} so that file stays free of bls / gfx /
// render_detail dependencies and the comparator can be unit-tested device-free.

#include "renderer/bls/bls_frame.h"
#include "renderer/debug/draw_trace.h"
#include "renderer/render_detail.h"

namespace whiteout::flakes::renderer::debug {

using TraceActorMap = std::unordered_map<u32, std::unique_ptr<model::Actor>>;

// Rank of `handle`'s top-level ancestor among the scene's top-level actors, in
// handle order. See TraceActorRef::rootActor for why a rank and not a handle.
// Linear in the actor count and only ever reached with tracing on.
u32 TraceRootOrdinal(const TraceActorMap& actors, u32 handle);

// FNV-1a over the *resolved* palette — frame.lights[0..n) plus the parallel
// ambient colours — rather than over a synthesised light ordering. Slot 0 is
// the DNC sun baseline, so a change that moves the day-night sample relative
// to pass execution alters ambient and diffuse with zero ordering change; only
// hashing the values catches it.
u64 HashLightPalette(const bls::FrameInputs& f, i32 lightCount);

// The two texture-animation matrices, which ApplyTexAnimPaletteToFrame writes
// per layer. Separate from cbHash so a texture-animation regression is named
// as one rather than surfacing as "the whole constant buffer moved".
u64 HashTexMtx(const bls::FrameInputs& f);

// Fill the fields every geoset draw shares and push the record. `d` arrives
// carrying what only the caller knows: shading model, blend class, PSO
// decision inputs, stream mask, palette path, CB hash, combined alpha,
// resolved texture ids, light count.
void RecordGeosetDraw(TraceDraw& d, const render_detail::RenderableView& view,
                      const model::GPUGeoset& geo, const render_detail::UnpackedLayer& layer,
                      i32 layerIndex, const bls::FrameInputs& frame);

// Non-geoset producers: particles, ribbons and corn are not IShadingModel
// draws, so they record identity by owning actor handle plus a unit index.
// The hooks stay for the life of the refactor — collapsing them into the
// geoset hook would delete the record of three of the four producers and of
// their interleaved ordering, and the loss would be invisible because the
// baseline would simply be re-recorded smaller.
void RecordProducerDraw(TraceDraw& d);

} // namespace whiteout::flakes::renderer::debug
