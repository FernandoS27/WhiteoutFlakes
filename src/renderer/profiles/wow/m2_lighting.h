#pragma once

// ============================================================================
// CM2Lighting — WoW's per-model light accumulator, device-free.
//
// The client builds one of these per model per frame (`CM2Model::SetupLighting`
// @ 0x100f6ab30): zero it, let the scene and the environment callback push
// lights in, then resolve. What comes out is one directional term plus at most
// three point lights, which is the entire budget an `.m2` batch ever sees.
//
// Verified against `CM2Lighting::AddDiffuse` @ 0x100f4fbc0, `AddLight` @
// 0x100f4fdb0, `SetupSunlight` @ 0x100f50190, `CShaderEffect::SetLocalLighting`
// @ 0x100e9a2e0 and `ComputeLocalLights` @ 0x100e9ac70 in a WoW 6.0.1 client.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::profiles::wow {

/// @brief `CM2Lighting`'s point-light budget — the array it insertion-sorts is
///        three deep (`AddLight`'s `count <= 2` guard).
inline constexpr u32 kM2MaxPointLights = 3;

/// @brief `CM2Light`'s attenuation, which is GL's `1/(k0 + k1·d + k2·d²)`:
///        `GLDevice::SetLight` @ 0x10180f450 forwards the three floats to
///        GL_CONSTANT / GL_LINEAR / GL_QUADRATIC_ATTENUATION.
///
/// These are constants, not file data. `CM2Model::AnimateMT` animates a light
/// block's colour and visibility tracks and never touches `attenuationStart` /
/// `attenuationEnd`, so every `.m2` point light in 6.0.1 falls back to what
/// `CM2Light::CM2Light` @ 0x100f4f4a0 left here.
inline constexpr Vector3f kM2Attenuation{0.0f, 0.7f, 0.03f};

/// @brief The portrait/character-select key light, from
///        `PortraitLightingCallback` @ 0x100c93320 — the client's own answer to
///        "light one model, no world around it", which is exactly a viewer.
///
/// The direction is the one the light *travels*: `GLDevice::SetLight` negates it
/// on the way to GL_POSITION, whose w=0 form points *toward* the source. Left
/// unnormalised here because the client leaves it so and `SetupSunlight`
/// normalises on resolve.
inline constexpr Vector3f kM2PortraitAmbient{0.45f, 0.45f, 0.45f};
inline constexpr Vector3f kM2PortraitDiffuse{1.0f, 1.0f, 1.0f};
inline constexpr Vector3f kM2PortraitDirection{-1.0f, 0.0f, -1.0f};

/// @brief One light offered to the accumulator: a runtime `CM2Light` reduced to
///        the fields `AddLight` reads.
struct M2LightInput {
    bool positional = false; ///< `CM2Light::type == 1`.
    bool visible = true;
    Vector3f positionWS{0.0f, 0.0f, 0.0f};
    Vector3f directionWS{0.0f, 0.0f, -1.0f}; ///< Direction of travel.
    Vector3f ambient{0.0f, 0.0f, 0.0f};
    Vector3f diffuse{0.0f, 0.0f, 0.0f};
    Vector3f attenuation = kM2Attenuation;
};

struct M2PointLight {
    Vector3f positionWS{0.0f, 0.0f, 0.0f};
    Vector3f diffuse{0.0f, 0.0f, 0.0f};
    Vector3f attenuation = kM2Attenuation;
};

/// @brief What reaches the shader, in the shape `SetLocalLighting` uploads it.
struct M2LightingResult {
    Vector3f ambient{0.0f, 0.0f, 0.0f};
    Vector3f diffuse{0.0f, 0.0f, 0.0f};
    Vector3f directionWS{0.0f, 0.0f, -1.0f}; ///< Normalised direction of travel.
    u32 pointCount = 0;
    M2PointLight points[kM2MaxPointLights];
};

/// @brief The accumulator. Construct with the model's world-space centre — the
///        point `AddLight` ranks point lights by, which is the sphere origin
///        `CM2Lighting::Initialize` is handed.
class M2Lighting {
public:
    explicit M2Lighting(const Vector3f& modelCentreWS) : centre_(modelCentreWS) {}

    void AddAmbient(const Vector3f& color);

    /// @brief The directional term. Note the assignment: `AddDiffuse` *sets*
    ///        colour and direction rather than accumulating them, so with two
    ///        directional lights only the last one is lit by. (It does also
    ///        accumulate a dir⊗colour basis, which no shader path reads.)
    void AddDiffuse(const Vector3f& color, const Vector3f& directionWS);

    /// @brief Route one light: invisible ones are dropped, positional ones
    ///        compete for the three nearest slots, the rest fold into the
    ///        ambient/diffuse/specular sums.
    void AddLight(const M2LightInput& light);

    M2LightingResult Resolve() const;

private:
    Vector3f centre_;
    Vector3f ambient_{0.0f, 0.0f, 0.0f};
    Vector3f diffuse_{0.0f, 0.0f, 0.0f};
    Vector3f direction_{0.0f, 0.0f, 0.0f};
    u32 pointCount_ = 0;
    M2PointLight points_[kM2MaxPointLights];
    f32 pointDistSq_[kM2MaxPointLights] = {};
};

} // namespace whiteout::flakes::renderer::profiles::wow
