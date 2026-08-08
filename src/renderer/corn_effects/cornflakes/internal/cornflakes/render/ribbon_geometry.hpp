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
    Float4 uvScaleBias;
    Float4 uvFactors;
    f32 cursor;
    Float3 normal;
    Float4 tangent;
};

struct RibbonGeometryOutput {
    std::span<const RibbonVertex> vertices;
};

struct RibbonUVConfig {
    bool customTextureU = false;
    bool flipU = false;
    bool flipV = false;
    bool rotate = false;
    u16 atlasSubDivX = 0;
    u16 atlasSubDivY = 0;
    bool correctDeformation = false;
    bool needsNormals = false;
    f32 normalBendingFactor = 0.0F;
};

RibbonGeometryOutput buildRibbonGeometry(const RenderPacket& packet, const ViewParams& view,
                                         const RibbonUVConfig& uv, IArena& arena);

}
