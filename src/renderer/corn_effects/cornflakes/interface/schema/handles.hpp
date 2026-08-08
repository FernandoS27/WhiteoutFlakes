#pragma once

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

template <typename Tag, typename Underlying>
struct IdHandle {
    Underlying value = 0;

    constexpr bool operator==(const IdHandle&) const = default;
};

struct EffectIdTag {};
struct EmitterIdTag {};
struct LayerIdTag {};
struct FrameIdTag {};

using EffectId = IdHandle<EffectIdTag, u64>;
using EmitterId = IdHandle<EmitterIdTag, u64>;
using LayerId = IdHandle<LayerIdTag, u32>;
using FrameId = IdHandle<FrameIdTag, u64>;

struct EffectHandle {
    EffectId id{};
    u32 generation = 1;

    constexpr bool operator==(const EffectHandle&) const = default;
};

struct CRegID {
    static constexpr u32 kSignatureMask = 0xC0EC0000U;
    static constexpr u32 kTagBitIndex = 20U;

    u32 raw = 0;

    constexpr bool isValid() const noexcept {
        return (raw & kSignatureMask) == kSignatureMask;
    }

    constexpr bool tagBit() const noexcept {
        return ((raw >> kTagBitIndex) & 1U) != 0U;
    }

    constexpr bool operator==(const CRegID&) const = default;
};

}
