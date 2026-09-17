// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "texture_io.h"

#include "export_text.h"

#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <system_error>
#include <variant>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = ::whiteout::textures;
namespace wem = ::whiteout::models::wem;

std::string PathOf(const wem::TextureRef& ref) {
    if (const auto* path = std::get_if<wem::TexturePath>(&ref.key)) {
        return path->value;
    }
    return ref.path;
}

std::string CacheKey(const std::string& path) {
    return export_text::Lower(export_text::TrimFixedWidth(path));
}

} // namespace

std::string TextureIdKey(const wem::TextureRef& ref) {
    if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key)) {
        return "#" + std::to_string(sno->id);
    }
    if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key)) {
        return "#" + std::to_string(fileId->value);
    }
    return {};
}

ContentRef ContentRefForKey(std::string_view key) {
    if (key.size() > 1 && key[0] == '#') {
        const std::string digits(key.substr(1));
        return ContentRef::FromFileId(static_cast<u32>(std::strtoul(digits.c_str(), nullptr, 10)));
    }
    return ContentRef::FromPath(std::string(key));
}

ContentRef ContentRefOf(const wem::TextureRef& ref) {
    if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key)) {
        return ContentRef::FromFileId(fileId->value);
    }
    if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key)) {
        return ContentRef::FromFileId(sno->id);
    }
    return ContentRef::FromPath(ref.path);
}

std::optional<TextureFile> ReadTextureFile(io::IContentProvider& provider, const ContentRef& ref) {
    TextureFile file;
    std::optional<std::vector<u8>> bytes = provider.ReadFile(ref, &file.extension);
    if (!bytes || bytes->empty()) {
        return std::nullopt;
    }
    file.bytes = std::move(*bytes);
    if (file.extension.empty()) {
        file.extension = renderer::model::SniffTextureExtension(file.bytes);
    }
    return file;
}

TextureDecode DecodeTexture(std::span<const u8> bytes, std::string_view extension) {
    TextureDecode decode;
    const std::string lowered = export_text::Lower(std::string(extension));
    try {
        decode.texture = renderer::model::DispatchTextureParser(
            lowered, [&](auto& parser) { return parser.parse(bytes); });
        if (!decode.texture) {
            decode.problem = "no decoder reads '" + lowered + "'";
        }
    } catch (const std::exception& e) {
        decode.texture.reset();
        decode.problem = std::string("did not decode: ") + e.what();
    }
    return decode;
}

TextureDecode ReadDecodedTexture(io::IContentProvider& provider, const ContentRef& ref) {
    std::optional<TextureFile> file = ReadTextureFile(provider, ref);
    if (!file) {
        return {std::nullopt, "not readable"};
    }
    return DecodeTexture(file->bytes, file->extension);
}

bool WriteFileCreatingDirs(const fs::path& path, std::span<const u8> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (file) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(file);
}

ExportTextureCache::ExportTextureCache(wem::Document& document, io::IContentProvider* provider)
    : document_(document), provider_(provider) {
    for (u32 i = 0; i < static_cast<u32>(document.textures.size()); ++i) {
        byPath_.emplace(CacheKey(PathOf(document.textures[i])), i);
    }
}

u32 ExportTextureCache::IndexOf(const std::string& path) const {
    const auto it = byPath_.find(CacheKey(path));
    return it == byPath_.end() ? wem::kInvalidIndex : it->second;
}

const tx::Texture* ExportTextureCache::Decode(const std::string& path) {
    const std::string key = CacheKey(path);
    if (key.empty() || provider_ == nullptr) {
        return nullptr;
    }
    auto cached = decoded_.find(key);
    if (cached == decoded_.end()) {
        cached = decoded_
                     .emplace(key, ReadDecodedTexture(*provider_, ContentRef::FromPath(
                                                                      export_text::TrimFixedWidth(path)))
                                       .texture)
                     .first;
    }
    return cached->second.has_value() ? &*cached->second : nullptr;
}

u32 ExportTextureCache::Intern(const std::string& path) {
    const u32 existing = IndexOf(path);
    if (existing != wem::kInvalidIndex) {
        return existing;
    }
    wem::TextureRef ref;
    ref.key = wem::TexturePath{path};
    ref.path = path;
    ref.declaredSpace = wem::ColorSpace::Linear;
    document_.textures.push_back(std::move(ref));
    const u32 index = static_cast<u32>(document_.textures.size() - 1);
    byPath_.emplace(CacheKey(path), index);
    return index;
}

} // namespace whiteout::flakes
