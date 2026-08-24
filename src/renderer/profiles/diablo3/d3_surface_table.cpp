#include "renderer/profiles/diablo3/d3_surface_table.h"

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"

#include <algorithm>
#include <map>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace {

using ::whiteout::flakes::io::D3SubObjectRef;
using ::whiteout::flakes::io::D3TextureRef;

// MaterialColors::dwMaterialFlags. Only the two bits v1 acts on are named; the
// rest travel to the shader raw. Read off the material's own flag word rather
// than off a RenderPass, which we do not have — see the header.
enum : u32 {
    kMatFlagAlphaBlend = 0x1u,
    kMatFlagAlphaTest = 0x2u,
    kMatFlagTwoSided = 0x4u,
};

// EMaterialTextureType -> the slot we consume it in.
//
// The RE recovered this enum without a single surviving string, because every
// branch of Render_ResolveMaterialTextureStages names itself through the CORE
// ASSET it falls back to: type 2 -> default_lightmap, type 3 (and 47..52) ->
// flat_NM, type 8 -> irradiance, 21 -> vignette, 24 -> shadow_mask, 56 ->
// dye_ramp, 59 -> Banner_Dye_Master, 7/9/39/60/61 -> engine render targets,
// 20/22/23/53 computed per draw, and **default -> the entry's own texture**.
//
// Measured over 300 corpus `.app` (49,695 texture entries), which is what
// decides how much of that we can act on:
//
//   * `dwTextureType == 0` is the default branch, and it is the only
//     per-material one: for Barbarian_Male, material `oneBatch_mat` names
//     texture 95384 and `Skeleton_mat` names 48525 through it. The mode is
//     exactly one such entry per variant (2944 variants), with 2-6 common.
//   * Types 2, 3 and 8 arrive almost entirely as a **model-wide shared block**
//     at `dwSlotIndex` 25..38 — byte-identical across every material in a file
//     (14 entries, same texture ids). That is the core-asset fallback list
//     written into the file, not a per-material layer: binding it would put one
//     normal map on every material of every model.
//
// So v1 consumes the default branch and nothing else, and the surface-table
// test prints the (slot, type) census so the day someone recovers what the
// 25..38 block actually is, the change is a table edit rather than an
// excavation. An unconsumed slot falls back to the type's documented default,
// which is what the shader's `SlotOn` false already means.
D3SlotKind SlotOfType(i32 type) {
    // Which *ordinal* within the default branch is diffuse, specular or
    // emissive is the one thing the RE does not state — the enum's names are
    // gone and the fallback trick only names the special types — so v1 takes
    // the first and leaves the rest unresolved rather than inventing an order.
    // Guessing wrong here does not degrade gracefully: a diffuse map bound into
    // the normal slot makes every surface face the viewer.
    return ::whiteout::flakes::io::D3EntryOwnsTexture(type) ? D3SlotKind::Diffuse
                                                            : D3SlotKind::Count;
}

i32 TextureIdOf(std::span<const D3TextureRef> textures, i32 sno) {
    for (usize i = 0; i < textures.size(); ++i) {
        if (textures[i].snoId == sno)
            return static_cast<i32>(i);
    }
    return -1;
}

Matrix44f UvMatrixOf(const d3n::MaterialTextureEntry& e) {
    Matrix44f m = Matrix44f::identity();
    const Vector4f rows[4] = {e.vUvRow0, e.vUvRow1, e.vUvRow2, e.vUvRow3};
    for (int r = 0; r < 4; ++r) {
        m.data[r][0] = rows[r].x;
        m.data[r][1] = rows[r].y;
        m.data[r][2] = rows[r].z;
        m.data[r][3] = rows[r].w;
    }
    return m;
}

// An all-zero UV block is what an entry with no authored matrix carries, and
// uploading it would collapse every texture coordinate to the origin.
bool UvMatrixIsAuthored(const d3n::MaterialTextureEntry& e) {
    const Vector4f rows[4] = {e.vUvRow0, e.vUvRow1, e.vUvRow2, e.vUvRow3};
    for (const Vector4f& v : rows) {
        if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f || v.w != 0.0f)
            return true;
    }
    return false;
}

} // namespace

