#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

struct SpatialLayerPayload {
    std::string_view name;
    u32 payloadType = 0;
    u32 payloadFlags = 0;
};

struct SpatialLayerResource {
    std::string_view name;

    std::string_view fullName;
    f32 cellSize = 0.75F;
    u32 flags = 1U;
    std::span<const SpatialLayerPayload> payloads;
};

const SpatialLayerResource* findSpatialLayerByName(std::span<const SpatialLayerResource> layers,
                                                   std::string_view name) noexcept;

}
