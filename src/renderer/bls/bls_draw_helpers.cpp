#include "bls_draw_helpers.h"
#include "coordinate_system.h"

#include <cmath>

namespace WhiteoutDex::bls {

i32 BuildLightPalette(FrameInputs&                                     frame,
                      const std::vector<FrameState::LightState>&       activeLights,
                      const Matrix44f&                                 viewMatrix,
                      const BaselineLights&                            baseline,
                      LightingMode                                     mode) {
    i32 count = 0;

    bool anyEnabled = false;
    for (const auto& L : activeLights) {
        if (L.enabled) { anyEnabled = true; break; }
    }
    bool addBaseline = false;
    switch (mode) {
        case LightingMode::Dynamic: addBaseline = !anyEnabled; break;
        case LightingMode::InGame:  addBaseline = true;        break;
        case LightingMode::Glue:    addBaseline = false;       break;
    }
    if (addBaseline) {
        ShaderLight& sl = frame.lights[count++];
        sl.ambient  = { baseline.ambient.x,       baseline.ambient.y,       baseline.ambient.z,       0.0f };
        sl.diffuse  = { baseline.diffuse.x,       baseline.diffuse.y,       baseline.diffuse.z,       0.0f };
        sl.position = { baseline.dirToSourceVS.x, baseline.dirToSourceVS.y, baseline.dirToSourceVS.z, 0.0f };
    }

    for (const auto& L : activeLights) {
        if (!L.enabled) continue;
        if (count >= kMaxLights) break;
        ShaderLight& sl = frame.lights[count++];
        sl.ambient = { L.ambient.x, L.ambient.y, L.ambient.z, 0.0f };
        sl.diffuse = { L.diffuse.x, L.diffuse.y, L.diffuse.z, 0.0f };
        if (L.kind == FrameState::LightKind::Directional) {

            Vector3f d = L.worldDir;
            f32      n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (n > 1e-6f) { d.x /= n; d.y /= n; d.z /= n; }
            const Vector3f lv = whiteout::transform_normal(Vector3f{-d.x, -d.y, -d.z}, viewMatrix);
            sl.position = { lv.x, lv.y, lv.z, 0.0f };
        } else {

            const Vector3f p = whiteout::transform_point(L.worldPos, viewMatrix);
            sl.position = { p.x, p.y, p.z, 1.0f };
        }
    }

    for (i32 i = count; i < kMaxLights; ++i) frame.lights[i] = {};
    return count;
}

RenderState MakeSdMeshRenderState(const MatParams& mat,
                                  i32              activeLights,
                                  bool             unlit,
                                  bool             hasBones) {
    RenderState rs;
    rs.shaderId        = GxShaderID::SD;
    rs.alphaMode       = static_cast<u8>(mat.alpha);
    rs.numColors       = 1;
    rs.numTexCoords    = 1;

    rs.numWeights      = static_cast<u8>(hasBones ? 4 : 0);
    rs.numLights       = static_cast<u8>(activeLights);
    rs.fogEnabled      = false;
    rs.depthWrite      = mat.DepthWriteEnabled();
    rs.lightingEnabled = !unlit && activeLights > 0;
    return rs;
}

PsoRequest MakePsoRequest(const BlsProgram* program,
                          VertexLayoutKind  layout,
                          const MatParams&  mat,
                          PermuteIndices    perm,
                          bool              lhClipSpace) {
    PsoRequest req{};
    req.program     = program;
    req.vsIndex     = perm.vs;
    req.psIndex     = perm.ps;
    req.material    = mat;
    req.layout      = layout;
    req.lhClipSpace = lhClipSpace;
    return req;
}

}
