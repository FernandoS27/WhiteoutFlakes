#pragma once

/// @file
/// @brief 128-byte per-instance vertex layout matching CornFx's GPU expansion shaders.

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

/// @brief Packed 128-byte instance attributes consumed by the corn_effects VS.
struct CornEffectsInstance {

    f32 normal[3]{0.0F, 0.0F, 1.0F};
    f32 _pad0 = 0.0F;

    f32 vertColor[4]{1.0F, 1.0F, 1.0F, 1.0F};

    f32 uv0[2]{0.0F, 0.0F};
    f32 _pad1[2]{0.0F, 0.0F};

    f32 modeSlot4[4]{1.0F, 0.0F, 0.0F, 0.0F};

    f32 modeSlot5[4]{0.0F, 1.0F, 0.0F, 0.0F};

    f32 random = 0.0F;
    f32 _pad2[3]{0.0F, 0.0F, 0.0F};

    f32 tangent[4]{1.0F, 0.0F, 0.0F, 1.0F};

    f32 pivot[4]{0.0F, 0.0F, 0.0F, 1.0F};
};

static_assert(sizeof(CornEffectsInstance) == 128, "CornEffectsInstance layout drift");

/// @brief Vertex-attribute description: location index + component count + byte offset.
struct CornEffectsAttribDesc {
    u32 location;
    u32 components;
    std::size_t byteOffset;
};

/// @brief Static attribute layout matching the corn_effects shaders' input declarations.
inline constexpr std::array<CornEffectsAttribDesc, 8> kCornEffectsAttribTable{{
    {1, 3, offsetof(CornEffectsInstance, normal)},
    {2, 4, offsetof(CornEffectsInstance, vertColor)},
    {3, 2, offsetof(CornEffectsInstance, uv0)},
    {4, 4, offsetof(CornEffectsInstance, modeSlot4)},
    {5, 4, offsetof(CornEffectsInstance, modeSlot5)},
    {6, 1, offsetof(CornEffectsInstance, random)},
    {7, 4, offsetof(CornEffectsInstance, tangent)},
    {8, 4, offsetof(CornEffectsInstance, pivot)},
}};

/// @brief Pack `packet`'s slot-of-arrays into a tightly-strided `CornEffectsInstance[]` in `arena`.
std::span<const std::byte> packCornEffectsInstanceStream(const RenderPacket& packet, IArena& arena);

} // namespace whiteout::cornflakes
