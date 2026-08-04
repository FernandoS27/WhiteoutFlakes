#pragma once

#include <functional>
#include <unordered_map>
#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::assets {

inline constexpr u32 kSamplerWrapBitsMask = 0x3;

enum class WrapMode : u32 {
    ClampClamp = 0,
    WrapClamp = 1,
    ClampWrap = 2,
    WrapWrap = 3,
};

class SamplerAssetManager {
public:
    explicit SamplerAssetManager(gfx::IGFXDevice& gfx);
    ~SamplerAssetManager();

    /// Destroy every cached sampler. Must be called while the gfx device
    /// is still alive — the destructor cannot, see its comment.
    void ReleaseGpu();

    SamplerAssetManager(const SamplerAssetManager&) = delete;
    SamplerAssetManager& operator=(const SamplerAssetManager&) = delete;

    gfx::SamplerHandle Get(const gfx::SamplerDesc& desc);

    gfx::SamplerHandle WrapVariant(u32 wrapFlags);
    gfx::SamplerHandle WrapVariant(WrapMode mode) {
        return WrapVariant(static_cast<u32>(mode));
    }

    gfx::SamplerHandle LinearWrap();

    // Hardware comparison sampler for shadow PCF. Linear filter +
    // ClampToEdge + comparison=LessEqual. The HD opaque PS sampler
    // `sd_s_shadow0..2` is declared `SamplerComparisonState` and uses
    // `SampleCmpLevelZero`, which requires a real comparison sampler.
    // Single shared handle since all three cascades use identical
    // settings; lazily created on first call.
    gfx::SamplerHandle ShadowComparison();

    usize DebugSamplerCount() const noexcept {
        return cache_.size();
    }

private:
    gfx::IGFXDevice& gfx_;
    gfx::SamplerHandle shadowComparison_ = gfx::SamplerHandle::Invalid;

    struct DescKey {
        gfx::Filter minF;
        gfx::Filter magF;
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
            h |= (u64)k.aU << 8;
            h |= (u64)k.aV << 16;
            h |= (u64)k.aW << 24;
            return std::hash<u64>{}(h);
        }
    };
    std::unordered_map<DescKey, gfx::SamplerHandle, DescKeyHash> cache_;
};

} // namespace whiteout::flakes::renderer::assets
