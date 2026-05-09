#pragma once

/// @file
/// @brief Per-layer compiled artefacts: the four VM scopes plus renderer/sampler resources.

#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/binding/spatial_layer_resource.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief One VM program (one scope of one layer) — bytecode, decoded form, and resolved bindings.
struct VMProgramDescriptor {

    std::span<const u8> cbemBytecode;

    std::span<const CBEMInstruction> decodedInstructions;

    std::span<const std::byte> constantsPool;

    std::span<const ExternalBinding> externals;

    std::span<const FunctionBinding> functions;

    std::array<u32, 5> registerCounts{}; ///< Register count per scope bucket (0..3 used; 4 reserved).
    u32 entryOffset = 0;
};

/// @brief One renderer entry on a layer: shader-perm flags, atlas/blend config, texture paths.
struct LayerRenderer {
    RendererClass cls = RendererClass::Billboard;

    bool hasUV = false;
    bool isBillboard = false;
    bool isAtlas = false;
    bool hasRandom = false;
    bool hasVC = false;
    bool hasNT = false;
    bool writeGBuffer = false;
    bool hasSoftParticles = false;
    bool hasAlphaLut = false;
    bool isLit = false;

    bool isDistortion = false;

    bool hasGeometryBillboard = false;
    bool hasGeometryRibbon = false;
    bool hasDiffuse = false;
    bool hasDiffuseRamp = false;
    bool hasEmissive = false;
    bool hasNormalBend = false;
    bool hasNormalWrap = false;
    bool hasLegacyLit = false;
    bool hasFlipUVs = false;
    bool hasTransformUVs = false;
    bool hasTextureUVs = false;
    bool hasTextureRepeat = false;
    bool hasCustomTextureU = false;
    bool hasCorrectDeformation = false;
    bool hasEnableSize2D = false;
    bool isRenderingEnabled = true;
    bool hasTransparent = false;

    u32 billboardingMode = 0;

    u32 transparentSortMode = 0;

    u32 atlasBlending = 0;
    u32 atlasDefinition = 0;
    u32 atlasSource = 0;
    f32 atlasDistortionStrength = 0.0F;
    std::string_view atlasMotionVectorsMapPath{};

    f32 softParticlesDistance = 0.0F;

    std::string_view alphaRemapMapPath{};

    bool textureFlipU = false;
    bool textureFlipV = false;
    bool textureRotateTexture = false;

    std::string_view diffuseTexturePath{};

    u16 atlasSubDivX = 0;
    u16 atlasSubDivY = 0;

    u8 blendMode = static_cast<u8>(BlendMode::Opaque);
};

/// @brief Asset-side scope tag values that map to the four `LayerProgram` scope programs.
enum class BlobScope : u8 {
    Init = 0,
    Physics = 3,
    TimeFixed = 4,
    TimeVarying = 5,
};

/// @brief Asset-declared default value for an attribute external.
struct AttributeDefault {
    std::string_view name;
    std::array<f32, 4> defaultValue{};
};

/// @brief Maps an external symbol to the global event slot id used by the event router.
struct EventExternalBinding {
    std::string_view externalName;
    u32 globalEventSlotId = 0;
};

/// @brief Compiled layer: four scope programs, renderers, samplers, attribute defaults.
struct LayerProgram {
    LayerId id;
    std::string_view name;
    std::string_view sourceUid;

    VMProgramDescriptor program;

    VMProgramDescriptor initProgram;
    VMProgramDescriptor physicsProgram;
    VMProgramDescriptor timeFixedProgram;
    VMProgramDescriptor timeVaryingProgram;
    std::span<const LayerRenderer> renderers;

    std::span<const SamplerResource> samplers;

    std::span<const SpatialLayerResource> spatialLayers;

    std::span<const AttributeDefault> attributeDefaults;

    std::span<const EventExternalBinding> eventExternals;
};

/// @brief Order in which a name lookup walks the four scope programs of a layer.
///
/// Init runs once at spawn; physics every tick; timeFixed/timeVarying replace
/// physics for the explicit-time scopes. Several runtime systems (lifeRatio
/// snapshot, attribute overrides, event externals) need to find a binding in
/// whichever scope declares it.
inline std::array<const VMProgramDescriptor*, 4> layerScopePrograms(const LayerProgram& layer) noexcept {
    return {&layer.initProgram, &layer.physicsProgram, &layer.timeFixedProgram,
            &layer.timeVaryingProgram};
}

/// @brief Externals view over the four scope programs in the same order as `layerScopePrograms`.
inline std::array<std::span<const ExternalBinding>, 4>
layerScopeExternals(const LayerProgram& layer) noexcept {
    return {layer.initProgram.externals, layer.physicsProgram.externals,
            layer.timeFixedProgram.externals, layer.timeVaryingProgram.externals};
}

/// @brief First binding named `name` across init/physics/timeFixed/timeVarying, or null.
inline const ExternalBinding* findBindingAcrossScopes(const LayerProgram& layer,
                                                     std::string_view name) noexcept {
    for (const auto& s : layerScopeExternals(layer)) {
        if (auto* hit = findBindingByName(s, name)) {
            return hit;
        }
    }
    return nullptr;
}

} // namespace whiteout::cornflakes
