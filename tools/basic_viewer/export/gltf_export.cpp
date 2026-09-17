// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "gltf_export.h"

#include "color_math.h"
#include "sc2_pbr_export.h"
#include "texture_codec.h"
#include "texture_io.h"

#include "io/wem/wem_export.h"
#include "io/wem/wem_profiles.h"
#include "renderer/profiles/sc2_heroes/sc2_team_colors.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/gltf/writer.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/textures/pbr_bake.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = ::whiteout::textures;
namespace wem = ::whiteout::models::wem;
namespace gltf = ::whiteout::models::gltf;

/// The join key the library stamps on every image it emits: `wem:texture:<i>`
/// names document texture `i` (gltf_core.h).
constexpr std::string_view kImageTextureKey = "wem:texture:";

/// A stored z is never negative, so its blue sits at 128 or above on every
/// texel; a map whose blue averages below this never stored one.
constexpr u64 kStoredZMinimumBlueMean = 64;

/// GLB chunks, and so the images packed into its binary buffer, align to 4.
constexpr std::size_t kGlbAlignment = 4;

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
    const std::span<const u8> pixels = texture.mipData(0);
    u64 blue = 0;
    for (std::size_t i = 2; i < pixels.size(); i += 4)
        blue += pixels[i];
    if (!pixels.empty() && blue / (pixels.size() / 4) < kStoredZMinimumBlueMean)
        texture.expandNormal(tx::Channel::R, tx::Channel::G, tx::Channel::B);
}

/// PNG-encode one decoded texture, restated for its binding.
EncodedTexture EncodePng(tx::Texture texture, NormalPacking packing) {
    EncodedTexture out;
    try {
        texture.format(tx::PixelFormat::RGBA8);
        RestateNormal(texture, packing);
        tx::png::Writer writer;
        out.bytes = writer.write(texture);
        if (out.bytes.empty())
            out.problem = "the PNG writer produced nothing";
    } catch (const std::exception& e) {
        out.bytes.clear();
        out.problem = e.what();
    }
    return out;
}

/// Resolve + decode + PNG-encode one document texture.
EncodedTexture EncodeAsPng(io::IContentProvider& provider, const wem::TextureRef& ref,
                           NormalPacking packing) {
    const std::optional<TextureFile> file = ReadTextureFile(provider, ContentRefOf(ref));
    if (!file)
        return {{}, "not readable"};
    TextureDecode decoded = DecodeTexture(file->bytes, file->extension);
    if (!decoded.texture)
        return {{}, decoded.problem};
    return EncodePng(std::move(*decoded.texture), packing);
}

/// The document texture index behind one glTF image, through its join key.
std::optional<u32> TextureBehindImage(const wem::Document& document, const gltf::Image& image) {
    if (image.name.rfind(kImageTextureKey, 0) != 0)
        return std::nullopt;
    const u32 index = static_cast<u32>(
        std::strtoul(image.name.c_str() + kImageTextureKey.size(), nullptr, 10));
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

/// Append @p png to buffer 0 at the next aligned offset and point @p image at it.
void EmbedImage(gltf::Asset& asset, gltf::Image& image, const std::vector<u8>& png) {
    if (asset.buffers.empty()) {
        asset.buffers.emplace_back();
    }
    std::vector<u8>& bin = asset.buffers[0].data;
    while (bin.size() % kGlbAlignment != 0)
        bin.push_back(0);
    gltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = static_cast<u32>(bin.size());
    view.byteLength = static_cast<u32>(png.size());
    bin.insert(bin.end(), png.begin(), png.end());
    asset.buffers[0].byteLength = static_cast<u32>(bin.size());
    asset.bufferViews.push_back(view);
    image.bufferView = static_cast<u32>(asset.bufferViews.size() - 1);
    image.mimeType = "image/png";
    image.uri.clear();
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
    const ExportSubject& subject = request.subject;
    const fs::path targetDir = subject.outPath.parent_path();
    const std::vector<NormalPacking> packings = NormalPackings(asset, authoredXInAlpha);
    TextureExportCounters& counters = report.textures;
    for (std::size_t imageIndex = 0; imageIndex < asset.images.size(); ++imageIndex) {
        gltf::Image& image = asset.images[imageIndex];
        const std::optional<u32> index = TextureBehindImage(document, image);
        if (!index) {
            ++counters.failed;
            continue;
        }
        EncodedTexture png{{}, "no provider"};
        if (const auto found = baked.find(*index); found != baked.end()) {
            png = EncodePng(found->second.texture, NormalPacking::None);
        } else if (subject.provider != nullptr) {
            png = EncodeAsPng(*subject.provider, document.textures[*index], packings[imageIndex]);
        }
        if (!png) {
            report.diagnostics.warn(wem::DiagCode::TextureUnresolved,
                                    "texture '" + wem::Describe(document.textures[*index]) +
                                        "' keeps its URI: " + png.problem);
            ++counters.failed;
            continue;
        }
        if (request.options.binary) {
            EmbedImage(asset, image, png.bytes);
        } else if (const fs::path outFile = targetDir / io::FsPathFromUtf8(image.uri);
                   !WriteFileCreatingDirs(outFile, png.bytes)) {
            report.diagnostics.warn(wem::DiagCode::Unspecified,
                                    "could not write '" + io::PathToUtf8(outFile) + "'");
            ++counters.failed;
            continue;
        }
        ++counters.exported;
    }
}

/// Write @p asset as `.glb`, or as `.gltf` with its buffer beside it. Empty on
/// success, otherwise why not.
std::string WriteAsset(gltf::Asset& asset, const fs::path& outPath, bool binary) {
    if (binary) {
        const std::vector<u8> bytes = gltf::Writer::ToGlb(asset);
        return WriteFileCreatingDirs(outPath, bytes) ? std::string()
                                                     : "could not write " + io::PathToUtf8(outPath);
    }
    // `.gltf` + `.bin`: the buffer needs a name for the JSON to reference.
    const std::string stem = io::PathToUtf8(outPath.stem());
    if (!asset.buffers.empty() && !asset.buffers[0].data.empty()) {
        asset.buffers[0].uri = stem + ".bin";
        asset.buffers[0].byteLength = static_cast<u32>(asset.buffers[0].data.size());
        const fs::path binPath = outPath.parent_path() / io::FsPathFromUtf8(stem + ".bin");
        if (!WriteFileCreatingDirs(binPath, asset.buffers[0].data))
            return "could not write " + io::PathToUtf8(binPath);
    }
    const std::string text = gltf::Writer::ToJsonText(asset);
    const std::span<const u8> json(reinterpret_cast<const u8*>(text.data()), text.size());
    return WriteFileCreatingDirs(outPath, json) ? std::string()
                                                : "could not write " + io::PathToUtf8(outPath);
}

} // namespace

