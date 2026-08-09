#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>

namespace whiteout::cornflakes::simt {

class PoolRegisterFile {
public:
    static constexpr std::size_t kAlignment = 64U;

    static constexpr std::size_t strideFor(std::size_t particles) noexcept {
        return ((particles + 15U) / 16U) * 16U;
    }

    PoolRegisterFile() = default;
    PoolRegisterFile(const PoolRegisterFile&) = delete;
    PoolRegisterFile& operator=(const PoolRegisterFile&) = delete;
    PoolRegisterFile(PoolRegisterFile&& o) noexcept {
        swap(o);
    }
    PoolRegisterFile& operator=(PoolRegisterFile&& o) noexcept {
        if (this != &o) {
            release();
            swap(o);
        }
        return *this;
    }
    ~PoolRegisterFile() {
        release();
    }

    void resize(std::size_t registers, std::size_t particles) {
        const std::size_t newStride = strideFor(particles) > particleStride_
                                          ? strideFor(particles)
                                          : particleStride_;
        if (registers == registers_ && newStride == particleStride_) {
            return;
        }
        const std::size_t words = registers * 4U * newStride;
        u32* planes = (words == 0U) ? nullptr : allocateZeroed(words);
        std::vector<u8> componentCount(registers * newStride, 0U);
        std::vector<u8> typeBank(registers * newStride, 0U);

        const std::size_t keepRegisters = (registers < registers_) ? registers : registers_;
        const std::size_t keepParticles =
            (newStride < particleStride_) ? newStride : particleStride_;
        for (std::size_t slot = 0; slot < keepRegisters; ++slot) {
            for (u8 c = 0; c < 4U; ++c) {
                const std::size_t src = (slot * 4U + c) * particleStride_;
                const std::size_t dst = (slot * 4U + c) * newStride;
                std::memcpy(planes + dst, planes_ + src, keepParticles * sizeof(u32));
            }
            std::memcpy(componentCount.data() + slot * newStride,
                        componentCount_.data() + slot * particleStride_, keepParticles);
            std::memcpy(typeBank.data() + slot * newStride,
                        typeBank_.data() + slot * particleStride_, keepParticles);
        }

        release();
        planes_ = planes;
        componentCount_.swap(componentCount);
        typeBank_.swap(typeBank);
        registers_ = registers;
        particleStride_ = newStride;
    }

    std::size_t registerCount() const noexcept {
        return registers_;
    }
    std::size_t particleStride() const noexcept {
        return particleStride_;
    }

    RegisterValue load(std::size_t slot, std::size_t particle) const noexcept {
        RegisterValue v;
        for (u8 c = 0; c < 4U; ++c) {
            const u32 word = planes_[(slot * 4U + c) * particleStride_ + particle];
            std::memcpy(&v.lanes[c], &word, sizeof(word));
        }
        v.componentCount = componentCount_[slot * particleStride_ + particle];
        v.typeBank = typeBank_[slot * particleStride_ + particle];
        return v;
    }

    void store(std::size_t slot, std::size_t particle, const RegisterValue& v) noexcept {
        for (u8 c = 0; c < 4U; ++c) {
            u32 word = 0U;
            std::memcpy(&word, &v.lanes[c], sizeof(word));
            planes_[(slot * 4U + c) * particleStride_ + particle] = word;
        }
        componentCount_[slot * particleStride_ + particle] = v.componentCount;
        typeBank_[slot * particleStride_ + particle] = v.typeBank;
    }

    u8 componentCount(std::size_t slot, std::size_t particle) const noexcept {
        return componentCount_[slot * particleStride_ + particle];
    }
    u8 typeBank(std::size_t slot, std::size_t particle) const noexcept {
        return typeBank_[slot * particleStride_ + particle];
    }
    void setTag(std::size_t slot, std::size_t particle, u8 components, u8 bank) noexcept {
        componentCount_[slot * particleStride_ + particle] = components;
        typeBank_[slot * particleStride_ + particle] = bank;
    }

    void setTagRange(std::size_t slot, std::size_t first, std::size_t count, u8 components,
                     u8 bank) noexcept {
        std::memset(componentCount_.data() + slot * particleStride_ + first, components, count);
        std::memset(typeBank_.data() + slot * particleStride_ + first, bank, count);
    }

    void clearParticle(std::size_t particle) noexcept {
        for (std::size_t slot = 0; slot < registers_; ++slot) {
            for (u8 c = 0; c < 4U; ++c) {
                planes_[(slot * 4U + c) * particleStride_ + particle] = 0U;
            }
            componentCount_[slot * particleStride_ + particle] = 0U;
            typeBank_[slot * particleStride_ + particle] = 0U;
        }
    }

    std::span<u32> plane(std::size_t slot, u8 c) noexcept {
        return {planes_ + (slot * 4U + c) * particleStride_, particleStride_};
    }
    std::span<const u32> plane(std::size_t slot, u8 c) const noexcept {
        return {planes_ + (slot * 4U + c) * particleStride_, particleStride_};
    }

    std::size_t planeBytes() const noexcept {
        return registers_ * 4U * particleStride_ * sizeof(u32);
    }
    std::size_t tagBytes() const noexcept {
        return componentCount_.size() + typeBank_.size();
    }

private:
    static u32* allocateZeroed(std::size_t words) {
        const std::size_t bytes = ((words * sizeof(u32) + kAlignment - 1U) / kAlignment) * kAlignment;
#if defined(_MSC_VER)
        void* p = _aligned_malloc(bytes, kAlignment);
#else
        void* p = std::aligned_alloc(kAlignment, bytes);
#endif
        if (p == nullptr) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc{};
#else
            std::abort();  // -fno-exceptions (web build)
#endif
        }
        std::memset(p, 0, bytes);
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
        componentCount_.clear();
        typeBank_.clear();
        registers_ = 0U;
        particleStride_ = 0U;
    }

    void swap(PoolRegisterFile& o) noexcept {
        std::swap(planes_, o.planes_);
        componentCount_.swap(o.componentCount_);
        typeBank_.swap(o.typeBank_);
        std::swap(registers_, o.registers_);
        std::swap(particleStride_, o.particleStride_);
    }

    u32* planes_ = nullptr;
    std::vector<u8> componentCount_;
    std::vector<u8> typeBank_;
    std::size_t registers_ = 0U;
    std::size_t particleStride_ = 0U;
};

}
