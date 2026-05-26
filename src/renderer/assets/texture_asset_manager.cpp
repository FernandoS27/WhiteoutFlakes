#include "renderer/assets/texture_asset_manager.h"

#include "renderer/assets/asset_manager.h"

namespace whiteout::flakes::renderer::assets {

namespace {

gfx::TextureHandle Make1x1(gfx::IGFXDevice& gfx, u32 rgba) {
    return gfx.CreateTexture(
        {
            .width = 1,
            .height = 1,
            .format = gfx::Format::R8G8B8A8_UNORM,
            .usage = gfx::TextureUsage::ShaderResource,
        },
        &rgba);
}
} // namespace

TextureAssetManager::TextureAssetManager(gfx::IGFXDevice& gfx) : gfx_(gfx) {

    defaults_.White = Make1x1(gfx_, 0xFFFFFFFFu);
    defaults_.Black = Make1x1(gfx_, 0x00000000u);

    defaults_.FlatNormal = Make1x1(gfx_, 0xFF008080u);

    defaults_.NeutralOrm = Make1x1(gfx_, 0x0000FFFFu);
    defaults_.Missing = Make1x1(gfx_, 0xFFFF00FFu);
}

TextureAssetManager::~TextureAssetManager() {
    for (auto& [name, h] : owned_) {
        if (h != gfx::TextureHandle::Invalid)
            gfx_.Destroy(h);
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
    if (it == owned_.end())
        return;
    if (it->second != gfx::TextureHandle::Invalid)
        gfx_.Destroy(it->second);
    owned_.erase(it);
}

gfx::TextureHandle TextureAssetManager::GetOwned(std::string_view name) const noexcept {
    auto it = owned_.find(name);
    return (it != owned_.end()) ? it->second : gfx::TextureHandle::Invalid;
}

std::vector<TextureAssetManager::DebugEntry> TextureAssetManager::DebugSnapshotOwned() const {
    std::vector<DebugEntry> out;
    out.reserve(owned_.size());
    for (const auto& [name, handle] : owned_) {
        out.push_back({name, handle});
    }
    return out;
}


std::unique_ptr<TextureAssetManager::ModelScope> TextureAssetManager::CreateModelScope(
    AssetManager* assets) {
    return std::unique_ptr<ModelScope>(new ModelScope(gfx_, *this, assets));
}

TextureAssetManager::ModelScope::~ModelScope() {
    Clear();
}

void TextureAssetManager::ModelScope::Clear() {
    for (auto& [id, e] : entries_) {
        if (e.slot != 0 && assets_) {
            assets_->Release(e.slot);
        } else if (e.tex != gfx::TextureHandle::Invalid) {
            gfx_.Destroy(e.tex);
        }
    }
    entries_.clear();
}

void TextureAssetManager::ModelScope::DropEntry(i32 textureId) {
    auto it = entries_.find(textureId);
    if (it == entries_.end())
        return;
    if (it->second.slot != 0 && assets_) {
        assets_->Release(it->second.slot);
    } else if (it->second.tex != gfx::TextureHandle::Invalid) {
        gfx_.Destroy(it->second.tex);
    }
    entries_.erase(it);
}

gfx::TextureHandle TextureAssetManager::ModelScope::Upload(i32 textureId,
                                                           const gfx::TextureDesc& desc,
                                                           const void* pixels, u32 wrapFlags) {

    DropEntry(textureId);
    auto& e = entries_[textureId];
    e.tex       = gfx_.CreateTexture(desc, pixels);
    e.slot      = 0;
    e.wrapFlags = wrapFlags;
    return e.tex;
}

void TextureAssetManager::ModelScope::BindSlot(i32 textureId,
                                               std::uint32_t slot,
                                               u32 wrapFlags) {
    auto it = entries_.find(textureId);
    if (it != entries_.end() && it->second.slot == slot && slot != 0) {
        it->second.wrapFlags = wrapFlags;
        return;
    }
    DropEntry(textureId);
    auto& e     = entries_[textureId];
    e.tex       = gfx::TextureHandle::Invalid;
    e.slot      = slot;
    e.wrapFlags = wrapFlags;
    // Caller obtained `slot` via AssetManager::Acquire, so its refcount
    // is already >= 1. ModelScope adopts that reference; DropEntry
    // issues the matching Release.
}

gfx::TextureHandle TextureAssetManager::ModelScope::Get(i32 textureId) const noexcept {
    auto it = entries_.find(textureId);
    if (it == entries_.end()) return gfx::TextureHandle::Invalid;
    const auto& e = it->second;
    if (e.slot != 0 && assets_)
        return assets_->TextureOf(e.slot);
    return e.tex;
}

u32 TextureAssetManager::ModelScope::WrapFlags(i32 textureId) const noexcept {
    auto it = entries_.find(textureId);
    return (it != entries_.end()) ? it->second.wrapFlags : kSamplerWrapBitsMask;
}

} // namespace whiteout::flakes::renderer::assets
