// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "gltf_export.h"

#include "io/wem/wem_profiles.h"
#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/gltf/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = whiteout::textures;
namespace gltf = ::whiteout::models::gltf;

/// The loader-shaped reference for one document texture — path, fileDataID or
/// SNO id (which is a fileDataID in all but name).
ContentRef RefForTexture(const wem::TextureRef& ref) {
    if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key))
        return ContentRef::FromFileId(fileId->value);
    if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key))
        return ContentRef::FromFileId(sno->id);
    return ContentRef::FromPath(ref.path);
}

/// Resolve + decode + PNG-encode one texture. Empty on any failure.
std::optional<std::vector<u8>> EncodeAsPng(io::IContentProvider& provider,
                                           const wem::TextureRef& ref) {
    std::string actualExt;
    std::optional<std::vector<u8>> bytes = provider.ReadFile(RefForTexture(ref), &actualExt);
    if (!bytes || bytes->empty())
        return std::nullopt;
    for (char& c : actualExt)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (actualExt.empty()) {
        actualExt = renderer::model::SniffTextureExtension(
            std::span<const u8>(bytes->data(), bytes->size()));
    }
    try {
        std::optional<tx::Texture> decoded = renderer::model::DispatchTextureParser(
            actualExt, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        if (!decoded)
            return std::nullopt;
        decoded->format(tx::PixelFormat::RGBA8);
        tx::png::Writer writer;
        std::vector<u8> encoded = writer.write(*decoded);
        if (encoded.empty())
            return std::nullopt;
        return encoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// The document texture behind one glTF image, through the `wem:texture:<i>`
/// join key the library stamps on every image it emits (gltf_core.h).
const wem::TextureRef* TextureBehindImage(const wem::Document& document,
                                          const gltf::Image& image) {
    constexpr const char* kPrefix = "wem:texture:";
    if (image.name.rfind(kPrefix, 0) != 0)
        return nullptr;
    const u32 index = static_cast<u32>(
        std::strtoul(image.name.c_str() + std::strlen(kPrefix), nullptr, 10));
    return index < document.textures.size() ? &document.textures[index] : nullptr;
}

/// Resolve every image and either embed it in buffer 0 (`.glb`) or write it
/// beside the model (`.gltf`). A failure keeps the suggested URI — the file
/// still validates, and the name says what to drop in by hand.
void ExportImages(const GltfExportRequest& request, const wem::Document& document,
                  gltf::Asset& asset, GltfExportReport& report) {
    const fs::path targetDir = request.outPath.parent_path();
    for (gltf::Image& image : asset.images) {
        const wem::TextureRef* ref = TextureBehindImage(document, image);
        if (ref == nullptr || request.provider == nullptr) {
            ++report.texturesFailed;
            continue;
        }
        std::optional<std::vector<u8>> png = EncodeAsPng(*request.provider, *ref);
        if (!png) {
            std::fprintf(stderr, "[viewer] Export glTF: texture not readable: %s\n",
                         wem::Describe(*ref).c_str());
            ++report.texturesFailed;
            continue;
        }
        if (request.binary) {
            if (asset.buffers.empty()) {
                asset.buffers.emplace_back();
            }
            std::vector<u8>& bin = asset.buffers[0].data;
            while (bin.size() % 4 != 0)
                bin.push_back(0);
            gltf::BufferView view;
            view.buffer = 0;
            view.byteOffset = static_cast<u32>(bin.size());
            view.byteLength = static_cast<u32>(png->size());
            bin.insert(bin.end(), png->begin(), png->end());
            asset.buffers[0].byteLength = static_cast<u32>(bin.size());
            asset.bufferViews.push_back(view);
            image.bufferView = static_cast<u32>(asset.bufferViews.size() - 1);
            image.mimeType = "image/png";
            image.uri.clear();
        } else {
            const fs::path outFile = targetDir / io::FsPathFromUtf8(image.uri);
            std::error_code ec;
            fs::create_directories(outFile.parent_path(), ec);
            std::ofstream file(outFile, std::ios::binary | std::ios::trunc);
            if (file)
                file.write(reinterpret_cast<const char*>(png->data()),
                           static_cast<std::streamsize>(png->size()));
            if (!file) {
                ++report.texturesFailed;
                continue;
            }
        }
        ++report.texturesExported;
    }
}

} // namespace

GltfExportReport ExportModelAsGltf(const GltfExportRequest& request) {
    GltfExportReport report;
    if (request.source == nullptr) {
        report.error = "no model to export";
        return report;
    }

    io::WemExportResult wemResult = io::ExportModelToWem(*request.source, request.provider, {});
    report.formatId = wemResult.formatId;
    report.diagnostics = wemResult.diagnostics;
    if (!wemResult.ok()) {
        report.error = wemResult.error;
        return report;
    }
    wem::Document document = std::move(*wemResult.document);
    if (!request.modelName.empty())
        document.name = request.modelName;
    if (request.scale != 1.0f)
        wem::RescaleDocument(document, request.scale);

    // Export takes any carried profile (GLTF_DESIGN §2); with no opinion the
    // document's own default — the profile the model was authored in — stands.
    wem::ProfileId profile = request.profile;
    if (profile == wem::ProfileId::Count || !document.carries(profile)) {
        profile = document.carries(document.defaultProfile) ? document.defaultProfile
                  : !document.profiles.empty()              ? document.profiles.front()
                                                            : wem::ProfileId::Generic;
    }
    report.profile = profile;

    const wem::GltfConverter converter;
    wem::Result<gltf::Asset> converted = converter.toGltf(document, profile);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = "the glTF conversion refused; see the diagnostics";
        return report;
    }
    gltf::Asset asset = converted.take();

    if (request.exportTextures)
        ExportImages(request, document, asset, report);

    std::error_code ec;
    fs::create_directories(request.outPath.parent_path(), ec);
    if (request.binary) {
        const std::vector<u8> bytes = gltf::Writer::ToGlb(asset);
        std::ofstream file(request.outPath, std::ios::binary | std::ios::trunc);
        if (file)
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            report.error = "could not write " + io::PathToUtf8(request.outPath);
            return report;
        }
    } else {
        // `.gltf` + `.bin`: the buffer needs a name for the JSON to reference.
        const std::string stem = io::PathToUtf8(request.outPath.stem());
        if (!asset.buffers.empty() && !asset.buffers[0].data.empty()) {
            asset.buffers[0].uri = stem + ".bin";
            asset.buffers[0].byteLength = static_cast<u32>(asset.buffers[0].data.size());
            const fs::path binPath = request.outPath.parent_path() / (stem + ".bin");
            std::ofstream bin(binPath, std::ios::binary | std::ios::trunc);
            if (bin)
                bin.write(reinterpret_cast<const char*>(asset.buffers[0].data.data()),
                          static_cast<std::streamsize>(asset.buffers[0].data.size()));
            if (!bin) {
                report.error = "could not write " + io::PathToUtf8(binPath);
                return report;
            }
        }
        const std::string text = gltf::Writer::ToJsonText(asset);
        std::ofstream file(request.outPath, std::ios::binary | std::ios::trunc);
        if (file)
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            report.error = "could not write " + io::PathToUtf8(request.outPath);
            return report;
        }
    }

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
