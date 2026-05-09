#include <cornflakes/interface/render/pool_extractor.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

u8 componentsForSlot(RenderSlot slot, RendererClass cls) noexcept {
    switch (slot) {
    case RenderSlot::Position:
        return 3;
    case RenderSlot::Size:
        return cls == RendererClass::Mesh ? 3 : 1;
    case RenderSlot::Enabled:
        return 1;
    case RenderSlot::Orientation:
        return 4;
    case RenderSlot::Axis0:
        return 3;
    case RenderSlot::Axis1:
        return 3;
    case RenderSlot::Rotation:
        return 1;
    case RenderSlot::Color:
        return 4;
    case RenderSlot::TextureID:
        return 1;
    case RenderSlot::SelfID:
    case RenderSlot::ParentID:
        return 2;
    default:
        return 0;
    }
}

const ExternalBinding* lookupBinding(const LayerProgram& layer, std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    if (auto* b = findBindingByName(layer.initProgram.externals, name)) {
        return b;
    }
    if (auto* b = findBindingByName(layer.timeFixedProgram.externals, name)) {
        return b;
    }
    if (auto* b = findBindingByName(layer.timeVaryingProgram.externals, name)) {
        return b;
    }
    return findBindingByName(layer.physicsProgram.externals, name);
}

std::span<const std::byte> packSlot(const ParticlePool& pool, const LayerProgram& layer,
                                    std::string_view name, u8 components, IArena& arena) {
    const ExternalBinding* b = lookupBinding(layer, name);
    if (b == nullptr || components == 0U || pool.size() == 0U) {
        return {};
    }
    const std::size_t totalFloats = pool.size() * static_cast<std::size_t>(components);
    auto buf = arenaArray<f32>(arena, totalFloats);
    for (std::size_t i = 0; i < pool.size(); ++i) {
        const auto& particle = pool.particle(i);
        if (particle.isDead()) {
            for (u8 c = 0; c < components; ++c) {
                buf[i * components + c] = 0.0F;
            }
            continue;
        }
        const auto exts = particle.externals();

        const u16 resolved = (b->canonicalSlot == 0U && b->slot != 0U) ? b->slot : b->canonicalSlot;
        const auto slotIdx = static_cast<std::size_t>(resolved);
        const f32* src = (slotIdx < exts.size()) ? exts[slotIdx].lanes : nullptr;
        for (u8 c = 0; c < components; ++c) {
            buf[i * components + c] = (src != nullptr) ? src[c] : 0.0F;
        }
    }
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(buf.data()),
                                      totalFloats * sizeof(f32)};
}

std::span<const std::byte> packSelfIds(const ParticlePool& pool, IArena& arena, bool parent) {
    if (pool.size() == 0U) {
        return {};
    }
    auto buf = arenaArray<u64>(arena, pool.size());
    for (std::size_t i = 0; i < pool.size(); ++i) {
        const auto& particle = pool.particle(i);

        if (particle.isDead()) {
            buf[i] = 0U;
            continue;
        }
        buf[i] = parent ? particle.parentSelfId() : particle.selfId();
    }
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(buf.data()),
                                      pool.size() * sizeof(u64)};
}

} // namespace

RenderPacket extractFromPool(const ParticlePool& pool, const LayerProgram& layer, EmitterId emitter,
                             RendererClass cls, const RenderInputMap& mapping, IArena& arena,
                             IssueBag&) {
    RenderPacket packet;
    packet.emitter = emitter;
    packet.layer = layer.id;
    packet.cls = cls;
    packet.particleCount = static_cast<u32>(pool.size());

    for (std::size_t s = 0; s < kRenderSlotCount; ++s) {
        const auto slot = static_cast<RenderSlot>(s);

        if (slot == RenderSlot::SelfID) {
            packet.slots[s] = packSelfIds(pool, arena, false);
            continue;
        }
        if (slot == RenderSlot::ParentID) {
            packet.slots[s] = packSelfIds(pool, arena, true);
            continue;
        }
        const u8 components = componentsForSlot(slot, cls);
        packet.slots[s] = packSlot(pool, layer, mapping.names[s], components, arena);
    }
    return packet;
}

} // namespace whiteout::cornflakes
