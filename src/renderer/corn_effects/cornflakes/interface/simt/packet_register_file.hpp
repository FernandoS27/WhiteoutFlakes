#pragma once

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace whiteout::cornflakes::simt {

constexpr bool constPoolHit(const DecodedRegId& d, std::size_t constantsPoolBytes) noexcept {
    if (d.scope != scope::kConstPool) {
        return false;
    }
    constexpr std::size_t kSlotBytes = 32U;
    const std::size_t off = static_cast<std::size_t>(d.localIdx) * kSlotBytes;
    return off + 16U <= constantsPoolBytes;
}

class PacketRegisterLayout {
public:
    static constexpr u32 kInvalidSlot = 0xFFFFFFFFU;

    void build(std::span<const CBEMInstruction> program, std::size_t constantsPoolBytes);

    u32 slotForRegId(u32 regId, std::size_t constantsPoolBytes) const noexcept {
        if (regId == kRegVoid) {
            return kInvalidSlot;
        }
        const auto d = decodeRegId(regId);
        if (constPoolHit(d, constantsPoolBytes) || d.scope >= kScopeRegisterBuckets) {
            return kInvalidSlot;
        }
        return slotFor(d.scope, d.localIdx);
    }

    u32 slotFor(u8 bankIndex, u32 localIdx) const noexcept {
        if (bankIndex >= kScopeRegisterBuckets) {
            return kInvalidSlot;
        }
        const Bank& b = banks_[bankIndex];
        if (localIdx < b.firstIndex || localIdx >= b.firstIndex + b.dense.size()) {
            return kInvalidSlot;
        }
        const u32 within = b.dense[localIdx - b.firstIndex];
        return (within == kUnused) ? kInvalidSlot : (b.base + within);
    }

    std::span<const std::pair<u8, u32>> slotSources() const noexcept {
        return {slotSources_.data(), slotSources_.size()};
    }

    std::span<const u32> liveInSlots() const noexcept {
        return {liveInSlots_.data(), liveInSlots_.size()};
    }

    u32 totalRegisters() const noexcept {
        return total_;
    }

    u32 bankCount(u8 bankIndex) const noexcept {
        return bankIndex < kScopeRegisterBuckets ? static_cast<u32>(banks_[bankIndex].count) : 0U;
    }

    u32 bankBase(u8 bankIndex) const noexcept {
        return bankIndex < kScopeRegisterBuckets ? banks_[bankIndex].base : 0U;
    }

    u32 rawIndexSpan(u8 bankIndex) const noexcept {
        if (bankIndex >= kScopeRegisterBuckets) {
            return 0U;
        }
        const Bank& b = banks_[bankIndex];
        return b.dense.empty() ? 0U : b.firstIndex + static_cast<u32>(b.dense.size());
    }

private:
    static constexpr u32 kUnused = 0xFFFFFFFFU;

    struct Bank {
        std::vector<u32> dense;
        u32 firstIndex = 0U;
        u32 base = 0U;
        u32 count = 0U;
    };

    std::array<Bank, kScopeRegisterBuckets> banks_{};
    std::vector<std::pair<u8, u32>> slotSources_;
    std::vector<u32> liveInSlots_;
    u32 total_ = 0U;
};

bool programReadsBeforeWrite(std::span<const CBEMInstruction> program,
                             std::size_t constantsPoolBytes) noexcept;

template <std::size_t N>
class PacketRegisterFile {
public:
    static constexpr std::size_t kLanes = N;

    static constexpr std::size_t kLaneStride = ((N + 15U) / 16U) * 16U;

    static constexpr std::size_t kAlignment = 64U;

    PacketRegisterFile() = default;
    PacketRegisterFile(const PacketRegisterFile&) = delete;
    PacketRegisterFile& operator=(const PacketRegisterFile&) = delete;

    ~PacketRegisterFile() {
        release();
    }

    void reset(const PacketRegisterLayout& layout) {
        const std::size_t want = static_cast<std::size_t>(layout.totalRegisters());
        if (want > capacityRegisters_) {
            release();
            if (want != 0U) {
                planes_ = allocate(want * 4U * kLaneStride);
                capacityRegisters_ = want;
            }
        }
        registers_ = want;
        componentCount_.assign(want, 0U);
        typeBank_.assign(want, 0U);
    }

    u32 registerCount() const noexcept {
        return static_cast<u32>(registers_);
    }

    std::span<u32> plane(u32 slot, u8 component) noexcept {
        return {planes_ + planeOffset(slot, component), kLaneStride};
    }
    std::span<const u32> plane(u32 slot, u8 component) const noexcept {
        return {planes_ + planeOffset(slot, component), kLaneStride};
    }

    void setTag(u32 slot, u8 componentCount, u8 typeBank) noexcept {
        componentCount_[slot] = componentCount;
        typeBank_[slot] = typeBank;
    }
    u8 componentCount(u32 slot) const noexcept {
        return componentCount_[slot];
    }
    u8 typeBank(u32 slot) const noexcept {
        return typeBank_[slot];
    }

    void storeLane(u32 slot, std::size_t lane, const RegisterValue& v) noexcept {
        for (u8 c = 0; c < 4U; ++c) {
            u32 word = 0U;
            std::memcpy(&word, &v.lanes[c], sizeof(word));
            planes_[planeOffset(slot, c) + lane] = word;
        }
        setTag(slot, v.componentCount, v.typeBank);
    }

    RegisterValue loadLane(u32 slot, std::size_t lane) const noexcept {
        RegisterValue v;
        for (u8 c = 0; c < 4U; ++c) {
            const u32 word = planes_[planeOffset(slot, c) + lane];
            std::memcpy(&v.lanes[c], &word, sizeof(word));
        }
        v.componentCount = componentCount_[slot];
        v.typeBank = typeBank_[slot];
        return v;
    }

    void gather(u32 slot, std::span<const RegisterValue> perLane) noexcept {
        const std::size_t n = (perLane.size() < N) ? perLane.size() : N;
        for (std::size_t lane = 0; lane < n; ++lane) {
            for (u8 c = 0; c < 4U; ++c) {
                u32 word = 0U;
                std::memcpy(&word, &perLane[lane].lanes[c], sizeof(word));
                planes_[planeOffset(slot, c) + lane] = word;
            }
        }
        if (n != 0U) {
            setTag(slot, perLane[0].componentCount, perLane[0].typeBank);
        }
    }

    void scatter(u32 slot, std::span<RegisterValue> perLane) const noexcept {
        const std::size_t n = (perLane.size() < N) ? perLane.size() : N;
        for (std::size_t lane = 0; lane < n; ++lane) {
            perLane[lane] = loadLane(slot, lane);
        }
    }

    std::size_t planeBytes() const noexcept {
        return registers_ * 4U * kLaneStride * sizeof(u32);
    }

private:
    std::size_t planeOffset(u32 slot, u8 component) const noexcept {
        return (static_cast<std::size_t>(slot) * 4U + component) * kLaneStride;
    }

    static u32* allocate(std::size_t words) {
        const std::size_t bytes = words * sizeof(u32);
#if defined(_MSC_VER)
        void* p = _aligned_malloc(bytes, kAlignment);
#else
        void* p = std::aligned_alloc(kAlignment, ((bytes + kAlignment - 1U) / kAlignment) * kAlignment);
#endif
        if (p == nullptr) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc{};
#else
            std::abort();  // -fno-exceptions (web build)
#endif
        }
        return static_cast<u32*>(p);
    }

    void release() noexcept {
        if (planes_ != nullptr) {
#if defined(_MSC_VER)
            _aligned_free(planes_);
#else
            std::free(planes_);
#endif
            planes_ = nullptr;
        }
        capacityRegisters_ = 0U;
        registers_ = 0U;
    }

    u32* planes_ = nullptr;
    std::size_t capacityRegisters_ = 0U;
    std::size_t registers_ = 0U;
    std::vector<u8> componentCount_;
    std::vector<u8> typeBank_;
};

}
