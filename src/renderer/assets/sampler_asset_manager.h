#pragma once

#include "whiteout/flakes/types.h"
#include "gfx/gfx.h"
#include <functional>
#include <unordered_map>

namespace whiteout::flakes::renderer::assets {

inline constexpr u32 kSamplerWrapBitsMask = 0x3;

enum class WrapMode : u32 {
    ClampClamp = 0,
    WrapClamp  = 1,
    ClampWrap  = 2,
    WrapWrap   = 3,
};

class SamplerAssetManager {
public:
    explicit SamplerAssetManager(gfx::IGFXDevice& gfx);
    ~SamplerAssetManager();

    SamplerAssetManager(const SamplerAssetManager&)            = delete;
    SamplerAssetManager& operator=(const SamplerAssetManager&) = delete;

    gfx::SamplerHandle Get(const gfx::SamplerDesc& desc);

    gfx::SamplerHandle WrapVariant(u32 wrapFlags);
    gfx::SamplerHandle WrapVariant(WrapMode mode) {
        return WrapVariant(static_cast<u32>(mode));
    }

    gfx::SamplerHandle LinearWrap();

    usize DebugSamplerCount() const noexcept { return cache_.size(); }

private:
    gfx::IGFXDevice& gfx_;

    struct DescKey {
        gfx::Filter      minF;
        gfx::Filter      magF;
        gfx::AddressMode aU;
        gfx::AddressMode aV;
        gfx::AddressMode aW;
        bool operator==(const DescKey&) const noexcept = default;
    };
    struct DescKeyHash {
        usize operator()(const DescKey& k) const noexcept {

            u64 h = 0;
            h |= (u64)k.minF << 0;
            h |= (u64)k.magF << 4;
            h |= (u64)k.aU   << 8;
            h |= (u64)k.aV   << 16;
            h |= (u64)k.aW   << 24;
            return std::hash<u64>{}(h);
        }
    };
    std::unordered_map<DescKey, gfx::SamplerHandle, DescKeyHash> cache_;
};

}
