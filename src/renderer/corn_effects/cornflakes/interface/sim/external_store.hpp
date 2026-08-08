#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

class ExternalStore {
public:
    void resize(std::size_t slots, std::size_t particles) {
        if (slots == slots_ && particles <= particleStride_) {
            return;
        }
        const std::size_t newStride = (particles > particleStride_) ? particles : particleStride_;
        std::vector<u32> planes(slots * 4U * newStride, 0U);
        std::vector<u8> componentCount(slots * newStride, 0U);
        std::vector<u8> typeBank(slots * newStride, 0U);

        const std::size_t keepSlots = (slots < slots_) ? slots : slots_;
        const std::size_t keepParticles =
            (newStride < particleStride_) ? newStride : particleStride_;
        for (std::size_t slot = 0; slot < keepSlots; ++slot) {
            for (u8 c = 0; c < 4U; ++c) {
                const std::size_t src = (slot * 4U + c) * particleStride_;
                const std::size_t dst = (slot * 4U + c) * newStride;
                for (std::size_t p = 0; p < keepParticles; ++p) {
                    planes[dst + p] = planes_[src + p];
                }
            }
            for (std::size_t p = 0; p < keepParticles; ++p) {
                componentCount[slot * newStride + p] = componentCount_[slot * particleStride_ + p];
                typeBank[slot * newStride + p] = typeBank_[slot * particleStride_ + p];
            }
        }

        planes_.swap(planes);
        componentCount_.swap(componentCount);
        typeBank_.swap(typeBank);
        slots_ = slots;
        particleStride_ = newStride;
    }

    std::size_t slotCount() const noexcept {
        return slots_;
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

    void clearParticle(std::size_t particle) noexcept {
        for (std::size_t slot = 0; slot < slots_; ++slot) {
            for (u8 c = 0; c < 4U; ++c) {
                planes_[(slot * 4U + c) * particleStride_ + particle] = 0U;
            }
            componentCount_[slot * particleStride_ + particle] = 0U;
            typeBank_[slot * particleStride_ + particle] = 0U;
        }
    }

    std::span<const u32> plane(std::size_t slot, u8 c) const noexcept {
        return {planes_.data() + (slot * 4U + c) * particleStride_, particleStride_};
    }

private:
    std::vector<u32> planes_;
    std::vector<u8> componentCount_;
    std::vector<u8> typeBank_;
    std::size_t slots_ = 0U;
    std::size_t particleStride_ = 0U;
};

struct ExternalView {
    ExternalStore* store = nullptr;
    std::size_t particle = 0U;
    std::size_t count = 0U;

    std::size_t size() const noexcept {
        return count;
    }
    bool empty() const noexcept {
        return count == 0U;
    }

    const RegisterValue operator[](std::size_t slot) const noexcept {
        return store->load(slot, particle);
    }
    void set(std::size_t slot, const RegisterValue& v) const noexcept {
        store->store(slot, particle, v);
    }
    void clear() const noexcept {
        if (store != nullptr) {
            store->clearParticle(particle);
        }
    }
};

}
