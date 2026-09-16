// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "gltf_export.h"

#include "io/wem/wem_profiles.h"
#include "renderer/model/model_source_utils.h"
#include "renderer/profiles/sc2_heroes/sc2_team_colors.h"
#include "sc2_pbr_export.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/gltf/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/textures/pbr_bake.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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

/// How a normal map's pixels are laid out on the way in, for the restatement
/// glTF's `normalTexture` needs: tangent-space x, y and z in red, green, blue.
enum class NormalPacking : u8 {
    None,     ///< Not bound as a normal map; the pixels cross as they are.
    Auto,     ///< Whatever the texels say — see `RestateNormal`.
    XInAlpha, ///< StarCraft II's DXT5nm: x in alpha, y in green, no z.
};

/// Restate @p texture as glTF reads a normal map. StarCraft II's maps are
/// DXT5nm (`m3_standard.slang` decodes `.ag`), and a two-channel map (BC5, as
/// Reforged ships) decodes with blue at zero — glTF reads both as normals
/// pointing into the surface. Only the packing changes: `m3_standard.slang`
/// builds its bitangent as `cross(n, t) * t.w`, which is glTF's own frame.
void RestateNormal(tx::Texture& texture, NormalPacking packing) {
    if (packing == NormalPacking::None)
        return;
    if (packing == NormalPacking::XInAlpha) {
        if (std::optional<tx::Texture> restated =
                tx::pbr::ConvertNormalXInAlpha(texture, tx::pbr::NormalRestatement{}))
            texture = std::move(*restated);
        return;
    }
    // A stored z is never negative, so its blue sits at 128 or above on every
    // texel; a map that averages far below that never stored one.
    const std::span<const u8> pixels = texture.mipData(0);
    u64 blue = 0;
    for (std::size_t i = 2; i < pixels.size(); i += 4)
        blue += pixels[i];
    if (!pixels.empty() && blue / (pixels.size() / 4) < 64)
        texture.expandNormal(tx::Channel::R, tx::Channel::G, tx::Channel::B);
}

