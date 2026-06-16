#pragma once

// ResolveLayerMaterial — pure transform mirroring WC3's
// `SelectModelMaterial` (Engine/Source/Model/ModelRender.cpp). It is the single
// source of truth for how a layer's material params are adjusted per draw pass:
// the plain colour draw (None), the fading-opaque promotion (Color, HD only),
// and the depth‑only prepass (Depth, HD only). No GPU state, no side effects —
// unit‑testable in isolation.

#include "bls_mat_params.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::bls {

// Mirrors WC3 `DepthFillType`. SD geosets always draw with None; HD fading
// geosets use Color and additionally enqueue a Depth twin for the prepass.
enum class DepthFill : u8 { None = 0, Color = 1, Depth = 2 };

// `color` is {geosetColor.rgb, combinedAlpha}. `isOpaqueFading` only matters for
// the Color path (HD). Returns the params to submit; `base` is left untouched.
MatParams ResolveLayerMaterial(DepthFill depthFill, const MatParams& base, bool isOpaqueFading,
                               const Vector4f& color);

} // namespace whiteout::flakes::renderer::bls
