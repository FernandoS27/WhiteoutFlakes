#include <cornflakes/core/determinism.hpp>
#include <cornflakes/interface/render/semantic_slot_reader.hpp>

namespace whiteout::cornflakes {

namespace {

template <typename T>
std::span<const T> reinterpretSlot(const RenderPacket& p, RenderSlot slot) noexcept {
    const auto& bytes = p.slots[static_cast<std::size_t>(slot)];
    if (bytes.empty() || bytes.size() < static_cast<std::size_t>(p.particleCount) * sizeof(T)) {
        return {};
    }
    return std::span<const T>{reinterpret_cast<const T*>(bytes.data()), p.particleCount};
}

} // namespace

std::span<const Float3> SemanticSlotReader::readPosition(const RenderPacket& p) const noexcept {
    return reinterpretSlot<Float3>(p, RenderSlot::Position);
}

std::span<const f32> SemanticSlotReader::readSize(const RenderPacket& p) const noexcept {
    if (p.cls == RendererClass::Mesh) {
        return {};
    }
    return reinterpretSlot<f32>(p, RenderSlot::Size);
}

std::span<const Float3> SemanticSlotReader::readScale(const RenderPacket& p) const noexcept {
    if (p.cls != RendererClass::Mesh) {
        return {};
    }
    return reinterpretSlot<Float3>(p, RenderSlot::Size);
}

std::span<const u8> SemanticSlotReader::readEnabled(const RenderPacket& p) const noexcept {
    return reinterpretSlot<u8>(p, RenderSlot::Enabled);
}

std::span<const Quat> SemanticSlotReader::readOrientation(const RenderPacket& p) const noexcept {
    return reinterpretSlot<Quat>(p, RenderSlot::Orientation);
}

std::span<const Float3> SemanticSlotReader::readAxis(const RenderPacket& p) const noexcept {
    return reinterpretSlot<Float3>(p, RenderSlot::Axis0);
}

std::span<const Float3> SemanticSlotReader::readNormalAxis(const RenderPacket& p) const noexcept {
    return reinterpretSlot<Float3>(p, RenderSlot::Axis1);
}

std::span<const f32> SemanticSlotReader::readRotation(const RenderPacket& p) const noexcept {
    return reinterpretSlot<f32>(p, RenderSlot::Rotation);
}

} // namespace whiteout::cornflakes
