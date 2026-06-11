#include "binding_internal.hpp"

#include <cornflakes/interface/binding/effect_binder.hpp>

#include <cstring>
#include <unordered_map>

namespace whiteout::cornflakes {

namespace {

u32 readPropU32(const AssetObject& p) noexcept {
    const auto bytes = fieldBytes(p, "PropertyValueNumeric");
    if (bytes.size() < sizeof(u32)) {
        return 0;
    }
    u32 v = 0;
    std::memcpy(&v, bytes.data(), sizeof(u32));
    return v;
}

f32 readPropF32(const AssetObject& p) noexcept {
    const auto bytes = fieldBytes(p, "PropertyValueNumeric");
    if (bytes.size() < sizeof(f32)) {
        return 0.0F;
    }
    f32 v = 0.0F;
    std::memcpy(&v, bytes.data(), sizeof(f32));
    return v;
}

bool readPropToggle(const AssetObject& p) noexcept {
    return readPropU32(p) != 0U;
}

constexpr RendererClass validateRendererClass(i32 raw) noexcept {
    const auto v = static_cast<u32>(raw);
    return v < static_cast<u32>(RendererClass::Count) ? static_cast<RendererClass>(v)
                                                      : RendererClass::Billboard;
}

struct RendererBuildState {
    LayerRenderer& out;
    bool transparentEnabled = false;
    bool opaqueEnabled = false;
    u32 transparentType = 0;
    u32 opaqueType = 0;
    bool sawEnableRendering = false;
};

using ToggleHandler = void (*)(RendererBuildState&, bool);

const std::unordered_map<std::string_view, ToggleHandler>& toggleHandlers() {
    static const std::unordered_map<std::string_view, ToggleHandler> kTable = {
        {"Lit",                [](RendererBuildState& s, bool on) { if (on) { s.out.isLit = true; s.out.hasNT = true; } }},
        {"LegacyLit",          [](RendererBuildState& s, bool on) { s.out.hasLegacyLit = on; }},
        {"SoftParticles",      [](RendererBuildState& s, bool on) { if (on) { s.out.hasSoftParticles = true; } }},
        {"AlphaRemap",         [](RendererBuildState& s, bool on) { if (on) { s.out.hasAlphaLut = true; s.out.hasRandom = true; } }},
        {"Atlas",              [](RendererBuildState& s, bool on) { if (on) { s.out.isAtlas = true; } }},
        {"Distortion",         [](RendererBuildState& s, bool on) { if (on) { s.out.isDistortion = true; } }},
        {"GeometryBillboard",  [](RendererBuildState& s, bool on) { s.out.hasGeometryBillboard = on; }},
        {"GeometryRibbon",     [](RendererBuildState& s, bool on) { s.out.hasGeometryRibbon = on; }},
        {"Diffuse",            [](RendererBuildState& s, bool on) { s.out.hasDiffuse = on; }},
        {"DiffuseRamp",        [](RendererBuildState& s, bool on) { s.out.hasDiffuseRamp = on; }},
        {"Emissive",           [](RendererBuildState& s, bool on) { s.out.hasEmissive = on; }},
        {"NormalBend",         [](RendererBuildState& s, bool on) { s.out.hasNormalBend = on; }},
        {"NormalWrap",         [](RendererBuildState& s, bool on) { s.out.hasNormalWrap = on; }},
        {"FlipUVs",            [](RendererBuildState& s, bool on) { s.out.hasFlipUVs = on; }},
        {"TransformUVs",       [](RendererBuildState& s, bool on) { s.out.hasTransformUVs = on; }},
        {"TextureUVs",         [](RendererBuildState& s, bool on) { s.out.hasTextureUVs = on; }},
        {"TextureRepeat",      [](RendererBuildState& s, bool on) { s.out.hasTextureRepeat = on; }},
        {"CustomTextureU",     [](RendererBuildState& s, bool on) { s.out.hasCustomTextureU = on; }},
        {"CorrectDeformation", [](RendererBuildState& s, bool on) { s.out.hasCorrectDeformation = on; }},
        {"EnableSize2D",       [](RendererBuildState& s, bool on) { s.out.hasEnableSize2D = on; }},
        {"EnableRendering",    [](RendererBuildState& s, bool on) { s.out.isRenderingEnabled = on; s.sawEnableRendering = true; }},
        {"Transparent",        [](RendererBuildState& s, bool on) { s.transparentEnabled = on; s.out.hasTransparent = on; }},
    };
    return kTable;
}

void applyRendererToggle(RendererBuildState& s, std::string_view pname, bool toggleOn) {
    const auto& table = toggleHandlers();
    if (auto it = table.find(pname); it != table.end()) {
        it->second(s, toggleOn);
    }
}

using SubPropHandler = void (*)(RendererBuildState&, const AssetObject&, IArena&);

const std::unordered_map<std::string_view, SubPropHandler>& subPropHandlers() {
    static const std::unordered_map<std::string_view, SubPropHandler> kTable = {
        {"Diffuse.DiffuseMap", [](RendererBuildState& s, const AssetObject& p, IArena& a) {
             if (auto path = fieldString(p, "PropertyValueStr"); !path.empty()) {
                 s.out.diffuseTexturePath = stableCopy(path, a);
             }
         }},
        {"Atlas.SubDiv", [](RendererBuildState& s, const AssetObject& p, IArena&) {

             const auto bytes = fieldBytes(p, "PropertyValueNumeric");
             if (bytes.size() >= 2U * sizeof(u32)) {
                 u32 subX = 0;
                 u32 subY = 0;
                 std::memcpy(&subX, bytes.data(), sizeof(u32));
                 std::memcpy(&subY, bytes.data() + sizeof(u32), sizeof(u32));
                 s.out.atlasSubDivX = static_cast<u16>(subX);
                 s.out.atlasSubDivY = static_cast<u16>(subY);
             }
         }},
        {"Atlas.Blending",            [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.atlasBlending = readPropU32(p); }},
        {"Atlas.Definition",          [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.atlasDefinition = readPropU32(p); }},
        {"Atlas.Source",              [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.atlasSource = readPropU32(p); }},
        {"Atlas.DistortionStrength",  [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.atlasDistortionStrength = readPropF32(p); }},
        {"Atlas.MotionVectorsMap", [](RendererBuildState& s, const AssetObject& p, IArena& a) {
             if (auto path = fieldString(p, "PropertyValueStr"); !path.empty()) {
                 s.out.atlasMotionVectorsMapPath = stableCopy(path, a);
             }
         }},
        {"AlphaRemap.AlphaMap", [](RendererBuildState& s, const AssetObject& p, IArena& a) {
             if (auto path = fieldString(p, "PropertyValueStr"); !path.empty()) {
                 s.out.alphaRemapMapPath = stableCopy(path, a);
             }
         }},
        {"SoftParticles.SoftnessDistance", [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.softParticlesDistance = readPropF32(p); }},
        {"TextureUVs.FlipU",               [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.textureFlipU = readPropToggle(p); }},
        {"TextureUVs.FlipV",               [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.textureFlipV = readPropToggle(p); }},
        {"TextureUVs.RotateTexture",       [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.textureRotateTexture = readPropToggle(p); }},
        {"BillboardingMode",               [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.billboardingMode = readPropU32(p); }},
        {"Transparent.SortMode",           [](RendererBuildState& s, const AssetObject& p, IArena&) { s.out.transparentSortMode = readPropU32(p); }},
        {"Transparent.Type",               [](RendererBuildState& s, const AssetObject& p, IArena&) { s.transparentType = readPropU32(p); }},
        {"Opaque",                         [](RendererBuildState& s, const AssetObject& p, IArena&) { s.opaqueEnabled = readPropToggle(p); }},
        {"Opaque.Type",                    [](RendererBuildState& s, const AssetObject& p, IArena&) { s.opaqueType = readPropU32(p); }},
    };
    return kTable;
}

void applyRendererSubProperty(RendererBuildState& s, const AssetObject& pObj,
                              std::string_view pname, IArena& arena) {
    const auto& table = subPropHandlers();
    if (auto it = table.find(pname); it != table.end()) {
        it->second(s, pObj, arena);
    }
}

BlendMode resolveBlendMode(const RendererBuildState& s) noexcept {
    if (s.transparentEnabled) {
        if (s.transparentType <= static_cast<u32>(BlendMode::BlendAdd)) {
            return static_cast<BlendMode>(s.transparentType);
        }
        return BlendMode::Opaque;
    }
    if (s.opaqueEnabled) {
        return s.opaqueType == 1U ? BlendMode::AlphaKey : BlendMode::Opaque;
    }
    return BlendMode::Opaque;
}

LayerRenderer buildRenderer(const EffectAssetModel& model, const AssetObject& rObj, IArena& arena) {
    LayerRenderer out;
    out.cls = validateRendererClass(
        fieldInt(rObj, "RendererClass").value_or(static_cast<i32>(RendererClass::Billboard)));
    out.isBillboard = (out.cls == RendererClass::Billboard);

    RendererBuildState s{out};

    for (const u32 pUid : fieldLinks(rObj, "Properties")) {
        const AssetObject* pObj = findObjectByUid(model, pUid);
        if (pObj == nullptr || pObj->type != "CLayerCompileCacheRendererProperty") {
            continue;
        }
        const auto pname = fieldString(*pObj, "PropertyName");
        const bool toggleOn = readPropToggle(*pObj);

        applyRendererToggle(s, pname, toggleOn);
        applyRendererSubProperty(s, *pObj, pname, arena);
    }

    const auto streamUids = fieldLinks(rObj, "Streams");
    if (!streamUids.empty()) {
        const auto inputs = arenaArray<RendererParticleInput>(arena, streamUids.size());
        std::size_t n = 0;
        for (const u32 sUid : streamUids) {
            const AssetObject* sObj = findObjectByUid(model, sUid);
            if (sObj == nullptr || sObj->type != "CLayerCompileCacheRendererParticleInput") {
                continue;
            }
            RendererParticleInput in;
            in.semantic = fieldUint(*sObj, "Semantic").value_or(0U);
            in.indexInStorage = fieldUint(*sObj, "IndexInStorage").value_or(0U);
            in.additionalFieldName = stableCopy(fieldString(*sObj, "AdditionalFieldName"), arena);
            inputs[n++] = in;
        }
        if (n > 0) {
            out.particleInputs = std::span<const RendererParticleInput>{inputs.data(), n};
        }
    }

    if (!s.sawEnableRendering) {
        out.isRenderingEnabled = true;
    }
    out.blendMode = static_cast<u8>(resolveBlendMode(s));
    if (out.cls == RendererClass::Billboard) {
        out.hasVC = true;
    }
    if (!out.diffuseTexturePath.empty()) {
        out.hasUV = true;
    }
    return out;
}

}

void loadRenderers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                   IArena& arena) {

    const auto fieldUids = fieldLinks(layerCache, "Fields");
    if (!fieldUids.empty()) {
        const auto names = arenaArray<std::string_view>(arena, fieldUids.size());
        for (std::size_t i = 0; i < fieldUids.size(); ++i) {
            const AssetObject* fObj = findObjectByUid(model, fieldUids[i]);
            const std::string_view nm =
                (fObj != nullptr) ? fieldString(*fObj, "FieldName") : std::string_view{};
            names[i] = stableCopy(nm, arena);
        }
        lp.renderFieldNames = std::span<const std::string_view>{names.data(), fieldUids.size()};
    }

    const auto rendererUids = fieldLinks(layerCache, "Renderers");
    if (rendererUids.empty()) {
        return;
    }
    const auto rArr = arenaArray<LayerRenderer>(arena, rendererUids.size());
    std::size_t written = 0;
    for (const u32 rUid : rendererUids) {
        const AssetObject* rObj = findObjectByUid(model, rUid);
        if (rObj == nullptr || rObj->type != "CLayerCompileCacheRenderer") {
            continue;
        }
        rArr[written++] = buildRenderer(model, *rObj, arena);
    }
    if (written > 0) {
        lp.renderers = std::span<const LayerRenderer>{rArr.data(), written};
    }
}

}
