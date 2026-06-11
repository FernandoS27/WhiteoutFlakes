#pragma once

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/render/render_view.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>

namespace whiteout::cornflakes {

struct RibbonVertex {
    Float3 position;
    Float4 color;
    f32 u;
    f32 v;
};

struct RibbonGeometryOutput {
    std::span<const RibbonVertex> vertices;
};

RibbonGeometryOutput buildRibbonGeometry(const RenderPacket& packet, const ViewParams& view,
                                         IArena& arena);

}
