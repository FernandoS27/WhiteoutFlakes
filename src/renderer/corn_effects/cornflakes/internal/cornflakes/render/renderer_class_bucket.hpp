#pragma once

/// @file
/// @brief Group packets by `RendererClass` so backends can submit billboards/ribbons/meshes/lights separately.

#include <cornflakes/interface/render/render_packet.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

/// @brief All packets that share a `RendererClass`, kept as pointers into the input span.
struct RendererClassBucket {
    RendererClass cls = RendererClass::Billboard;
    std::vector<const RenderPacket*> packets;
};

using RendererClassBuckets = std::array<RendererClassBucket, 4>;

/// @brief Partition `packets` into one bucket per `RendererClass` (Billboard/Ribbon/Mesh/Light).
RendererClassBuckets bucketByClass(std::span<const RenderPacket> packets);

} // namespace whiteout::cornflakes
