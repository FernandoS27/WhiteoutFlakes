#include <cornflakes/render/instance_stream.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

const f32* slotFloats(const RenderPacket& packet, RenderSlot slot, std::size_t componentCount,
                      std::size_t particleCount) noexcept {
    const auto& bytes = packet.slots[static_cast<std::size_t>(slot)];
    const std::size_t needed = particleCount * componentCount * sizeof(f32);
    if (bytes.size() < needed) {
        return nullptr;
    }
    return reinterpret_cast<const f32*>(bytes.data());
}

void copyVec3(f32 dst[3], const f32* src, std::size_t laneStride, std::size_t particle) noexcept {
    if (src == nullptr) {
        return;
    }
    const std::size_t base = particle * laneStride;
    dst[0] = src[base + 0];
    dst[1] = src[base + 1];
    dst[2] = src[base + 2];
}

void copyVec4(f32 dst[4], const f32* src, std::size_t laneStride, std::size_t particle) noexcept {
    if (src == nullptr) {
        return;
    }
    const std::size_t base = particle * laneStride;
    dst[0] = src[base + 0];
    dst[1] = src[base + 1];
    dst[2] = src[base + 2];
    dst[3] = src[base + 3];
}

} // namespace

std::span<const std::byte> packCornEffectsInstanceStream(const RenderPacket& packet, IArena& arena) {
    const std::size_t particles = packet.particleCount;
    if (particles == 0U) {
        return {};
    }
    const auto buf = arenaArray<CornEffectsInstance>(arena, particles);

    for (std::size_t i = 0; i < particles; ++i) {
        buf[i] = CornEffectsInstance{};
    }

    const f32* posF = slotFloats(packet, RenderSlot::Position, 3U, particles);
    const f32* axisF = slotFloats(packet, RenderSlot::Axis0, 3U, particles);
    const f32* normalAxisF = slotFloats(packet, RenderSlot::Axis1, 3U, particles);

    const auto& sizeBytes = packet.slots[static_cast<std::size_t>(RenderSlot::Size)];
    const std::size_t sizeStride =
        (sizeBytes.size() == particles * sizeof(f32) * 3U)
            ? 3U
            : (sizeBytes.size() == particles * sizeof(f32) * 1U ? 1U : 0U);
    const f32* sizeF = (sizeStride > 0) ? reinterpret_cast<const f32*>(sizeBytes.data()) : nullptr;

    const f32* orientationF = slotFloats(packet, RenderSlot::Orientation, 4U, particles);

    for (std::size_t i = 0; i < particles; ++i) {
        auto& inst = buf[i];

        copyVec3(&inst.pivot[0], posF, 3U, i);

        copyVec3(&inst.modeSlot4[0], axisF, 3U, i);

        copyVec3(&inst.modeSlot5[0], normalAxisF, 3U, i);

        if (sizeStride == 1U && sizeF != nullptr) {
            inst.vertColor[3] = sizeF[i];
        } else if (sizeStride == 3U && sizeF != nullptr) {
            const std::size_t base = i * 3U;
            inst.vertColor[0] = sizeF[base + 0];
            inst.vertColor[1] = sizeF[base + 1];
            inst.vertColor[2] = sizeF[base + 2];
        }

        copyVec4(&inst.tangent[0], orientationF, 4U, i);
    }

    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(buf.data()),
                                      particles * sizeof(CornEffectsInstance)};
}

} // namespace whiteout::cornflakes
