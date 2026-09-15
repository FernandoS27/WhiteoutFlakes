#pragma once

// The clustered point-light set of the Warcraft III 3.0.0 HD family (PS t16-t18
// + cb1 rows 39.w-42) and the arithmetic behind it. The main light block is
// bls::MainLight.
//
// Device-free on purpose — the packing is what has to match the engine, and it
// is tested without a GPU (tests/wc3_lighting_frame_test.cpp). See
// WC3_30_LIGHTING_DESIGN.md §3.2-§3.4 for where each value comes from.

#include "bls/bls_cb_layout.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <vector>

namespace whiteout::flakes::renderer::profiles::wc3 {

using LightState = model::FrameState::LightState;

// A light colour the way the engine stores it: EnvSet truncates each channel to
// a byte, and the shader-side unpack divides by 255 again. There is no sRGB
// conversion anywhere on the light path.
Vector3f EngineLightColor(const Vector3f& rgb);

// GxuLight_AttenuationCullRadius: the distance at which
// `exp(-e d^2) / (1 + l d + q d^2)` falls to (1/255) / radiance. Closed form
// without the exponential term, otherwise eight clamped Newton steps on the log
// of the equation. 0 = the light never reaches 1/255; 100000 = no falloff.
f32 ClusterCullRadius(f32 radiance, f32 quadratic, f32 linear, f32 exponential);

// CGxLightToShaderLight for an omni light: colour x intensity, view-space
// position, the three falloff terms, and no shadow slot.
bls::HdClusterLight PackClusterLight(const LightState& light, const Matrix44f& view);

// The three structured buffers plus the grid they are indexed through.
struct ClusterSet {
    std::vector<bls::HdClusterLight> lights; // t16
    std::vector<u32> indices;                // t17, two u16 indices per u32
    std::vector<u32> tiles;                  // t18, listOffset << 10 | count
    u32 gridWidth = 1;
    u32 gridHeight = 1;
    u32 lightCount = 0; // real records in `lights` (an empty set still pads one)
};

// Every enabled omni light with a non-zero cull radius, in ONE tile covering the
// viewport. Shades the same as the engine's 16-px binning: a light only leaves
// a tile where its radiance is below 1/255, and the shader skips those itself.
// The per-tile cap (1023) is the only difference.
void BuildSingleTileClusterSet(std::span<const LightState> lights, const Matrix44f& view,
                               ClusterSet& out);

// GxuLight_BuildClusterGrid's tiling: ~16-px tiles, the long side capped at 400,
// every light listed in each tile its sphere of influence can reach on screen.
// The test is a conservative rectangle — the projection of the sphere's view-
// space bounding box, or the whole screen once the box crosses the near plane —
// where the engine projects an ellipse, so tiles hold a superset of the
// engine's lists and shade identically to the single tile. A tile whose list
// equals its left or upper neighbour's reuses that record, as the engine does.
// `projection` is the left-handed D3D projection the pass renders with.
//
// `shadowSlotPositions` are the world positions of this frame's point-shadow
// slots: a shadow-casting light at one of them records that slot, any other
// caster -2 ("caster without a slot"), and the rest -1.
void BuildBinnedClusterSet(std::span<const LightState> lights, const Matrix44f& view,
                           const Matrix44f& projection, u32 viewportWidth, u32 viewportHeight,
                           ClusterSet& out, std::span<const Vector3f> shadowSlotPositions = {});

// Grid dimensions BuildBinnedClusterSet uses for a viewport.
void ClusterGridDims(u32 viewportWidth, u32 viewportHeight, u32& gridWidth, u32& gridHeight);

// cb1 rows 39.w-42 for `set` over a viewport of the given pixel size.
void FillClusterConstants(const ClusterSet& set, f32 viewportWidth, f32 viewportHeight,
                          bls::HdPsClusteredCb& out);

} // namespace whiteout::flakes::renderer::profiles::wc3
