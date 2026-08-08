#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>

namespace whiteout::cornflakes {

class SemanticSlotReader {
public:
    std::span<const Float3> readPosition(const RenderPacket& p) const noexcept;

    std::span<const f32> readSize(const RenderPacket& p) const noexcept;

    std::span<const Float3> readScale(const RenderPacket& p) const noexcept;
    std::span<const u8> readEnabled(const RenderPacket& p) const noexcept;
    std::span<const Quat> readOrientation(const RenderPacket& p) const noexcept;
    std::span<const Float3> readAxis(const RenderPacket& p) const noexcept;
    std::span<const Float3> readNormalAxis(const RenderPacket& p) const noexcept;
    std::span<const f32> readRotation(const RenderPacket& p) const noexcept;
};

}
