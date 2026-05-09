#pragma once

/// @file
/// @brief Strongly-typed integer handles (Effect/Emitter/Layer/Frame ids) and `CRegID`.

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

/// @brief Tag-typed integer wrapper preventing accidental cross-id assignment.
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

/// @brief EffectId paired with a generation counter; invalidated on reload.
struct EffectHandle {
    EffectId id{};
    u32 generation = 1;

    constexpr bool operator==(const EffectHandle&) const = default;
};

/// @brief CornFx-format register id (`0xC0EC` signature in the high half).
struct CRegID {
    static constexpr u32 kSignatureMask = 0xC0EC0000U; ///< High 16 bits when valid.
    static constexpr u32 kTagBitIndex = 20U;           ///< Bit position of the type-tag flag.

    u32 raw = 0;

    /// @brief True when the high half carries the canonical signature.
    constexpr bool isValid() const noexcept {
        return (raw & kSignatureMask) == kSignatureMask;
    }

    constexpr bool tagBit() const noexcept {
        return ((raw >> kTagBitIndex) & 1U) != 0U;
    }

    constexpr bool operator==(const CRegID&) const = default;
};

} // namespace whiteout::cornflakes
