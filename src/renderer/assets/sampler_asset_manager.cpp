#include "renderer/assets/sampler_asset_manager.h"

namespace whiteout::flakes::renderer::assets {

SamplerAssetManager::SamplerAssetManager(gfx::IGFXDevice& gfx) : gfx_(gfx) {}

SamplerAssetManager::~SamplerAssetManager() {
    // No gfx_.Destroy here — see ReleaseGpu. The asset managers are
    // destroyed after the device (render_service_impl.h's destruction-order
    // contract), so `gfx_` is a dangling reference by now.
    cache_.clear();
    shadowComparison_ = gfx::SamplerHandle::Invalid;
}

void SamplerAssetManager::ReleaseGpu() {
    for (auto& [key, handle] : cache_) {
        gfx_.Destroy(handle);
    }
    cache_.clear();
    if (shadowComparison_ != gfx::SamplerHandle::Invalid) {
        gfx_.Destroy(shadowComparison_);
        shadowComparison_ = gfx::SamplerHandle::Invalid;
    }
}

gfx::SamplerHandle SamplerAssetManager::Get(const gfx::SamplerDesc& desc) {
    const DescKey key{desc.minFilter, desc.magFilter, desc.addressU, desc.addressV, desc.addressW};
    if (auto it = cache_.find(key); it != cache_.end())
        return it->second;
    gfx::SamplerHandle h = gfx_.CreateSampler(desc);
    cache_.emplace(key, h);
    return h;
}

gfx::SamplerHandle SamplerAssetManager::WrapVariant(u32 wrapFlags) {
    using AM = gfx::AddressMode;
    const u32 bits = wrapFlags & kSamplerWrapBitsMask;
    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    sd.addressU = (bits & 0x1) ? AM::Wrap : AM::Clamp;
    sd.addressV = (bits & 0x2) ? AM::Wrap : AM::Clamp;
    sd.addressW = AM::Clamp;
    return Get(sd);
}

gfx::SamplerHandle SamplerAssetManager::LinearWrap() {
    using AM = gfx::AddressMode;
    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    sd.addressU = AM::Wrap;
    sd.addressV = AM::Wrap;
    sd.addressW = AM::Wrap;
    return Get(sd);
}

gfx::SamplerHandle SamplerAssetManager::ShadowComparison() {
    if (shadowComparison_ != gfx::SamplerHandle::Invalid)
        return shadowComparison_;
    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    // Clamp prevents the cascade's edge texels from wrapping into the
    // opposite side of the depth atlas at grazing fragments.
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    sd.comparison = true;
    sd.comparisonFunc = gfx::CompareOp::LessEqual;
    shadowComparison_ = gfx_.CreateSampler(sd);
    return shadowComparison_;
}

} // namespace whiteout::flakes::renderer::assets
