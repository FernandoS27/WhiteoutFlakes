#pragma once

// Channel-typed 1x1 stub pixels used while an async texture load is in
// flight. Picking the right stub per channel matters more than per-shape —
// a white "stub" in the ORM slot would falsely report full metallic, a
// white normal would push lighting into the screen plane, etc.

#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::assets {

enum class TextureChannelKind : u8 {
    Diffuse,    // (255, 255, 255, 255) — neutral white albedo
    Orm,        // (255, 255,   0,   0) — occlusion=1, roughness=1, metal=0
    Normal,     // (127, 127, 255,   0) — flat tangent-space normal
    Emissive,   // (  0,   0,   0,   0) — no emission
};

inline std::array<u8, 4> StubPixelRGBA(TextureChannelKind kind) {
    switch (kind) {
    case TextureChannelKind::Orm:
        return {255, 255, 0, 0};
    case TextureChannelKind::Normal:
        return {127, 127, 255, 0};
    case TextureChannelKind::Emissive:
        return {0, 0, 0, 0};
    case TextureChannelKind::Diffuse:
    default:
        return {255, 255, 255, 255};
    }
}

} // namespace whiteout::flakes::renderer::assets
