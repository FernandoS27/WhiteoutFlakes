#pragma once

#include <cornflakes/interface/binding/event_payload_decl.hpp>
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

struct VMProgramDescriptor {

    std::span<const u8> cbemBytecode;

    std::span<const CBEMInstruction> decodedInstructions;

    std::span<const std::byte> constantsPool;

    std::span<const ExternalBinding> externals;

    std::span<const FunctionBinding> functions;

    std::array<u32, 5> registerCounts{};
    u32 entryOffset = 0;
};

struct RendererParticleInput {
    u32 semantic = 0;
    u32 indexInStorage = 0;
    std::string_view additionalFieldName;
};

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
    std::string_view atlasDefinitionPath{};
    u32 atlasSource = 0;
    f32 atlasDistortionStrength = 0.0F;
    std::string_view atlasMotionVectorsMapPath{};

    f32 softParticlesDistance = 0.0F;

    f32 normalBendingFactor = 0.0F;

    std::string_view alphaRemapMapPath{};

    bool textureFlipU = false;
    bool textureFlipV = false;
    bool textureRotateTexture = false;

    std::string_view diffuseTexturePath{};

    u16 atlasSubDivX = 0;
    u16 atlasSubDivY = 0;

    u8 blendMode = static_cast<u8>(BlendMode::Opaque);

    std::span<const RendererParticleInput> particleInputs{};
};

enum class BlobScope : u8 {
    Init = 0,
    Physics = 3,
    TimeFixed = 4,
    TimeVarying = 5,
};

struct AttributeDefault {
    std::string_view name;
    std::array<f32, 4> defaultValue{};
};

struct EventExternalBinding {
    std::string_view externalName;
    u32 globalEventSlotId = 0;
};

struct LayerProgram {
    LayerId id;
    std::string_view name;
    std::string_view sourceUid;

    VMProgramDescriptor program;

    VMProgramDescriptor initProgram;
    VMProgramDescriptor physicsProgram;
    VMProgramDescriptor timeFixedProgram;
    VMProgramDescriptor timeVaryingProgram;

    const VMProgramDescriptor& evolveProgram() const noexcept {
        if (!physicsProgram.cbemBytecode.empty()) {
            return physicsProgram;
        }
        if (!timeFixedProgram.cbemBytecode.empty()) {
            return timeFixedProgram;
        }
        return timeVaryingProgram;
    }

    std::span<const LayerRenderer> renderers;

    std::span<const std::string_view> renderFieldNames{};

    std::span<const SamplerResource> samplers;

    std::span<const SpatialLayerResource> spatialLayers;

    std::span<const AttributeDefault> attributeDefaults;

    std::span<const EventExternalBinding> eventExternals;

    std::span<const KickedEventPayloadDecl> kickedEventDecls;
    std::span<const EventPayloadElement> rootEventDecl;
};

inline std::array<const VMProgramDescriptor*, 4> layerScopePrograms(const LayerProgram& layer) noexcept {
    return {&layer.initProgram, &layer.physicsProgram, &layer.timeFixedProgram,
            &layer.timeVaryingProgram};
}

inline std::array<std::span<const ExternalBinding>, 4>
layerScopeExternals(const LayerProgram& layer) noexcept {
    return {layer.initProgram.externals, layer.physicsProgram.externals,
            layer.timeFixedProgram.externals, layer.timeVaryingProgram.externals};
}

inline const ExternalBinding* findBindingAcrossScopes(const LayerProgram& layer,
                                                     std::string_view name) noexcept {
    for (const auto& s : layerScopeExternals(layer)) {
        if (auto* hit = findBindingByName(s, name)) {
            return hit;
        }
    }
    return nullptr;
}

}
