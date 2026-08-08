#include "bls_draw_helpers.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::bls {

using namespace ::whiteout::flakes::renderer::model;

namespace {

// GxuLightSelect (Engine/Source/Gx/GxuLight.cpp) does not score against the
// shaded point directly. It snaps the selection point to a 1024-unit XY grid
// (Z is discarded — culling is 2D) and measures against an 896-unit apron
// centred on the cell, deliberately smaller than the cell itself. The visible
// consequence is that a geoset's light set changes in cell-sized steps rather
// than sliding continuously as it moves.
constexpr f32 kLightCellSize = 1024.0f;
constexpr f32 kLightApronSize = 896.0f;

// The same quadratic the shaders use for per-pixel falloff (`ps/hd.bls`,
// `ps/sd_on_hd.bls` and `vs/sd_highspec.bls` all inline `1/(1 + 5e-4*d^2)`), so
// a light is culled right about where its contribution stops being visible.
constexpr f32 kLightQuadraticAtten = 0.00050000002f;
constexpr f32 kMinLightFitness = 0.0015f;

// GxuLightSelect drops an omni light sitting exactly here: it is the position a
// CGxLight still holds when nothing ever called EnvSet(COORD_LIGHTPOSITION).
// We always populate worldPos from the light's node, so for us this only ever
// fires on a light that genuinely lands on (0,0,1) — kept anyway because the
// engine would drop that light too.
constexpr Vector3f kUnplacedOmniPos = {0.0f, 0.0f, 1.0f};

bool IsPositional(const FrameState::LightState& L) {
    // Only Omni is a point light. WC3's CreateLight_1 is
    // `if (type) EnvCreateLight() else EnvCreateOmniLight()`, so Directional
    // *and* Ambient both go through the directional path.
    return L.kind == FrameState::LightKind::Omni;
}

// 2D distance from `p` to the axis-aligned square centred on `c` with the given
// half-extent; 0 inside. Mirrors DistanceToRect in GxuLight.cpp.
f32 DistanceToCellRect(f32 cx, f32 cy, f32 halfExtent, f32 px, f32 py) {
    const f32 dx = std::max(std::abs(px - cx) - halfExtent, 0.0f);
    const f32 dy = std::max(std::abs(py - cy) - halfExtent, 0.0f);
    return std::sqrt(dx * dx + dy * dy);
}

struct ScoredLight {
    const FrameState::LightState* light = nullptr;
    f32 fitness = 0.0f;
    bool positional = false;
};

// CGxuLight::HasHigherPriority: a directional light outranks an omni light
// unconditionally, whatever their fitness; ties within a class go to the
// higher fitness.
bool HasHigherPriority(const ScoredLight& a, const ScoredLight& b) {
    if (a.positional != b.positional)
        return !a.positional;
    return a.fitness > b.fitness;
}

void WriteShaderLight(ShaderLight& sl, Vector3f& ambColorOut, const FrameState::LightState& L,
                      const Matrix44f& viewMatrix) {
    sl.ambient = {L.ambIntensity, L.ambIntensity, L.ambIntensity, 0.0f};
    sl.diffuse = {L.diffuse.x, L.diffuse.y, L.diffuse.z, 0.0f};
    ambColorOut = L.ambientColor;

    if (IsPositional(L)) {
        const Vector3f p = whiteout::transform_point(L.worldPos, viewMatrix);
        sl.position = {p.x, p.y, p.z, 1.0f};
        return;
    }
    Vector3f d = L.worldDir;
    const f32 n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (n > 1e-6f) {
        d.x /= n;
        d.y /= n;
        d.z /= n;
    }
    const Vector3f lv = whiteout::transform_normal(Vector3f{-d.x, -d.y, -d.z}, viewMatrix);
    sl.position = {lv.x, lv.y, lv.z, 0.0f};
}

} // namespace

