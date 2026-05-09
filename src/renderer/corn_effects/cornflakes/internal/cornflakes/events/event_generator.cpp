#include <cornflakes/core/determinism.hpp>
#include <cornflakes/events/event_generator.hpp>

#include <cstring>

namespace whiteout::cornflakes {

std::span<std::byte> EventGenerator::emit(u32 generatorKey, u32 tickId, u32 particleCount,
                                          IArena& arena, PayloadCacheStore& store,
                                          IssueBag&) const {

    const std::size_t bytes = sizeof(SEventHeader_Generator);
    const auto blob = arenaArray<std::byte>(arena, bytes);

    SEventHeader_Generator header;
    header.generatorKey = generatorKey;
    header.tickId = tickId;
    header.particleCount = particleCount;

    std::memcpy(blob.data(), &header, sizeof(header));

    const PayloadKey key{generatorKey, 0U, 0U};
    store.add(key, blob);
    return blob;
}

} // namespace whiteout::cornflakes
