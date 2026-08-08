#include <cornflakes/interface/sim/layer_tick_harness.hpp>
#include <cornflakes/interface/simt/layer_stream_store.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <algorithm>
#include <new>

namespace whiteout::cornflakes::simt {

namespace {

std::size_t roundUp(std::size_t n, std::size_t mult) noexcept {
    return ((n + mult - 1U) / mult) * mult;
}

}

BankPlacement placementForBank(std::size_t normalisedScope) noexcept {
    switch (normalisedScope) {
    case scope::kConstPool:
        return BankPlacement::UniformConstPool;
    case scope::kLocal:
        return BankPlacement::PacketScratch;
    case scope::kInput:
        return BankPlacement::UniformInput;
    case scope::kStream:
        return BankPlacement::ParticleStream;
    default:
        break;
    }
    return BankPlacement::ParticleStream;
}

const char* bankPlacementName(BankPlacement placement) noexcept {
    switch (placement) {
    case BankPlacement::UniformConstPool:
        return "uniform-const-pool";
    case BankPlacement::PacketScratch:
        return "packet-scratch";
    case BankPlacement::UniformInput:
        return "uniform-input";
    case BankPlacement::ParticleStream:
        return "particle-stream";
    }
    return "?";
}

u8 LayerStreamStore::componentCountFor(const ExternalBinding& binding) noexcept {
    const u32 bytes = binding.storageSize;
    if (bytes >= 4U && bytes <= 16U && (bytes % 4U) == 0U) {
        return static_cast<u8>(bytes / 4U);
    }
    return 4U;
}

void LayerStreamStore::reset(const LayerProgram& layer, std::size_t particleCount) {
    externals_.clear();
    streamRegisters_.clear();
    storage_.reset();
    floatsAllocated_ = 0U;
    requested_ = particleCount;
    capacity_ = roundUp(particleCount, kPacketWidth);
    if (capacity_ == 0U) {
        return;
    }

    const std::size_t externalCount = LayerTickHarness::externalStorageSizeFor(layer);

    std::vector<u8> externalWidths(externalCount, 4U);
    for (const auto* s : layerScopePrograms(layer)) {
        for (const auto& b : s->externals) {
            const std::size_t slot = resolveExternalSlot(b);
            if (slot < externalWidths.size()) {
                externalWidths[slot] = componentCountFor(b);
            }
        }
    }

    std::size_t streamRegisterCount = 0U;
    for (const auto* s : layerScopePrograms(layer)) {
        const std::size_t idx = scope::kStream + 1U;
        const std::size_t count = (idx < s->registerCounts.size()) ? s->registerCounts[idx] : 0U;
        streamRegisterCount = std::max(streamRegisterCount, count);
    }

    std::size_t planeCount = 0U;
    for (const u8 w : externalWidths) {
        planeCount += w;
    }
    planeCount += streamRegisterCount * 4U;

    floatsAllocated_ = planeCount * capacity_;
    if (floatsAllocated_ == 0U) {
        return;
    }
    // Not `new (align_val_t) f32[n]()`: with exceptions enabled MSVC rejects that
    // form (C2956) because the matching deallocation function is a usual one.
    storage_.reset(static_cast<f32*>(
        ::operator new[](floatsAllocated_ * sizeof(f32), std::align_val_t{kPlaneAlignment})));
    std::fill_n(storage_.get(), floatsAllocated_, 0.0F);

    f32* cursor = storage_.get();
    externals_.reserve(externalCount);
    for (std::size_t slot = 0; slot < externalCount; ++slot) {
        Stream st;
        st.slotIndex = slot;
        st.componentCount = externalWidths[slot];
        for (u8 c = 0; c < st.componentCount; ++c) {
            st.planes[c] = cursor;
            cursor += capacity_;
        }
        externals_.push_back(st);
    }
    streamRegisters_.reserve(streamRegisterCount);
    for (std::size_t reg = 0; reg < streamRegisterCount; ++reg) {
        Stream st;
        st.slotIndex = reg;
        st.componentCount = 4U;
        for (u8 c = 0; c < 4U; ++c) {
            st.planes[c] = cursor;
            cursor += capacity_;
        }
        streamRegisters_.push_back(st);
    }
}

}
