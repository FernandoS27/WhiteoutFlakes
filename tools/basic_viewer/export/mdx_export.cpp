// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdx_export.h"

#include "export_text.h"
#include "export_texture_set.h"
#include "sc2_pbr_export.h"
#include "texture_codec.h"
#include "texture_io.h"

#include "io/wem/wem_export.h"
#include "io/wem/wem_profiles.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/mdx/writer.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = ::whiteout::textures;
namespace wem = ::whiteout::models::wem;

/// How a StarCraft II normal map is restated for Reforged.
///
/// The packing is settled — x rides alpha on the way in and red on the way out
/// — and these two are not: they are conventions of the *art*, not of either
/// container, and they are named here rather than spelled at the call site
/// because they are the one thing in this file a future measurement could
/// overturn.
///
/// Overturned once already: the original swap+invert pair rotated every
/// tangent-space normal 90°. The reaper sweep scored identity best (5.92
/// against 6.07 for the rotation, with a FLAT map at 6.02 between them), and
/// the HD footman shield — the crispest normal-mapped surface in the corpus —
/// agrees on every metric (diff, edge energy, luminance correlation). The two
/// engines share the axis conventions; only the channel PACKING differs.
constexpr tx::pbr::NormalRestatement kNormalRestatement{/*swapXY=*/false, /*invertY=*/false};

