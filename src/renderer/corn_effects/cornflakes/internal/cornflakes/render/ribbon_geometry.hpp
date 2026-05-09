#pragma once

/// @file
/// @brief CPU expansion of ribbon `RenderPacket`s into triangle strip vertices.

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/render_view.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>

namespace whiteout::cornflakes {

/// @brief One emitted ribbon vertex.
struct RibbonVertex {
    Float3 position;
    Float4 color;
    f32 u;
    f32 v;
};

/// @brief Output of one ribbon expansion — vertices live in the supplied arena.
struct RibbonGeometryOutput {
    std::span<const RibbonVertex> vertices;
};

/// @brief Expand one ribbon packet into a triangle strip in `arena`.
RibbonGeometryOutput buildRibbonGeometry(const RenderPacket& packet, const ViewParams& view,
                                         IArena& arena);

} // namespace whiteout::cornflakes
