#pragma once

/// @file
/// @brief Vector of `LayerTickHarness` slots — the per-layer particle store of `EffectRuntime`.

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/sim/layer_tick_harness.hpp>

#include <cstddef>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Per-layer container of `LayerTickHarness` slots; sized once and reused each frame.
class ParticlePool {
public:
    ParticlePool() = default;

    void resize(std::size_t count);

    std::size_t size() const noexcept {
        return particles_.size();
    }

    LayerTickHarness& particle(std::size_t i) noexcept {
        return particles_[i];
    }
    const LayerTickHarness& particle(std::size_t i) const noexcept {
        return particles_[i];
    }

    /// @brief Forward `resizeForLayer` to every slot.
    void resizeForLayer(const LayerProgram& layer);

    /// @brief Run init scope on every slot. Each slot's seed is `baseSeed + slotIndex`.
    bool initBatch(const LayerProgram& layer, u32 baseSeed, IArena& arena, IssueBag& issues);

    /// @brief Run init scope on `[startIdx, startIdx+count)`; used by spawn drains.
    bool initRange(const LayerProgram& layer, u32 baseSeed, std::size_t startIdx, std::size_t count,
                   IArena& arena, IssueBag& issues);

    /// @brief Run per-tick scopes on every slot.
    bool tickBatch(const LayerProgram& layer, IArena& arena, IssueBag& issues);

private:
    std::vector<LayerTickHarness> particles_;
};

} // namespace whiteout::cornflakes
