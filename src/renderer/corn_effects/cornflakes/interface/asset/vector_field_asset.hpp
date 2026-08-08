#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace whiteout::cornflakes {

inline constexpr u8 kPkvfFileVersion = 1U;

inline constexpr std::size_t kPkvfHeaderSize = 60U;

enum class VectorFieldDataType : u32 {
    Fp32 = 0,
    Fp16 = 1,
    U8SN = 2,
};

inline constexpr u8 kCoordinateFrameRightHandZUp = 1U;

struct VectorFieldAsset {
    std::array<u32, 4> dimensions{};
    std::array<f32, 4> boundsMin{};
    std::array<f32, 4> boundsMax{};
    f32 intensityMultiplier = 1.0F;
    VectorFieldDataType dataType = VectorFieldDataType::Fp32;
    u8 coordinateFrame = kCoordinateFrameRightHandZUp;
    std::span<const std::byte> data;
};

std::optional<VectorFieldAsset> parsePkvf(std::span<const std::byte> bytes) noexcept;

u32 vectorFieldScalarSize(VectorFieldDataType type) noexcept;

}