f32 SrgbToLinear(f32 c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// PNG-encode one decoded texture. Empty on any failure.
std::optional<std::vector<u8>> EncodePng(tx::Texture texture, NormalPacking packing) {
    try {
        texture.format(tx::PixelFormat::RGBA8);
        RestateNormal(texture, packing);
        tx::png::Writer writer;
        std::vector<u8> encoded = writer.write(texture);
        if (encoded.empty())
            return std::nullopt;
        return encoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Resolve + decode + PNG-encode one texture. Empty on any failure.
std::optional<std::vector<u8>> EncodeAsPng(io::IContentProvider& provider,
                                           const wem::TextureRef& ref, NormalPacking packing) {
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
        return EncodePng(std::move(*decoded), packing);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// The document texture index behind one glTF image, through the
/// `wem:texture:<i>` join key the library stamps on every image it emits
/// (gltf_core.h).
std::optional<u32> TextureBehindImage(const wem::Document& document, const gltf::Image& image) {
    constexpr const char* kPrefix = "wem:texture:";
    if (image.name.rfind(kPrefix, 0) != 0)
        return std::nullopt;
    const u32 index = static_cast<u32>(
        std::strtoul(image.name.c_str() + std::strlen(kPrefix), nullptr, 10));
    if (index >= document.textures.size())
        return std::nullopt;
    return index;
}

/// How each image's pixels have to be restated, from the slots that bind it. An
/// image a material also samples as colour or data keeps its pixels: rewriting
/// them for the normal binding would break the other.
std::vector<NormalPacking> NormalPackings(const gltf::Asset& asset, bool authoredXInAlpha) {
    std::vector<u8> normalUse(asset.images.size(), 0);
    std::vector<u8> otherUse(asset.images.size(), 0);
    const auto mark = [&](const gltf::TextureInfo& info, std::vector<u8>& use) {
        if (!info.present() || info.index >= asset.textures.size())
            return;
        const u32 source = asset.textures[info.index].source;
        if (source < use.size())
            use[source] = 1;
    };
    for (const gltf::Material& material : asset.materials) {
        mark(material.normalTexture, normalUse);
        mark(material.pbr.baseColorTexture, otherUse);
        mark(material.pbr.metallicRoughnessTexture, otherUse);
        mark(material.occlusionTexture, otherUse);
        mark(material.emissiveTexture, otherUse);
    }
    const NormalPacking packing = authoredXInAlpha ? NormalPacking::XInAlpha : NormalPacking::Auto;
    std::vector<NormalPacking> out(asset.images.size(), NormalPacking::None);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (normalUse[i] != 0 && otherUse[i] == 0)
            out[i] = packing;
    }
    return out;
}

/// Resolve every image and either embed it in buffer 0 (`.glb`) or write it
/// beside the model (`.gltf`). A failure keeps the suggested URI — the file
/// still validates, and the name says what to drop in by hand.
///
/// A texture the surface bake wrote comes from @p baked instead of the provider,
/// since it names a file nobody has. Those are already in glTF's layout, normals
/// included, so only a texture read from a file is restated.
void ExportImages(const GltfExportRequest& request, const wem::Document& document,
                  const std::map<u32, BakedTexture>& baked, bool authoredXInAlpha,
                  gltf::Asset& asset, GltfExportReport& report) {
    const fs::path targetDir = request.outPath.parent_path();
    const std::vector<NormalPacking> packings = NormalPackings(asset, authoredXInAlpha);
    for (std::size_t imageIndex = 0; imageIndex < asset.images.size(); ++imageIndex) {
        gltf::Image& image = asset.images[imageIndex];
        const std::optional<u32> index = TextureBehindImage(document, image);
        if (!index) {
            ++report.texturesFailed;
            continue;
        }
        std::optional<std::vector<u8>> png;
        if (const auto found = baked.find(*index); found != baked.end()) {
            png = EncodePng(found->second.texture, NormalPacking::None);
        } else if (request.provider != nullptr) {
            png = EncodeAsPng(*request.provider, document.textures[*index], packings[imageIndex]);
        }
        if (!png) {
            std::fprintf(stderr, "[viewer] Export glTF: texture not readable: %s\n",
                         wem::Describe(document.textures[*index]).c_str());
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

    // StarCraft II states a surface as specular + gloss + an exponent, and glTF
    // as metallic-roughness, which is Reforged's vocabulary too. No slot
    // reference crosses that: the maps have to be baked, and Export to MDX's
    // bake is the one measured against the engine. Everything it crosses is
    // pixels, so it runs only with textures on, and only when the caller did
    // not ask for the source profile's own slots.
    const bool bakeSurface = request.exportTextures && request.provider != nullptr &&
                             (request.profile == wem::ProfileId::Count ||
                              request.profile == wem::ProfileId::Wc3Reforged);
    io::WemExportOptions wemOptions;
    // The bake reads StarCraft II's standard material, which a Heroes-only
    // MADD has to be reversed into first, as Export to MDX does.
    wemOptions.retargetHeroesToStarCraft2 = bakeSurface;
    io::WemExportResult wemResult =
        io::ExportModelToWem(*request.source, request.provider, wemOptions);
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

    const bool authoredSc2 =
        document.carries(wem::ProfileId::Sc2) || document.carries(wem::ProfileId::Heroes);
    Sc2PbrBakeResult baked;
    if (bakeSurface && authoredSc2 && !document.carries(wem::ProfileId::Wc3Reforged)) {
        // glTF has no team slot for a runtime to fill, so the colour on screen
        // is baked in: the palette entry the shading resolves the swatch to, in
        // linear light like the albedo it lerps.
        const Vector3f& swatch =
            renderer::profiles::sc2_heroes::ResolveSc2TeamColor(request.teamColor).diffuse;
        baked = BakeSc2AsReforgedPbr(
            document, request.provider, tx::pbr::NormalRestatement{}, report.diagnostics,
            Vector3f{SrgbToLinear(swatch.x), SrgbToLinear(swatch.y), SrgbToLinear(swatch.z)});
    }
    // A derive that failed leaves its profile declared with no materials behind
    // it, and the authored slots are better than none.
    const auto hasMaterials = [&](wem::ProfileId id) {
        return document.carries(id) &&
               std::any_of(document.models.begin(), document.models.end(),
                           [id](const wem::Model& model) { return model.setFor(id) != nullptr; });
    };

    // Export takes any carried profile (GLTF_DESIGN §2). With no opinion it
    // takes Reforged wherever a `.mdx` carries it or the bake above derived it,
    // then the profile the model was authored in. A `.mdx` declares classic
    // first whenever one material has a classic layer, so the default alone
    // exported heroleonid as its one alpha-hidden SD geoset — while the viewer
    // draws any model with an HD layer in HD, and glTF's PBR is Reforged's own
    // vocabulary.
    wem::ProfileId profile = request.profile;
    if (profile == wem::ProfileId::Count || !hasMaterials(profile)) {
        profile = hasMaterials(wem::ProfileId::Wc3Reforged)   ? wem::ProfileId::Wc3Reforged
                  : document.carries(document.defaultProfile) ? document.defaultProfile
                  : !document.profiles.empty()                ? document.profiles.front()
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
        ExportImages(request, document, baked.baked, authoredSc2, asset, report);

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
