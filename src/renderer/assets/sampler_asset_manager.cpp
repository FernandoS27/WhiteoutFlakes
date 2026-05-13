#include "renderer/assets/sampler_asset_manager.h"

namespace whiteout::flakes::renderer::assets {

SamplerAssetManager::SamplerAssetManager(gfx::IGFXDevice& gfx) : gfx_(gfx) {}

SamplerAssetManager::~SamplerAssetManager() {
    for (auto& [key, handle] : cache_) {
        gfx_.Destroy(handle);
    }
    cache_.clear();
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

} // namespace whiteout::flakes::renderer::assets
