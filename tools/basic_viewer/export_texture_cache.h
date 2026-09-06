#pragma once

// ============================================================================
// The texture cache every cross-format export driver wants: resolve a source
// path to its `Document::textures` index, decode it once, intern the textures
// the bake invents. Extracted for `wc3_to_sc2_export`; `sc2_pbr_export.cpp`
// still carries its original local copy and can adopt this one when it is
// next touched — the two are the same shape on purpose.
// ============================================================================

#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/wem/document.h>
#include <whiteout/textures/texture.h>

#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace whiteout::flakes {

class ExportTextureCache {
public:
    ExportTextureCache(::whiteout::models::wem::Document& document, io::IContentProvider* provider)
        : document_(document), provider_(provider) {
        for (u32 i = 0; i < static_cast<u32>(document.textures.size()); ++i) {
            byPath_.emplace(Key(PathOf(document.textures[i])), i);
        }
    }

    /// The `Document::textures` index for @p path, or `kInvalidIndex`.
    u32 IndexOf(const std::string& path) const {
        const auto it = byPath_.find(Key(path));
        return it == byPath_.end() ? ::whiteout::models::wem::kInvalidIndex : it->second;
    }

    /// The decoded texture behind @p path, or null. Failures cache too.
    const ::whiteout::textures::Texture* Decode(const std::string& path) {
        const std::string key = Key(path);
        if (key.empty() || provider_ == nullptr) {
            return nullptr;
        }
        const auto cached = decoded_.find(key);
        if (cached != decoded_.end()) {
            return cached->second.has_value() ? &*cached->second : nullptr;
        }
        std::optional<::whiteout::textures::Texture> texture;
        std::string extension;
        std::optional<std::vector<u8>> bytes =
            provider_->ReadFile(ContentRef::FromPath(Trim(path)), &extension);
        if (bytes && !bytes->empty()) {
            extension = Lower(extension);
            if (extension.empty()) {
                extension = renderer::model::SniffTextureExtension(
                    std::span<const u8>(bytes->data(), bytes->size()));
            }
            texture = renderer::model::DispatchTextureParser(
                extension, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        }
        const auto inserted = decoded_.emplace(key, std::move(texture)).first;
        return inserted->second.has_value() ? &*inserted->second : nullptr;
    }

    /// Adds a texture the source never had and returns its document index.
    u32 Intern(const std::string& path) {
        const u32 existing = IndexOf(path);
        if (existing != ::whiteout::models::wem::kInvalidIndex) {
            return existing;
        }
        ::whiteout::models::wem::TextureRef ref;
        ref.key = ::whiteout::models::wem::TexturePath{path};
        ref.path = path;
        ref.declaredSpace = ::whiteout::models::wem::ColorSpace::Linear;
        document_.textures.push_back(std::move(ref));
        const u32 index = static_cast<u32>(document_.textures.size() - 1);
        byPath_.emplace(Key(path), index);
        return index;
    }

private:
    static std::string Trim(std::string value) {
        while (!value.empty() && (value.back() == '\0' || value.back() == ' ')) {
            value.pop_back();
        }
        return value;
    }
    static std::string Lower(std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }
    static std::string Key(const std::string& path) {
        return Lower(Trim(path));
    }
    static std::string PathOf(const ::whiteout::models::wem::TextureRef& ref) {
        if (const auto* path = std::get_if<::whiteout::models::wem::TexturePath>(&ref.key)) {
            return path->value;
        }
        return ref.path;
    }

    ::whiteout::models::wem::Document& document_;
    io::IContentProvider* provider_ = nullptr;
    std::unordered_map<std::string, u32> byPath_;
    std::unordered_map<std::string, std::optional<::whiteout::textures::Texture>> decoded_;
};

} // namespace whiteout::flakes
