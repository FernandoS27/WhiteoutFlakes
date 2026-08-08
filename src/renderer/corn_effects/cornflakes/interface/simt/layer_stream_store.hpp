#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace whiteout::cornflakes::simt {

enum class BankPlacement : u8 {
    UniformConstPool,
    PacketScratch,
    UniformInput,
    ParticleStream,
};

BankPlacement placementForBank(std::size_t normalisedScope) noexcept;

const char* bankPlacementName(BankPlacement placement) noexcept;

class LayerStreamStore {
public:
    static constexpr std::size_t kPacketWidth = 32U;

    static constexpr std::size_t kPlaneAlignment = 64U;

    struct Stream {
        std::size_t slotIndex = 0U;
        u8 componentCount = 0U;
        std::array<f32*, 4> planes{};
    };

    struct AlignedPlaneDeleter {
        void operator()(f32* p) const noexcept {
            ::operator delete[](static_cast<void*>(p), std::align_val_t{kPlaneAlignment});
        }
    };

    LayerStreamStore() = default;

    void reset(const LayerProgram& layer, std::size_t particleCount);

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t requestedCount() const noexcept {
        return requested_;
    }

    std::span<const Stream> externals() const noexcept {
        return {externals_.data(), externals_.size()};
    }
    std::span<const Stream> streamRegisters() const noexcept {
        return {streamRegisters_.data(), streamRegisters_.size()};
    }

    std::size_t bytesAllocated() const noexcept {
        return floatsAllocated_ * sizeof(f32);
    }

    static u8 componentCountFor(const ExternalBinding& binding) noexcept;

private:
    std::vector<Stream> externals_;
    std::vector<Stream> streamRegisters_;
    std::unique_ptr<f32[], AlignedPlaneDeleter> storage_;
    std::size_t floatsAllocated_ = 0U;
    std::size_t capacity_ = 0U;
    std::size_t requested_ = 0U;
};

}