std::unique_ptr<D3SurfaceTable>
BuildD3SurfaceTable(const d3n::Appearances& app, u32 lookIndex,
                    std::span<const D3TextureRef> textures, std::span<const D3SubObjectRef> emitted,
                    ::whiteout::flakes::io::D3SnoCache* cache, D3TypeCensus* census) {
    auto table = std::make_unique<D3SurfaceTable>();
    auto& surfaces = table->Surfaces();
    surfaces.resize(emitted.size());

    std::map<i32, usize> typeCounts;
    usize unmatched = 0;
    usize pastCap = 0;

    for (usize g = 0; g < emitted.size(); ++g) {
        const d3n::GeoSet& set = (emitted[g].geoSet == 0) ? app.tGeoSet0 : app.tGeoSet1;
        if (emitted[g].index >= set.arSubObjects.size())
            continue;
        const d3n::SubObject& sub = set.arSubObjects[emitted[g].index];
        D3Surface& s = surfaces[g];
        s.rigid = sub.arVertexInfluences.empty();

        const d3n::SubObjectAppearance* variant = ::whiteout::flakes::io::D3VariantFor(app, sub, lookIndex);
        if (!variant) {
            // A name that finds no material is content, not a bug: the surface
            // stays invalid and the actor keeps the unlit fallback for it.
            ++unmatched;
            continue;
        }

        std::shared_ptr<const d3n::Material> keepAlive;
        const d3n::UberMaterial* mat = ::whiteout::flakes::io::D3MaterialOf(*variant, cache, keepAlive);
        if (!mat)
            continue;

        const auto& colors = mat->tColors;
        s.diffuse = colors.vDiffuse;
        s.specular = colors.vSpecular;
        s.emissive = colors.vEmissive;
        s.ambient = colors.vAmbient;
        // Raw, with no invented default: 99.4% of shipped materials leave this
        // at 0, and substituting a plausible-looking 20 there fabricated a
        // broad highlight on every surface. The shader clamps to 1 and only
        // reads this at all once a specular map resolves.
        s.shininess = colors.flShininess;
        s.materialFlags = static_cast<u32>(colors.dwMaterialFlags);
        s.alphaBlend = (s.materialFlags & kMatFlagAlphaBlend) != 0;
        s.twoSided = (s.materialFlags & kMatFlagTwoSided) != 0;
        s.alphaTestThreshold = (s.materialFlags & kMatFlagAlphaTest) != 0 ? (1.0f / 255.0f) : 0.0f;
        // No embedded translucent material means the original had no
        // translucent Shaders id either, and skipped the sub-object outright
        // when it faded. §6.3.
        s.noTranslucentVariant = !s.alphaBlend;

        // Every entry, not the first twelve. The matTex0..11 ceiling bounds
        // the engine's texture *matrices*, and a shipped material routinely
        // carries more entries than that (12,551 of 49,695 sampled entries sit
        // past twelve) because most of them are the shared core-asset block
        // this build does not consume. Counted, so the day the block is
        // understood the number is already in front of whoever reads it.
        if (mat->arTextures.size() > kD3MaxTextureStages)
            pastCap += mat->arTextures.size() - kD3MaxTextureStages;

        for (const d3n::MaterialTextureEntry& entry : mat->arTextures) {
            ++typeCounts[entry.dwTextureType];

            const D3SlotKind kind = SlotOfType(entry.dwTextureType);
            if (kind == D3SlotKind::Count)
                continue;

            D3Slot& slot = s.slots[static_cast<u32>(kind)];
            if (slot.textureId >= 0)
                continue; // first entry of a kind wins
            slot.rawType = entry.dwTextureType;
            slot.textureId = TextureIdOf(textures, entry.snoTexture.id);
            // UV set 0. `dwTextureFlags` was the obvious candidate for a set
            // selector and measurement says it is not one: it reads 0x1 on
            // every per-material entry and 0x2 on every shared-block entry, so
            // it separates the two families rather than choosing a UV set.
            // Reading bit 0 as "set 1" would put every D3 diffuse on the wrong
            // coordinates, and D3 authors both sets on every vertex, so nothing
            // about the geometry would say so.
            slot.uvSource = 0;
            slot.wrapFlags = 0x3;
            if (UvMatrixIsAuthored(entry))
                slot.uvTransform = UvMatrixOf(entry);
            if (slot.textureId >= 0)
                s.valid = true;
        }
    }

    if (census) {
        census->counts.assign(typeCounts.begin(), typeCounts.end());
        census->unmatchedMaterials = unmatched;
        census->entriesPastStageCap = pastCap;
    }
    return table;
}

core::SurfaceClass D3ClassifySurface(const D3Surface& surface) {
    core::SurfaceClass c;
    c.visible = surface.valid;
    if (surface.alphaBlend)
        c.blend = core::BlendClass::Transparent;
    else if (surface.alphaTestThreshold > 0.0f)
        c.blend = core::BlendClass::AlphaKey;
    else
        c.blend = core::BlendClass::Opaque;
    return c;
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