GltfExportReport ExportModelAsGltf(const GltfExportRequest& request) {
    GltfExportReport report;
    const ExportSubject& subject = request.subject;
    const GltfExportOptions& options = request.options;
    if (subject.source == nullptr) {
        report.error = "no model to export";
        return report;
    }

    // StarCraft II states a surface as specular + gloss + an exponent, and glTF
    // as metallic-roughness, which is Reforged's vocabulary too. No slot
    // reference crosses that: the maps have to be baked, and Export to MDX's
    // bake is the one measured against the engine. Everything it crosses is
    // pixels, so it runs only with textures on.
    const bool bakeSurface = options.textures && subject.provider != nullptr;
    io::WemExportOptions wemOptions;
    // The bake reads StarCraft II's standard material, which a Heroes-only
    // MADD has to be reversed into first, as Export to MDX does.
    wemOptions.retargetHeroesToStarCraft2 = bakeSurface;
    std::optional<wem::Document> exported = ConvertThroughWem(subject, wemOptions, report);
    if (!exported)
        return report;
    wem::Document& document = *exported;
    if (!subject.modelName.empty())
        document.name = subject.modelName;

    const bool authoredSc2 =
        document.carries(wem::ProfileId::Sc2) || document.carries(wem::ProfileId::Heroes);
    Sc2PbrBakeResult baked;
    if (bakeSurface && authoredSc2 && !document.carries(wem::ProfileId::Wc3Reforged)) {
        // glTF has no team slot for a runtime to fill, so the colour on screen
        // is baked in: the palette entry the shading resolves the swatch to, in
        // linear light like the albedo it lerps.
        const Vector3f& swatch =
            renderer::profiles::sc2_heroes::ResolveSc2TeamColor(request.teamColor).diffuse;
        baked = BakeSc2AsReforgedPbr(document, subject.provider, tx::pbr::NormalRestatement{},
                                     report.diagnostics,
                                     Vector3f{color::SrgbToLinear(swatch.x),
                                              color::SrgbToLinear(swatch.y),
                                              color::SrgbToLinear(swatch.z)});
    }
    // A derive that failed leaves its profile declared with no materials behind
    // it, and the authored slots are better than none.
    const auto hasMaterials = [&](wem::ProfileId id) {
        return document.carries(id) &&
               std::any_of(document.models.begin(), document.models.end(),
                           [id](const wem::Model& model) { return model.setFor(id) != nullptr; });
    };

    // Export takes any carried profile (GLTF_DESIGN §2): Reforged wherever a
    // `.mdx` carries it or the bake above derived it, then the profile the
    // model was authored in. A `.mdx` declares classic first whenever one
    // material has a classic layer, so the default alone exported heroleonid as
    // its one alpha-hidden SD geoset — while the viewer draws any model with an
    // HD layer in HD, and glTF's PBR is Reforged's own vocabulary.
    report.profile = hasMaterials(wem::ProfileId::Wc3Reforged)   ? wem::ProfileId::Wc3Reforged
                     : document.carries(document.defaultProfile) ? document.defaultProfile
                     : !document.profiles.empty()                ? document.profiles.front()
                                                                 : wem::ProfileId::Generic;

    const wem::GltfConverter converter;
    wem::Result<gltf::Asset> converted = converter.toGltf(document, report.profile);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = "the glTF conversion refused; see the diagnostics";
        return report;
    }
    gltf::Asset asset = converted.take();

    if (options.textures)
        ExportImages(request, document, baked.baked, authoredSc2, asset, report);

    report.error = WriteAsset(asset, subject.outPath, options.binary);
    if (!report.error.empty())
        return report;

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
