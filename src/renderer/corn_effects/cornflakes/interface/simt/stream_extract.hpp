#pragma once

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/simt/layer_stream_store.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes::simt {

std::span<const std::byte> extractSlotFromStore(const LayerStreamStore& store,
                                                std::size_t slotIndex, u8 components,
                                                std::span<const bool> liveMask,
                                                std::size_t particleCount, IArena& arena);

}
