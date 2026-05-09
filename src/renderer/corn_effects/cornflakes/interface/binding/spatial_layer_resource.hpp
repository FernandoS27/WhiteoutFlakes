#pragma once

/// @file
/// @brief Bound spatial layer resource — proximity-hash backing for closestF3/allocate/insert.

#include <cornflakes/interface/core/types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief One named payload field carried alongside a spatial-layer entry.
struct SpatialLayerPayload {
    std::string_view name;
    u32 payloadType = 0;
    u32 payloadFlags = 0;
};

/// @brief Bound spatial-hash backing layer (e.g. for proximity queries / collision-style lookups).
struct SpatialLayerResource {
    std::string_view name;

    std::string_view fullName;
    f32 cellSize = 0.75F;
    u32 flags = 1U;
    std::span<const SpatialLayerPayload> payloads;
};

const SpatialLayerResource* findSpatialLayerByName(std::span<const SpatialLayerResource> layers,
                                                   std::string_view name) noexcept;

} // namespace whiteout::cornflakes
