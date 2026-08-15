#include "m2_surface_table.h"

#include <whiteout/models/m2/m2.h>

#include <algorithm>

namespace whiteout::flakes::renderer::profiles::wow {
namespace {

namespace wm2 = ::whiteout::m2;

// A combo lookup that answers -1 rather than reading off the end. Malformed
// base+stride pairs do occur in shipped data, and one is a missing texture, not
// a crash.
i32 ComboAt(const std::vector<u16>& combos, u32 base, u32 unit) {
    const u64 i = static_cast<u64>(base) + unit;
    return i < combos.size() ? static_cast<i32>(combos[i]) : -1;
}

// The first value of a track's first sequence, which is the whole of it for a
// non-animated one and the bind pose for the rest.
template <class T>
const T* FirstValue(const wm2::AnimationTrack<T>& track) {
    if (track.values.empty() || track.values[0].empty())
        return nullptr;
    return &track.values[0][0];
}

// True when the track carries exactly one value and it is zero. This is the
// client's "do not render" gate, and it outranks the blend mode — see
// OrgrimmarSmokeEmitter.m2, an Opaque batch that draws nothing.
bool IsZeroConstantWeight(const wm2::AnimationTrack<i16>& track) {
    if (track.values.size() != 1 || track.values[0].size() != 1)
        return false;
    return track.values[0][0] == 0;
}

// fixed16: 0 transparent, 0x7FFF opaque.
f32 WeightToAlpha(i16 v) {
    return static_cast<f32>(v) / 32767.0f;
}

} // namespace

std::unique_ptr<M2SurfaceTable> BuildM2SurfaceTable(const wm2::Model& model, usize profileIndex) {
    auto table = std::make_unique<M2SurfaceTable>();
    if (profileIndex >= model.skinProfiles.size())
        return table;

    const auto& skin = model.skinProfiles[profileIndex];
    const bool useCombinerCombos =
        hasFlag(model.globalFlags.value, wm2::GlobalFlag::UseTextureCombinerCombos);

    auto& out = table->Surfaces();
    out.reserve(skin.batches.size());

    for (const auto& batch : skin.batches) {
        M2Surface s;
        s.shaderId = batch.shaderId;
        s.textureCount =
            static_cast<u8>(std::clamp<u32>(batch.textureCount, 1u, kM2MaxTextureUnits));
        s.priorityPlane = batch.priorityPlane;
        s.materialLayer = batch.materialLayer & 7u; // InitElement packs it as (layer << 4) & 0x70
        s.skinSectionIndex = batch.skinSectionIndex;

        // The material. Global flag 0x08 redirects the *second* texture's
        // material through textureCombinerCombos[shaderId] instead of
        // materialIndex + 1; resolve it now, because CM2Shared::GetEffect does
        // not and a batch that reached the draw path unresolved picks the wrong
        // blend mode.
        u32 materialIndex = batch.materialIndex;
        if (useCombinerCombos && batch.textureCount > 1 &&
            batch.shaderId < model.textureCombinerCombos.size()) {
            materialIndex = model.textureCombinerCombos[batch.shaderId];
        }
        if (materialIndex < model.materials.size()) {
            const auto& mat = model.materials[materialIndex];
            s.blend = M2BlendFromRaw(mat.blendingMode);
            s.materialFlags = mat.flags;
        }

        for (u32 u = 0; u < s.textureCount; ++u) {
            const i32 tex = ComboAt(model.textureCombos, batch.textureComboIndex, u);
            s.textureId[u] = (tex >= 0 && static_cast<usize>(tex) < model.textures.size()) ? tex : -1;

            // -1 in this table means environment mapping — but a *miss* also
            // returns -1, and the table is empty on every modern model (the
            // wiki marks it unused from Cataclysm; Alexstrasza ships zero
            // entries). Conflating the two would claim every batch is
            // env-mapped. The vertex shader's name is the real authority on
            // where a unit's UVs come from, so a miss falls back to UV set 0.
            const i32 coord = ComboAt(model.textureCoordCombos, batch.textureCoordComboIndex, u);
            s.texCoord[u] = static_cast<i8>(
                (coord < 0 && model.textureCoordCombos.empty()) ? 0 : coord);

            const i32 xf =
                ComboAt(model.textureTransformCombos, batch.textureTransformComboIndex, u);
            s.transformId[u] =
                (xf >= 0 && static_cast<usize>(xf) < model.textureTransforms.size()) ? xf : -1;

            const i32 w = ComboAt(model.textureWeightCombos, batch.textureWeightComboIndex, u);
            s.weightId[u] =
                (w >= 0 && static_cast<usize>(w) < model.textureWeights.size()) ? w : -1;

            if (s.weightId[u] >= 0) {
                const auto& track = model.textureWeights[s.weightId[u]].weight;
                if (IsZeroConstantWeight(track))
                    s.suppressed = true;
                if (const i16* v = FirstValue(track))
                    s.unitWeights[u] = WeightToAlpha(*v);
            }
        }

        // Element alpha: the colour track's alpha times the *first* unit's
        // weight, whatever the texture count — the client uses only weight[0]
        // for whole-element alpha and leaves the rest to the per-unit float4.
        f32 elementAlpha = s.unitWeights[0];
        s.colorIndex = batch.colorIndex;
        if (s.colorIndex >= 0 && static_cast<usize>(s.colorIndex) < model.colors.size()) {
            const auto& c = model.colors[s.colorIndex];
            if (const Vector3f* rgb = FirstValue(c.color))
                s.elementColor = *rgb;
            if (const i16* a = FirstValue(c.alpha))
                elementAlpha *= WeightToAlpha(*a);
        }
        s.elementAlpha = std::clamp(elementAlpha, 0.0f, 1.0f);

        s.pixelShader = M2PixelShaderFor(s.textureCount, s.shaderId);
        s.vertexShader = M2VertexShaderFor(s.textureCount, s.shaderId);

        out.push_back(s);
    }

    return table;
}

core::SurfaceClass M2ClassifySurface(const M2Surface& surface, f32 modelAlpha, f32 elementAlpha) {
    // A constant-zero weight track means "do not draw" and outranks everything,
    // blend mode included.
    if (surface.suppressed)
        return {.visible = false};

    // BlendAdd is exempt from the alpha cull: it is premultiplied, so it still
    // adds light at zero alpha, and the client draws it on a fully faded model.
    if (surface.blend != M2Blend::BlendAdd &&
        (modelAlpha < kM2CullModelAlpha || elementAlpha <= 0.0f))
        return {.visible = false};

    core::BlendClass blend = core::BlendClass::Transparent;
    if (modelAlpha >= kM2OpaqueModelAlpha) {
        if (surface.blend == M2Blend::Opaque)
            blend = core::BlendClass::Opaque;
        else if (surface.blend == M2Blend::AlphaKey)
            blend = core::BlendClass::AlphaKey;
    }

    // A transparent batch that still writes depth is drawn twice: once with the
    // blend forced opaque to lay depth down, then blended against it. Batches
    // that opt out of depth writing get the single blended draw.
    const bool twin = blend == core::BlendClass::Transparent &&
                      (surface.materialFlags & kM2NoDepthWrite) == 0;

    return {.visible = true,
            .blend = blend,
            .needsDepthFill = twin,
            .needsDepthTwin = twin};
}

} // namespace whiteout::flakes::renderer::profiles::wow
