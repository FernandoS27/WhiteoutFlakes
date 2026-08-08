#include <cornflakes/interface/simt/stream_extract.hpp>

namespace whiteout::cornflakes::simt {

std::span<const std::byte> extractSlotFromStore(const LayerStreamStore& store,
                                                std::size_t slotIndex, u8 components,
                                                std::span<const bool> liveMask,
                                                std::size_t particleCount, IArena& arena) {
    if (components == 0U || particleCount == 0U) {
        return {};
    }
    const auto streams = store.externals();
    if (slotIndex >= streams.size()) {
        return {};
    }
    const auto& stream = streams[slotIndex];

    const std::size_t totalFloats = particleCount * static_cast<std::size_t>(components);
    auto buf = arenaArray<f32>(arena, totalFloats);

    for (std::size_t i = 0; i < particleCount; ++i) {
        const bool live = (i < liveMask.size()) ? liveMask[i] : false;
        for (u8 c = 0; c < components; ++c) {
            const bool haveComponent = (c < stream.componentCount) && stream.planes[c] != nullptr;
            buf[(i * components) + c] = (live && haveComponent) ? stream.planes[c][i] : 0.0F;
        }
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(buf.data());
    return std::span<const std::byte>{bytes, totalFloats * sizeof(f32)};
}

}
