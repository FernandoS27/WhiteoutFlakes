#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/sim/external_store.hpp>
#include <cornflakes/interface/sim/layer_tick_harness.hpp>
#include <cornflakes/interface/simt/packet_register_file.hpp>
#include <cornflakes/interface/simt/pool_register_file.hpp>
#include <cornflakes/interface/simt/simt_interpreter.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

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

    void resizeForLayer(const LayerProgram& layer);

    bool initBatch(const LayerProgram& layer, u32 baseSeed, IArena& arena, IssueBag& issues);

    bool initRange(const LayerProgram& layer, u32 baseSeed, std::size_t startIdx, std::size_t count,
                   IArena& arena, IssueBag& issues);

    bool tickBatch(const LayerProgram& layer, IArena& arena, IssueBag& issues);

    struct SimtTickResult {
        bool ok = true;
        bool ranSimt = false;
        std::size_t packetsRun = 0U;
        std::size_t particlesRun = 0U;
    };

    SimtTickResult tickBatchSimt(const LayerProgram& layer, IArena& arena, IssueBag& issues);

    void enablePoolRegisters(const LayerProgram& layer);

    bool poolRegistersEnabled() const noexcept {
        return poolRegistersEnabled_;
    }

    std::span<const RegisterValue> materialiseBank(std::size_t particle, u8 bank) const;

private:
    std::vector<LayerTickHarness> particles_;

    ExternalStore externals_;

    void rebindExternals();
    LayerTickHarness::Config layout_{};
    bool layoutKnown_ = false;

    std::array<simt::PoolRegisterFile, kScopeRegisterBuckets> poolRegisters_;

    simt::PacketRegisterLayout evolveLayout_;
    simt::PoolPublishTargets poolTargets_{};
    bool poolRegistersEnabled_ = false;

    mutable std::array<std::vector<RegisterValue>, kScopeRegisterBuckets> materialiseScratch_;

    void syncPoolRegisterFiles();
    void seedPoolRegisters(std::size_t particle);

#if defined(CORNFLAKES_SIMT_PROBE)
    unsigned long long probeLastPoolBytes_ = 0U;
    unsigned long long probeLastAosBytes_ = 0U;
#endif
};

#if defined(CORNFLAKES_SIMT_PROBE)
void reportSimtProbe() noexcept;
#endif

}
