// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdx_export.h"

#include "sc2_pbr_export.h"

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
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = whiteout::textures;

/// How a StarCraft II normal map is restated for Reforged.
///
/// The packing is settled — x rides alpha on the way in and red on the way out
/// — and these two are not: they are conventions of the *art*, not of either
/// container, and they are named here rather than spelled at the call site
/// because they are the one thing in this file a future measurement could
/// overturn.
constexpr tx::pbr::NormalRestatement kNormalRestatement{/*swapXY=*/true, /*invertY=*/true};

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

/// Encode a texture the export *made* rather than read.
///
/// Separate from `EncodeForWc3` because every choice that one makes has already
/// been made: the container is fixed by what the map is (BC5 for a two-channel
/// normal, BC3 for an ORM whose alpha carries the team mask), and the texture
/// arrives linear and carrying its own `TextureKind`, which is what picks the
/// mip filter. Running it through the alpha sniff would demote a BC5 normal to
/// BC1 and box-filter a roughness map.
std::optional<std::vector<u8>> EncodeBaked(const BakedTexture& baked) {
    try {
        tx::Texture texture = baked.texture;
        texture.setSrgb(false);
        texture.generateMipmaps(
            tx::computeMaxMipCount(texture.width(), texture.height(), texture.depth()));
        texture.format(baked.format);
        tx::dds::Writer writer;
        std::vector<u8> bytes = writer.write(texture);
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// The folder a converted model's textures land in, named for the game they
/// came out of.
///
/// A Warcraft III mod is one flat namespace rooted at the map or the `.w3mod`,
/// and `Assets/Textures/Marine_Diffuse.dds` dropped straight into it is a name
/// collision waiting for the second game. One folder per source keeps a rip of
/// StarCraft II beside a rip of anything else, and keeps both out of the way of
/// Warcraft III's own `Textures/` — which the stock maps this export names live
/// in and must not be shadowed by.
///
/// Empty for a source with no folder of its own; those textures keep whatever
/// path the model gave them.
const char* AssetFolderFor(wem::ProfileId source) {
    switch (source) {
    case wem::ProfileId::Sc2:
        return "Star2";
    case wem::ProfileId::Heroes:
        return "Heroes";
    default:
        return "";
    }
}

/// Where one texture is written, relative to the model, and what the model then
/// calls it.
///
/// A texture the source named keeps its name and its folders — that is what a
/// Warcraft III path looks like and what a modder expects to find. One the
/// source only had an id for gets `<model>_<index>`, because `#158949.dds` says
/// nothing to anyone and two ids in one folder must not collide. Both go under
/// @p folder when the source has one.
fs::path OutputRelPath(const std::string& fileName, const std::string& modelStem, std::size_t index,
                       const char* extension, const char* folder) {
    fs::path relative;
    if (!fileName.empty() && fileName[0] != '#') {
        std::string normalized = fileName;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        relative = io::FsPathFromUtf8(normalized);
        relative.replace_extension(extension);
    } else {
        relative = io::FsPathFromUtf8(modelStem + "_" + std::to_string(index) + extension);
    }
    if (folder != nullptr && *folder != '\0') {
        return io::FsPathFromUtf8(folder) / relative;
    }
    return relative;
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
                    const std::vector<wem::TextureRef>& refs,
                    const std::map<u32, BakedTexture>& baked, const char* folder,
                    MdxExportReport& report) {
    const char* extension = Wc3TextureExtension(request.profile);
    const fs::path targetDir = request.outPath.parent_path();
    const std::string stem = io::PathToUtf8(request.outPath.stem());

    for (std::size_t i = 0; i < model.textures.size(); ++i) {
        ::whiteout::mdx::Texture& texture = model.textures[i];
        if (texture.replaceableId != 0)
            continue;
        // Past the document's own textures are the ones the CONVERSION named:
        // Warcraft III's stock neutral maps, one per HD slot a material had
        // nothing of its own for. Those ship with the game — there is no file
        // here to write, and rewriting the reference to `Textures/Black32.dds`
        // beside the model would point it at nothing.
        if (i >= refs.size())
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

        const fs::path relative = OutputRelPath(key, stem, i, extension, folder);
        const fs::path outFile = targetDir / relative;
        // Written before the read, so a texture that cannot be resolved still
        // leaves the model naming the file a user can drop in by hand.
        texture.fileName = ToMdxPath(relative);

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            ++report.texturesSkipped;
            continue;
        }
        if (request.provider == nullptr && baked.find(static_cast<u32>(i)) == baked.end()) {
            ++report.texturesFailed;
            continue;
        }

        // A baked map has no file behind it to read — it was made out of two
        // or three of the source's own maps a moment ago — so it short-circuits
        // the resolve and goes straight to the encoder.
        const auto wasBaked = baked.find(static_cast<u32>(i));
        if (wasBaked != baked.end()) {
            std::optional<std::vector<u8>> made = EncodeBaked(wasBaked->second);
            if (!made) {
                std::fprintf(stderr, "[viewer] Export MDX: could not encode the baked %s\n",
                             texture.fileName.c_str());
                ++report.texturesFailed;
                continue;
            }
            fs::create_directories(outFile.parent_path(), ec);
            std::ofstream made_file(outFile, std::ios::binary);
            if (made_file)
                made_file.write(reinterpret_cast<const char*>(made->data()),
                                static_cast<std::streamsize>(made->size()));
            if (!made_file) {
                ++report.texturesFailed;
                continue;
            }
            ++report.texturesExported;
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
    // A Heroes of the Storm model goes through StarCraft II on the way out.
    // Warcraft III has no shader graph either, so there is nothing to be gained
    // by carrying a MADD into a format that cannot read one — and everything
    // downstream, the surface crossing included, is written against the
    // standard material the retarget produces.
    wemOptions.retargetHeroesToStarCraft2 = true;
    const io::WemExportResult exported =
        io::ExportModelToWem(*request.source, request.provider, wemOptions);
    report.diagnostics.append(exported.diagnostics);
    if (!exported.ok()) {
        report.error = exported.error;
        return report;
    }
    report.formatId = exported.formatId;

    // StarCraft II states a surface as specular + gloss and Reforged as
    // roughness + metalness, and no slot reference crosses that: the ORM has to
    // be *made*. Done before the conversion, on the document, so the derived
    // material has its slot filled by the time `toMdx` reads it and the baked
    // map is a document texture like any other. A model that is not StarCraft
    // II leaves this untouched.
    wem::Document document = std::move(*exported.document);
    Sc2PbrBakeResult pbr;
    if (request.profile == wem::ProfileId::Wc3Reforged) {
        if (request.exportTextures) {
            pbr = BakeSc2AsReforgedPbr(document, request.provider, kNormalRestatement,
                                       report.diagnostics);
        } else if (document.carries(wem::ProfileId::Sc2) ||
                   document.carries(wem::ProfileId::Heroes)) {
            // The whole StarCraft II surface crossing is baked pixels — the ORM
            // that carries the roughness, the metalness and the team mask, and
            // the base colour that mask needs lightened. With the textures
            // turned off there is nowhere to put any of it, so the model goes
            // out with a Reforged material and no maps: grey, unlit-looking and
            // with no team colour at all. Worth saying, because from the
            // outside that reads as a broken converter.
            report.diagnostics.warn(
                wem::DiagCode::LossyKindConversion,
                "the StarCraft II surface conversion needs to write textures; with them turned "
                "off the model keeps no roughness, metalness or team colour");
        }
    }

    io::MdxExportOptions mdxOptions;
    mdxOptions.profile = request.profile;
    io::MdxExportResult converted = io::ConvertWemToMdx(document, mdxOptions);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = converted.error;
        return report;
    }
    report.scale = converted.scale;
    report.derived = converted.derived;

    ::whiteout::mdx::Model& model = *converted.model;
    if (request.exportTextures)
        ExportTextures(request, model, document.textures, pbr.baked,
                       AssetFolderFor(document.defaultProfile), report);

    // The folder, before the file. `ExportTextures` makes one per texture it
    // writes, which is why an export with textures beside it always worked and
    // one with none silently did not: `Writer::write` opens an `ofstream` and a
    // failed open is not an exception, so the export reported success and left
    // no file. Seven of forty Heroes effect models landed exactly there.
    std::error_code dirError;
    fs::create_directories(request.outPath.parent_path(), dirError);
    try {
        ::whiteout::mdx::Writer writer;
        writer.write(io::PathToUtf8(request.outPath), model);
    } catch (const std::exception& e) {
        report.error = std::string("could not write the model: ") + e.what();
        return report;
    }
    if (!fs::exists(request.outPath)) {
        report.error = "could not write the model to " + io::PathToUtf8(request.outPath);
        return report;
    }

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
