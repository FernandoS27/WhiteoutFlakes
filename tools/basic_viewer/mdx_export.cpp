// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdx_export.h"

#include "io/wem/wem_profiles.h"
#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/mdx/writer.h>
#include <whiteout/textures/blp/writer.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = whiteout::textures;

std::string Lower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

/// The reference the loader would resolve this name through. `#<id>` is
/// `ContentRef::Describe`'s spelling of a fileDataID or a SNO, and it is what a
/// World of Warcraft or Diablo III texture arrives as — those formats address a
/// texture by id and never by name.
ContentRef RefForTextureName(const std::string& name) {
    if (name.size() > 1 && name[0] == '#')
        return ContentRef::FromFileId(
            static_cast<u32>(std::strtoul(name.c_str() + 1, nullptr, 10)));
    return ContentRef::FromPath(name);
}

/// Whether any texel is not fully opaque. Decides BC1 against BC3, and whether
/// a BLP1 needs an alpha plane at all — a paletted BLP with eight bits of alpha
/// is twice the size of one without, and most model textures have none.
bool HasAlpha(const tx::Texture& texture) {
    const std::span<const u8> pixels = texture.mipData(0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0xFF)
            return true;
    }
    return false;
}

/// Encode @p source as the container @p profile reads.
///
/// **Reforged** takes `.dds`, and takes it block-compressed: BC1 where the
/// texture is opaque and BC3 where it is not. The sRGB flag is cleared first
/// because the writer promotes an sRGB texture to a DX10 header, and the
/// legacy `DXT1`/`DXT5` FourCC is what every Warcraft III tool in existence
/// reads.
///
/// **Classic** takes BLP1 and nothing else — not BLP2, which is World of
/// Warcraft's and which Warcraft III cannot open. Paletted rather than JPEG:
/// the JPEG path is lossy on exactly the hard edges a model texture is made of.
///
/// Mips are generated in both cases. Warcraft III minifies without them and the
/// result crawls.
std::optional<std::vector<u8>> EncodeForWc3(const tx::Texture& source, wem::ProfileId profile) {
    try {
        tx::Texture texture = source;
        texture.format(tx::PixelFormat::RGBA8);
        const bool alpha = HasAlpha(texture);
        texture.generateMipmaps(
            tx::computeMaxMipCount(texture.width(), texture.height(), texture.depth()));

        if (profile == wem::ProfileId::Wc3Reforged) {
            texture.setSrgb(false);
            texture.format(alpha ? tx::PixelFormat::BC3 : tx::PixelFormat::BC1);
            tx::dds::Writer writer;
            std::vector<u8> bytes = writer.write(texture);
            if (bytes.empty())
                return std::nullopt;
            return bytes;
        }

        tx::blp::SaveOptions options;
        options.version = tx::blp::BlpVersion::BLP1;
        options.encoding = tx::blp::BlpEncoding::Palettized;
        options.alpha = alpha ? tx::blp::BlpAlphaDepth::Eight : tx::blp::BlpAlphaDepth::Zero;
        tx::blp::Writer writer;
        std::vector<u8> bytes = writer.write(texture, options);
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Where one texture is written, relative to the model, and what the model then
/// calls it.
///
/// A texture the source named keeps its name and its folders — that is what a
/// Warcraft III path looks like and what a modder expects to find. One the
/// source only had an id for gets `<model>_<index>`, because `#158949.dds` says
/// nothing to anyone and two ids in one folder must not collide.
fs::path OutputRelPath(const std::string& fileName, const std::string& modelStem, std::size_t index,
                       const char* extension) {
    if (!fileName.empty() && fileName[0] != '#') {
        std::string normalized = fileName;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        fs::path relative = io::FsPathFromUtf8(normalized);
        relative.replace_extension(extension);
        return relative;
    }
    return io::FsPathFromUtf8(modelStem + "_" + std::to_string(index) + extension);
}

/// The same path as Warcraft III spells it. MDX texture paths use backslashes,
/// and a forward slash is the one thing the game's own loader will not take.
std::string ToMdxPath(const fs::path& relative) {
    std::string out = io::PathToUtf8(relative);
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

/// Write every file-backed texture of @p model beside it, converted, and
/// repoint the model at what was written.
///
/// `replaceableId != 0` is skipped: those name a runtime team colour, glow or
/// tileset that no file backs, in either game.
void ExportTextures(const MdxExportRequest& request, ::whiteout::mdx::Model& model,
                    const std::vector<wem::TextureRef>& refs, MdxExportReport& report) {
    const char* extension = Wc3TextureExtension(request.profile);
    const fs::path targetDir = request.outPath.parent_path();
    const std::string stem = io::PathToUtf8(request.outPath.stem());

    for (std::size_t i = 0; i < model.textures.size(); ++i) {
        ::whiteout::mdx::Texture& texture = model.textures[i];
        if (texture.replaceableId != 0)
            continue;

        // `toMdx` writes one MDX texture per document texture in order, so the
        // index is the join — the same one `NameIdTextures` makes. An id key is
        // the only thing the model itself cannot state.
        std::string key = texture.fileName;
        if (key.empty() && i < refs.size()) {
            const wem::TextureRef& ref = refs[i];
            if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key))
                key = "#" + std::to_string(sno->id);
            else if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key))
                key = "#" + std::to_string(fileId->value);
            else if (!ref.path.empty())
                key = ref.path;
        }
        if (key.empty())
            continue;

        const fs::path relative = OutputRelPath(key, stem, i, extension);
        const fs::path outFile = targetDir / relative;
        // Written before the read, so a texture that cannot be resolved still
        // leaves the model naming the file a user can drop in by hand.
        texture.fileName = ToMdxPath(relative);

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            ++report.texturesSkipped;
            continue;
        }
        if (request.provider == nullptr) {
            ++report.texturesFailed;
            continue;
        }

        std::string actualExt;
        std::optional<std::vector<u8>> bytes =
            request.provider->ReadFile(RefForTextureName(key), &actualExt);
        if (!bytes || bytes->empty()) {
            std::fprintf(stderr, "[viewer] Export MDX: texture not readable: %s\n", key.c_str());
            ++report.texturesFailed;
            continue;
        }
        actualExt = Lower(actualExt);
        if (actualExt.empty()) {
            // An id-addressed asset has no name to take an extension from — a
            // World of Warcraft root manifest stores none — so the container's
            // own magic answers instead.
            actualExt = renderer::model::SniffTextureExtension(
                std::span<const u8>(bytes->data(), bytes->size()));
        }

        std::optional<tx::Texture> decoded = renderer::model::DispatchTextureParser(
            actualExt, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        std::optional<std::vector<u8>> encoded =
            decoded ? EncodeForWc3(*decoded, request.profile) : std::nullopt;
        if (!encoded) {
            std::fprintf(stderr, "[viewer] Export MDX: texture convert failed %s -> %s\n",
                         key.c_str(), extension);
            ++report.texturesFailed;
            continue;
        }

        fs::create_directories(outFile.parent_path(), ec);
        std::ofstream file(outFile, std::ios::binary);
        if (file)
            file.write(reinterpret_cast<const char*>(encoded->data()),
                       static_cast<std::streamsize>(encoded->size()));
        if (!file) {
            ++report.texturesFailed;
            continue;
        }
        ++report.texturesExported;
    }
}

} // namespace

