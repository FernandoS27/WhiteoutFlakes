#pragma once

/// @file
/// @brief `RenderPacket` — one renderer's worth of per-particle data ready for a backend.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

/// @brief Top-level renderer family selector.
enum class RendererClass : u32 {
    Billboard = 0,
    Ribbon = 1,
    Mesh = 2,
    Light = 3,
    Count,
};

/// @brief CornFx `Transparent.Type` mapped 1:1 from the asset.
enum class BlendMode : u8 {
    Add = 0,
    NoAlphaAdd = 1,
    Blend = 2,
    BlendAdd = 3,
    Opaque = 4,
    AlphaKey = 5,
    Count,
};

/// @brief Billboard alignment mode — drives the GPU/CPU expansion path.
enum class BillboardMode : u8 {
    ScreenAligned = 0,
    ViewposAligned = 1,
    AxisAlignedQuad = 2,
    AxisAlignedSpheroid = 3,
    AxisAlignedCapsule = 4,
    PlaneAligned = 5,
};

/// @brief Per-renderer input slot; index into `RenderPacket::slots`.
enum class RenderSlot : u32 {
    Position = 0,
    Size = 1,
    Enabled = 2,
    Orientation = 3,
    Axis0 = 4,
    Axis1 = 5,
    Rotation = 6,
    Color = 7,
    TextureID = 8,

    SelfID = 9,
    ParentID = 10,
    Count = 11,
};

inline constexpr std::size_t kRenderSlotCount = static_cast<std::size_t>(RenderSlot::Count);

/// @brief One renderer's output for one tick — typed slot-of-arrays handed to a backend.
///
/// Each slot is a span of raw bytes whose element layout is determined by
/// `RenderSlot` semantics (Position is `Float3[]`, Size is `f32[]`, etc.).
/// Empty spans mean "the layer has no binding for this slot" — backends should
/// fall back to defaults rather than treating it as an error.
struct RenderPacket {
    EmitterId emitter;
    LayerId layer;
    RendererClass cls = RendererClass::Billboard;
    u32 particleCount = 0;

    u8 blendMode = static_cast<u8>(BlendMode::Opaque);

    u8 billboardingMode = static_cast<u8>(BillboardMode::ScreenAligned);

    std::array<std::span<const std::byte>, kRenderSlotCount> slots;
};

} // namespace whiteout::cornflakes
