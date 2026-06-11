#pragma once

#include <cornflakes/interface/render/render_packet.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

struct RendererClassBucket {
    RendererClass cls = RendererClass::Billboard;
    std::vector<const RenderPacket*> packets;
};

using RendererClassBuckets = std::array<RendererClassBucket, 4>;

RendererClassBuckets bucketByClass(std::span<const RenderPacket> packets);

}