const char* Wc3TextureExtension(wem::ProfileId profile) {
    if (profile == wem::ProfileId::Wc3Reforged)
        return ".dds";
    if (profile == wem::ProfileId::Wc3Classic)
        return ".blp";
    return "";
}

MdxExportReport ExportModelAsMdx(const MdxExportRequest& request) {
    MdxExportReport report;

    if (request.source == nullptr) {
        report.error = "no model on screen";
        return report;
    }
    if (*Wc3TextureExtension(request.profile) == '\0') {
        report.error = std::string(wem::Profile(request.profile).displayName) +
                       " is not a Warcraft III profile";
        return report;
    }

    // Through WEM, always — even for a model that is already Warcraft III. The
    // converters only speak the interchange document, and a `.mdx` round trip
    // through it is the one this build tests every day.
    io::WemExportOptions wemOptions;
    wemOptions.documentName = request.modelName;
    wemOptions.materialLook = request.materialLook;
    const io::WemExportResult exported =
        io::ExportModelToWem(*request.source, request.provider, wemOptions);
    report.diagnostics.append(exported.diagnostics);
    if (!exported.ok()) {
        report.error = exported.error;
        return report;
    }
    report.formatId = exported.formatId;

    io::MdxExportOptions mdxOptions;
    mdxOptions.profile = request.profile;
    io::MdxExportResult converted = io::ConvertWemToMdx(*exported.document, mdxOptions);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = converted.error;
        return report;
    }
    report.scale = converted.scale;
    report.derived = converted.derived;

    ::whiteout::mdx::Model& model = *converted.model;
    if (request.exportTextures)
        ExportTextures(request, model, exported.document->textures, report);

    try {
        ::whiteout::mdx::Writer writer;
        writer.write(io::PathToUtf8(request.outPath), model);
    } catch (const std::exception& e) {
        report.error = std::string("could not write the model: ") + e.what();
        return report;
    }

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