i32 BuildLightPalette(FrameInputs& frame, const LightingContext& ctx, const Matrix44f& viewMatrix,
                      const Vector3f& geosetCentroidWS) {
    static const std::vector<FrameState::LightState> kNoLights;
    const auto& sceneLights = ctx.sceneLights ? *ctx.sceneLights : kNoLights;

    i32 count = 0;

    bool anyEnabled = false;
    for (const auto& L : sceneLights) {
        if (L.enabled) {
            anyEnabled = true;
            break;
        }
    }
    bool addBaseline = false;
    switch (ctx.mode) {
    case LightingMode::Dynamic:
        addBaseline = !anyEnabled;
        break;
    case LightingMode::InGame:
        addBaseline = true;
        break;
    case LightingMode::Glue:
        addBaseline = false;
        break;
    }
    if (addBaseline) {
        // The engine's world sun is a registry light like any other, but it is
        // directional so the priority sort would put it first regardless — and
        // ours is view-dependent (dirToSourceVS is already in view space), so
        // it stays special-cased at slot 0 instead of being scored.
        ShaderLight& sl = frame.lights[count];
        frame.lightAmbientColors[count] = ctx.baseline.ambientColor;
        ++count;
        sl.ambient = {ctx.baseline.ambient.x, ctx.baseline.ambient.y, ctx.baseline.ambient.z, 0.0f};
        sl.diffuse = {ctx.baseline.diffuse.x, ctx.baseline.diffuse.y, ctx.baseline.diffuse.z, 0.0f};
        sl.position = {ctx.baseline.dirToSourceVS.x, ctx.baseline.dirToSourceVS.y,
                       ctx.baseline.dirToSourceVS.z, 0.0f};
    }

    const f32 cellX =
        std::floor((geosetCentroidWS.x + kLightCellSize * 0.5f) / kLightCellSize) * kLightCellSize;
    const f32 cellY =
        std::floor((geosetCentroidWS.y + kLightCellSize * 0.5f) / kLightCellSize) * kLightCellSize;
    const f32 apronHalf = kLightApronSize * 0.5f;

    // Runs per geoset per frame, so keep the top-N in a fixed array and insert
    // in priority order rather than sorting the whole scene — the engine's
    // CPriorityQ is dequeued at most kMaxLights times for the same reason.
    // `count` is 0 or 1 (the baseline), so there is always at least one slot.
    const i32 slots = kMaxLights - count;
    ScoredLight best[kMaxLights];
    i32 nBest = 0;

    for (const auto& L : sceneLights) {
        if (!L.enabled)
            continue;

        const bool positional = IsPositional(L);
        f32 dist = 0.0f;
        f32 fitness = L.ambIntensity + L.dirIntensity;
        if (positional) {
            if (L.worldPos.x == kUnplacedOmniPos.x && L.worldPos.y == kUnplacedOmniPos.y &&
                L.worldPos.z == kUnplacedOmniPos.z)
                continue;
            dist = DistanceToCellRect(cellX, cellY, apronHalf, L.worldPos.x, L.worldPos.y);
            fitness /= 1.0f + dist * dist * kLightQuadraticAtten;
        }
        if (dist > 0.0f && fitness < kMinLightFitness)
            continue;
        if (fitness <= 0.0f)
            continue;

        const ScoredLight cand{&L, fitness, positional};
        if (nBest < slots)
            ++nBest;
        else if (!HasHigherPriority(cand, best[nBest - 1]))
            continue;

        i32 i = nBest - 1;
        for (; i > 0 && HasHigherPriority(cand, best[i - 1]); --i)
            best[i] = best[i - 1];
        best[i] = cand;
    }

    for (i32 i = 0; i < nBest; ++i) {
        WriteShaderLight(frame.lights[count], frame.lightAmbientColors[count], *best[i].light,
                         viewMatrix);
        ++count;
    }

    for (i32 i = count; i < kMaxLights; ++i) {
        frame.lights[i] = {};
        frame.lightAmbientColors[i] = {0.0f, 0.0f, 0.0f};
    }
    return count;
}

RenderState MakeSdMeshRenderState(const MatParams& mat, i32 activeLights, bool unlit,
                                  bool hasBones) {
    RenderState rs;
    rs.shaderId = GxShaderID::SD;
    rs.alphaMode = static_cast<u8>(mat.alpha);
    rs.numColors = 1;
    rs.numTexCoords = 1;

    rs.numWeights = static_cast<u8>(hasBones ? 4 : 0);
    rs.numLights = static_cast<u8>(activeLights);
    rs.fogEnabled = false;
    rs.depthWrite = mat.DepthWriteEnabled();
    rs.lightingEnabled = !unlit && activeLights > 0;
    return rs;
}

PsoRequest MakePsoRequest(const BlsProgram* program, VertexLayoutKind layout, const MatParams& mat,
                          PermuteIndices perm, bool lhClipSpace) {
    PsoRequest req{};
    req.program = program;
    req.vsIndex = perm.vs;
    req.psIndex = perm.ps;
    req.material = mat;
    req.layout = layout;
    req.lhClipSpace = lhClipSpace;
    return req;
}

} // namespace whiteout::flakes::renderer::bls
