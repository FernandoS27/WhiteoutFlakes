#pragma once

/// @file
/// @brief 32-bit LCG matching CornFx's `state * 0x000D0F95 + 0x00D19EC3` exactly.

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

/// @brief Bit-exact replica of CornFx `TFastRandU32`. Engine-verified constants — do not change.
class TFastRandU32 {
public:
    static constexpr u32 kMultiplier = 0x000D0F95U; ///< LCG multiplier; engine-locked.
    static constexpr u32 kIncrement = 0x00D19EC3U;  ///< LCG increment; engine-locked.

    static constexpr u32 advanceStatic(u32 state) noexcept {
        return (state * kMultiplier) + kIncrement;
    }

    constexpr TFastRandU32() noexcept = default;
    constexpr explicit TFastRandU32(u32 seed) noexcept : m_state(seed) {}

    constexpr u32 state() const noexcept {
        return m_state;
    }

    constexpr void setState(u32 state) noexcept {
        m_state = state;
    }

    constexpr u32 advance() noexcept {
        m_state = advanceStatic(m_state);
        return m_state;
    }

private:
    u32 m_state = 0;
};

} // namespace whiteout::cornflakes
