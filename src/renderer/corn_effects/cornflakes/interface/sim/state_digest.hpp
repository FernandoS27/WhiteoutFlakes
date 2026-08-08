#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/sim/spawn_event.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace whiteout::cornflakes {

class EffectRuntime;

inline constexpr u64 kDigestOffsetBasis = 0xCBF29CE484222325ULL;
inline constexpr u64 kDigestPrime = 0x100000001B3ULL;

class DigestHasher {
public:
    void mixU8(u8 v) noexcept {
        value_ ^= static_cast<u64>(v);
        value_ *= kDigestPrime;
    }
    void mixU32(u32 v) noexcept {
        for (int i = 0; i < 4; ++i) {
            mixU8(static_cast<u8>((v >> (i * 8)) & 0xFFU));
        }
    }
    void mixU64(u64 v) noexcept {
        for (int i = 0; i < 8; ++i) {
            mixU8(static_cast<u8>((v >> (i * 8)) & 0xFFU));
        }
    }
    void mixF32(f32 v) noexcept;
    void mixBool(bool v) noexcept {
        mixU8(v ? 1U : 0U);
    }
    void mixSize(std::size_t v) noexcept {
        mixU64(static_cast<u64>(v));
    }

    void mixRegister(const RegisterValue& rv) noexcept {
        for (const f32 lane : rv.lanes) {
            mixF32(lane);
        }
        mixU8(rv.componentCount);
        mixU8(rv.typeBank);
    }

    static bool isDefaultRegister(const RegisterValue& rv) noexcept;

    template <class Regs>
    void mixRegisterSpan(const Regs& regs) noexcept {
        std::size_t live = regs.size();
        while (live > 0U && isDefaultRegister(regs[live - 1U])) {
            --live;
        }
        mixSize(live);
        for (std::size_t i = 0; i < live; ++i) {
            mixRegister(regs[i]);
        }
    }

    void mixSpawnEvent(const SpawnEvent& ev) noexcept;

    void mixProximityEntry(const ProximityEntry& e) noexcept;

    u64 value() const noexcept {
        return value_;
    }

private:
    u64 value_ = kDigestOffsetBasis;
};

struct LayerStateDigest {
    u64 particles = kDigestOffsetBasis;

    u64 spawnQueue = kDigestOffsetBasis;

    std::size_t slotCount = 0U;
    std::size_t aliveCount = 0U;
    std::size_t spawnQueueLength = 0U;
};

struct EffectStateDigest {
    u64 combined = kDigestOffsetBasis;

    std::vector<LayerStateDigest> layers;

    u64 spatial = kDigestOffsetBasis;

    std::string toHex() const;
};

EffectStateDigest digestEffect(const EffectRuntime& rt);

}