/// Encode @p source as the container @p profile reads: Reforged's
/// block-compressed `.dds`, or classic's BLP1.
EncodedTexture EncodeForWc3(const tx::Texture& source, wem::ProfileId profile) {
    return profile == wem::ProfileId::Wc3Reforged ? EncodeGameDds(source)
                                                  : EncodeClassicBlp(source);
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
        relative = io::FsPathFromUtf8(export_text::ForwardSlashes(fileName));
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
    const ExportSubject& subject = request.subject;
    const char* extension = Wc3TextureExtension(request.options.profile);
    const fs::path targetDir = subject.outPath.parent_path();
    const std::string stem = io::PathToUtf8(subject.outPath.stem());
    const std::vector<bool> used = MdxTexturesUsed(model);
    TextureExportCounters& counters = report.textures;

    for (std::size_t i = 0; i < model.textures.size(); ++i) {
        ::whiteout::mdx::Texture& texture = model.textures[i];
        if (texture.replaceableId != 0)
            continue;
        // `toMdx` writes an entry per document texture, read or not. The caller
        // prunes the unread ones, so their files would serve nothing.
        if (!used[i]) {
            ++counters.unused;
            continue;
        }
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
        if (key.empty()) {
            key = TextureIdKey(refs[i]);
            if (key.empty())
                key = refs[i].path;
        }
        if (key.empty())
            continue;

        const fs::path relative = OutputRelPath(key, stem, i, extension, folder);
        const fs::path outFile = targetDir / relative;
        // Written before the read, so a texture that cannot be resolved still
        // leaves the model naming the file a user can drop in by hand.
        texture.fileName = ToMdxPath(relative);

        // A baked map has no file behind it to read — it was made out of two
        // or three of the source's own maps a moment ago — so it short-circuits
        // the resolve and goes straight to the encoder, over any file already
        // there: that file is an earlier export's bake, and keeping it hides
        // every fix to the bake.
        if (const auto wasBaked = baked.find(static_cast<u32>(i)); wasBaked != baked.end()) {
            const EncodedTexture made = EncodeBaked(wasBaked->second);
            if (!made.note.empty()) {
                report.diagnostics.warn(wem::DiagCode::Unspecified,
                                        "the baked texture '" + texture.fileName + "' " + made.note);
            }
            if (!made) {
                report.diagnostics.warn(wem::DiagCode::Unspecified,
                                        "the baked texture '" + texture.fileName +
                                            "' did not encode: " + made.problem);
                ++counters.failed;
                continue;
            }
            if (!WriteFileCreatingDirs(outFile, made.bytes)) {
                report.diagnostics.warn(wem::DiagCode::Unspecified,
                                        "could not write '" + io::PathToUtf8(outFile) + "'");
                ++counters.failed;
                continue;
            }
            ++counters.exported;
            continue;
        }

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            ++counters.skipped;
            continue;
        }
        if (subject.provider == nullptr) {
            ++counters.failed;
            continue;
        }

        const std::optional<TextureFile> file =
            ReadTextureFile(*subject.provider, ContentRefForKey(key));
        if (!file) {
            report.diagnostics.warn(wem::DiagCode::TextureUnresolved,
                                    "texture '" + key + "' is not readable");
            ++counters.failed;
            continue;
        }
        const TextureDecode decoded = DecodeTexture(file->bytes, file->extension);
        const EncodedTexture encoded = decoded.texture
                                           ? EncodeForWc3(*decoded.texture, request.options.profile)
                                           : EncodedTexture{{}, decoded.problem};
        if (!encoded) {
            report.diagnostics.warn(wem::DiagCode::TextureUnresolved,
                                    "texture '" + key + "' did not convert to " + extension +
                                        ": " + encoded.problem);
            ++counters.failed;
            continue;
        }
        if (!WriteFileCreatingDirs(outFile, encoded.bytes)) {
            report.diagnostics.warn(wem::DiagCode::Unspecified,
                                    "could not write '" + io::PathToUtf8(outFile) + "'");
            ++counters.failed;
            continue;
        }
        ++counters.exported;
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
    const ExportSubject& subject = request.subject;
    const MdxExportOptions& options = request.options;

    if (subject.source == nullptr) {
        report.error = "no model on screen";
        return report;
    }
    if (*Wc3TextureExtension(options.profile) == '\0') {
        report.error = std::string(wem::Profile(options.profile).displayName) +
                       " is not a Warcraft III profile";
        return report;
    }

    // Through WEM, always — even for a model that is already Warcraft III. The
    // converters only speak the interchange document, and a `.mdx` round trip
    // through it is the one this build tests every day.
    io::WemExportOptions wemOptions;
    wemOptions.documentName = subject.modelName;
    // A Heroes of the Storm model goes through StarCraft II on the way out.
    // Warcraft III has no shader graph either, so there is nothing to be gained
    // by carrying a MADD into a format that cannot read one — and everything
    // downstream, the surface crossing included, is written against the
    // standard material the retarget produces.
    wemOptions.retargetHeroesToStarCraft2 = true;
    std::optional<wem::Document> exported = ConvertThroughWem(subject, wemOptions, report);
    if (!exported)
        return report;
    wem::Document& document = *exported;

    // StarCraft II states a surface as specular + gloss and Reforged as
    // roughness + metalness, and no slot reference crosses that: the ORM has to
    // be *made*. Done before the conversion, on the document, so the derived
    // material has its slot filled by the time `toMdx` reads it and the baked
    // map is a document texture like any other. A model that is not StarCraft
    // II leaves this untouched.
    Sc2PbrBakeResult pbr;
    if (options.profile == wem::ProfileId::Wc3Reforged) {
        if (options.textures) {
            pbr = BakeSc2AsReforgedPbr(document, subject.provider, kNormalRestatement,
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
    mdxOptions.profile = options.profile;
    io::MdxExportResult converted = io::ConvertWemToMdx(document, mdxOptions);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = converted.error;
        return report;
    }
    report.scale = converted.scale;

    ::whiteout::mdx::Model& model = *converted.model;
    if (options.textures)
        ExportTextures(request, model, document.textures, pbr.baked,
                       AssetFolderFor(document.defaultProfile), report);
    // After the writes, which join `model.textures` to the document by index.
    // A table entry nothing reads is the same garbage whether or not its file
    // was written: StarCraft II's specular went into the baked ORM, and a
    // material that dropped a slot dropped its texture with it.
    PruneMdxTextures(model, MdxTexturesUsed(model));

    // The folder, before the file. `ExportTextures` makes one per texture it
    // writes, which is why an export with textures beside it always worked and
    // one with none silently did not: `Writer::write` opens an `ofstream` and a
    // failed open is not an exception, so the export reported success and left
    // no file. Seven of forty Heroes effect models landed exactly there.
    report.error = WriteModelFile(subject.outPath, [&](const std::string& path) {
        ::whiteout::mdx::Writer writer;
        writer.write(path, model);
    });
    if (!report.error.empty())
        return report;

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
