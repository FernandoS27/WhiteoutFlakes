#pragma once

/// @file
/// @brief Helpers for initialising payload blobs and reading/writing typed fields into them.

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace whiteout::cornflakes {

/// @brief Allocates a zero-filled payload blob sized for `elementCount` elements.
class PayloadInitializer {
public:
    std::span<std::byte> initialize(IArena& arena, u32 elementCount) const;
};

/// @brief Bounds-checked typed writes into a payload blob.
class PayloadElementBuilder {
public:
    bool writeU32(std::span<std::byte> blob, u32 offset, u32 value, IssueBag& issues) const;
    bool writeF32(std::span<std::byte> blob, u32 offset, f32 value, IssueBag& issues) const;
    bool writeFloat3(std::span<std::byte> blob, u32 offset, Float3 value, IssueBag& issues) const;
};

/// @brief Bounds-checked typed reads from a payload blob.
class PayloadElementExtractor {
public:
    std::optional<u32> readU32(std::span<const std::byte> blob, u32 offset, IssueBag& issues) const;
    std::optional<f32> readF32(std::span<const std::byte> blob, u32 offset, IssueBag& issues) const;
    std::optional<Float3> readFloat3(std::span<const std::byte> blob, u32 offset,
                                     IssueBag& issues) const;
};

} // namespace whiteout::cornflakes
