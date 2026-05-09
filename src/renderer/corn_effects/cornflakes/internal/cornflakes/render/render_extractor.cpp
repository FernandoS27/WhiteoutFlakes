#include <cornflakes/core/determinism.hpp>
#include <cornflakes/render/render_extractor.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

template <typename T>
std::span<const std::byte> arenaSlot(IArena& arena, u32 particleCount, const T* source) {
    if (particleCount == 0) {
        return {};
    }
    const auto typed = arenaArray<T>(arena, particleCount);
    if (source != nullptr) {
        std::memcpy(typed.data(), source, sizeof(T) * particleCount);
    }
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(typed.data()),
                                      sizeof(T) * particleCount};
}

} // namespace

RenderPacket RenderExtractor::extractPacket(const ParticlePage& page, EmitterId emitter,
                                            LayerId layer, RendererClass cls, IArena& frameArena,
                                            IssueBag&) const {
    RenderPacket pkt;
    pkt.emitter = emitter;
    pkt.layer = layer;
    pkt.cls = cls;
    pkt.particleCount = page.particleCount;

    if (page.particleCount == 0) {
        return pkt;
    }

    const Float3* posSource =
        page.positions.size() >= page.particleCount ? page.positions.data() : nullptr;
    pkt.slots[static_cast<std::size_t>(RenderSlot::Position)] =
        arenaSlot<Float3>(frameArena, page.particleCount, posSource);

    if (cls == RendererClass::Mesh) {

        const auto scales = arenaArray<Float3>(frameArena, page.particleCount);
        for (auto& s : scales) {
            s = Float3{1.0F, 1.0F, 1.0F};
        }
        pkt.slots[static_cast<std::size_t>(RenderSlot::Size)] = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(scales.data()), sizeof(Float3) * scales.size()};
    } else {
        const auto sizes = arenaArray<f32>(frameArena, page.particleCount);
        for (auto& s : sizes) {
            s = 1.0F;
        }
        pkt.slots[static_cast<std::size_t>(RenderSlot::Size)] = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(sizes.data()), sizeof(f32) * sizes.size()};
    }

    const auto enabled = arenaArray<u8>(frameArena, page.particleCount);
    for (u32 i = 0; i < page.particleCount; ++i) {
        const bool live = (page.lifeRatios.size() > i) ? (page.lifeRatios[i] < 1.0F) : true;
        enabled[i] = live ? u8{1} : u8{0};
    }
    pkt.slots[static_cast<std::size_t>(RenderSlot::Enabled)] = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(enabled.data()), enabled.size()};

    return pkt;
}

std::vector<RenderPacket> RenderExtractor::extract(const MediumState& medium,
                                                   const LayerProgram& layer, IArena& frameArena,
                                                   IssueBag& issues) const {
    std::vector<RenderPacket> out;
    if (medium.pages.empty()) {
        return out;
    }

    const bool hasRenderers = !layer.renderers.empty();
    for (const auto& page : medium.pages) {
        if (hasRenderers) {
            for (const auto& r : layer.renderers) {
                auto pkt = extractPacket(page, medium.emitter, layer.id, r.cls, frameArena, issues);
                pkt.blendMode = r.blendMode;
                pkt.billboardingMode = static_cast<u8>(r.billboardingMode);
                out.push_back(std::move(pkt));
            }
        } else {
            out.push_back(extractPacket(page, medium.emitter, layer.id, RendererClass::Billboard,
                                        frameArena, issues));
        }
    }
    return out;
}

} // namespace whiteout::cornflakes
