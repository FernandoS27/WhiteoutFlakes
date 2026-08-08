#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

enum class RendererClass : u32 {
    Billboard = 0,
    Ribbon = 1,
    Mesh = 2,
    Light = 3,
    Count,
};

enum class BlendMode : u8 {
    Add = 0,
    NoAlphaAdd = 1,
    Blend = 2,
    BlendAdd = 3,
    Opaque = 4,
    AlphaKey = 5,
    Count,
};

enum class BillboardMode : u8 {
    ScreenAligned = 0,
    ViewposAligned = 1,
    AxisAlignedQuad = 2,
    AxisAlignedSpheroid = 3,
    AxisAlignedCapsule = 4,
    PlaneAligned = 5,
};

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
    TextureU = 11,
    Cursor = 12,
    Count = 13,
};

inline constexpr std::size_t kRenderSlotCount = static_cast<std::size_t>(RenderSlot::Count);

struct RenderPacket {
    EmitterId emitter;
    LayerId layer;
    u32 rendererIndex = 0;
    RendererClass cls = RendererClass::Billboard;
    u32 particleCount = 0;

    u8 blendMode = static_cast<u8>(BlendMode::Opaque);

    u8 billboardingMode = static_cast<u8>(BillboardMode::ScreenAligned);

    std::array<std::span<const std::byte>, kRenderSlotCount> slots;
};

}
