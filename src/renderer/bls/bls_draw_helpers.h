#pragma once

#include "bls_frame.h"
#include "bls_permuter.h"
#include "bls_pso_builder.h"
#include "render_target.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::bls {

struct BaselineLights {
    Vector3f ambient = {0.0f, 0.0f, 0.0f};
    Vector3f diffuse = {1.0f, 1.0f, 1.0f};
    // KLBC colour of the sun light, for the SD-on-HD ambient compensation.
    Vector3f ambientColor = {0.0f, 0.0f, 0.0f};

    Vector3f dirToSourceVS = {0.0f, 0.0f, 1.0f};
};

// Everything the per-geoset palette build needs that stays constant for a pass.
// `sceneLights` is the whole scene's light list, not one actor's: WC3 keeps a
// single global registry (`s_lights` in Engine/Source/Gx/GxuLight.cpp) that any
// geoset in range draws from, so a torch on one model lights the model beside
// it. Our per-scene collection is also the context separation WC3 gets from
// EGxuLightContext — separate scenes never see each other's lights.
struct LightingContext {
    const std::vector<model::FrameState::LightState>* sceneLights = nullptr;
    BaselineLights baseline;
    LightingMode mode = LightingMode::InGame;
};

// Selects at most kMaxLights lights for one geoset and writes them into
// `frame.lights` / `frame.lightAmbientColors`, returning the count.
//
// `geosetCentroidWS` is the geoset's local centroid pushed through the actor's
// world transform — the same point WC3's RenderGeosetPrep hands to
// GxuLightSelect. Selection quantises it to a 1024-unit XY grid and ranks
// lights by fitness; see the constants in the .cpp.
i32 BuildLightPalette(FrameInputs& frame, const LightingContext& ctx, const Matrix44f& viewMatrix,
                      const Vector3f& geosetCentroidWS);

RenderState MakeSdMeshRenderState(const MatParams& mat, i32 activeLights, bool unlit,
                                  bool hasBones = false);

PsoRequest MakePsoRequest(const BlsProgram* program, VertexLayoutKind layout, const MatParams& mat,
                          PermuteIndices perm, bool lhClipSpace = false);

} // namespace whiteout::flakes::renderer::bls
