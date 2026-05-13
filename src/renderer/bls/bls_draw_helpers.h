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

    Vector3f dirToSourceVS = {0.0f, 0.0f, 1.0f};
};

i32 BuildLightPalette(FrameInputs& frame,
                      const std::vector<model::FrameState::LightState>& activeLights,
                      const Matrix44f& viewMatrix, const BaselineLights& baseline,
                      LightingMode mode);

RenderState MakeSdMeshRenderState(const MatParams& mat, i32 activeLights, bool unlit,
                                  bool hasBones = false);

PsoRequest MakePsoRequest(const BlsProgram* program, VertexLayoutKind layout, const MatParams& mat,
                          PermuteIndices perm, bool lhClipSpace = false);

} // namespace whiteout::flakes::renderer::bls
