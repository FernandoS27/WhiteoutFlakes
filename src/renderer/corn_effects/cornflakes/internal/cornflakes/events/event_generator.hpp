#pragma once

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/events/payload_cache_store.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

struct SEventHeader_Generator {
    u32 generatorKey = 0;
    u32 tickId = 0;
    u32 particleCount = 0;
    u32 reserved = 0;
};

class EventGenerator {
public:
    std::span<std::byte> emit(u32 generatorKey, u32 tickId, u32 particleCount, IArena& arena,
                              PayloadCacheStore& store, IssueBag& issues) const;
};

}
