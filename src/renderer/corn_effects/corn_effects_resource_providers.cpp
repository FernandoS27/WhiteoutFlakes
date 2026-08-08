#include "renderer/corn_effects/corn_effects_resource_providers.h"

#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"

#include <cstring>
#include <filesystem>
#include <utility>

namespace whiteout::flakes::renderer::corn_effects {

namespace {

std::string NormalizeKey(std::string_view path) {
    return model::NormalizeTextureKey(path);
}

} // namespace

// ---------------------------------------------------------------- mesh / vf

void CornEffectsMeshProvider::SetContentProvider(io::IContentProvider* content) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (content_ == content)
        return;
    content_ = content;
    // Cached misses were recorded against the previous provider's search
    // scope, so a source change has to get another chance at them. Hits are
    // dropped too: spans handed out earlier stay valid only while the buffer
    // lives, so this is safe exclusively before any runtime binds — which is
    // where the scene wires its provider.
    cache_.clear();
}

std::span<const std::byte> CornEffectsMeshProvider::readMesh(std::string_view path) {
    if (path.empty())
        return {};

    const std::string key = NormalizeKey(path);

    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = cache_.find(key); it != cache_.end())
        return {it->second.data(), it->second.size()};
    if (!content_)
        return {};

    // Insert first, unconditionally: an empty entry is the cached miss, so a
    // resource that does not resolve costs one read rather than one per spawn.
    auto& slot = cache_[key];
    if (auto bytes = content_->ReadFile(key); bytes && !bytes->empty()) {
        slot.resize(bytes->size());
        std::memcpy(slot.data(), bytes->data(), bytes->size());
    }
    return {slot.data(), slot.size()};
}

// ------------------------------------------------------------------ texture

void CornEffectsTextureProvider::SetContentProvider(io::IContentProvider* content) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (content_ == content)
        return;
    content_ = content;
    cache_.clear();
}

::whiteout::cornflakes::TextureImageDesc CornEffectsTextureProvider::readTexture(
    std::string_view path) {
    using ::whiteout::cornflakes::TextureImageDesc;
    using ::whiteout::cornflakes::TextureSourceFormat;

    if (path.empty())
        return {};

    const std::string key = NormalizeKey(path);

    std::lock_guard<std::mutex> lock(mutex_);
    auto emit = [](const Image& img) {
        TextureImageDesc desc;
        desc.texels = {img.texels.data(), img.texels.size()};
        desc.width = img.width;
        desc.height = img.height;
        desc.format = img.format;
        desc.srgb = img.srgb;
        return desc;
    };

    if (const auto it = cache_.find(key); it != cache_.end())
        return emit(it->second);
    if (!content_)
        return {};

    auto& img = cache_[key]; // empty entry doubles as the cached miss

    std::string foundExt;
    auto bytes = content_->ReadFile(key, &foundExt);
    if (!bytes || bytes->empty())
        return emit(img);
    if (foundExt.empty())
        foundExt = model::ExtensionLower(std::filesystem::path(key));

    auto decoded = model::DispatchTextureParser(
        foundExt, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
    if (!decoded)
        return emit(img);

    // BC1 is the one format cornflakes samples without expanding, matching the
    // engine — everything else it wants uncompressed, and RGBA8 is the only
    // uncompressed target WhiteoutLib transcodes to.
    using PF = whiteout::textures::PixelFormat;
    if (decoded->format() == PF::BC1) {
        img.format = TextureSourceFormat::Dxt1;
    } else {
        decoded->format(PF::RGBA8);
        img.format = TextureSourceFormat::Rgba8;
    }

    const auto mip0 = decoded->mipData(0);
    if (mip0.empty())
        return emit(img);

    img.texels.resize(mip0.size());
    std::memcpy(img.texels.data(), mip0.data(), mip0.size());
    img.width = decoded->width();
    img.height = decoded->height();
    img.srgb = decoded->isSrgb();
    return emit(img);
}

} // namespace whiteout::flakes::renderer::corn_effects
