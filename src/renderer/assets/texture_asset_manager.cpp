#include "renderer/assets/texture_asset_manager.h"

namespace whiteout::flakes::renderer::assets {

namespace {

gfx::TextureHandle Make1x1(gfx::IGFXDevice& gfx, u32 rgba) {
    return gfx.CreateTexture({
        .width  = 1,
        .height = 1,
        .format = gfx::Format::R8G8B8A8_UNORM,
        .usage  = gfx::TextureUsage::ShaderResource,
    }, &rgba);
}
}

TextureAssetManager::TextureAssetManager(gfx::IGFXDevice& gfx) : gfx_(gfx) {

    defaults_.White       = Make1x1(gfx_, 0xFFFFFFFFu);
    defaults_.Black       = Make1x1(gfx_, 0x00000000u);

    defaults_.FlatNormal  = Make1x1(gfx_, 0xFF008080u);

    defaults_.NeutralOrm  = Make1x1(gfx_, 0x0000FFFFu);
    defaults_.Missing     = Make1x1(gfx_, 0xFFFF00FFu);
}

TextureAssetManager::~TextureAssetManager() {

    for (auto& [k, e] : shared_) {
        if (e.handle != gfx::TextureHandle::Invalid) gfx_.Destroy(e.handle);
    }
    shared_.clear();
    for (auto& [name, h] : owned_) {
        if (h != gfx::TextureHandle::Invalid) gfx_.Destroy(h);
    }
    owned_.clear();
    gfx_.Destroy(defaults_.White);
    gfx_.Destroy(defaults_.Black);
    gfx_.Destroy(defaults_.FlatNormal);
    gfx_.Destroy(defaults_.NeutralOrm);
    gfx_.Destroy(defaults_.Missing);
    defaults_ = {};
}

void TextureAssetManager::RegisterOwned(std::string name, gfx::TextureHandle handle) {
    if (auto it = owned_.find(name); it != owned_.end()) {
        if (it->second != gfx::TextureHandle::Invalid && it->second != handle)
            gfx_.Destroy(it->second);
        if (handle == gfx::TextureHandle::Invalid) {
            owned_.erase(it);
            return;
        }
        it->second = handle;
        return;
    }
    if (handle != gfx::TextureHandle::Invalid)
        owned_.emplace(std::move(name), handle);
}

void TextureAssetManager::ReleaseOwned(std::string_view name) {
    auto it = owned_.find(name);
    if (it == owned_.end()) return;
    if (it->second != gfx::TextureHandle::Invalid) gfx_.Destroy(it->second);
    owned_.erase(it);
}

gfx::TextureHandle TextureAssetManager::GetOwned(std::string_view name) const noexcept {
    auto it = owned_.find(name);
    return (it != owned_.end()) ? it->second : gfx::TextureHandle::Invalid;
}

std::vector<TextureAssetManager::DebugEntry>
TextureAssetManager::DebugSnapshotOwned() const {
    std::vector<DebugEntry> out;
    out.reserve(owned_.size());
    for (const auto& [name, handle] : owned_) {
        out.push_back({name, handle});
    }
    return out;
}

gfx::TextureHandle
TextureAssetManager::AcquireShared(std::string_view        key,
                                   const gfx::TextureDesc& desc,
                                   const void*             pixels) {

    {
        std::lock_guard<std::mutex> lock(sharedMutex_);
        sharedTotalAcquires_++;
        if (auto it = shared_.find(key); it != shared_.end()) {
            it->second.refCount++;
            sharedCacheHits_++;
            return it->second.handle;
        }
    }

    gfx::TextureHandle h = gfx_.CreateTexture(desc, pixels);
    {
        std::lock_guard<std::mutex> lock(sharedMutex_);
        SharedEntry e;
        e.handle   = h;
        e.refCount = 1;
        auto [it, inserted] = shared_.emplace(std::string(key), e);
        if (!inserted) {

            gfx_.Destroy(h);
            it->second.refCount++;
            sharedCacheHits_++;
        }
        return it->second.handle;
    }
}

void TextureAssetManager::ReleaseShared(std::string_view key) {
    gfx::TextureHandle toDestroy = gfx::TextureHandle::Invalid;
    {
        std::lock_guard<std::mutex> lock(sharedMutex_);
        auto it = shared_.find(key);
        if (it == shared_.end()) return;
        if (it->second.refCount > 0) it->second.refCount--;
        if (it->second.refCount == 0) {
            toDestroy = it->second.handle;
            shared_.erase(it);
        }
    }
    if (toDestroy != gfx::TextureHandle::Invalid) gfx_.Destroy(toDestroy);
}

bool TextureAssetManager::IsCachedShared(std::string_view key) const {
    std::lock_guard<std::mutex> lock(sharedMutex_);
    return shared_.find(key) != shared_.end();
}

gfx::TextureHandle
TextureAssetManager::TryAcquireShared(std::string_view key) {
    std::lock_guard<std::mutex> lock(sharedMutex_);
    sharedTotalAcquires_++;
    auto it = shared_.find(key);
    if (it == shared_.end()) return gfx::TextureHandle::Invalid;
    it->second.refCount++;
    sharedCacheHits_++;
    return it->second.handle;
}

gfx::TextureHandle
TextureAssetManager::LookupShared(std::string_view key) const {
    std::lock_guard<std::mutex> lock(sharedMutex_);
    auto it = shared_.find(key);
    return (it != shared_.end()) ? it->second.handle : gfx::TextureHandle::Invalid;
}

TextureAssetManager::SharedStats
TextureAssetManager::GetSharedStats() const {
    std::lock_guard<std::mutex> lock(sharedMutex_);
    SharedStats s;
    s.uniqueEntries = shared_.size();
    s.totalAcquires = sharedTotalAcquires_;
    s.cacheHits     = sharedCacheHits_;
    return s;
}

void TextureAssetManager::ResetSharedStats() {
    std::lock_guard<std::mutex> lock(sharedMutex_);
    sharedTotalAcquires_ = 0;
    sharedCacheHits_     = 0;
}

std::unique_ptr<TextureAssetManager::ModelScope>
TextureAssetManager::CreateModelScope() {

    return std::unique_ptr<ModelScope>(new ModelScope(gfx_, *this));
}

TextureAssetManager::ModelScope::~ModelScope() {
    Clear();
}

void TextureAssetManager::ModelScope::Clear() {
    for (auto& [id, e] : entries_) {
        if (e.tex == gfx::TextureHandle::Invalid) continue;
        if (e.sharedKey.empty())
            gfx_.Destroy(e.tex);
        else
            mgr_.ReleaseShared(e.sharedKey);
    }
    entries_.clear();
}

void TextureAssetManager::ModelScope::DropEntry(i32 textureId) {
    auto it = entries_.find(textureId);
    if (it == entries_.end()) return;
    if (it->second.tex != gfx::TextureHandle::Invalid) {
        if (it->second.sharedKey.empty())
            gfx_.Destroy(it->second.tex);
        else
            mgr_.ReleaseShared(it->second.sharedKey);
    }
    entries_.erase(it);
}

gfx::TextureHandle
TextureAssetManager::ModelScope::Upload(i32                     textureId,
                                        const gfx::TextureDesc& desc,
                                        const void*             pixels,
                                        u32                wrapFlags) {

    DropEntry(textureId);
    auto& e = entries_[textureId];
    e.tex       = gfx_.CreateTexture(desc, pixels);
    e.wrapFlags = wrapFlags;
    e.sharedKey.clear();
    return e.tex;
}

gfx::TextureHandle
TextureAssetManager::ModelScope::UploadShared(i32                     textureId,
                                              std::string_view        sharedKey,
                                              const gfx::TextureDesc& desc,
                                              const void*             pixels,
                                              u32                wrapFlags) {

    DropEntry(textureId);
    auto& e = entries_[textureId];
    e.sharedKey.assign(sharedKey);
    e.tex       = mgr_.AcquireShared(e.sharedKey, desc, pixels);
    e.wrapFlags = wrapFlags;
    return e.tex;
}

gfx::TextureHandle
TextureAssetManager::ModelScope::BindShared(i32              textureId,
                                            std::string_view sharedKey,
                                            u32         wrapFlags) {

    DropEntry(textureId);
    gfx::TextureHandle h = mgr_.TryAcquireShared(sharedKey);
    if (h == gfx::TextureHandle::Invalid) {

        return gfx::TextureHandle::Invalid;
    }
    auto& e = entries_[textureId];
    e.sharedKey.assign(sharedKey);
    e.tex       = h;
    e.wrapFlags = wrapFlags;
    return h;
}

gfx::TextureHandle
TextureAssetManager::ModelScope::Get(i32 textureId) const noexcept {
    auto it = entries_.find(textureId);
    return (it != entries_.end()) ? it->second.tex : gfx::TextureHandle::Invalid;
}

u32 TextureAssetManager::ModelScope::WrapFlags(i32 textureId) const noexcept {
    auto it = entries_.find(textureId);
    return (it != entries_.end()) ? it->second.wrapFlags : kSamplerWrapBitsMask;
}

}
